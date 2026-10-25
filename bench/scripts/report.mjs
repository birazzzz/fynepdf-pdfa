// Turns a run's records into the report: report.json (every number) and report.html.
//
//   node scripts/report.mjs --run baseline
//
// Writes /bench/reports/<run>/public/ (sets A, B, C and E: what can be shown to anyone) and
// /bench/reports/<run>/internal/ (everything, with set D, FynePDF's own files).

import fs from "node:fs";
import path from "node:path";

import { readJson, RESULTS, writeJson } from "./lib.mjs";

const runName = process.argv[process.argv.indexOf("--run") + 1] || "baseline";
const root = path.join(RESULTS, runName);
const meta = readJson(path.join(root, "run.json"));
if (!meta) throw new Error(`no run called ${runName}`);

const SET_NAMES = { a: "A. Feature set", b: "B. Rule set (veraPDF corpus)", c: "C. Real world (1,000 web PDFs)", d: "D. FynePDF's own test files", e: "E. Office and print" };
const TASKS = ["pdfa-1b", "pdfa-2b", "pdfa-3b", "grayscale"];
const TEXT_LOST = 0.9;

function walk(dir) {
  if (!fs.existsSync(dir)) return [];
  return fs.readdirSync(dir, { withFileTypes: true }).flatMap((entry) => (entry.isDirectory() ? walk(path.join(dir, entry.name)) : [path.join(dir, entry.name)]));
}

const inputs = new Map(walk(path.join(root, "inputs")).map((file) => [path.basename(file, ".json"), readJson(file)]));
const records = walk(path.join(root, "records")).map((file) => readJson(file)).filter(Boolean);

const total = (object) => Object.values(object || {}).reduce((sum, n) => sum + n, 0);

/** What the output lost of what the input had: bookmarks, links, form fields, ... */
function losses(input, output) {
  if (!input?.ok || !output?.ok) return [];
  const lost = [];
  for (const key of ["bookmarks", "links", "fields", "signatures", "attachments"]) if ((output[key] ?? 0) < (input[key] ?? 0)) lost.push(key);
  if (total(output.annotations) < total(input.annotations)) lost.push("comments");
  if (input.tagged && !output.tagged) lost.push("tags");
  if ((output.pages ?? 0) < (input.pages ?? 0)) lost.push("pages");
  return lost;
}

/** One conversion, judged. */
function judge(record) {
  const input = inputs.get(record.id);
  const level = record.task.match(/^pdfa-(\d\w)$/)?.[1];
  const converted = Boolean(record.convert?.ok);
  const valid = level ? Boolean(record.validation?.compliant) : null;
  const claim = record.inspect?.claim ?? null;
  return {
    ...record,
    input,
    level,
    converted,
    valid,
    falseClaim: Boolean(level && converted && claim && !valid),
    textKept: record.text ?? null,
    textLost: record.text != null && record.text < TEXT_LOST,
    looksSame: record.looks?.comparable ? record.looks.pagesDifferent === 0 && record.looks.pagesMissing === 0 : null,
    lost: losses(input?.inspect, record.inspect),
    colourLeft: record.colour?.comparable ? record.colour.pagesWithColour > 0 : null,
    sizeRatio: converted && input?.bytes ? record.convert.outBytes / input.bytes : null,
    seconds: record.convert?.seconds ?? null,
    memoryMB: record.convert?.maxRssKB ? record.convert.maxRssKB / 1024 : null,
    alreadyValid: level ? Boolean(input?.validation?.[level]?.compliant) : null,
  };
}

const judged = records.map(judge).filter((j) => j.input);

function quantile(values, q) {
  const sorted = values.filter((v) => v != null && Number.isFinite(v)).sort((a, b) => a - b);
  if (!sorted.length) return null;
  return sorted[Math.min(sorted.length - 1, Math.floor(q * sorted.length))];
}

