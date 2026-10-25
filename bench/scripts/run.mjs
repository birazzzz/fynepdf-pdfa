// Runs the bench: every tool on every file of the chosen sets, then checks each result.
//
//   node scripts/run.mjs --run baseline --tools ghostscript,ocrmypdf,libreoffice,picture-copy \
//        --sets a,b,c,d,e [--tasks pdfa-2b,grayscale] [--limit 20] [--steps convert,validate,inspect,compare]
//
// Steps, each resumable (a finished step is skipped when the run is started again):
//   convert   each tool makes its output for each task the file is meant for
//   validate  veraPDF checks inputs and outputs against the level asked for
//   inspect   pdf.js reads pages, bookmarks, links, fields, comments, attachments, tags, text
//   compare   Poppler draws the first pages of input and output; the text is compared
// Everything lands in /data/results/<run>/: the outputs, one record per conversion, and one
// per input. report.mjs turns them into the report.

import fs from "node:fs";
import path from "node:path";

import { colourLeft, compareLooks, compareText } from "./compare.mjs";
import { family } from "./families.mjs";
import { log, pool, readJson, RESULTS, run, SETS, sha256File, workers, writeJson } from "./lib.mjs";
import { TOOLS } from "./tools.mjs";

function args() {
  const parsed = { run: "baseline", tools: Object.keys(TOOLS).join(","), sets: "a,b,c,d,e", tasks: "", limit: "0", steps: "convert,validate,inspect,compare" };
  const argv = process.argv.slice(2);
  for (let i = 0; i < argv.length; i += 2) parsed[argv[i].replace(/^--/, "")] = argv[i + 1];
  return {
    run: parsed.run,
    tools: parsed.tools.split(",").filter(Boolean),
    sets: parsed.sets.split(",").filter(Boolean),
    tasks: parsed.tasks.split(",").filter(Boolean),
    limit: Number(parsed.limit) || 0,
    steps: new Set(parsed.steps.split(",")),
  };
}

const PUBLISH = { a: "yes", b: "yes", c: "numbers-only", d: "no", e: "yes" };

/** Every file of the chosen sets, with what it is for. */
function loadFiles(sets, limit) {
  const files = [];
  for (const set of sets) {
    const index = readJson(path.join(SETS, set, "index.json"));
    if (!index) throw new Error(`set ${set} has no index; build or import it first`);
    let entries = index.files;
    if (limit) entries = entries.slice(0, limit);
    for (const entry of entries) {
      const id = entry.id.startsWith(`${set}-`) ? entry.id : `${set}-${entry.id}`;
      files.push({
        set,
        id,
        path: path.join(SETS, set, entry.file),
        title: entry.title ?? null,
        features: entry.features ?? [],
        levels: entry.levels ?? ["2b"],
        expect: entry.expect ?? null,
        clause: entry.clause ?? null,
        family: set === "a" ? "FynePDF bench (made by script)" : family(entry.producer, entry.creator),
        publish: PUBLISH[set],
      });
    }
  }
  return files;
}

function tasksFor(file, only) {
  const tasks = file.levels.map((level) => `pdfa-${level}`);
  if (file.set !== "b") tasks.push("grayscale");
  return only.length ? tasks.filter((task) => only.includes(task)) : tasks;
}

