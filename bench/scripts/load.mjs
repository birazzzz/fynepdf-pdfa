// What converting costs, per tool and level: seconds and peak memory (median, 95th percentile,
// most) over every file it converted, and the files it took longest on.
//
//   node scripts/load.mjs [--run baseline] [--tools fynepdf-pdfa,ghostscript-latest,ghostscript] [--worst 5]

import fs from "node:fs";
import path from "node:path";

import { readJson, RESULTS } from "./lib.mjs";

const argv = process.argv.slice(2);
const option = (name, fallback) => {
  const at = argv.indexOf(`--${name}`);
  return at >= 0 ? argv[at + 1] : fallback;
};
const runName = option("run", "baseline");
const tools = option("tools", "fynepdf-pdfa,ghostscript-latest,ghostscript").split(",");
const worst = Number(option("worst", "5"));

const quantile = (sorted, q) => (sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(q * sorted.length))] : NaN);
console.log("tool                 level  files  seconds: median   p95    max   total   memory MB: median   p95    max");
for (const tool of tools) {
  const toolDir = path.join(RESULTS, runName, "records", tool);
  for (const task of fs.existsSync(toolDir) ? fs.readdirSync(toolDir).sort() : []) {
    const seconds = [], memory = [], slow = [];
    for (const set of fs.readdirSync(path.join(toolDir, task)))
      for (const name of fs.readdirSync(path.join(toolDir, task, set))) {
        const convert = readJson(path.join(toolDir, task, set, name))?.convert;
        if (!convert || typeof convert.seconds !== "number") continue;
        seconds.push(convert.seconds);
        if (convert.maxRssKB) memory.push(convert.maxRssKB / 1024);
        slow.push([convert.seconds, name.replace(/\.json$/, ""), (convert.maxRssKB ?? 0) / 1024]);
      }
    seconds.sort((a, b) => a - b);
    memory.sort((a, b) => a - b);
    const total = seconds.reduce((sum, s) => sum + s, 0);
    const f = (n, d = 2) => (Number.isFinite(n) ? n.toFixed(d) : "-").padStart(7);
    console.log(
      `${tool.padEnd(20)} ${task.replace("pdfa-", "").padEnd(6)} ${String(seconds.length).padStart(5)}  ${f(quantile(seconds, 0.5))} ${f(quantile(seconds, 0.95))} ${f(seconds.at(-1))} ${f(total, 0)}        ${f(quantile(memory, 0.5), 0)} ${f(quantile(memory, 0.95), 0)} ${f(memory.at(-1), 0)}`,
    );
    for (const [s, id, mb] of slow.sort((a, b) => b[0] - a[0]).slice(0, worst)) console.log(`${" ".repeat(30)}${s.toFixed(2)} s  ${mb.toFixed(0)} MB  ${id}`);
  }
}
