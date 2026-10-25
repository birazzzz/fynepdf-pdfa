// Where a run got slower than another: per file, our converter's seconds in both, the files
// that gained most first, and the totals.
//
//   node scripts/slower.mjs --before /data/results/b3-final-ours/records [--run baseline] [--task pdfa-2b] [--top 15]

import fs from "node:fs";
import path from "node:path";

import { readJson, RESULTS } from "./lib.mjs";

const argv = process.argv.slice(2);
const option = (name, fallback) => {
  const at = argv.indexOf(`--${name}`);
  return at >= 0 ? argv[at + 1] : fallback;
};
const before = option("before", "");
const task = option("task", "pdfa-2b");
const top = Number(option("top", "15"));
const now = path.join(RESULTS, option("run", "baseline"), "records", task === "grayscale" ? "fynepdf-grayscale" : "fynepdf-pdfa", task);
const rows = [];
let totalBefore = 0, totalNow = 0;
for (const set of fs.readdirSync(now)) {
  for (const name of fs.readdirSync(path.join(now, set))) {
    const a = readJson(path.join(before, task, set, name))?.convert?.seconds;
    const b = readJson(path.join(now, set, name))?.convert?.seconds;
    if (typeof a !== "number" || typeof b !== "number") continue;
    totalBefore += a;
    totalNow += b;
    rows.push([b - a, a, b, name.replace(/\.json$/, "")]);
  }
}
console.log(`${task}: ${rows.length} files, before ${totalBefore.toFixed(1)} s, now ${totalNow.toFixed(1)} s`);
for (const [gain, a, b, id] of rows.sort((x, y) => y[0] - x[0]).slice(0, top)) console.log(`  +${gain.toFixed(2)} s  ${a.toFixed(2)} -> ${b.toFixed(2)}  ${id}`);
