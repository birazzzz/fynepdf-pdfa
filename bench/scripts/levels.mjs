// The PDF/A gate's numbers: per set and level, each tool's valid outputs and false claims. A
// valid output that stands in for a file the tool couldn't read (its input couldn't be drawn,
// and every page of the output is blank) counts as a failure, shown apart.
//
//   node scripts/levels.mjs [--run baseline] [--tools fynepdf-pdfa,ghostscript-latest,ghostscript]

import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import { readJson, RESULTS, run } from "./lib.mjs";

const argv = process.argv.slice(2);
const option = (name, fallback) => {
  const at = argv.indexOf(`--${name}`);
  return at >= 0 ? argv[at + 1] : fallback;
};
const runName = option("run", "baseline");
const tools = option("tools", "fynepdf-pdfa,ghostscript-latest,ghostscript").split(",");
const root = path.join(RESULTS, runName);

/** Whether every page of a PDF (the first 20) is white, drawn in grey at 10 dpi. */
async function blank(file) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "blank-"));
  try {
    const result = await run("pdftoppm", ["-r", "10", "-gray", "-l", "20", file, path.join(dir, "p")], { timeoutMs: 120_000 });
    const pages = fs.readdirSync(dir).filter((name) => name.endsWith(".pgm"));
    if (result.code !== 0 || !pages.length) return false;
    for (const name of pages) {
      const data = fs.readFileSync(path.join(dir, name));
      // P5 header: magic, width, height, maximum, each followed by white space.
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

const rows = new Map();  // "set level" -> tool -> counts
const standins = [];
for (const tool of tools) {
  const toolDir = path.join(root, "records", tool);
  for (const task of fs.existsSync(toolDir) ? fs.readdirSync(toolDir).sort() : []) {
    const level = task.match(/^pdfa-(\d\w)$/)?.[1];
    if (!level) continue;
    for (const set of fs.readdirSync(path.join(toolDir, task)).sort()) {
      for (const name of fs.readdirSync(path.join(toolDir, task, set))) {
        const record = readJson(path.join(toolDir, task, set, name));
        const key = `${set} ${level}`;
        if (!rows.has(key)) rows.set(key, {});
        const counts = (rows.get(key)[tool] ??= { files: 0, valid: 0, falseClaims: 0, standins: 0 });
        counts.files++;
        if (!record?.convert?.ok) continue;
        const claim = record.inspect ? record.inspect.claim ?? null : record.convert.report?.conforms ? level : null;
        if (record.validation?.compliant) {
          const output = path.join(root, "out", tool, task, set, name.replace(/\.json$/, ".pdf"));
          if (record.looks?.reason === "input could not be drawn" && fs.existsSync(output) && (await blank(output))) {
            counts.standins++;
            standins.push(`${tool} ${task} ${record.id}`);
          } else {
            counts.valid++;
          }
        } else if (claim) {
          counts.falseClaims++;
        }
      }
    }
  }
}

const cell = (c) => (c ? `${c.valid}/${c.files} fc ${c.falseClaims}${c.standins ? ` (+${c.standins} blank)` : ""}` : "-");
console.log(["set level".padEnd(10), ...tools.map((t) => t.padEnd(28))].join(" "));
for (const [key, byTool] of [...rows].sort()) console.log([key.padEnd(10), ...tools.map((t) => cell(byTool[t]).padEnd(28))].join(" "));
if (standins.length) {
  console.log("\nvalid but blank, for a file that couldn't be read:");
  for (const line of standins) console.log(`  ${line}`);
}