async function main() {
  const options = args();
  const root = path.join(RESULTS, options.run);
  const files = loadFiles(options.sets, options.limit);
  const jobs = [];
  for (const file of files)
    for (const task of tasksFor(file, options.tasks))
      for (const tool of options.tools) if (TOOLS[tool].tasks.includes(task)) jobs.push({ file, task, tool });

  const recordPath = (job) => path.join(root, "records", job.tool, job.task, job.file.set, `${job.file.id}.json`);
  const outputPath = (job) => path.join(root, "out", job.tool, job.task, job.file.set, `${job.file.id}.pdf`);
  const inputPath = (file) => path.join(root, "inputs", file.set, `${file.id}.json`);
  const textPath = (kind, key) => path.join(root, "text", kind, `${key}.txt`);
  const outputKey = (job) => `${job.tool}/${job.task}/${job.file.set}/${job.file.id}`;

  // The run's description: tools and versions, the files and their checksums. A tool added to
  // an existing run (ours, batch by batch) joins its list.
  const runFile = path.join(root, "run.json");
  const described = readJson(runFile, null) ?? { run: options.run, started: new Date().toISOString(), workers, tools: [], sets: [], versions: {} };
  for (const tool of options.tools) {
    if (!described.tools.includes(tool)) described.tools.push(tool);
    described.versions[tool] = { label: TOOLS[tool].label, version: await TOOLS[tool].version() };
  }
  for (const set of options.sets) if (!described.sets.includes(set)) described.sets.push(set);
  described.versions.verapdf ??= (await run("verapdf", ["--version"])).stdout.split("\n")[0].trim();
  described.versions.poppler ??= (await run("pdftoppm", ["-v"])).stderr.split("\n")[0].trim();
  writeJson(runFile, described);
  for (const file of files) {
    const target = inputPath(file);
    if (!fs.existsSync(target)) writeJson(target, { ...file, bytes: fs.statSync(file.path).size, sha256: sha256File(file.path) });
  }
  log(`${options.run}: ${files.length} files, ${jobs.length} conversions, ${workers} at a time`);

  if (options.steps.has("convert")) {
    let done = 0;
    await pool(jobs, workers, async (job, _, slot) => {
      const record = recordPath(job);
      if (readJson(record)?.convert) return;
      const output = outputPath(job);
      fs.mkdirSync(path.dirname(output), { recursive: true });
      fs.rmSync(output, { force: true });
      const result = await TOOLS[job.tool].convert(job.file.path, output, job.task, slot);
      const made = fs.existsSync(output) && fs.statSync(output).size > 0;
      // A tool that says what it did (ours prints one line of JSON) has it kept.
      let report;
      try {
        report = JSON.parse(result.stdout);
      } catch {}
      writeJson(record, {
        tool: job.tool,
        task: job.task,
        set: job.file.set,
        id: job.file.id,
        convert: {
          ok: result.code === 0 && made,
          code: result.code,
          timedOut: result.timedOut,
          seconds: Math.round(result.seconds * 1000) / 1000,
          maxRssKB: result.maxRssKB,
          outBytes: made ? fs.statSync(output).size : null,
          error: result.code === 0 && made ? undefined : (result.stderr || result.stdout).trim().split("\n").slice(-4).join("\n").slice(0, 600),
          report: report && typeof report === "object" ? report : undefined,
        },
      });
      if (++done % 200 === 0) log(`converted ${done}/${jobs.length}`);
    });
    log("convert: done");
  }

  if (options.steps.has("validate")) {
    // One veraPDF run per level, over inputs and outputs together, in batches.
    const byLevel = new Map();
    const want = (level, file, apply) => {
      if (!byLevel.has(level)) byLevel.set(level, []);
      byLevel.get(level).push({ file, apply });
    };
    for (const file of files)
      for (const level of file.levels) {
        if (options.tasks.length && !options.tasks.includes(`pdfa-${level}`)) continue;
        const target = inputPath(file);
        if (readJson(target)?.validation?.[level]) continue;
        want(level, file.path, (verdict) => {
          const record = readJson(target);
          record.validation = { ...record.validation, [level]: verdict };
          writeJson(target, record);
        });
      }
    for (const job of jobs) {
      const level = job.task.match(/^pdfa-(\d\w)$/)?.[1];
      if (!level) continue;
      const record = readJson(recordPath(job));
      if (!record?.convert?.ok || record.validation) continue;
      want(level, outputPath(job), (verdict) => {
        const current = readJson(recordPath(job));
        current.validation = verdict;
        writeJson(recordPath(job), current);
      });
    }
    for (const [level, items] of byLevel) {
      log(`validate ${level}: ${items.length} files`);
      for (let i = 0; i < items.length; i += 300) await validateBatch(level, items.slice(i, i + 300));
    }
    log("validate: done");
  }

  if (options.steps.has("inspect")) {
    const tasks = [];
    for (const file of files) {
      const target = inputPath(file);
      if (readJson(target)?.inspect) continue;
      tasks.push(async () => {
        const inspect = await inspectFile(file.path, textPath("inputs", `${file.set}/${file.id}`));
        writeJson(target, { ...readJson(target), inspect });
      });
    }
    for (const job of jobs) {
      const record = readJson(recordPath(job));
      if (!record?.convert?.ok || record.inspect) continue;
      tasks.push(async () => {
        const inspect = await inspectFile(outputPath(job), textPath("outputs", outputKey(job)));
        writeJson(recordPath(job), { ...readJson(recordPath(job)), inspect });
      });
    }
    log(`inspect: ${tasks.length} files`);
    await pool(tasks, workers + 2, (task) => task());
    log("inspect: done");
  }

  if (options.steps.has("compare")) {
    const todo = jobs.filter((job) => {
      const record = readJson(recordPath(job));
      return record?.convert?.ok && (!record.looks || (job.task === "grayscale" && !record.colour));
    });
    log(`compare: ${todo.length} conversions`);
    let done = 0;
    await pool(todo, workers + 2, async (job) => {
      const record = readJson(recordPath(job));
      if (!record.looks) {
        record.looks = await compareLooks(job.file.path, outputPath(job), { gray: job.task === "grayscale" });
        const read = (file) => (fs.existsSync(file) ? fs.readFileSync(file, "utf8") : "");
        record.text = compareText(read(textPath("inputs", `${job.file.set}/${job.file.id}`)), read(textPath("outputs", outputKey(job))));
      }
      // A Grayscale result should have no colour left at all.
      if (job.task === "grayscale" && !record.colour) record.colour = await colourLeft(outputPath(job));
      writeJson(recordPath(job), record);
      if (++done % 500 === 0) log(`compared ${done}/${todo.length}`);
    });
    log("compare: done");
  }
  writeJson(runFile, { ...readJson(runFile), finished: new Date().toISOString() });
}

