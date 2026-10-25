// A batch gate's numbers: every tool on the same files, those whose fonts are already embedded
// (by our converter's font check: no "font-not-embedded" or "font-missing" problem), per set
// and level. Valid, false claims, pages that look different, text lost, and what ours lost that
// Ghostscript kept.
//
//   node scripts/gate.mjs [--run baseline] [--ours fynepdf-pdfa] [--against ghostscript-latest]

import fs from "node:fs";
import path from "node:path";

import { readJson, RESULTS } from "./lib.mjs";

const argv = process.argv.slice(2);
const option = (name, fallback) => {
  const at = argv.indexOf(`--${name}`);
  return at >= 0 ? argv[at + 1] : fallback;
};
const run = option("run", "baseline");
const ours = option("ours", "fynepdf-pdfa");
const against = option("against", "ghostscript-latest");
const root = path.join(RESULTS, run);
const meta = readJson(path.join(root, "run.json"));
const tools = meta.tools.filter((tool) => tool !== "picture-copy");

const total = (object) => Object.values(object || {}).reduce((sum, n) => sum + n, 0);
function losses(input, output) {
  if (!input?.ok || !output?.ok) return [];
  const lost = [];
  for (const key of ["bookmarks", "links", "fields", "signatures", "attachments"]) if ((output[key] ?? 0) < (input[key] ?? 0)) lost.push(key);
  if (total(output.annotations) < total(input.annotations)) lost.push("comments");
  if (input.tagged && !output.tagged) lost.push("tags");
  if ((output.pages ?? 0) < (input.pages ?? 0)) lost.push("pages");
  return lost;
}

const record = (tool, task, set, id) => readJson(path.join(root, "records", tool, task, set, `${id}.json`));
const fontsEmbedded = (r) => r?.convert?.ok && !(r.convert.report?.problems ?? []).some((p) => /^font-(not-embedded|missing)/.test(p));

const rows = [];
for (const task of ["pdfa-1b", "pdfa-2b", "pdfa-3b"]) {
  const dir = path.join(root, "records", ours, task);
  if (!fs.existsSync(dir)) continue;
  for (const set of fs.readdirSync(dir).sort()) {
    const ids = fs.readdirSync(path.join(dir, set)).map((f) => f.replace(/\.json$/, ""));
    const population = ids.filter((id) => fontsEmbedded(record(ours, task, set, id)));
    if (!population.length) continue;
    for (const tool of tools) {
      let files = 0, valid = 0, falseClaims = 0, looksDifferent = 0, comparable = 0, textLost = 0, lost = 0, lostVsAgainst = 0;
      const lostKinds = {};
      for (const id of population) {
        const r = record(tool, task, set, id);
        if (!r) continue;
        files++;
        const input = readJson(path.join(root, "inputs", set, `${id}.json`));
        const ok = r.convert?.ok;
        if (ok && r.validation?.compliant) valid++;
        if (ok && r.inspect?.claim && !r.validation?.compliant) falseClaims++;
        if (r.looks?.comparable) {
          comparable++;
          if (r.looks.pagesDifferent > 0 || r.looks.pagesMissing > 0) looksDifferent++;
        }
        if (r.text != null && r.text < 0.9) textLost++;
        const mine = losses(input?.inspect, r.inspect);
        if (mine.length) lost++;
        for (const kind of mine) lostKinds[kind] = (lostKinds[kind] ?? 0) + 1;
        if (tool === ours) {
          const theirs = losses(input?.inspect, record(against, task, set, id)?.inspect);
          if (mine.some((kind) => !theirs.includes(kind))) lostVsAgainst++;
        }
      }
      rows.push({ task, set, tool, population: population.length, files, valid, falseClaims, comparable, looksDifferent, textLost, lost, lostKinds, lostVsAgainst: tool === ours ? lostVsAgainst : null });
    }
  }
}

const pct = (a, b) => (b ? `${((100 * a) / b).toFixed(1)}%` : "-");
console.log(`files whose fonts are embedded (by ${ours}'s check); lost-vs = lost something ${against} kept`);
console.log("task     set tool                files  valid    false  looks-diff  text-lost  lost  lost-vs  lost-kinds");
for (const r of rows)
  console.log(
    `${r.task.padEnd(8)} ${r.set}   ${r.tool.padEnd(19)} ${String(r.files).padStart(5)}  ${pct(r.valid, r.files).padStart(6)}  ${String(r.falseClaims).padStart(5)}  ${`${r.looksDifferent}/${r.comparable}`.padStart(10)}  ${String(r.textLost).padStart(9)}  ${String(r.lost).padStart(4)}  ${String(r.lostVsAgainst ?? "").padStart(7)}  ${JSON.stringify(r.lostKinds)}`,
  );
