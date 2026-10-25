// The tables in docs/benchmarks.md (of this project and of fynepdf-grayscale), from a run's public
// sets only (A, B, C, E; never the private set D), as Markdown:
//
//   node scripts/report.mjs --run baseline        (first: the quality numbers come from it)
//   node scripts/docs-tables.mjs --run baseline
//
// Valid counts follow scripts/levels.mjs: a valid output standing in for a file the tool couldn't
// read (the input couldn't be drawn and every page of the output is blank) counts as a failure.
// Times are each conversion's wall time (GNU time) while 4 ran at once on 8 CPUs.

import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import { readJson, RESULTS, run } from "./lib.mjs";

const argv = process.argv.slice(2);
const runName = argv[argv.indexOf("--run") + 1] && argv.includes("--run") ? argv[argv.indexOf("--run") + 1] : "baseline";
const root = path.join(RESULTS, runName);
const report = readJson(path.join("/bench/reports", runName, "public", "report.json"));
if (!report) throw new Error("run scripts/report.mjs first");
const PUBLIC = ["a", "b", "c", "e"];
const PDFA_TOOLS = ["fynepdf-pdfa", "ghostscript-latest", "ghostscript", "ocrmypdf", "libreoffice"];
const GRAY_TOOLS = ["fynepdf-grayscale", "ghostscript-latest", "ghostscript", "picture-copy"];
const NAMES = {
  "fynepdf-pdfa": "**fynepdf-pdfa**",
  "fynepdf-grayscale": "**fynepdf-grayscale**",
  "ghostscript-latest": "Ghostscript 10.08",
  ghostscript: "Ghostscript 10.00",
  ocrmypdf: "OCRmyPDF 17.13",
  libreoffice: "LibreOffice 25.2",
  "picture-copy": "Pages as pictures",
};

