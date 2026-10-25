// Files another tool makes valid PDF/A and ours doesn't, grouped by the problems ours reports,
// for closing the gap batch by batch.
//
//   node scripts/versus.mjs [--level 2b] [--against ghostscript-latest] [--run baseline] [--examples 4]

import fs from "node:fs";
import path from "node:path";

import { readJson, RESULTS } from "./lib.mjs";

const argv = process.argv.slice(2);
const option = (name, fallback) => {
  const at = argv.indexOf(`--${name}`);
  return at >= 0 ? argv[at + 1] : fallback;
};
const level = option("level", "2b");
const against = option("against", "ghostscript-latest");
const run = option("run", "baseline");
const examples = Number(option("examples", "4"));
const root = path.join(RESULTS, run, "records");

const groups = new Map();
const totals = { theirs: 0, ours: 0, both: 0 };
for (const set of fs.readdirSync(path.join(root, "fynepdf-pdfa", `pdfa-${level}`))) {
  const dir = path.join(root, "fynepdf-pdfa", `pdfa-${level}`, set);
  for (const name of fs.readdirSync(dir)) {
    const ours = readJson(path.join(dir, name));
    const theirs = readJson(path.join(root, against, `pdfa-${level}`, set, name));
    const oursValid = Boolean(ours?.validation?.compliant), theirsValid = Boolean(theirs?.validation?.compliant);
    if (oursValid) totals.ours++;
    if (theirsValid) totals.theirs++;
    if (oursValid && theirsValid) totals.both++;
    if (!theirsValid || oursValid) continue;
    const problems = ours?.convert?.ok ? ours.convert.report?.problems ?? [] : [`(failed) ${ours?.convert?.report?.error ?? "?"}`];
    const key = [...new Set(problems.map((p) => p.replace(/: .*/, "")))].sort().join(" + ") || "(no problem reported)";
    const entry = groups.get(key) ?? { count: 0, sets: {}, examples: [] };
    entry.count++;
    entry.sets[set] = (entry.sets[set] ?? 0) + 1;
    if (entry.examples.length < examples) entry.examples.push(`${ours?.id ?? name} ${problems.join("; ").slice(0, 140)}`);
    groups.set(key, entry);
  }
}
console.log(`${level}: ${against} valid ${totals.theirs}, ours ${totals.ours}, both ${totals.both}`);
console.log(`valid with ${against}, not with ours, by our problems:`);
for (const [key, entry] of [...groups].sort((a, b) => b[1].count - a[1].count)) {
  console.log(`${String(entry.count).padStart(4)}  ${key}  ${JSON.stringify(entry.sets)}`);
  for (const example of entry.examples) console.log(`        ${example}`);
}