async function inspectFile(file, textOut) {
  fs.mkdirSync(path.dirname(textOut), { recursive: true });
  const result = await run("node", ["/bench/scripts/inspect-file.mjs", file, textOut], { timeoutMs: 120_000 });
  try {
    return JSON.parse(result.stdout);
  } catch {
    return { ok: false, error: result.timedOut ? "timed out" : (result.stderr || "no output").trim().split("\n").slice(-1)[0].slice(0, 300) };
  }
}

/** veraPDF over a batch of files at one level; each item's |apply| gets its verdict. */
async function validateBatch(level, items) {
  const report = await validateToFile(level, items, 1_200_000);
  const byName = new Map();
  // One process writes {report}; several write {reports: [{report}, ...]}.
  const collect = (r) => {
    for (const part of r?.reports ?? (r ? [r] : [])) for (const job of part?.report?.jobs ?? []) byName.set(job.itemDetails?.name, job);
  };
  collect(report);
  // A batch that died or hung (a veraPDF worker crashing on one file leaves the rest waiting):
  // the files it didn't report on, one at a time.
  for (const item of items.filter((i) => !byName.has(i.file))) collect(await validateToFile(level, [item], 300_000, 1));
  for (const item of items) {
    const job = byName.get(item.file);
    const verdict = job?.validationResult?.[0];
    if (!verdict) {
      item.apply({ compliant: false, error: job?.taskException?.exceptionMessage || job?.taskResult?.[0]?.exceptionMessage || "veraPDF could not check the file" });
      continue;
    }
    item.apply({
      compliant: verdict.compliant === true,
      status: verdict.jobEndStatus,
      failedRules: (verdict.details?.ruleSummaries ?? [])
        .filter((rule) => rule.status === "failed" || rule.ruleStatus === "FAILED")
        .map((rule) => ({ clause: rule.clause, test: rule.testNumber, checks: rule.failedChecks, description: (rule.description || "").slice(0, 240) })),
    });
  }
}

// veraPDF's report for a batch is larger than run() keeps of a program's output, so it goes
// to a file.
async function validateToFile(level, items, timeoutMs, processes = 3) {
  const out = path.join("/tmp", `vera-${process.pid}-${Date.now()}.json`);
  await run("sh", ["-c", `verapdf --flavour ${level} --format json --maxfailuresdisplayed 1 --processes ${processes} "$@" > ${out}`, "sh", ...items.map((item) => item.file)], {
    timeoutMs,
    env: { JAVA_OPTS: "-Xmx2g" },
  });
  const report = readJson(out);
  fs.rmSync(out, { force: true });
  return report;
}

await main();
