// The converters the bench compares. Each takes one input and writes one output for a task:
// "pdfa-1b", "pdfa-2b", "pdfa-3b" or "grayscale". Commands are fixed here so every run, and
// anyone re-running the bench, converts the same way.

import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import { run } from "./lib.mjs";

const TIMEOUT = Number(process.env.BENCH_TIMEOUT_MS) || 300_000;
const level = (task) => task.match(/^pdfa-(\d)b$/)?.[1];
// Our command lines are built, not installed: their build time stands for the version.
const built = (binary) =>
  fs.existsSync(binary) ? `${binary.split("/").pop()} ${fs.statSync(binary).mtime.toISOString().slice(0, 16)} build` : "not built";

// Ghostscript exactly as FynePDF's servers ran it before fynepdf-pdfa: an sRGB output intent
// declared through a PostScript prefix (as Ghostscript's PDFA_def.ps does), every colour
// converted to RGB, policy 1 (drop what PDF/A forbids rather than abort).
function ghostscriptPrefix(latest) {
  const file = path.join(os.tmpdir(), latest ? "bench-pdfa-def-latest.ps" : "bench-pdfa-def.ps");
  if (fs.existsSync(file)) return file;
  let icc;
  if (latest) {
    // A source build keeps its colour profiles inside the program.
    icc = "%rom%iccprofiles/default_rgb.icc";
  } else {
    const root = "/usr/share/ghostscript";
    const version = fs.readdirSync(root).toSorted().toReversed().find((v) => fs.existsSync(path.join(root, v, "iccprofiles", "default_rgb.icc")));
    icc = path.join(root, version, "iccprofiles", "default_rgb.icc");
  }
  const literal = icc.replaceAll(/[\\()]/g, (c) => `\\${c}`);
  const prefix = [
    "%!",
    `/ICCProfile (${literal}) def`,
    "[/_objdef {icc_PDFA} /type /stream /OBJ pdfmark",
    "[{icc_PDFA} << /N 3 >> /PUT pdfmark",
    "[{icc_PDFA} ICCProfile (r) file /PUT pdfmark",
    "[/_objdef {OutputIntent_PDFA} /type /dict /OBJ pdfmark",
    "[{OutputIntent_PDFA} <<",
    "  /Type /OutputIntent",
    "  /S /GTS_PDFA1",
    "  /DestOutputProfile {icc_PDFA}",
    "  /OutputConditionIdentifier (sRGB IEC61966-2.1)",
    "  /Info (sRGB IEC61966-2.1)",
    "  /RegistryName (http://www.color.org)",
    ">> /PUT pdfmark",
    "[{Catalog} << /OutputIntents [ {OutputIntent_PDFA} ] >> /PUT pdfmark",
    "",
  ].join("\n");
  fs.writeFileSync(`${file}.tmp${process.pid}`, prefix);
  fs.renameSync(`${file}.tmp${process.pid}`, file);
  return file;
}

const GREY_FLAGS = ["-sDEVICE=pdfwrite", "-dProcessColorModel=/DeviceGray", "-dColorConversionStrategy=/Gray", "-dNOPAUSE", "-dBATCH"];
const LATEST_GS = "/opt/gs-10.08.0/bin";

function ghostscript(gs, latest) {
  return {
    tasks: ["pdfa-1b", "pdfa-2b", "pdfa-3b", "grayscale"],
    version: async () => `Ghostscript ${(await run(gs, ["--version"])).stdout.trim()}`,
    convert: (input, output, task) =>
      task === "grayscale"
        ? run(gs, [...GREY_FLAGS, `-sOutputFile=${output}`, input], { measure: true, timeoutMs: TIMEOUT })
        : run(
            gs,
            [`-dPDFA=${level(task)}`, "-dBATCH", "-dNOPAUSE", "-sColorConversionStrategy=RGB", "-sDEVICE=pdfwrite", "-dPDFACompatibilityPolicy=1", `-sOutputFile=${output}`, ghostscriptPrefix(latest), input],
            { measure: true, timeoutMs: TIMEOUT },
          ),
  };
}

