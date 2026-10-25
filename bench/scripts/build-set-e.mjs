// Set E, office and print: real documents exported by real programs.
//
//   node host/make-chrome-pdfs.mjs      (on the host first: Chrome's prints, in set-e/out)
//   node scripts/build-set-e.mjs        (in the container: LibreOffice, LaTeX, and the index)
//
// - LibreOffice 25.2 (the production backend's version) exports set-e/html/*.html twice: as
//   an ordinary tagged PDF, and as PDF/A-2b (files that are already valid PDF/A).
// - pdfLaTeX builds set-e/latex/paper.tex (Type 1 fonts, links, bookmarks, a photo); latex,
//   dvips with bitmap fonts and ps2pdf build it again (Type 3 fonts, as old arXiv papers).
// - Chrome's prints from the host are copied in.

import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import { family } from "./families.mjs";
import { log, must, run, SETS, writeJson } from "./lib.mjs";

const OUT = path.join(SETS, "e");
const SOURCE = "/bench/set-e";

async function producer(file) {
  const info = (await run("pdfinfo", [file])).stdout;
  const field = (name) => info.match(new RegExp(`^${name}:\\s*(.*)$`, "m"))?.[1]?.trim() ?? "";
  return { producer: field("Producer"), creator: field("Creator") };
}

async function libreOffice(html, target, pdfa) {
  const options = { UseTaggedPDF: { type: "boolean", value: "true" }, ExportBookmarks: { type: "boolean", value: "true" } };
  if (pdfa) options.SelectPdfVersion = { type: "long", value: "2" };
  const work = fs.mkdtempSync(path.join(os.tmpdir(), "lo-e-"));
  // The HTML and its pictures are copied next to each other, as LibreOffice resolves them.
  for (const file of fs.readdirSync(path.dirname(html))) fs.copyFileSync(path.join(path.dirname(html), file), path.join(work, file));
  await must("soffice", [`-env:UserInstallation=file://${work}/profile`, "--headless", "--norestore", "--convert-to", `pdf:writer_web_pdf_Export:${JSON.stringify(options)}`, "--outdir", path.join(work, "out"), path.join(work, path.basename(html))], { timeoutMs: 300_000 });
  fs.copyFileSync(path.join(work, "out", path.basename(html).replace(/\.html$/, ".pdf")), target);
  fs.rmSync(work, { recursive: true, force: true });
}

async function latex(target, { bitmap }) {
  const work = fs.mkdtempSync(path.join(os.tmpdir(), "tex-e-"));
  fs.copyFileSync(path.join(SOURCE, "latex", "paper.tex"), path.join(work, "paper.tex"));
  fs.copyFileSync(path.join(SOURCE, "html", "photo.jpg"), path.join(work, "photo.jpg"));
  const options = { cwd: work, timeoutMs: 300_000 };
  if (bitmap) {
    await must("latex", ["-interaction=nonstopmode", "paper.tex"], options);
    // An empty font map: dvips finds no Type 1 fonts and renders bitmap (PK) fonts at 600 dpi.
    await must("dvips", ["-q", "-D", "600", "-u", "/dev/null", "-o", "paper.ps", "paper.dvi"], options);
    await must("ps2pdf", ["paper.ps", "paper.pdf"], options);
  } else {
    await must("pdflatex", ["-interaction=nonstopmode", "paper.tex"], options);
    await must("pdflatex", ["-interaction=nonstopmode", "paper.tex"], options);
  }
  fs.copyFileSync(path.join(work, "paper.pdf"), target);
  fs.rmSync(work, { recursive: true, force: true });
}

async function main() {
  fs.rmSync(OUT, { recursive: true, force: true });
  fs.mkdirSync(OUT, { recursive: true });
  const files = [];
  const add = async (id, title, features, made) => {
    const file = path.join(OUT, `${id}.pdf`);
    await made(file);
    const info = await producer(file);
    files.push({ id, title, features, levels: ["1b", "2b", "3b"], file: path.basename(file), ...info, family: family(info.producer, info.creator) });
    log("made", id, `${fs.statSync(file).size} bytes`, info.producer);
  };

  for (const name of ["invoice", "report", "languages"]) {
    const html = path.join(SOURCE, "html", `${name}.html`);
    await add(`libreoffice-${name}`, `LibreOffice: ${name}`, ["office", "tagged"], (file) => libreOffice(html, file, false));
    await add(`libreoffice-${name}-pdfa2b`, `LibreOffice PDF/A-2b: ${name}`, ["office", "already-pdfa"], (file) => libreOffice(html, file, true));
    const chrome = path.join(SOURCE, "out", `chrome-${name}.pdf`);
    if (fs.existsSync(chrome)) await add(`chrome-${name}`, `Chrome: ${name}`, ["browser"], async (file) => fs.copyFileSync(chrome, file));
    else log(`no Chrome print of ${name}: run host/make-chrome-pdfs.mjs on the host first`);
  }
  await add("pdflatex-paper", "pdfLaTeX: paper", ["latex", "type1"], (file) => latex(file, { bitmap: false }));
  await add("dvips-bitmap-paper", "latex + dvips (bitmap fonts) + ps2pdf: paper", ["latex", "type3"], (file) => latex(file, { bitmap: true }));

  const chromeVersion = path.join(SOURCE, "out", "chrome-version.txt");
  writeJson(path.join(OUT, "index.json"), {
    set: "e",
    made: new Date().toISOString(),
    chrome: fs.existsSync(chromeVersion) ? fs.readFileSync(chromeVersion, "utf8").trim().split("\n").pop() : null,
    libreoffice: (await run("soffice", ["--version"])).stdout.trim(),
    files,
  });
  log(`set E: ${files.length} files`);
}

await main();