/** The numbers for a group of conversions. */
function summarise(group) {
  const n = group.length;
  const count = (fn) => group.filter(fn).length;
  const withText = group.filter((j) => j.textKept != null);
  const comparable = group.filter((j) => j.looksSame != null);
  return {
    files: n,
    converted: count((j) => j.converted),
    valid: count((j) => j.valid),
    falseClaims: count((j) => j.falseClaim),
    unlabelledInvalid: count((j) => j.converted && j.level && !j.valid && !j.falseClaim),
    failed: count((j) => !j.converted),
    timedOut: count((j) => j.convert?.timedOut),
    withText: withText.length,
    textLost: count((j) => j.textLost),
    comparable: comparable.length,
    looksDifferent: count((j) => j.looksSame === false),
    lostSomething: count((j) => j.lost.length > 0),
    colourChecked: count((j) => j.colourLeft != null),
    colourLeft: count((j) => j.colourLeft === true),
    lostByKind: Object.fromEntries(["bookmarks", "links", "fields", "signatures", "comments", "attachments", "tags", "pages"].map((kind) => [kind, count((j) => j.lost.includes(kind))])),
    sizeRatioMedian: quantile(group.map((j) => j.sizeRatio), 0.5),
    secondsMedian: quantile(group.map((j) => j.seconds), 0.5),
    secondsP95: quantile(group.map((j) => j.seconds), 0.95),
    memoryMedianMB: quantile(group.map((j) => j.memoryMB), 0.5),
    memoryMaxMB: quantile(group.map((j) => j.memoryMB), 1),
  };
}

function groupBy(items, key) {
  const map = new Map();
  for (const item of items) {
    const k = key(item);
    if (!map.has(k)) map.set(k, []);
    map.get(k).push(item);
  }
  return map;
}

function topRules(group, limit = 8) {
  const counts = new Map();
  for (const j of group) {
    if (!j.converted || j.valid || !j.level) continue;
    for (const rule of j.validation?.failedRules ?? []) {
      const key = `${rule.clause}-${rule.test}`;
      const entry = counts.get(key) ?? { clause: rule.clause, test: rule.test, description: rule.description, files: 0 };
      entry.files++;
      counts.set(key, entry);
    }
  }
  return [...counts.values()].sort((a, b) => b.files - a.files).slice(0, limit);
}

function build(includePrivate) {
  const pool = judged.filter((j) => includePrivate || j.set !== "d");
  const tools = meta.tools;
  const sets = meta.sets.filter((s) => includePrivate || s !== "d");
  const data = { run: meta.run, started: meta.started, finished: meta.finished, versions: meta.versions, workers: meta.workers, sets: {}, overall: {}, bySet: {}, features: {}, rules: {}, families: {}, topRules: {} };

  for (const set of sets) {
    const files = [...inputs.values()].filter((i) => i.set === set);
    data.sets[set] = { name: SET_NAMES[set], files: files.length, publish: files[0]?.publish };
  }
  for (const tool of tools) {
    data.overall[tool] = {};
    data.topRules[tool] = {};
    for (const task of TASKS) {
      const group = pool.filter((j) => j.tool === tool && j.task === task);
      if (!group.length) continue;
      data.overall[tool][task] = summarise(group);
      if (task !== "grayscale") data.topRules[tool][task] = topRules(group);
    }
    data.bySet[tool] = {};
    for (const set of sets) {
      data.bySet[tool][set] = {};
      for (const task of TASKS) {
        const group = pool.filter((j) => j.tool === tool && j.task === task && j.set === set);
        if (group.length) data.bySet[tool][set][task] = summarise(group);
      }
    }
  }

  // Sets A and E, file by file.
  for (const set of ["a", "e"].filter((s) => sets.includes(s))) {
    data.features[set] = [...inputs.values()]
      .filter((i) => i.set === set)
      .sort((x, y) => x.id.localeCompare(y.id))
      .map((input) => ({
        id: input.id,
        title: input.title,
        features: input.features,
        alreadyValid: Object.fromEntries(Object.entries(input.validation ?? {}).map(([level, v]) => [level, v.compliant])),
        results: Object.fromEntries(
          pool
            .filter((j) => j.id === input.id)
            .map((j) => [
              `${j.tool}|${j.task}`,
              { converted: j.converted, valid: j.valid, falseClaim: j.falseClaim, textLost: j.textLost, looksSame: j.looksSame, lost: j.lost, error: j.converted ? undefined : j.convert?.error?.split("\n").slice(-1)[0]?.slice(0, 160), rules: (j.validation?.failedRules ?? []).map((r) => r.clause) },
            ]),
        ),
      }));
  }

  // Set B: a "fail" file should come out valid; a "pass" file should stay valid.
  if (sets.includes("b")) {
    for (const tool of tools) {
      data.rules[tool] = {};
      for (const level of ["1b", "2b", "3b"]) {
        const group = pool.filter((j) => j.tool === tool && j.set === "b" && j.level === level);
        if (!group.length) continue;
        const fixable = group.filter((j) => j.input.expect === "fail");
        const clean = group.filter((j) => j.input.expect === "pass");
        const byClause = [...groupBy(fixable, (j) => j.input.clause).entries()].map(([clause, items]) => ({ clause, files: items.length, fixed: items.filter((j) => j.valid).length, falseClaims: items.filter((j) => j.falseClaim).length }));
        data.rules[tool][level] = {
          fixable: fixable.length,
          fixed: fixable.filter((j) => j.valid).length,
          clean: clean.length,
          stillValid: clean.filter((j) => j.valid).length,
          falseClaims: group.filter((j) => j.falseClaim).length,
          failed: group.filter((j) => !j.converted).length,
          worstClauses: byClause.sort((x, y) => x.fixed / x.files - y.fixed / y.files || y.files - x.files).slice(0, 12),
        };
      }
    }
  }

  // Set C, by the program that made the file.
  if (sets.includes("c")) {
    const families = [...groupBy([...inputs.values()].filter((i) => i.set === "c"), (i) => i.family).entries()].sort((x, y) => y[1].length - x[1].length);
    data.families = {
      order: families.map(([name, files]) => ({ name, files: files.length })),
      tools: Object.fromEntries(
        tools.map((tool) => [
          tool,
          Object.fromEntries(
            families.map(([name]) => {
              const group = pool.filter((j) => j.tool === tool && j.set === "c" && j.task === "pdfa-2b" && j.input.family === name);
              return [name, group.length ? summarise(group) : null];
            }),
          ),
        ]),
      ),
    };
  }
  return data;
}