export const TOOLS = {
  ghostscript: { label: "Ghostscript 10.00.0 (Debian bookworm's package, the command FynePDF's servers used)", ...ghostscript("gs", false) },
  "ghostscript-latest": { label: "Ghostscript 10.08.0 (latest release, same command)", ...ghostscript(`${LATEST_GS}/gs`, true) },

  // OCRmyPDF's PDF/A output with OCR switched off (--tesseract-timeout 0, as its documentation
  // suggests for converting without OCR), one process, defaults otherwise. It runs on the latest
  // Ghostscript: it refuses 10.0.0 to 10.02.0 for files that have text.
  ocrmypdf: {
    label: "OCRmyPDF (no OCR, on Ghostscript 10.08.0)",
    tasks: ["pdfa-1b", "pdfa-2b", "pdfa-3b"],
    version: async () => `OCRmyPDF ${(await run("ocrmypdf", ["--version"])).stdout.trim()}`,
    convert: (input, output, task) =>
      run("ocrmypdf", ["--skip-text", "--tesseract-timeout", "0", "--jobs", "1", "--output-type", `pdfa-${level(task)}`, input, output], {
        measure: true,
        timeoutMs: TIMEOUT,
        env: { PATH: `${LATEST_GS}:${process.env.PATH}` },
      }),
  },

  // LibreOffice opening the PDF in Draw and exporting it as PDF/A, as Gotenberg does.
  libreoffice: {
    label: "LibreOffice (Draw import, PDF/A export)",
    tasks: ["pdfa-1b", "pdfa-2b", "pdfa-3b"],
    version: async () => (await run("soffice", ["--version"])).stdout.trim(),
    convert: async (input, output, task, slot) => {
      const profile = path.join(os.tmpdir(), `bench-lo-profile-${slot}`);
      const outdir = fs.mkdtempSync(path.join(os.tmpdir(), "bench-lo-out-"));
      // LibreOffice names the output after the input; a link gives it a plain name.
      const link = path.join(outdir, "in.pdf");
      fs.symlinkSync(input, link);
      const options = JSON.stringify({ SelectPdfVersion: { type: "long", value: level(task) } });
      const result = await run(
        "soffice",
        [`-env:UserInstallation=file://${profile}`, "--headless", "--norestore", "--infilter=draw_pdf_import", "--convert-to", `pdf:draw_pdf_Export:${options}`, "--outdir", path.join(outdir, "out"), link],
        { measure: true, timeoutMs: TIMEOUT },
      );
      const made = path.join(outdir, "out", "in.pdf");
      if (fs.existsSync(made)) fs.copyFileSync(made, output);
      else if (result.code === 0) result.code = 1;
      fs.rmSync(outdir, { recursive: true, force: true });
      return result;
    },
  },

  // Ours: this repository's command line (../scripts/build-linux.sh), mounted at
  // /opt/fynepdf-pdfa by bench.sh. It always writes a file: labelled PDF/A when it is one,
  // without the label (and with the reasons on stdout) when it isn't.
  "fynepdf-pdfa": {
    label: "fynepdf-pdfa (ours)",
    tasks: ["pdfa-1b", "pdfa-2b", "pdfa-3b"],
    version: async () => built("/opt/fynepdf-pdfa/fynepdf-pdfa"),
    convert: (input, output, task) =>
      run("/opt/fynepdf-pdfa/fynepdf-pdfa", [input, output, "--part", level(task), "--fonts", "/opt/fynepdf-pdfa-fonts"], {
        measure: true,
        timeoutMs: TIMEOUT,
      }),
  },

  // Ours for Grayscale: the sister project's command line, mounted at /opt/fynepdf-grayscale by
  // bench.sh when ../../fynepdf-grayscale is checked out next to this repository and built.
  "fynepdf-grayscale": {
    label: "fynepdf-grayscale (ours)",
    tasks: ["grayscale"],
    version: async () => built("/opt/fynepdf-grayscale/fynepdf-grayscale"),
    convert: (input, output) =>
      run("/opt/fynepdf-grayscale/fynepdf-grayscale", [input, output], { measure: true, timeoutMs: TIMEOUT }),
  },

  // Grayscale by pictures, as many apps do it (FynePDF's desktop app did before fynepdf-pdfa):
  // every page drawn at 180 dpi and saved as a grey JPEG at quality 84, one picture per page.
  // Text stops being text.
  "picture-copy": {
    label: "Picture copy (pages drawn as grey JPEGs)",
    tasks: ["grayscale"],
    version: async () => `pdftoppm ${(await run("pdftoppm", ["-v"])).stderr.split("\n")[0].replace(/^pdftoppm version /, "")} + img2pdf ${(await run("img2pdf", ["--version"])).stdout.trim().replace(/^img2pdf /, "")}`,
    convert: async (input, output) => {
      const work = fs.mkdtempSync(path.join(os.tmpdir(), "bench-raster-"));
      const started = performance.now();
      const render = await run("pdftoppm", ["-r", "180", "-gray", "-jpeg", "-jpegopt", "quality=84", input, path.join(work, "p")], { measure: true, timeoutMs: TIMEOUT });
      let result = render;
      const pages = fs.readdirSync(work).filter((f) => f.endsWith(".jpg")).sort().map((f) => path.join(work, f));
      if (render.code === 0 && pages.length) {
        const wrap = await run("img2pdf", [...pages, "-o", output], { measure: true, timeoutMs: TIMEOUT });
        result = { ...wrap, seconds: (performance.now() - started) / 1000, maxRssKB: Math.max(render.maxRssKB ?? 0, wrap.maxRssKB ?? 0) };
      } else if (render.code === 0) result = { ...render, code: 1, stderr: "no pages rendered" };
      fs.rmSync(work, { recursive: true, force: true });
      return result;
    },
  },
};
