// Reads one PDF with pdf.js, a reader that is none of the converters, and prints what a
// conversion must keep, as JSON:
//
//   node scripts/inspect-file.mjs <file.pdf> <text-out.txt>
//
// pages, bookmarks, links, form fields, signature fields, comments by kind, attachments,
// tagged or not, document JavaScript, the PDF/A claim in its XMP, and the text of the first
// 50 pages (written to <text-out.txt>, for the text comparison).

import fs from "node:fs";

import { pdfjsPath } from "./lib.mjs";

const [file, textOut] = process.argv.slice(2);
const MAX_TEXT_PAGES = 50;
const MAX_ANNOT_PAGES = 1000;

// pdf.js writes its warnings to standard output, where this script's JSON goes.
console.log = console.info = console.warn = () => {};
const pdfjs = await import(pdfjsPath);
const deps = "/opt/deps/node_modules/pdfjs-dist";

function countOutline(items) {
  return (items || []).reduce((sum, item) => sum + 1 + countOutline(item.items), 0);
}

const result = { ok: false };
try {
  const task = pdfjs.getDocument({
    data: new Uint8Array(fs.readFileSync(file)),
    cMapUrl: `${deps}/cmaps/`,
    cMapPacked: true,
    standardFontDataUrl: `${deps}/standard_fonts/`,
    isEvalSupported: false,
    disableFontFace: true,
    useSystemFonts: false,
    verbosity: 0,
  });
  const doc = await task.promise;
  result.pages = doc.numPages;

  const meta = await doc.getMetadata().catch(() => null);
  const xmp = meta?.metadata?.getRaw?.() ?? "";
  const part = xmp.match(/pdfaid:part(?:>|\s*=\s*["'])\s*(\d)/)?.[1];
  const conformance = xmp.match(/pdfaid:conformance(?:>|\s*=\s*["'])\s*([abuABU])/)?.[1];
  result.claim = part ? `${part}${(conformance || "").toLowerCase()}` : null;
  result.producer = meta?.info?.Producer ?? null;

  result.bookmarks = countOutline(await doc.getOutline().catch(() => null));
  result.attachments = Object.keys((await doc.getAttachments().catch(() => null)) || {}).length;
  result.tagged = Boolean((await doc.getMarkInfo().catch(() => null))?.Marked);
  result.javascript = Object.keys((await doc.getJSActions().catch(() => null)) || {}).length;

  const fields = (await doc.getFieldObjects().catch(() => null)) || {};
  result.fields = Object.keys(fields).length;
  result.signatures = Object.values(fields).flat().filter((field) => field?.type === "signature").length;

  const annots = {};
  let links = 0;
  const texts = [];
  for (let n = 1; n <= Math.min(doc.numPages, MAX_ANNOT_PAGES); n++) {
    const page = await doc.getPage(n);
    for (const annot of await page.getAnnotations({ intent: "display" }).catch(() => [])) {
      const kind = annot.subtype || "Unknown";
      if (kind === "Link") links++;
      else if (kind !== "Widget") annots[kind] = (annots[kind] || 0) + 1;
    }
    if (n <= MAX_TEXT_PAGES) {
      const content = await page.getTextContent().catch(() => null);
      texts.push((content?.items || []).map((item) => item.str ?? "").join(" "));
    }
    page.cleanup();
  }
  result.links = links;
  result.annotations = annots;
  if (textOut) fs.writeFileSync(textOut, texts.join("\n"));
  result.ok = true;
  await doc.destroy();
} catch (error) {
  result.error = `${error?.name ?? "Error"}: ${error?.message ?? error}`.slice(0, 300);
}
process.stdout.write(JSON.stringify(result));
