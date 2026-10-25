// Sets B, C and D: files we didn't make, copied into the bench volume with an index each.
//
//   node scripts/import-sets.mjs b c d
//
// B, the rule set: veraPDF's test corpus (CC BY 4.0), the PDF/A-1b, 2b and 3b folders and
//   the Isartor suite. Each file breaks (or keeps) one rule; the level it is for comes from
//   its folder.
// C, real world: the 1,000 PDFs of Digital Corpora's SafeDocs crawl, zip 0000, grouped by the
//   program that made them. Not redistributed: results name each file by its number.
// D, ours: FynePDF's own test files, mounted read-only under /private. Never published.

import fs from "node:fs";
import path from "node:path";

import { family } from "./families.mjs";
import { log, must, pool, run, SETS, slug, workers, writeJson } from "./lib.mjs";

const CORPUS = "/bench/downloads/verapdf-corpus-src/corpus";
const SAFEDOCS_ZIP = "/bench/downloads/safedocs-0000/0000.zip";
const PRIVATE = "/private";

function pdfsUnder(dir) {
  const found = [];
  const walk = (at) => {
    for (const entry of fs.readdirSync(at, { withFileTypes: true })) {
      const full = path.join(at, entry.name);
      if (entry.isDirectory()) walk(full);
      else if (/\.pdf$/i.test(entry.name)) found.push(full);
    }
  };
  if (fs.existsSync(dir)) walk(dir);
  return found.sort();
}

/** The commit a clone is at, read from .git (the container has no git). */
function gitHead(repo) {
  const head = fs.readFileSync(path.join(repo, ".git", "HEAD"), "utf8").trim();
  if (!head.startsWith("ref: ")) return head;
  const ref = head.slice(5);
  const loose = path.join(repo, ".git", ref);
  if (fs.existsSync(loose)) return fs.readFileSync(loose, "utf8").trim();
  const packed = fs.readFileSync(path.join(repo, ".git", "packed-refs"), "utf8");
  return packed.split("\n").find((line) => line.endsWith(` ${ref}`))?.split(" ")[0] ?? "";
}

/** What pdfinfo says about a file: producer, pages, version, tagged, forms, JavaScript. */
async function describe(file) {
  const result = await run("pdfinfo", ["-isodates", file], { timeoutMs: 30_000 });
  const info = {};
  for (const line of result.stdout.split("\n")) {
    const at = line.indexOf(":");
    if (at > 0) info[line.slice(0, at).trim()] = line.slice(at + 1).trim();
  }
  return {
    readable: result.code === 0,
    producer: info.Producer || "",
    creator: info.Creator || "",
    pages: Number(info.Pages) || null,
    version: info["PDF version"] || null,
    encrypted: (info.Encrypted || "no").startsWith("yes"),
    tagged: info.Tagged === "yes",
    form: info.Form || "none",
    javascript: info.JavaScript === "yes",
    error: result.code === 0 ? undefined : result.stderr.trim().split("\n").slice(-1)[0],
  };
}

async function importB() {
  const out = path.join(SETS, "b");
  fs.rmSync(out, { recursive: true, force: true });
  const folders = [
    ["PDF_A-1b", "1b"],
    ["PDF_A-2b", "2b"],
    ["PDF_A-3b", "3b"],
    ["Isartor test files/PDFA-1b", "1b"],
  ];
  const commit = gitHead(CORPUS);
  const files = [];
  for (const [folder, level] of folders) {
    for (const file of pdfsUnder(path.join(CORPUS, folder))) {
      const relative = path.relative(CORPUS, file);
      const name = path.basename(file, ".pdf");
      const id = `${level}-${slug(name)}`;
      const target = path.join(out, level, `${slug(name)}.pdf`);
      fs.mkdirSync(path.dirname(target), { recursive: true });
      fs.copyFileSync(file, target);
      const clause = path.basename(path.dirname(file));
      files.push({ id, file: path.relative(out, target), levels: [level], expect: /-pass-/i.test(name) ? "pass" : /-fail-/i.test(name) ? "fail" : null, clause, source: relative });
    }
  }
  writeJson(path.join(out, "index.json"), { set: "b", source: "https://github.com/veraPDF/veraPDF-corpus", commit, licence: "CC BY 4.0", files });
  log(`set B: ${files.length} files from veraPDF-corpus ${commit.slice(0, 8)}`);
}

async function importC() {
  const out = path.join(SETS, "c");
  fs.rmSync(out, { recursive: true, force: true });
  fs.mkdirSync(out, { recursive: true });
  // Unpacked by Python's zipfile into a folder of its own; the names are the corpus's numbers.
  await must("python3", ["-I", "-c", `import zipfile; zipfile.ZipFile(${JSON.stringify(SAFEDOCS_ZIP)}).extractall(${JSON.stringify(out)})`], { timeoutMs: 900_000 });
  const pdfs = pdfsUnder(out);
  const files = await pool(pdfs, workers * 2, async (file) => {
    const head = Buffer.alloc(1024);
    const fd = fs.openSync(file, "r");
    fs.readSync(fd, head, 0, 1024, 0);
    fs.closeSync(fd);
    const info = await describe(file);
    return {
      id: `c-${path.basename(file, ".pdf")}`,
      file: path.relative(out, file),
      levels: ["2b"],
      bytes: fs.statSync(file).size,
      isPdf: head.includes("%PDF-"),
      family: family(info.producer, info.creator),
      ...info,
    };
  });
  writeJson(path.join(out, "index.json"), { set: "c", source: "https://digitalcorpora.org/cc-main-2021-31-pdf-untruncated/", zip: "zipfiles/0000-0999/0000.zip", publish: "results and file numbers only", files });
  const counts = {};
  for (const entry of files) counts[entry.family] = (counts[entry.family] || 0) + 1;
  log(`set C: ${files.length} files`, JSON.stringify(counts));
}

async function importD() {
  const out = path.join(SETS, "d");
  fs.rmSync(out, { recursive: true, force: true });
  fs.mkdirSync(out, { recursive: true });
  const pdfs = pdfsUnder(PRIVATE);
  const used = new Set();
  const files = await pool(pdfs, workers, async (file) => {
    let name = slug(path.relative(PRIVATE, file).replace(/\.pdf$/i, ""));
    while (used.has(name)) name += "-x";
    used.add(name);
    const target = path.join(out, `${name}.pdf`);
    fs.copyFileSync(file, target);
    const info = await describe(target);
    return { id: `d-${name}`, file: path.basename(target), levels: ["1b", "2b", "3b"], bytes: fs.statSync(target).size, family: family(info.producer, info.creator), ...info };
  });
  writeJson(path.join(out, "index.json"), { set: "d", publish: false, files });
  log(`set D: ${files.length} files`);
}

const wanted = process.argv.slice(2);
for (const [name, fn] of [
  ["b", importB],
  ["c", importC],
  ["d", importD],
]) {
  if (!wanted.length || wanted.includes(name)) await fn();
}