// ---------------------------------------------------------------------------------------------
// HTML

const SHORT = { ghostscript: "Ghostscript 10.00 (production)", "ghostscript-latest": "Ghostscript 10.08", ocrmypdf: "OCRmyPDF", libreoffice: "LibreOffice", "picture-copy": "Picture copy", "fynepdf-pdfa": "Ours (fynepdf-pdfa)", "fynepdf-grayscale": "Ours (fynepdf-grayscale)" };
const short = (tool) => SHORT[tool] ?? tool;

const esc = (value) => String(value ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
const pct = (part, whole) => (whole ? `${((100 * part) / whole).toFixed(part === whole || part === 0 ? 0 : 1)}%` : "–");
const num = (value, digits = 2) => (value == null ? "–" : Number(value).toFixed(digits));

function overallTable(data, tasks) {
  const rows = [];
  for (const tool of Object.keys(data.overall)) {
    for (const task of tasks) {
      const s = data.overall[tool][task];
      if (!s) continue;
      const pdfa = task !== "grayscale";
      rows.push(`<tr>
        <th scope="row" class="tool">${esc(short(tool))}</th><td class="nowrap">${task}</td><td class="n">${s.files}</td>
        <td class="n">${pct(s.converted, s.files)}</td>
        ${pdfa ? `<td class="n strong">${pct(s.valid, s.files)}</td><td class="n ${s.falseClaims ? "bad" : ""}">${s.falseClaims}</td>` : `<td class="n ${s.colourLeft ? "bad" : ""}">${s.colourLeft} <small>of ${s.colourChecked}</small></td>`}
        <td class="n ${s.textLost ? "warn" : ""}">${s.textLost} <small>of ${s.withText}</small></td>
        <td class="n ${s.looksDifferent ? "warn" : ""}">${s.looksDifferent} <small>of ${s.comparable}</small></td>
        <td class="n ${s.lostSomething ? "warn" : ""}">${s.lostSomething}</td>
        <td class="n">${num(s.sizeRatioMedian)}×</td>
        <td class="n">${num(s.secondsMedian)} / ${num(s.secondsP95)} s</td>
        <td class="n">${num(s.memoryMedianMB, 0)} / ${num(s.memoryMaxMB, 0)} MB</td>
      </tr>`);
    }
  }
  const pdfa = tasks[0] !== "grayscale";
  return `<div class="scroll"><table>
    <thead><tr><th>Tool</th><th>Task</th><th class="n">Files</th><th class="n">Converted</th>
    ${pdfa ? `<th class="n">Valid PDF/A</th><th class="n">False claims</th>` : `<th class="n">Colour left</th>`}
    <th class="n">Text lost</th><th class="n">Pages look different</th><th class="n">Lost bookmarks, links, forms…</th>
    <th class="n">Size (median)</th><th class="n">Time (median / 95%)</th><th class="n">Memory (median / max)</th></tr></thead>
    <tbody>${rows.join("")}</tbody></table></div>`;
}

/** One task, one row per set and tool: the real-world set reads very differently from the rule set. */
function bySetTable(data, task) {
  const rows = [];
  for (const [set, info] of Object.entries(data.sets)) {
    for (const tool of Object.keys(data.bySet)) {
      const s = data.bySet[tool][set]?.[task];
      if (!s) continue;
      const verdict =
        task === "grayscale"
          ? `<td class="n ${s.colourLeft ? "bad" : ""}">${s.colourLeft} <small>of ${s.colourChecked}</small></td>`
          : `<td class="n strong">${pct(s.valid, s.files)}</td><td class="n ${s.falseClaims ? "bad" : ""}">${s.falseClaims}</td>`;
      rows.push(`<tr><td class="nowrap">${esc(info.name.split(" (")[0])}</td><th scope="row" class="tool">${esc(short(tool))}</th><td class="n">${s.files}</td>
        ${verdict}<td class="n">${s.failed}</td>
        <td class="n ${s.textLost ? "warn" : ""}">${s.textLost} <small>of ${s.withText}</small></td>
        <td class="n ${s.looksDifferent ? "warn" : ""}">${s.looksDifferent} <small>of ${s.comparable}</small></td>
        <td class="n">${s.lostByKind.bookmarks} / ${s.lostByKind.links} / ${s.lostByKind.fields} / ${s.lostByKind.tags}</td>
        <td class="n">${num(s.sizeRatioMedian)}×</td><td class="n">${num(s.secondsMedian)} / ${num(s.secondsP95)} s</td><td class="n">${num(s.memoryMaxMB, 0)} MB</td></tr>`);
    }
  }
  const verdictHead = task === "grayscale" ? `<th class="n">Colour left</th>` : `<th class="n">Valid</th><th class="n">False claims</th>`;
  return `<div class="scroll"><table><thead><tr><th>Set</th><th>Tool</th><th class="n">Files</th>${verdictHead}<th class="n">Failed</th>
    <th class="n">Text lost</th><th class="n">Pages look different</th><th class="n">Files losing bookmarks / links / fields / tags</th><th class="n">Size</th><th class="n">Time (median / 95%)</th><th class="n">Memory (max)</th></tr></thead>
    <tbody>${rows.join("")}</tbody></table></div>`;
}

function featureTable(data, set) {
  const tools = Object.keys(data.overall);
  const columns = [];
  for (const task of TASKS) for (const tool of tools) if (data.overall[tool][task]) columns.push([tool, task]);
  const cell = (r) => {
    if (!r) return `<td class="c muted">·</td>`;
    if (!r.converted) return `<td class="c bad" title="${esc(r.error)}">failed</td>`;
    const marks = [];
    if (r.valid === true) marks.push(`<span class="ok">valid</span>`);
    else if (r.falseClaim) marks.push(`<span class="bad">false claim</span>`);
    else if (r.valid === false) marks.push(`<span class="warn">not valid</span>`);
    if (r.textLost) marks.push(`<span class="bad">text lost</span>`);
    if (r.looksSame === false) marks.push(`<span class="warn">looks different</span>`);
    if (r.lost.length) marks.push(`<span class="warn">lost ${esc(r.lost.join(", "))}</span>`);
    if (!marks.length) marks.push(`<span class="ok">ok</span>`);
    return `<td class="c" title="${esc(r.rules.join(" "))}">${marks.join("<br>")}</td>`;
  };
  const head = columns.map(([tool, task]) => `<th class="c rot"><span>${esc(short(tool))}<br>${task}</span></th>`).join("");
  const body = data.features[set]
    .map((f) => `<tr><th scope="row" class="file">${esc(f.id.replace(/^[ae]-/, ""))}<small>${esc(f.title ?? "")}</small></th>${columns.map(([tool, task]) => cell(f.results[`${tool}|${task}`])).join("")}</tr>`)
    .join("");
  return `<div class="scroll"><table class="grid"><thead><tr><th>File</th>${head}</tr></thead><tbody>${body}</tbody></table></div>`;
}

function rulesTable(data) {
  const rows = [];
  for (const [tool, levels] of Object.entries(data.rules))
    for (const [level, r] of Object.entries(levels))
      rows.push(`<tr><th scope="row" class="tool">${esc(short(tool))}</th><td>${level}</td><td class="n strong">${r.fixed} <small>of ${r.fixable}</small> (${pct(r.fixed, r.fixable)})</td>
        <td class="n">${r.stillValid} <small>of ${r.clean}</small> (${pct(r.stillValid, r.clean)})</td><td class="n ${r.falseClaims ? "bad" : ""}">${r.falseClaims}</td><td class="n">${r.failed}</td></tr>`);
  return `<div class="scroll"><table><thead><tr><th>Tool</th><th>Level</th><th class="n">Broken files made valid</th><th class="n">Valid files kept valid</th><th class="n">False claims</th><th class="n">Failed to convert</th></tr></thead><tbody>${rows.join("")}</tbody></table></div>`;
}

function familiesTable(data) {
  const tools = Object.keys(data.families.tools ?? {}).filter((tool) => Object.values(data.families.tools[tool]).some(Boolean));
  const head = tools.map((tool) => `<th class="n">${esc(short(tool))}</th>`).join("");
  const body = data.families.order
    .map(({ name, files }) => `<tr><th scope="row">${esc(name)}</th><td class="n">${files}</td>${tools.map((tool) => {
      const s = data.families.tools[tool][name];
      return `<td class="n">${s ? `${pct(s.valid, s.files)}${s.falseClaims ? ` <span class="bad">${s.falseClaims} false</span>` : ""}${s.textLost ? ` <span class="warn">${s.textLost} text</span>` : ""}` : "–"}</td>`;
    }).join("")}</tr>`)
    .join("");
  return `<div class="scroll"><table><thead><tr><th>Made by</th><th class="n">Files</th>${head}</tr></thead><tbody>${body}</tbody></table></div>`;
}

function rulesList(data) {
  const parts = [];
  for (const [tool, tasks] of Object.entries(data.topRules)) {
    const rules = tasks["pdfa-2b"] ?? [];
    if (!rules.length) continue;
    parts.push(`<h3>${esc(short(tool))}: PDF/A-2b</h3><ol class="rules">${rules.map((r) => `<li><b>${esc(r.clause)} (test ${r.test})</b>, ${r.files} files. <span class="muted">${esc(r.description)}</span></li>`).join("")}</ol>`);
  }
  return parts.join("");
}

function html(data, includePrivate) {
  const setsList = Object.entries(data.sets).map(([set, s]) => `<li><b>${esc(s.name)}</b>: ${s.files} files${set === "c" ? " (results and file numbers only; the files aren't redistributed)" : ""}${set === "d" ? " (private, never published)" : ""}</li>`).join("");
  const versions = Object.entries(data.versions).map(([key, v]) => `<li>${esc(typeof v === "string" ? v : `${v.label}: ${v.version}`)}</li>`).join("");
  return `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>PDF/A Converter Bench</title>
<style>
:root { --bg:#fbfaf7; --fg:#1d2433; --muted:#6b7385; --line:#e3e1da; --head:#f1efe8; --ok:#1f7a3f; --warn:#a15c00; --bad:#b3261e; --accent:#2f6fde; }
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) { --bg:#16181d; --fg:#e7e9ee; --muted:#9aa1b0; --line:#2c3038; --head:#1f2229; --ok:#5cc58a; --warn:#e3a84f; --bad:#f07b72; --accent:#7aa7ff; } }
body { background:var(--bg); color:var(--fg); font:15px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; margin:0; padding:24px 16px 64px; }
main { max-width:1200px; margin:0 auto; }
h1 { font-size:28px; margin:0 0 4px; } h2 { margin-top:40px; border-bottom:1px solid var(--line); padding-bottom:6px; } h3 { margin-top:24px; font-size:16px; }
.muted, small { color:var(--muted); } .lead { color:var(--muted); max-width:760px; }
.scroll { overflow-x:auto; border:1px solid var(--line); border-radius:8px; }
table { border-collapse:collapse; width:100%; font-size:13px; }
th, td { padding:6px 8px; border-bottom:1px solid var(--line); text-align:left; vertical-align:top; }
thead th { background:var(--head); position:sticky; top:0; font-weight:600; }
.n { text-align:right; white-space:nowrap; } .c { text-align:center; } .strong { font-weight:700; }
.nowrap { white-space:nowrap; } th.tool { min-width:140px; }
.ok { color:var(--ok); } .warn { color:var(--warn); } .bad { color:var(--bad); font-weight:600; }
.grid td { font-size:11px; min-width:62px; } .grid th.file { min-width:220px; } .grid th.file small { display:block; font-weight:400; }
.rot span { display:inline-block; font-size:11px; line-height:1.2; }
.rules li { margin-bottom:6px; } ul { padding-left:20px; }
</style></head><body><main>
<h1>PDF/A Converter Bench</h1>
<p class="lead">Run <b>${esc(data.run)}</b>, ${esc((data.finished ?? data.started ?? "").slice(0, 10))}. Every tool converts the same files; veraPDF checks the results; pdf.js and Poppler check what each conversion kept and how the pages look. ${includePrivate ? "Internal report: includes FynePDF's own test files." : ""}</p>
<h2>Files</h2><ul>${setsList}</ul>
<h2>PDF/A-2b, set by set</h2>
<p class="lead"><b>Valid</b>: veraPDF passes the file at the level asked for. <b>False claim</b>: the file says it is PDF/A but isn't (the worst result). <b>Text lost</b>: less than 90% of the text survives, or it comes out garbled. <b>Pages look different</b>: more than 1% of a page changed (first 10 pages, 30 dpi). Size is output ÷ input.</p>
${bySetTable(data, "pdfa-2b")}
<h2>PDF/A, all sets together</h2>
<p class="lead">Every level. The rule set's 1,771 small files weigh heavily here; the table above is the fairer read.</p>
${overallTable(data, ["pdfa-1b", "pdfa-2b", "pdfa-3b"])}
<h2>Grayscale</h2>
<p class="lead"><b>Colour left</b>: files whose result still has colour on a page (more than 0.1% of its pixels). Pages are compared with the input drawn in grey by Poppler, whose grey formula is close to ours (Ghostscript's differs, so its "looks different" count runs higher for that reason alone). The picture copy is how the desktop app's Grayscale works today.</p>
${overallTable(data, ["grayscale"])}
${bySetTable(data, "grayscale")}
${data.features.a ? `<h2>Feature set, file by file</h2><p class="lead">Hover a cell to see the failed rules or the error.</p>${featureTable(data, "a")}` : ""}
${data.features.e ? `<h2>Office and print, file by file</h2>${featureTable(data, "e")}` : ""}
${Object.keys(data.rules).length ? `<h2>Rule set (veraPDF corpus)</h2><p class="lead">Each file breaks, or keeps, one PDF/A rule. A converter should make the broken ones valid and keep the valid ones valid.</p>${rulesTable(data)}` : ""}
${data.families.order ? `<h2>Real world, by the program that made the file (PDF/A-2b)</h2><p class="lead">Valid PDF/A-2b per tool. 1,000 PDFs from Digital Corpora's SafeDocs web crawl, zip 0000.</p>${familiesTable(data)}` : ""}
<h2>Why files fail</h2><p class="lead">The PDF/A-2b rules each tool's output breaks most often.</p>${rulesList(data)}
<h2>Tools and versions</h2><ul>${versions}</ul>
<p class="muted">Conversions ran ${data.workers} at a time in one container (8 CPUs, 8 GB), so times are under load. Memory is the peak resident size of the converter and its child processes.</p>
</main></body></html>`;
}

for (const includePrivate of [false, true]) {
  const data = build(includePrivate);
  const out = path.join("/bench/reports", runName, includePrivate ? "internal" : "public");
  writeJson(path.join(out, "report.json"), data);
  fs.writeFileSync(path.join(out, "report.html"), html(data, includePrivate));
  console.log(`${includePrivate ? "internal" : "public"} report: ${out}/report.html (${judged.filter((j) => includePrivate || j.set !== "d").length} conversions)`);
}