async function blank(file) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "blank-"));
  try {
    const result = await run("pdftoppm", ["-r", "10", "-gray", "-l", "20", file, path.join(dir, "p")], { timeoutMs: 120_000 });
    const pages = fs.readdirSync(dir).filter((name) => name.endsWith(".pgm"));
    if (result.code !== 0 || !pages.length) return false;
    for (const name of pages) {
      const data = fs.readFileSync(path.join(dir, name));
      let at = 0;
      for (let fields = 0; fields < 4; fields++) {
        while (/\s/.test(String.fromCharCode(data[at]))) at++;
        while (!/\s/.test(String.fromCharCode(data[at]))) at++;
      }
      at++;
      for (; at < data.length; at++) if (data[at] < 250) return false;
    }
    return true;
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

/** Per tool and task (optionally one set): valid, false claims, stand-ins, time and memory. */
async function counts(tool, task, sets = PUBLIC) {
  const c = { files: 0, valid: 0, falseClaims: 0, standins: 0, seconds: 0, worst: 0, memory: 0 };
  for (const set of sets) {
    const dir = path.join(root, "records", tool, task, set);
    for (const name of fs.existsSync(dir) ? fs.readdirSync(dir) : []) {
      const record = readJson(path.join(dir, name));
      c.files++;
      const convert = record?.convert;
      if (convert && typeof convert.seconds === "number") {
        c.seconds += convert.seconds;
        c.worst = Math.max(c.worst, convert.seconds);
        c.memory = Math.max(c.memory, (convert.maxRssKB ?? 0) / 1024);
      }
      if (!task.startsWith("pdfa") || !convert?.ok) continue;
      const level = task.slice(5);
      const claim = record.inspect ? record.inspect.claim ?? null : convert.report?.conforms ? level : null;
      if (record.validation?.compliant) {
        const output = path.join(root, "out", tool, task, set, name.replace(/\.json$/, ".pdf"));
        if (record.looks?.reason === "input could not be drawn" && fs.existsSync(output) && (await blank(output))) c.standins++;
        else c.valid++;
      } else if (claim) {
        c.falseClaims++;
      }
    }
  }
  return c;
}

const n = (x) => x.toLocaleString("en-US");
const pct = (part, whole) => (whole ? `${((100 * part) / whole).toFixed(1)}%` : "-");
const s = (x) => (x >= 100 ? n(Math.round(x)) : x.toFixed(x >= 10 ? 0 : 1));
const KINDS = ["bookmarks", "links", "fields", "tags", "comments", "attachments", "signatures", "pages"];
const lostTable = (title, task, tools) => {
  console.log(`\n### ${title}: files that lost each kind of thing\n`);
  console.log(`| Tool | ${KINDS.join(" | ")} |`);
  console.log(`| --- |${KINDS.map(() => " --- |").join("")}`);
  for (const tool of tools) {
    const o = report.overall[tool]?.[task];
    if (o) console.log(`| ${NAMES[tool]} | ${KINDS.map((k) => n(o.lostByKind?.[k] ?? 0)).join(" | ")} |`);
  }
};

for (const task of ["pdfa-2b", "pdfa-1b", "pdfa-3b"]) {
  // The corpus's 1b and 3b files go to their own levels; the real-world set only to 2b.
  console.log(`\n### PDF/A-${task.slice(5)}, sets ${task === "pdfa-2b" ? "A, B, C and E" : "A, B and E"}\n`);
  console.log("| Tool | Valid PDF/A | False claims | Text lost | Pages look different | Files that lost something | Median size | Time, summed | Slowest file | Most memory |");
  console.log("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |");
  for (const tool of PDFA_TOOLS) {
    const o = report.overall[tool]?.[task];
    if (!o) continue;
    const c = await counts(tool, task);
    console.log(
      `| ${NAMES[tool]} | ${n(c.valid)} of ${n(c.files)} (${pct(c.valid, c.files)})${c.standins ? `, +${c.standins} blank stand-ins` : ""} | ${n(c.falseClaims)} | ${n(o.textLost)} of ${n(o.withText)} | ${n(o.looksDifferent)} | ${n(o.lostSomething)} | ${o.sizeRatioMedian.toFixed(2)}× | ${s(c.seconds)} s | ${s(c.worst)} s | ${Math.round(c.memory)} MB |`,
    );
  }
}

lostTable("PDF/A-2b", "pdfa-2b", PDFA_TOOLS);

console.log("\n### PDF/A-2b by set\n");
console.log(`| Set | ${PDFA_TOOLS.map((t) => NAMES[t]).join(" | ")} |`);
console.log(`| --- |${PDFA_TOOLS.map(() => " --- |").join("")}`);
for (const set of PUBLIC) {
  const cells = [];
  for (const tool of PDFA_TOOLS) {
    const c = await counts(tool, "pdfa-2b", [set]);
    cells.push(c.files ? `${n(c.valid)} / ${n(c.files)}, ${c.falseClaims} false` : "-");
  }
  console.log(`| ${report.sets[set].name} | ${cells.join(" | ")} |`);
}

// Grayscale doesn't run on the veraPDF corpus (a PDF/A test).
console.log("\n### Grayscale, sets A, C and E\n");
console.log("| Tool | Colour left | Text lost | Pages look different | Files that lost something | Median size | Time, summed | Slowest file | Most memory |");
console.log("| --- | --- | --- | --- | --- | --- | --- | --- | --- |");
for (const tool of GRAY_TOOLS) {
  const o = report.overall[tool]?.grayscale;
  if (!o) continue;
  const c = await counts(tool, "grayscale");
  console.log(
    `| ${NAMES[tool]} | ${n(o.colourLeft)} of ${n(o.colourChecked)} | ${n(o.textLost)} of ${n(o.withText)} | ${n(o.looksDifferent)} | ${n(o.lostSomething)} | ${o.sizeRatioMedian.toFixed(2)}× | ${s(c.seconds)} s | ${s(c.worst)} s | ${Math.round(c.memory)} MB |`,
  );
}

lostTable("Grayscale", "grayscale", GRAY_TOOLS);
