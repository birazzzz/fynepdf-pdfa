// A quick look at one tool's PDF/A results in a run, for working on the converter (the report is
// for reading): per set and level, how many are valid, falsely claimed, or honestly unlabelled;
// the veraPDF rules behind false claims and behind unlabelled files that could have passed; the
// problems the tool itself reported.
//
//   node scripts/summary.mjs [--run baseline] [--tool fynepdf-pdfa] [--level 2b] [--examples 3]

import fs from "node:fs";
import path from "node:path";

import { readJson, RESULTS } from "./lib.mjs";

const argv = process.argv.slice(2);
const option = (name, fallback) => {
  const at = argv.indexOf(`--${name}`);
  return at >= 0 ? argv[at + 1] : fallback;
};
const run = option("run", "baseline");
const tool = option("tool", "fynepdf-pdfa");
const onlyLevel = option("level", "");
const examples = Number(option("examples", "3"));

const root = path.join(RESULTS, run, "records", tool);
const rows = new Map();
const rules = new Map();
const problems = new Map();
const bump = (map, key, example) => {
  const entry = map.get(key) ?? { count: 0, examples: [] };
  entry.count++;
  if (entry.examples.length < examples) entry.examples.push(example);
  map.set(key, entry);
};

for (const task of fs.existsSync(root) ? fs.readdirSync(root) : []) {
  const level = task.match(/^pdfa-(\d\w)$/)?.[1];
  if (!level || (onlyLevel && level !== onlyLevel)) continue;
  for (const set of fs.readdirSync(path.join(root, task))) {
    for (const name of fs.readdirSync(path.join(root, task, set))) {
      const record = readJson(path.join(root, task, set, name));
      const input = readJson(path.join(RESULTS, run, "inputs", set, name));
      const key = `${set} ${level}`;
      const row = rows.get(key) ?? { files: 0, converted: 0, valid: 0, falseClaims: 0, unlabelled: 0, unlabelledValidish: 0, inputValid: 0 };
      rows.set(key, row);
      row.files++;
      if (input?.validation?.[level]?.compliant) row.inputValid++;
      if (!record?.convert?.ok) {
        bump(problems, `(failed) ${record?.convert?.report?.error ?? record?.convert?.error?.slice(0, 60) ?? "?"}`, record?.id);
        continue;
      }
      row.converted++;
      const valid = record.validation?.compliant === true;
      // The claim as pdf.js reads it from the output; before the inspect step, as the tool said.
      const claim = record.inspect ? record.inspect.claim ?? null : record.convert.report?.conforms ? level : null;
      const failed = (record.validation?.failedRules ?? []).map((r) => `${r.clause}-${r.test}`);
      if (valid) row.valid++;
      else if (claim) {
        row.falseClaims++;
        for (const rule of failed) bump(rules, `false claim  ${level} ${rule}`, record.id);
      } else {
        row.unlabelled++;
        // Unlabelled files whose only failure is the missing label: the problems we reported
        // were too cautious (or veraPDF didn't see them).
        const others = failed.filter((rule) => !rule.startsWith("6.6.4-") && !rule.startsWith("6.7.11-"));
        if (!others.length) row.unlabelledValidish++;
        for (const rule of others) bump(rules, `unlabelled   ${level} ${rule}`, record.id);
      }
      for (const problem of record.convert.report?.problems ?? []) bump(problems, `${level} ${problem.replace(/: .*/, "")}`, record.id);
    }
  }
}

console.log("set level  files  converted  valid  false-claims  unlabelled (only-label-missing)  inputs-valid");
for (const [key, r] of [...rows].sort()) {
  console.log(`${key.padEnd(10)} ${String(r.files).padStart(5)} ${String(r.converted).padStart(10)} ${String(r.valid).padStart(6)} ${String(r.falseClaims).padStart(13)} ${String(r.unlabelled).padStart(11)} (${r.unlabelledValidish}) ${String(r.inputValid).padStart(13)}`);
}
console.log("\nveraPDF rules failed (count, examples):");
for (const [key, entry] of [...rules].sort((a, b) => b[1].count - a[1].count).slice(0, 60)) console.log(`${String(entry.count).padStart(5)}  ${key}  ${entry.examples.join(" ")}`);
console.log("\nproblems we reported (count, examples):");
for (const [key, entry] of [...problems].sort((a, b) => b[1].count - a[1].count).slice(0, 40)) console.log(`${String(entry.count).padStart(5)}  ${key}  ${entry.examples.join(" ")}`);
