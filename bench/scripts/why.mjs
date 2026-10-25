// Why one of a run's outputs fails veraPDF: each failed rule with the objects it failed on.
//
//   node scripts/why.mjs <set>-<id> [--level 2b] [--tool fynepdf-pdfa] [--run baseline] [--input]
//
// --input checks the input file instead of the tool's output.

import path from "node:path";

import { RESULTS, run, SETS, readJson } from "./lib.mjs";

const argv = process.argv.slice(2);
const option = (name, fallback) => {
  const at = argv.indexOf(`--${name}`);
  return at >= 0 ? argv[at + 1] : fallback;
};
const id = argv[0];
const level = option("level", "2b");
const tool = option("tool", "fynepdf-pdfa");
const runName = option("run", "baseline");
const set = id.split("-")[0];
let file = path.join(RESULTS, runName, "out", tool, `pdfa-${level}`, set, `${id}.pdf`);
if (argv.includes("--input")) {
  const index = readJson(path.join(SETS, set, "index.json"));
  const entry = index.files.find((f) => (f.id.startsWith(`${set}-`) ? f.id : `${set}-${f.id}`) === id);
  file = path.join(SETS, set, entry.file);
}
const record = readJson(path.join(RESULTS, runName, "records", tool, `pdfa-${level}`, set, `${id}.json`));
if (record?.convert?.report) console.log("ours:", JSON.stringify(record.convert.report));
const result = await run("verapdf", ["--flavour", level, "--format", "json", "--maxfailuresdisplayed", "4", file], { timeoutMs: 600_000, env: { JAVA_OPTS: "-Xmx2g" } });
let report;
try {
  report = JSON.parse(result.stdout);
} catch {
  console.log(result.stdout.slice(0, 2000), result.stderr.slice(0, 2000));
  process.exit(1);
}
const job = (report.report ?? report.reports?.[0]?.report).jobs[0];
const verdict = job.validationResult[0];
console.log(file, "compliant:", verdict.compliant);
for (const rule of verdict.details.ruleSummaries.filter((r) => r.ruleStatus === "FAILED" || r.status === "failed")) {
  console.log(`\n${rule.clause}-${rule.testNumber} (${rule.failedChecks}): ${rule.description}`);
  for (const check of rule.checks ?? []) console.log("   ", check.context, check.errorMessage ? `-- ${check.errorMessage}` : "");
}
