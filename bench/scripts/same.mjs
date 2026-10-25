// Whether a new build changes any output: converts a run's inputs again with another build of
// ours and compares each output byte for byte with the run's, listing the ones that differ.
//
//   node scripts/same.mjs --binary /opt/check/fynepdf-pdfa [--run baseline] [--task pdfa-2b] [--sets a,b,c,d,e] [--keep <dir>]
//
// --keep: the outputs that differ are kept there, as <id>.pdf, to look at.

import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import { pool, readJson, RESULTS, run, workers } from "./lib.mjs";

const argv = process.argv.slice(2);
const option = (name, fallback) => {
  const at = argv.indexOf(`--${name}`);
  return at >= 0 ? argv[at + 1] : fallback;
};
const binary = option("binary", "/opt/check/fynepdf-pdfa");
const runName = option("run", "baseline");
const task = option("task", "pdfa-2b");
const sets = option("sets", "a,b,c,d,e").split(",");
const keep = option("keep", null);
if (keep) fs.mkdirSync(keep, { recursive: true });
const root = path.join(RESULTS, runName);
const tool = task === "grayscale" ? "fynepdf-grayscale" : "fynepdf-pdfa";
const part = task.match(/^pdfa-(\d)/)?.[1];

const jobs = [];
for (const set of sets) {
  const dir = path.join(root, "records", tool, task, set);
  for (const name of fs.existsSync(dir) ? fs.readdirSync(dir) : []) {
    const id = name.replace(/\.json$/, "");
    const input = readJson(path.join(root, "inputs", set, name))?.path;
    if (input) jobs.push({ set, id, input, before: path.join(root, "out", tool, task, set, `${id}.pdf`) });
  }
}
const scratch = fs.mkdtempSync(path.join(os.tmpdir(), "same-"));
let same = 0, gone = 0, peak = [0, ""];
const differ = [];
await pool(jobs, workers, async (job) => {
  const after = path.join(scratch, `${job.id}.pdf`);
  const extra = part ? ["--part", part, "--fonts", "/opt/fynepdf-pdfa-fonts"] : [];
  const result = await run(binary, [job.input, after, ...extra], { measure: true, timeoutMs: 300_000 });
  if ((result.maxRssKB ?? 0) > peak[0]) peak = [result.maxRssKB, job.id];
  const had = fs.existsSync(job.before), has = fs.existsSync(after);
  if (!had && !has) gone++;
  else if (had && has && Buffer.compare(fs.readFileSync(job.before), fs.readFileSync(after)) === 0) same++;
  else {
    differ.push(job.id);
    if (keep && has) fs.copyFileSync(after, path.join(keep, `${job.id}.pdf`));
  }
  fs.rmSync(after, { force: true });
});
fs.rmSync(scratch, { recursive: true, force: true });
console.log(`${task}: ${jobs.length} files, identical ${same}, no output either time ${gone}, different ${differ.length}; peak ${(peak[0] / 1024).toFixed(0)} MB (${peak[1]})`);
for (const id of differ.sort()) console.log(`  ${id}`);
