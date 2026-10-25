// Set A, the feature set: small PDFs we make ourselves, each with one thing a PDF/A converter
// has to handle (a font not embedded, a colour space, transparency, a form, an action, ...).
// Every file is made by this script from nothing but the container's own fonts and pictures
// drawn by make-images.py, so the set can be published with the bench.
//
//   node scripts/build-set-a.mjs            (writes /data/sets/a/*.pdf and index.json)

import fs from "node:fs";
import path from "node:path";

import { log, must, readJson, requireDep, SETS, writeJson } from "./lib.mjs";

const lib = requireDep("pdf-lib");
const { PDFDocument, StandardFonts, rgb, cmyk, PDFName, PDFString, PDFHexString, PDFDict, PDFBool, BlendMode, AFRelationship } = lib;
const fontkit = requireDep("@pdf-lib/fontkit");

const OUT = path.join(SETS, "a");
const WORK = path.join(SETS, ".work-a");
const IMAGES = path.join(WORK, "images");
const FONT_DIR = "/usr/share/fonts/truetype";
const SANS = `${FONT_DIR}/liberation2/LiberationSans-Regular.ttf`;
const SANS_BOLD = `${FONT_DIR}/liberation2/LiberationSans-Bold.ttf`;
const SERIF = `${FONT_DIR}/liberation2/LiberationSerif-Regular.ttf`;
const MONO = `${FONT_DIR}/liberation2/LiberationMono-Regular.ttf`;
const CARLITO = `${FONT_DIR}/crosextra/Carlito-Regular.ttf`;
const CALADEA = `${FONT_DIR}/crosextra/Caladea-Regular.ttf`;
const DEJAVU = `${FONT_DIR}/dejavu/DejaVuSans.ttf`;
const DATE = new Date("2026-10-24T00:00:00Z");
const A4 = [595, 842];

const fixtures = [];

/** Registers a test file: |make| returns the PDF's bytes, or writes |file| itself. */
function fixture(id, title, features, make, { levels = ["1b", "2b", "3b"], expect } = {}) {
  fixtures.push({ id, title, features, levels, expect, make });
}

// ---------------------------------------------------------------------------------------------
// Helpers

async function newDoc(title) {
  const doc = await PDFDocument.create({ updateMetadata: false });
  doc.registerFontkit(fontkit);
  doc.setTitle(title);
  doc.setProducer("FynePDF PDF/A bench, set A");
  doc.setCreator("build-set-a.mjs");
  doc.setCreationDate(DATE);
  doc.setModificationDate(DATE);
  return doc;
}

const labelFonts = new WeakMap();

/** The page's heading and description, in an embedded subset of Liberation Sans. */
async function label(doc, page, title, lines = []) {
  if (!labelFonts.has(doc)) labelFonts.set(doc, await doc.embedFont(fs.readFileSync(SANS), { subset: true }));
  const font = labelFonts.get(doc);
  let y = page.getHeight() - 56;
  page.drawText(title, { x: 50, y, size: 15, font, color: rgb(0.1, 0.1, 0.1) });
  y -= 22;
  for (const line of lines) {
    page.drawText(line, { x: 50, y, size: 10, font, color: rgb(0.3, 0.3, 0.3) });
    y -= 14;
  }
  return font;
}

async function page1(title, heading, lines, size = A4) {
  const doc = await newDoc(title);
  const page = doc.addPage(size);
  await label(doc, page, heading, lines);
  return { doc, page, ctx: doc.context };
}

function setResource(page, kind, name, value) {
  const resources = page.node.normalizedEntries().Resources;
  let dict = resources.lookupMaybe(PDFName.of(kind), PDFDict);
  if (!dict) {
    dict = page.doc.context.obj({});
    resources.set(PDFName.of(kind), dict);
  }
  dict.set(PDFName.of(name), value);
}

/** Appends raw content operators to the page, in their own q/Q. */
function addContent(page, ops, { lzw: useLzw = false } = {}) {
  const ctx = page.doc.context;
  const text = `q\n${ops}\nQ\n`;
  const stream = useLzw ? ctx.stream(lzw(Buffer.from(text, "latin1")), { Filter: "LZWDecode" }) : ctx.flateStream(text);
  page.node.addContentStream(ctx.register(stream));
}

const str = (text) => `(${text.replace(/[\\()]/g, (c) => `\\${c}`)})`;

function metrics(file) {
  const font = fontkit.create(fs.readFileSync(file));
  const scale = 1000 / font.unitsPerEm;
  const widths = [];
  for (let code = 32; code <= 126; code++) widths.push(Math.round(font.glyphForCodePoint(code).advanceWidth * scale));
  const box = font.bbox;
  return {
    widths,
    bbox: [box.minX, box.minY, box.maxX, box.maxY].map((v) => Math.round(v * scale)),
    ascent: Math.round(font.ascent * scale),
    descent: Math.round(font.descent * scale),
    capHeight: Math.round((font.capHeight || font.ascent) * scale),
  };
}

/**
 * A simple (WinAnsi) font dictionary named |baseFont| with |metricsFile|'s widths. Not embedded
 * unless |embed|; then the whole TrueType file goes in. |widthScale| makes the widths disagree
 * with the embedded font on purpose.
 */
function simpleFont(doc, { baseFont, metricsFile, embed = false, widthScale = 1, subtype = "TrueType" }) {
  const ctx = doc.context;
  const m = metrics(metricsFile);
  const descriptor = {
    Type: "FontDescriptor",
    FontName: baseFont,
    Flags: 32,
    FontBBox: m.bbox,
    ItalicAngle: 0,
    Ascent: m.ascent,
    Descent: m.descent,
    CapHeight: m.capHeight,
    StemV: 80,
  };
  if (embed) {
    const bytes = fs.readFileSync(metricsFile);
    descriptor.FontFile2 = ctx.register(ctx.flateStream(bytes, { Length1: bytes.length }));
  }
  return ctx.register(
    ctx.obj({
      Type: "Font",
      Subtype: subtype,
      BaseFont: baseFont,
      FirstChar: 32,
      LastChar: 126,
      Widths: m.widths.map((w) => Math.round(w * widthScale)),
      Encoding: "WinAnsiEncoding",
      FontDescriptor: ctx.register(ctx.obj(descriptor)),
    }),
  );
}

function standardFont(doc, baseFont) {
  const encoding = baseFont === "Symbol" || baseFont === "ZapfDingbats" ? {} : { Encoding: "WinAnsiEncoding" };
  return doc.context.register(doc.context.obj({ Type: "Font", Subtype: "Type1", BaseFont: baseFont, ...encoding }));
}

function image(doc, { width, height, bpc = 8, colorSpace, data, filter = "flate", extra = {} }) {
  const ctx = doc.context;
  const dict = { Type: "XObject", Subtype: "Image", Width: width, Height: height, BitsPerComponent: bpc, ColorSpace: colorSpace, ...extra };
  if (filter === "flate") return ctx.register(ctx.flateStream(data, dict));
  if (filter === "lzw") return ctx.register(ctx.stream(lzw(data), { ...dict, Filter: "LZWDecode" }));
  return ctx.register(ctx.stream(data, { ...dict, Filter: filter }));
}

function form(doc, ops, { bbox = [0, 0, 100, 100], resources = {}, group } = {}) {
  const ctx = doc.context;
  return ctx.register(ctx.stream(ops, { Type: "XObject", Subtype: "Form", BBox: bbox, Resources: resources, ...(group ? { Group: group } : {}) }));
}

function pixels(width, height, channels, fn, bytesPerSample = 1) {
  const data = Buffer.alloc(width * height * channels * bytesPerSample);
  let at = 0;
  for (let y = 0; y < height; y++)
    for (let x = 0; x < width; x++)
      for (const value of fn(x, y)) {
        if (bytesPerSample === 2) {
          data.writeUInt16BE(value, at);
          at += 2;
        } else data[at++] = value;
      }
  return data;
}

function addAnnot(page, dict) {
  const ctx = page.doc.context;
  const ref = ctx.register(ctx.obj({ Type: "Annot", ...dict }));
  page.node.addAnnot(ref);
  return ref;
}

function xmpPacket({ title, pdfaPart, creatorTool }) {
  const pdfaid = pdfaPart
    ? `<rdf:Description rdf:about="" xmlns:pdfaid="http://www.aiim.org/pdfa/ns/id/"><pdfaid:part>${pdfaPart}</pdfaid:part><pdfaid:conformance>B</pdfaid:conformance></rdf:Description>`
    : "";
  const tool = creatorTool
    ? `<rdf:Description rdf:about="" xmlns:xmp="http://ns.adobe.com/xap/1.0/"><xmp:CreatorTool>${creatorTool}</xmp:CreatorTool></rdf:Description>`
    : "";
  return (
    `<?xpacket begin="﻿" id="W5M0MpCehiHzreSzNTczkc9d"?><x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">` +
    `${pdfaid}<rdf:Description rdf:about="" xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title><rdf:Alt><rdf:li xml:lang="x-default">${title}</rdf:li></rdf:Alt></dc:title></rdf:Description>` +
    `${tool}</rdf:RDF></x:xmpmeta><?xpacket end="w"?>`
  );
}

function setXmp(doc, packet) {
  const ctx = doc.context;
  doc.catalog.set(PDFName.of("Metadata"), ctx.register(ctx.stream(Buffer.from(packet, "utf8"), { Type: "Metadata", Subtype: "XML" })));
}

/** PDF's LZW (12-bit codes at most, EarlyChange 1), for the LZW test file. */
function lzw(input) {
  const out = [];
  let buffer = 0;
  let bits = 0;
  const emit = (code, width) => {
    buffer = (buffer << width) | code;
    bits += width;
    while (bits >= 8) {
      out.push((buffer >>> (bits - 8)) & 0xff);
      bits -= 8;
    }
    buffer &= (1 << bits) - 1;
  };
  // The decoder reads each code one dictionary entry behind the encoder; EarlyChange 1 widens
  // the codes one entry early. Together: |next| at or past 512 means 10 bits, and so on.
  const width = (next) => (next >= 2048 ? 12 : next >= 1024 ? 11 : next >= 512 ? 10 : 9);
  let table = new Map();
  let next = 258;
  emit(256, 9);
  if (!input.length) {
    emit(257, 9);
  } else {
    let w = input[0];
    for (let i = 1; i < input.length; i++) {
      const c = input[i];
      const key = w * 256 + c;
      if (table.has(key)) {
        w = table.get(key);
        continue;
      }
      emit(w, width(next));
      table.set(key, next++);
      if (next >= 4094) {
        emit(256, width(next));
        table = new Map();
        next = 258;
      }
      w = c;
    }
    emit(w, width(next));
    emit(257, width(next + 1));
  }
  if (bits > 0) out.push((buffer << (8 - bits)) & 0xff);
  return Buffer.from(out);
}

const save = (doc, options) => doc.save({ useObjectStreams: false, ...options });

// ---------------------------------------------------------------------------------------------
// Controls and fonts

fixture("control-clean", "Control: a clean page", ["control"], async () => {
  const { doc, page } = await page1("Control: a clean page", "Control: a clean page", [
    "Text in an embedded subset font and plain RGB colours. Nothing PDF/A forbids,",
    "but no output intent, no XMP metadata and no file ID: the smallest possible fix.",
  ]);
  page.drawRectangle({ x: 50, y: 500, width: 200, height: 120, color: rgb(0.2, 0.5, 0.8) });
  return save(doc);
});

fixture("font-std14", "Standard 14 fonts, not embedded", ["fonts-not-embedded", "standard-14"], async () => {
  const { doc, page } = await page1("Standard 14 fonts", "Standard 14 fonts, not embedded", [
    "Helvetica, Times, Courier, Symbol and ZapfDingbats by name only, as pdf-lib,",
    "ReportLab and many invoice generators write them. PDF/A needs every font embedded.",
  ]);
  let y = 680;
  for (const name of ["Helvetica", "Helvetica-Bold", "Times-Roman", "Times-Italic", "Courier", "Courier-Bold"]) {
    const font = await doc.embedFont(name);
    page.drawText(`${name}: The quick brown fox jumps over the lazy dog 0123456789`, { x: 50, y, size: 12, font });
    y -= 26;
  }
  page.drawText("αβγδεπσω ∑∫√∞", { x: 50, y, size: 16, font: await doc.embedFont(StandardFonts.Symbol) });
  page.drawText("✓✔✕✖✗✘", { x: 250, y, size: 16, font: await doc.embedFont(StandardFonts.ZapfDingbats) });
  return save(doc);
});

fixture("font-truetype-not-embedded", "Arial, Times New Roman and Courier New, not embedded", ["fonts-not-embedded", "truetype"], async () => {
  const { page, doc } = await page1("TrueType fonts not embedded", "Arial, Times New Roman, Courier New: not embedded", [
    "The names older Office and print drivers write (ArialMT, Arial,Bold, TimesNewRomanPSMT),",
    "with widths only. Free fonts with the same widths exist for all of them (Liberation).",
  ]);
  const fonts = [
    ["F1", "ArialMT", SANS],
    ["F2", "Arial,Bold", SANS_BOLD],
    ["F3", "TimesNewRomanPSMT", SERIF],
    ["F4", "CourierNewPSMT", MONO],
  ];
  let ops = "BT\n";
  let y = 680;
  for (const [key, name, file] of fonts) {
    setResource(page, "Font", key, simpleFont(doc, { baseFont: name, metricsFile: file }));
    ops += `/${key} 14 Tf 1 0 0 1 50 ${y} Tm ${str(`${name}: The quick brown fox jumps over the lazy dog`)} Tj\n`;
    y -= 28;
  }
  addContent(page, `${ops}ET`);
  return save(doc);
});

fixture("font-office-not-embedded", "Calibri and Cambria, not embedded", ["fonts-not-embedded", "truetype"], async () => {
  const { page, doc } = await page1("Office fonts not embedded", "Calibri and Cambria: not embedded", [
    "Microsoft's Office fonts by name only. Carlito and Caladea are free fonts with the",
    "same widths.",
  ]);
  setResource(page, "Font", "F1", simpleFont(doc, { baseFont: "Calibri", metricsFile: CARLITO }));
  setResource(page, "Font", "F2", simpleFont(doc, { baseFont: "Cambria", metricsFile: CALADEA }));
  addContent(page, `BT /F1 14 Tf 50 680 Td ${str("Calibri: The quick brown fox jumps over the lazy dog")} Tj 0 -28 Td /F2 14 Tf ${str("Cambria: The quick brown fox jumps over the lazy dog")} Tj ET`);
  return save(doc);
});

fixture("font-unknown-not-embedded", "A font nobody has, not embedded", ["fonts-not-embedded", "no-substitute"], async () => {
  const { page, doc } = await page1("Unknown font not embedded", "A font with no free match, not embedded", [
    "FyneBenchSans exists nowhere, and Tahoma has no free font with its widths. A converter",
    "must draw the text another way or say the file can't be made PDF/A.",
  ]);
  setResource(page, "Font", "F1", simpleFont(doc, { baseFont: "FyneBenchSans", metricsFile: DEJAVU }));
  setResource(page, "Font", "F2", simpleFont(doc, { baseFont: "Tahoma", metricsFile: DEJAVU }));
  addContent(page, `BT /F1 14 Tf 50 680 Td ${str("FyneBenchSans: The quick brown fox jumps over the lazy dog")} Tj 0 -28 Td /F2 14 Tf ${str("Tahoma: The quick brown fox jumps over the lazy dog")} Tj ET`);
  return save(doc);
});

fixture("font-cjk-not-embedded", "Japanese CID font, not embedded", ["fonts-not-embedded", "cid", "cjk"], async () => {
  const { page, doc, ctx } = await page1("CJK font not embedded", "A Japanese CID font, not embedded", [
    "Kozuka Mincho by name with the UniJIS-UCS2-H CMap, as Acrobat's Asian font packs wrote",
    "it. Readers substitute a font; PDF/A needs one embedded.",
  ]);
  const descriptor = ctx.register(ctx.obj({ Type: "FontDescriptor", FontName: "KozMinPr6N-Regular", Flags: 4, FontBBox: [-437, -340, 1147, 1317], ItalicAngle: 0, Ascent: 880, Descent: -120, CapHeight: 742, StemV: 80 }));
  const cid = ctx.register(
    ctx.obj({
      Type: "Font",
      Subtype: "CIDFontType0",
      BaseFont: "KozMinPr6N-Regular",
      CIDSystemInfo: ctx.obj({ Registry: PDFString.of("Adobe"), Ordering: PDFString.of("Japan1"), Supplement: 6 }),
      FontDescriptor: descriptor,
      DW: 1000,
    }),
  );
  setResource(page, "Font", "FJ", ctx.register(ctx.obj({ Type: "Font", Subtype: "Type0", BaseFont: "KozMinPr6N-Regular-UniJIS-UCS2-H", Encoding: "UniJIS-UCS2-H", DescendantFonts: [cid] })));
  addContent(page, "BT /FJ 28 Tf 50 660 Td <65E5672C8A9E306E30C630AD30B930C8> Tj ET");
  return save(doc);
});

fixture("font-type3", "Type 3 font", ["type3"], async () => {
  const { page, doc, ctx } = await page1("Type 3 font", "A Type 3 font", [
    "Glyphs drawn by content streams, as dvips and some scanners write them. Allowed in PDF/A,",
    "but converters often redraw or drop them.",
  ]);
  const procs = {
    sq: "1000 0 0 0 800 800 d1 0 0 800 800 re f",
    tr: "1000 0 0 0 800 800 d1 0 0 m 800 0 l 400 800 l h f",
    di: "1000 0 0 0 800 800 d1 400 0 m 800 400 l 400 800 l 0 400 l h f",
  };
  const charProcs = Object.fromEntries(Object.entries(procs).map(([name, ops]) => [name, ctx.register(ctx.stream(ops))]));
  setResource(
    page,
    "Font",
    "T3",
    ctx.register(
      ctx.obj({
        Type: "Font",
        Subtype: "Type3",
        FontBBox: [0, 0, 800, 800],
        FontMatrix: [0.001, 0, 0, 0.001, 0, 0],
        CharProcs: ctx.obj(charProcs),
        Encoding: ctx.obj({ Type: "Encoding", Differences: [65, "sq", "tr", "di"] }),
        FirstChar: 65,
        LastChar: 67,
        Widths: [1000, 1000, 1000],
        Resources: ctx.obj({}),
      }),
    ),
  );
  addContent(page, "BT /T3 48 Tf 0.1 0.4 0.7 rg 50 620 Td (ABCCBA) Tj ET");
  return save(doc);
});

fixture("font-truetype-embedded-full", "TrueType font embedded whole", ["control", "truetype"], async () => {
  const { page, doc } = await page1("TrueType embedded", "A TrueType font embedded whole", [
    "Liberation Serif embedded in full, with matching widths. Should need no font work.",
  ]);
  setResource(page, "Font", "F1", simpleFont(doc, { baseFont: "LiberationSerif", metricsFile: SERIF, embed: true }));
  addContent(page, `BT /F1 16 Tf 50 680 Td ${str("Embedded: The quick brown fox jumps over the lazy dog")} Tj ET`);
  return save(doc);
});

fixture("font-widths-mismatch", "Embedded font with wrong widths", ["font-widths"], async () => {
  const { page, doc } = await page1("Font widths mismatch", "An embedded font whose /Widths disagree with it", [
    "The font program says one advance width, the PDF another (30% wider). PDF/A requires",
    "them to agree.",
  ]);
  setResource(page, "Font", "F1", simpleFont(doc, { baseFont: "LiberationSans", metricsFile: SANS, embed: true, widthScale: 1.3 }));
  addContent(page, `BT /F1 16 Tf 50 680 Td ${str("Spaced out: The quick brown fox jumps over the lazy dog")} Tj ET`);
  return save(doc);
});

fixture("font-invisible-not-embedded", "Scan with invisible text in a font not embedded", ["fonts-not-embedded", "invisible-text", "scan"], async () => {
  const { page, doc } = await page1("Invisible text", "A scan with invisible text in Helvetica, not embedded", [
    "Like an OCR'd scan: a picture of the page, with the words on top in rendering mode 3.",
    "PDF/A-2 and 3 allow a font that's only used invisibly to stay out; PDF/A-1 doesn't.",
  ]);
  const jpg = await doc.embedJpg(fs.readFileSync(path.join(IMAGES, "scan-1.jpg")));
  page.drawImage(jpg, { x: 50, y: 60, width: 495, height: 700 });
  setResource(page, "Font", "FH", standardFont(doc, "Helvetica"));
  addContent(page, `BT 3 Tr /FH 11 Tf 75 700 Td ${str("FynePDF bench: a scanned page")} Tj 0 -40 Td ${str("This page was drawn as a picture of text")} Tj ET`);
  return save(doc);
});

fixture("text-clip-mode", "Text used as a clipping path", ["text-render-mode"], async () => {
  const { page, doc, ctx } = await page1("Text clip", "Text as a clipping path (rendering mode 7)", [
    "A gradient seen through the letters. The font is embedded; converters must keep the clip.",
  ]);
  setResource(page, "Font", "F1", simpleFont(doc, { baseFont: "LiberationSans-Bold", metricsFile: SANS_BOLD, embed: true }));
  setResource(page, "Shading", "Sh1", ctx.register(ctx.obj({ ShadingType: 2, ColorSpace: "DeviceRGB", Coords: [50, 0, 545, 0], Function: ctx.obj({ FunctionType: 2, Domain: [0, 1], C0: [0.9, 0.1, 0.2], C1: [0.1, 0.3, 0.9], N: 1 }), Extend: [true, true] })));
  addContent(page, "BT 7 Tr /F1 72 Tf 50 560 Td (FynePDF) Tj ET /Sh1 sh");
  return save(doc);
});

// ---------------------------------------------------------------------------------------------
// Colour

fixture("colour-device-rgb", "Device RGB colours", ["colour-rgb"], async () => {
  const { page, doc } = await page1("Device RGB", "Device RGB colours", ["Fills and text in DeviceRGB. PDF/A needs an output intent that says what they mean."]);
  [rgb(0.9, 0.2, 0.2), rgb(0.2, 0.7, 0.3), rgb(0.2, 0.3, 0.9)].forEach((color, i) => page.drawRectangle({ x: 50 + i * 170, y: 560, width: 150, height: 120, color }));
  return save(doc);
});

fixture("colour-device-cmyk", "Device CMYK colours", ["colour-cmyk"], async () => {
  const { page, doc } = await page1("Device CMYK", "Device CMYK colours", ["Fills and text in DeviceCMYK, as print-ready files have them."]);
  [cmyk(0, 0.9, 0.8, 0), cmyk(0.8, 0, 0.9, 0.1), cmyk(1, 0.6, 0, 0), cmyk(0, 0, 0, 1)].forEach((color, i) => page.drawRectangle({ x: 50 + i * 125, y: 560, width: 110, height: 120, color }));
  return save(doc);
});

fixture("colour-rgb-and-cmyk", "RGB and CMYK on one page", ["colour-rgb", "colour-cmyk"], async () => {
  const { page, doc } = await page1("RGB and CMYK", "DeviceRGB and DeviceCMYK on the same page", [
    "One output intent can't cover both: a converter has to convert one, or give it a profile.",
  ]);
  page.drawRectangle({ x: 50, y: 560, width: 220, height: 120, color: rgb(0.9, 0.4, 0.1) });
  page.drawRectangle({ x: 300, y: 560, width: 220, height: 120, color: cmyk(0, 0.6, 0.9, 0) });
  return save(doc);
});

fixture("colour-spot", "Spot colour (Separation)", ["colour-spot"], async () => {
  const { page, doc, ctx } = await page1("Spot colour", "A spot ink (Separation colour space)", ["A named ink with a CMYK alternate and a tint transform, as packaging and brand files use."]);
  const tint = ctx.obj({ FunctionType: 2, Domain: [0, 1], C0: [0, 0, 0, 0], C1: [0, 0.91, 0.76, 0], N: 1 });
  setResource(page, "ColorSpace", "CS0", ctx.obj(["Separation", "PANTONE 185 C", "DeviceCMYK", tint]));
  addContent(page, "/CS0 cs 1 scn 50 560 200 120 re f 0.4 scn 300 560 200 120 re f");
  return save(doc);
});

fixture("colour-lab", "Lab colour", ["colour-lab"], async () => {
  const { page, doc, ctx } = await page1("Lab colour", "Lab colours", ["CIE Lab is device independent and allowed; converters should leave it alone."]);
  setResource(page, "ColorSpace", "CS1", ctx.obj(["Lab", ctx.obj({ WhitePoint: [0.9505, 1, 1.089], Range: [-100, 100, -100, 100] })]));
  addContent(page, "/CS1 cs 55 70 50 scn 50 560 200 120 re f 70 -40 60 scn 300 560 200 120 re f");
  return save(doc);
});

fixture("colour-icc-image", "ICC-based picture next to a DeviceRGB one", ["colour-icc", "colour-rgb"], async () => {
  const { page, doc, ctx } = await page1("ICC picture", "A picture with its own sRGB profile, beside a DeviceRGB one", ["The ICC-based one is fine as it is; the DeviceRGB one needs the output intent."]);
  const icc = ctx.register(ctx.flateStream(fs.readFileSync(path.join(IMAGES, "srgb.icc")), { N: 3 }));
  const data = pixels(64, 64, 3, (x, y) => [x * 4, y * 4, 160]);
  setResource(page, "XObject", "Im1", image(doc, { width: 64, height: 64, colorSpace: ctx.obj(["ICCBased", icc]), data }));
  setResource(page, "XObject", "Im2", image(doc, { width: 64, height: 64, colorSpace: "DeviceRGB", data }));
  addContent(page, "q 200 0 0 200 50 480 cm /Im1 Do Q q 200 0 0 200 300 480 cm /Im2 Do Q");
  return save(doc);
});

fixture("colour-cmyk-images", "CMYK pictures (raw and JPEG)", ["colour-cmyk", "image-cmyk"], async () => {
  const { page, doc } = await page1("CMYK pictures", "CMYK pictures: a raw one and a CMYK JPEG", ["As print shops and Photoshop exports have them."]);
  setResource(page, "XObject", "Im1", image(doc, { width: 64, height: 64, colorSpace: "DeviceCMYK", data: pixels(64, 64, 4, (x, y) => [x * 4, y * 4, 0, 20]) }));
  setResource(page, "XObject", "Im2", image(doc, { width: 400, height: 300, colorSpace: "DeviceCMYK", data: fs.readFileSync(path.join(IMAGES, "photo-cmyk.jpg")), filter: "DCTDecode", extra: { Decode: [1, 0, 1, 0, 1, 0, 1, 0] } }));
  addContent(page, "q 200 0 0 200 50 480 cm /Im1 Do Q q 240 0 0 180 300 500 cm /Im2 Do Q");
  return save(doc);
});

fixture("colour-indexed-image", "Indexed (palette) picture", ["colour-indexed"], async () => {
  const { page, doc, ctx } = await page1("Palette picture", "An indexed picture on a DeviceRGB palette", ["Converters must convert the palette, not the indices."]);
  const palette = ctx.obj(["Indexed", "DeviceRGB", 3, PDFHexString.of("e53935" + "43a047" + "1e88e5" + "fdd835")]);
  setResource(page, "XObject", "Im1", image(doc, { width: 16, height: 16, colorSpace: palette, data: pixels(16, 16, 1, (x, y) => [((x >> 2) + (y >> 2)) % 4]) }));
  addContent(page, "q 240 0 0 240 50 440 cm /Im1 Do Q");
  return save(doc);
});

fixture("colour-shading", "Gradients (shadings)", ["colour-rgb", "shading"], async () => {
  const { page, doc, ctx } = await page1("Gradients", "An axial and a radial gradient in DeviceRGB", ["Shadings carry their own colour space and functions."]);
  const fn = ctx.obj({ FunctionType: 2, Domain: [0, 1], C0: [1, 0.8, 0], C1: [0.6, 0, 0.6], N: 1 });
  setResource(page, "Shading", "Sh1", ctx.register(ctx.obj({ ShadingType: 2, ColorSpace: "DeviceRGB", Coords: [50, 0, 300, 0], Function: fn, Extend: [true, true] })));
  setResource(page, "Shading", "Sh2", ctx.register(ctx.obj({ ShadingType: 3, ColorSpace: "DeviceRGB", Coords: [420, 600, 0, 420, 600, 90], Function: fn, Extend: [false, true] })));
  addContent(page, "q 50 540 250 140 re W n /Sh1 sh Q q 330 520 180 160 re W n /Sh2 sh Q");
  return save(doc);
});

fixture("colour-pattern", "Tiling pattern", ["colour-rgb", "pattern"], async () => {
  const { page, doc, ctx } = await page1("Pattern", "A coloured tiling pattern", ["Patterns have their own content and resources."]);
  setResource(page, "Pattern", "P1", ctx.register(ctx.stream("1 0.4 0.2 rg 0 0 10 10 re f 0.2 0.4 1 rg 10 10 10 10 re f", { Type: "Pattern", PatternType: 1, PaintType: 1, TilingType: 1, BBox: [0, 0, 20, 20], XStep: 20, YStep: 20, Resources: {} })));
  addContent(page, "/Pattern cs /P1 scn 50 520 300 160 re f");
  return save(doc);
});

fixture("colour-inline-image", "Inline picture", ["colour-rgb", "inline-image"], async () => {
  const { page, doc } = await page1("Inline picture", "A picture written inside the content stream", ["BI ... ID ... EI, as some scanners and old drivers write them."]);
  addContent(page, "q 160 0 0 160 50 500 cm BI /W 2 /H 2 /CS /RGB /BPC 8 /F /AHx ID ff000000ff000000ffffffff> EI Q");
  return save(doc);
});

// ---------------------------------------------------------------------------------------------
// Transparency

fixture("transparency-opacity", "See-through shapes and a transparent PNG", ["transparency"], async () => {
  const { page, doc } = await page1("Transparency", "See-through shapes and a PNG with an alpha channel", [
    "Allowed in PDF/A-2 and 3. PDF/A-1 forbids transparency: a converter must flatten it or refuse.",
  ]);
  page.drawRectangle({ x: 50, y: 520, width: 220, height: 160, color: rgb(0.9, 0.2, 0.2), opacity: 0.5 });
  page.drawRectangle({ x: 150, y: 470, width: 220, height: 160, color: rgb(0.2, 0.3, 0.9), opacity: 0.5 });
  page.drawImage(await doc.embedPng(fs.readFileSync(path.join(IMAGES, "alpha.png"))), { x: 360, y: 450, width: 180, height: 180 });
  return save(doc);
});

fixture("transparency-blend-modes", "Blend modes", ["transparency", "blend-mode"], async () => {
  const { page, doc } = await page1("Blend modes", "Multiply and Screen blend modes", ["Standard blend modes are allowed in PDF/A-2 and 3."]);
  page.drawRectangle({ x: 50, y: 520, width: 220, height: 160, color: rgb(0.95, 0.75, 0.1) });
  page.drawRectangle({ x: 150, y: 470, width: 220, height: 160, color: rgb(0.1, 0.6, 0.9), blendMode: BlendMode.Multiply });
  page.drawRectangle({ x: 300, y: 520, width: 220, height: 160, color: rgb(0.8, 0.1, 0.5), blendMode: BlendMode.Screen });
  return save(doc);
});

fixture("transparency-soft-mask", "Luminosity soft mask", ["transparency", "soft-mask"], async () => {
  const { page, doc, ctx } = await page1("Soft mask", "A shape faded by a luminosity soft mask", ["A gradient decides how much of the shape shows."]);
  const grey = ctx.register(ctx.obj({ ShadingType: 2, ColorSpace: "DeviceGray", Coords: [50, 0, 545, 0], Function: ctx.obj({ FunctionType: 2, Domain: [0, 1], C0: [1], C1: [0], N: 1 }), Extend: [true, true] }));
  const mask = form(doc, "/ShM sh", { bbox: [0, 0, 595, 842], resources: ctx.obj({ Shading: ctx.obj({ ShM: grey }) }), group: ctx.obj({ S: "Transparency", CS: "DeviceGray" }) });
  setResource(page, "ExtGState", "GSm", ctx.register(ctx.obj({ Type: "ExtGState", SMask: ctx.obj({ Type: "Mask", S: "Luminosity", G: mask }) })));
  addContent(page, "/GSm gs 0.1 0.5 0.2 rg 50 480 495 200 re f");
  return save(doc);
});

fixture("transparency-page-group", "Page transparency group", ["transparency", "page-group"], async () => {
  const { page, doc, ctx } = await page1("Page group", "A page with a transparency group", ["The group's colour space must agree with the output intent in PDF/A-2."]);
  page.node.set(PDFName.of("Group"), ctx.obj({ Type: "Group", S: "Transparency", CS: "DeviceCMYK", I: false, K: false }));
  page.drawRectangle({ x: 50, y: 520, width: 220, height: 160, color: rgb(0.2, 0.6, 0.3), opacity: 0.6 });
  return save(doc);
});

// ---------------------------------------------------------------------------------------------
// Pictures and streams

fixture("image-jpeg", "JPEG photo", ["image-jpeg", "colour-rgb"], async () => {
  const { page, doc } = await page1("JPEG photo", "A JPEG photo", ["The most common picture in PDFs. A converter should not re-encode it."]);
  page.drawImage(await doc.embedJpg(fs.readFileSync(path.join(IMAGES, "photo.jpg"))), { x: 50, y: 380, width: 495, height: 371 });
  return save(doc);
});

fixture("image-16bit", "16-bit picture", ["image-16bit"], async () => {
  const { page, doc } = await page1("16-bit picture", "A picture with 16 bits per sample", ["Allowed from PDF 1.5; PDF/A-1 (PDF 1.4) forbids it."]);
  setResource(page, "XObject", "Im1", image(doc, { width: 32, height: 32, bpc: 16, colorSpace: "DeviceRGB", data: pixels(32, 32, 3, (x, y) => [x * 2000, y * 2000, 30000], 2) }));
  addContent(page, "q 240 0 0 240 50 440 cm /Im1 Do Q");
  return save(doc);
});

fixture("image-jpx", "JPEG 2000 picture", ["image-jpx"], async (file) => {
  await must("img2pdf", [path.join(IMAGES, "photo.jp2"), "-o", file, "--title", "JPEG 2000 picture"]);
});

fixture("image-ccitt", "CCITT fax picture", ["image-ccitt", "scan"], async (file) => {
  await must("img2pdf", [path.join(IMAGES, "fax.tif"), "-o", file, "--title", "CCITT fax picture"]);
});

fixture("image-interpolate", "Picture with Interpolate", ["image-interpolate"], async () => {
  const { page, doc } = await page1("Interpolate", "A picture with /Interpolate true", ["PDF/A forbids /Interpolate true."]);
  setResource(page, "XObject", "Im1", image(doc, { width: 8, height: 8, colorSpace: "DeviceRGB", data: pixels(8, 8, 3, (x, y) => [x * 32, y * 32, 128]), extra: { Interpolate: true } }));
  addContent(page, "q 240 0 0 240 50 440 cm /Im1 Do Q");
  return save(doc);
});

fixture("stream-lzw", "LZW-compressed streams", ["lzw"], async () => {
  const { page, doc } = await page1("LZW", "Content and a picture compressed with LZW", ["Old PDFs used LZW; PDF/A forbids it. A converter must recompress."]);
  setResource(page, "XObject", "Im1", image(doc, { width: 16, height: 16, colorSpace: "DeviceRGB", data: pixels(16, 16, 3, (x, y) => [x * 16, 255 - y * 16, 90]), filter: "lzw" }));
  addContent(page, "0.8 0.3 0.1 rg 50 560 200 120 re f q 160 0 0 160 300 520 cm /Im1 Do Q", { lzw: true });
  return save(doc);
});

// ---------------------------------------------------------------------------------------------
// Forms, comments, links

async function formPage(title, heading, lines) {
  const { doc, page } = await page1(title, heading, lines);
  const font = labelFonts.get(doc);
  const form = doc.getForm();
  const name = form.createTextField("name");
  name.setText("Ada Lovelace");
  name.addToPage(page, { x: 50, y: 640, width: 250, height: 24, font });
  const agree = form.createCheckBox("agree");
  agree.addToPage(page, { x: 50, y: 600, width: 18, height: 18 });
  agree.check();
  const plan = form.createRadioGroup("plan");
  plan.addOptionToPage("free", page, { x: 50, y: 560, width: 18, height: 18 });
  plan.addOptionToPage("pro", page, { x: 90, y: 560, width: 18, height: 18 });
  plan.select("pro");
  const country = form.createDropdown("country");
  country.addOptions(["Nepal", "India", "Japan"]);
  country.select("Nepal");
  country.addToPage(page, { x: 50, y: 510, width: 150, height: 24, font });
  return { doc, page, form, font };
}

fixture("forms-with-appearances", "Filled form with appearances", ["forms"], async () => {
  const { doc, form, font } = await formPage("Form with appearances", "A filled form, every field with its appearance", ["Text field, check box, radio buttons and a list. PDF/A keeps forms if each has an appearance."]);
  form.updateFieldAppearances(font);
  return save(doc);
});

fixture("forms-need-appearances", "Form without appearances (NeedAppearances)", ["forms", "need-appearances"], async () => {
  const { doc, form } = await formPage("Form without appearances", "A filled form with no appearances, NeedAppearances true", ["Each reader draws the fields itself. PDF/A forbids NeedAppearances; appearances must be made."]);
  for (const field of form.getFields()) for (const widget of field.acroField.getWidgets()) widget.dict.delete(PDFName.of("AP"));
  form.acroForm.dict.set(PDFName.of("NeedAppearances"), PDFBool.True);
  return save(doc, { updateFieldAppearances: false });
});

fixture("forms-xfa", "Form with XFA", ["forms", "xfa"], async () => {
  const { doc, form, font } = await formPage("Form with XFA", "A form that also carries XFA", ["XFA forms are forbidden in PDF/A; the AcroForm must stay and the XFA go."]);
  form.updateFieldAppearances(font);
  const xfa = '<xdp:xdp xmlns:xdp="http://ns.adobe.com/xdp/"><template xmlns="http://www.xfa.org/schema/xfa-template/3.3/"><subform name="form1"/></template></xdp:xdp>';
  // pdf-lib drops /XFA when it saves, so the key is written as /XFAX and renamed in the bytes
  // (same length, and the AcroForm dictionary isn't compressed without object streams).
  form.acroForm.dict.set(PDFName.of("XFAX"), doc.context.register(doc.context.flateStream(xfa)));
  const bytes = Buffer.from(await save(doc));
  const at = bytes.indexOf("/XFAX ");
  if (at < 0) throw new Error("the XFA placeholder is missing");
  bytes.write("/XFA  ", at, "latin1");
  return bytes;
});

function commentDicts(page, withAppearance) {
  const ctx = page.doc.context;
  const ap = (ops, w, h) => (withAppearance ? { F: 4, AP: ctx.obj({ N: form(page.doc, ops, { bbox: [0, 0, w, h] }) }) } : {});
  addAnnot(page, { Subtype: "Text", Rect: [400, 680, 420, 700], Contents: PDFString.of("A sticky note"), Name: "Comment", C: [1, 0.85, 0], ...ap("1 0.85 0 rg 0 0 20 20 re f", 20, 20) });
  addAnnot(page, { Subtype: "Highlight", Rect: [50, 600, 300, 620], QuadPoints: [50, 620, 300, 620, 50, 600, 300, 600], C: [1, 1, 0], Contents: PDFString.of("Highlighted"), ...ap("1 1 0 rg 0 0 250 20 re f", 250, 20) });
  addAnnot(page, { Subtype: "Square", Rect: [50, 480, 250, 580], C: [0.9, 0.1, 0.1], BS: ctx.obj({ W: 2 }), ...ap("0.9 0.1 0.1 RG 2 w 1 1 198 98 re S", 200, 100) });
  addAnnot(page, { Subtype: "FreeText", Rect: [300, 480, 545, 520], Contents: PDFString.of("Typed comment"), DA: PDFString.of("0 g /Helv 12 Tf"), ...ap("BT 0 g 4 14 Td ET", 245, 40) });
}

fixture("annots-without-appearances", "Comments without appearances", ["annotations", "no-appearance"], async () => {
  const { doc, page } = await page1("Comments without appearances", "A note, a highlight, a box and a typed comment: no appearances", ["PDF/A needs each comment to carry its own appearance and the print flag."]);
  commentDicts(page, false);
  return save(doc);
});

fixture("annots-with-appearances", "Comments with appearances", ["annotations"], async () => {
  const { doc, page } = await page1("Comments with appearances", "The same comments, each with its appearance and the print flag", ["Should pass through untouched."]);
  commentDicts(page, true);
  return save(doc);
});

fixture("annots-multimedia", "Sound and file-attachment comments", ["annotations", "multimedia", "attachments"], async () => {
  const { doc, page, ctx } = await page1("Multimedia comments", "A sound comment and a file-attachment comment", ["Sound annotations are forbidden in PDF/A; file attachments depend on the level."]);
  const sound = ctx.register(ctx.stream(Buffer.alloc(800, 128), { Type: "Sound", R: 8000, C: 1, B: 8, E: "Raw" }));
  addAnnot(page, { Subtype: "Sound", Rect: [50, 640, 70, 660], Sound: sound, Contents: PDFString.of("A sound") });
  const file = ctx.register(ctx.obj({ Type: "Filespec", F: PDFString.of("note.txt"), UF: PDFString.of("note.txt"), EF: ctx.obj({ F: ctx.register(ctx.stream("A text file inside a comment.", { Type: "EmbeddedFile", Subtype: "text/plain" })) }) }));
  addAnnot(page, { Subtype: "FileAttachment", Rect: [100, 640, 120, 660], FS: file, Contents: PDFString.of("An attached file"), Name: "PushPin" });
  return save(doc);
});

fixture("links-and-bookmarks", "Links and bookmarks", ["links", "bookmarks"], async () => {
  const doc = await newDoc("Links and bookmarks");
  const ctx = doc.context;
  const pages = [];
  for (let n = 1; n <= 3; n++) {
    const page = doc.addPage(A4);
    await label(doc, page, `Chapter ${n}`, ["Three pages with a two-level outline, a link to page 3 and a web link.", "PDF/A keeps both; converters sometimes drop them."]);
    pages.push(page);
  }
  addAnnot(pages[0], { Subtype: "Link", Rect: [50, 600, 250, 620], Border: [0, 0, 0], F: 4, Dest: ctx.obj([pages[2].ref, "Fit"]) });
  addAnnot(pages[0], { Subtype: "Link", Rect: [50, 570, 250, 590], Border: [0, 0, 0], F: 4, A: ctx.obj({ S: "URI", URI: PDFString.of("https://www.pdfa.org/") }) });
  const font = labelFonts.get(doc);
  pages[0].drawText("Go to chapter 3", { x: 52, y: 604, size: 12, font, color: rgb(0, 0.3, 0.8) });
  pages[0].drawText("www.pdfa.org", { x: 52, y: 574, size: 12, font, color: rgb(0, 0.3, 0.8) });
  const outlines = ctx.nextRef();
  const items = [ctx.nextRef(), ctx.nextRef(), ctx.nextRef()];
  const child = ctx.nextRef();
  items.forEach((ref, i) =>
    ctx.assign(
      ref,
      ctx.obj({
        Title: PDFString.of(`Chapter ${i + 1}`),
        Parent: outlines,
        Dest: ctx.obj([pages[i].ref, "XYZ", null, null, null]),
        ...(i > 0 ? { Prev: items[i - 1] } : {}),
        ...(i < 2 ? { Next: items[i + 1] } : {}),
        ...(i === 0 ? { First: child, Last: child, Count: 1 } : {}),
      }),
    ),
  );
  ctx.assign(child, ctx.obj({ Title: PDFString.of("Section 1.1"), Parent: items[0], Dest: ctx.obj([pages[0].ref, "XYZ", 0, 700, null]) }));
  ctx.assign(outlines, ctx.obj({ Type: "Outlines", First: items[0], Last: items[2], Count: 4 }));
  doc.catalog.set(PDFName.of("Outlines"), outlines);
  doc.catalog.set(PDFName.of("PageMode"), PDFName.of("UseOutlines"));
  return save(doc);
});

fixture("javascript", "JavaScript", ["javascript"], async () => {
  const { doc, page, ctx } = await page1("JavaScript", "JavaScript in the document, on opening and on a link", ["PDF/A forbids JavaScript. It must be removed, and the pages must still look the same."]);
  doc.addJavaScript("greeting", 'app.alert("Hello from document JavaScript");');
  doc.catalog.set(PDFName.of("OpenAction"), ctx.obj({ S: "JavaScript", JS: PDFString.of('app.alert("Opened");') }));
  addAnnot(page, { Subtype: "Link", Rect: [50, 600, 250, 620], Border: [0, 0, 0], F: 4, A: ctx.obj({ S: "JavaScript", JS: PDFString.of("this.print();") }) });
  return save(doc);
});

fixture("actions-forbidden", "Launch and named actions", ["actions"], async () => {
  const { doc, page, ctx } = await page1("Actions", "A launch action, a Print action and an allowed NextPage", ["PDF/A forbids Launch and named actions other than first/last/next/previous page."]);
  addAnnot(page, { Subtype: "Link", Rect: [50, 640, 250, 660], Border: [0, 0, 0], F: 4, A: ctx.obj({ S: "Launch", F: PDFString.of("calc.exe") }) });
  addAnnot(page, { Subtype: "Link", Rect: [50, 610, 250, 630], Border: [0, 0, 0], F: 4, A: ctx.obj({ S: "Named", N: "Print" }) });
  addAnnot(page, { Subtype: "Link", Rect: [50, 580, 250, 600], Border: [0, 0, 0], F: 4, A: ctx.obj({ S: "Named", N: "NextPage" }) });
  doc.addPage(A4);
  return save(doc);
});

fixture("attachments", "Attached files", ["attachments"], async () => {
  const { doc } = await page1("Attachments", "A text file and a PDF attached to the document", ["PDF/A-1 forbids attachments, PDF/A-2 allows only PDF/A files, PDF/A-3 allows any file."]);
  await doc.attach(Buffer.from("Notes attached to the bench file.\n"), "notes.txt", { mimeType: "text/plain", description: "Notes", creationDate: DATE, modificationDate: DATE, afRelationship: AFRelationship.Data });
  const inner = await newDoc("Inner PDF");
  await label(inner, inner.addPage(A4), "The attached PDF");
  await doc.attach(await save(inner), "inner.pdf", { mimeType: "application/pdf", description: "Inner PDF", creationDate: DATE, modificationDate: DATE, afRelationship: AFRelationship.Source });
  return save(doc);
});

fixture("layers", "Layers (optional content)", ["optional-content"], async () => {
  const { doc, page, ctx } = await page1("Layers", "Two layers, one hidden", ["Optional content is allowed in PDF/A-2 and 3, but each configuration needs a /Name (this one has none)."]);
  const prices = ctx.register(ctx.obj({ Type: "OCG", Name: PDFString.of("Prices") }));
  const notes = ctx.register(ctx.obj({ Type: "OCG", Name: PDFString.of("Notes") }));
  doc.catalog.set(PDFName.of("OCProperties"), ctx.obj({ OCGs: [prices, notes], D: ctx.obj({ Order: [prices, notes], ON: [prices], OFF: [notes] }) }));
  setResource(page, "Properties", "oc1", prices);
  setResource(page, "Properties", "oc2", notes);
  addContent(page, "/OC /oc1 BDC 0.1 0.6 0.3 rg 50 560 200 100 re f EMC /OC /oc2 BDC 0.9 0.1 0.1 rg 300 560 200 100 re f EMC");
  return save(doc);
});

// ---------------------------------------------------------------------------------------------
// Security, structure, metadata

async function plainBytes(title) {
  const { doc, page } = await page1(title, title, ["A plain page, then changed by qpdf or pyHanko."]);
  page.drawRectangle({ x: 50, y: 560, width: 200, height: 100, color: rgb(0.3, 0.5, 0.7) });
  return save(doc);
}

fixture("encrypted-owner-password", "Encrypted, opens without a password", ["encryption"], async (file) => {
  const plain = path.join(WORK, "encrypted-owner-plain.pdf");
  fs.writeFileSync(plain, await plainBytes("Encrypted with an owner password only"));
  await must("qpdf", ["--encrypt", "", "owner-secret", "256", "--print=low", "--", plain, file]);
});

fixture("encrypted-user-password", "Needs a password to open", ["encryption", "password"], async (file) => {
  const plain = path.join(WORK, "encrypted-user-plain.pdf");
  fs.writeFileSync(plain, await plainBytes("Needs a password to open"));
  await must("qpdf", ["--encrypt", "user-secret", "owner-secret", "256", "--", plain, file]);
}, { expect: "cannot-open" });

fixture("signed", "Digitally signed", ["signature"], async (file) => {
  const plain = path.join(WORK, "signed-plain.pdf");
  fs.writeFileSync(plain, await plainBytes("Digitally signed"));
  const key = path.join(WORK, "key.pem");
  const cert = path.join(WORK, "cert.pem");
  await must("python3", [
    "-c",
    `
import datetime
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID
key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "FynePDF bench test signer")])
start = datetime.datetime(2026, 10, 1, tzinfo=datetime.timezone.utc)
cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
        .serial_number(1).not_valid_before(start).not_valid_after(start + datetime.timedelta(days=3650))
        .sign(key, hashes.SHA256()))
open("${key}", "wb").write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
open("${cert}", "wb").write(cert.public_bytes(serialization.Encoding.PEM))
`,
  ]);
  await must("pyhanko", ["sign", "addsig", "--field", "1/350,60,545,120/Signature1", "--reason", "Bench test", "pemder", "--key", key, "--cert", cert, "--no-pass", plain, file]);
});

fixture("damaged-xref", "Damaged cross-reference table", ["damaged"], async () => {
  const bytes = Buffer.from(await plainBytes("Damaged cross-reference table"));
  const text = bytes.toString("latin1");
  const at = text.lastIndexOf("startxref");
  const offset = Number(text.slice(at + 9).trim().split(/\s/)[0]);
  // Point startxref 40 bytes off, and break one entry's offset, as a careless edit does.
  let broken = `${text.slice(0, at)}startxref\n${offset - 40}\n%%EOF\n`;
  broken = broken.replace(/(\n)(\d{10}) 00000 n/, (_, nl, num) => `${nl}${String(Number(num) + 7).padStart(10, "0")} 00000 n`);
  return Buffer.from(broken, "latin1");
});

fixture("pdf-2-0", "PDF 2.0 file", ["pdf-2.0"], async () => {
  const { doc, page } = await page1("PDF 2.0", "A file that says it is PDF 2.0", ["PDF/A-2 and 3 are based on PDF 1.7; a converter must write a version they allow."]);
  page.drawRectangle({ x: 50, y: 560, width: 200, height: 100, color: rgb(0.5, 0.3, 0.7) });
  const bytes = Buffer.from(await save(doc));
  bytes.write("%PDF-2.0", 0, "latin1");
  return bytes;
});

fixture("pdfa-false-claim", "Claims PDF/A-2b but isn't", ["false-claim", "fonts-not-embedded"], async () => {
  const { doc, page } = await page1("Claims PDF/A", "Says it is PDF/A-2b in its metadata, but its fonts aren't embedded", ["A converter must fix it for real, or take the claim away."]);
  page.drawText("Helvetica, not embedded", { x: 50, y: 640, size: 14, font: await doc.embedFont(StandardFonts.Helvetica) });
  setXmp(doc, xmpPacket({ title: "Claims PDF/A", pdfaPart: 2 }));
  return save(doc);
});

fixture("metadata-mismatch", "Document info and XMP disagree", ["metadata"], async () => {
  const { doc } = await page1("Title in document info", "The document info and the XMP metadata disagree", ["PDF/A needs the two to say the same thing."]);
  setXmp(doc, xmpPacket({ title: "Title in XMP", creatorTool: "Some other tool" }));
  return save(doc);
});

fixture("postscript-xobject", "PostScript XObject", ["postscript"], async () => {
  const { doc, page, ctx } = await page1("PostScript XObject", "A PostScript XObject", ["Embedded PostScript is forbidden in PDF/A."]);
  setResource(page, "XObject", "PS1", ctx.register(ctx.stream("%!PS\n/Helvetica findfont 12 scalefont setfont 72 72 moveto (PostScript) show\n", { Type: "XObject", Subtype: "PS" })));
  addContent(page, "/PS1 Do");
  return save(doc);
});

fixture("old-pdf-1-3", "A hand-written PDF 1.3", ["old", "fonts-not-embedded", "standard-14"], async () => {
  const content = "BT /F1 18 Tf 72 720 Td (A hand-written PDF 1.3 with no binary comment line) Tj ET\n";
  const objects = [
    "<< /Type /Catalog /Pages 2 0 R >>",
    "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
    "<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman >>",
    `<< /Length ${content.length} >>\nstream\n${content}endstream`,
    "<< /Producer (FynePDF PDF/A bench, set A) /Title (A hand-written PDF 1.3) >>",
  ];
  let body = "%PDF-1.3\n";
  const offsets = [];
  objects.forEach((object, i) => {
    offsets.push(body.length);
    body += `${i + 1} 0 obj\n${object}\nendobj\n`;
  });
  const xref = body.length;
  body += `xref\n0 ${objects.length + 1}\n0000000000 65535 f \n${offsets.map((o) => `${String(o).padStart(10, "0")} 00000 n \n`).join("")}`;
  body += `trailer\n<< /Size ${objects.length + 1} /Root 1 0 R /Info 6 0 R >>\nstartxref\n${xref}\n%%EOF\n`;
  return Buffer.from(body, "latin1");
});

// ---------------------------------------------------------------------------------------------
// Scans and big files

fixture("scan-jpeg", "Phone-style scan (3 JPEG pages)", ["scan", "image-jpeg"], async (file) => {
  await must("img2pdf", [1, 2, 3].map((n) => path.join(IMAGES, `scan-${n}.jpg`)).concat(["-o", file, "--title", "Phone-style scan"]));
});

fixture("scan-ocr", "Scan with an OCR text layer", ["scan", "ocr", "invisible-text"], async (file) => {
  const scan = path.join(WORK, "scan-for-ocr.pdf");
  await must("img2pdf", [1, 2, 3].map((n) => path.join(IMAGES, `scan-${n}.jpg`)).concat(["-o", scan]));
  await must("ocrmypdf", ["--output-type", "pdf", "--optimize", "0", "--jobs", "2", "-l", "eng", "--title", "Scan with an OCR text layer", scan, file], { timeoutMs: 600_000 });
});

fixture("scan-large", "Large scan (60 pages, about 15 MB)", ["scan", "image-jpeg", "large"], async (file) => {
  const pages = Array.from({ length: 60 }, (_, i) => path.join(IMAGES, `big-${String(i + 1).padStart(2, "0")}.jpg`));
  await must("img2pdf", [...pages, "-o", file, "--title", "Large scan"]);
});

fixture("large-300-pages", "300 pages of text and photos", ["large", "image-jpeg"], async () => {
  const doc = await newDoc("300 pages");
  const photo = await doc.embedJpg(fs.readFileSync(path.join(IMAGES, "photo.jpg")));
  for (let n = 1; n <= 300; n++) {
    const page = doc.addPage(A4);
    const font = await label(doc, page, `Page ${n} of 300`, ["Many pages share one font and one photo: converters should not copy them per page."]);
    for (let line = 0; line < 30; line++) page.drawText(`Line ${line + 1}: the quick brown fox jumps over the lazy dog, page ${n}.`, { x: 50, y: 700 - line * 18, size: 10, font });
    if (n % 10 === 1) page.drawImage(photo, { x: 300, y: 60, width: 245, height: 184 });
  }
  return doc.save();
});

// ---------------------------------------------------------------------------------------------

// With file ids as arguments, only those are made again; otherwise the whole set.
async function main() {
  const only = process.argv.slice(2);
  if (!only.length) fs.rmSync(OUT, { recursive: true, force: true });
  fs.mkdirSync(OUT, { recursive: true });
  fs.mkdirSync(WORK, { recursive: true });
  if (!fs.existsSync(path.join(IMAGES, "srgb.icc"))) await must("python3", ["/bench/scripts/make-images.py", IMAGES], { timeoutMs: 600_000 });
  const previous = only.length ? readJson(path.join(OUT, "index.json"), { files: [] }).files : [];
  const index = new Map(previous.map((entry) => [entry.id, entry]));
  let made = 0;
  for (const item of fixtures) {
    if (only.length && !only.includes(item.id)) continue;
    const file = path.join(OUT, `${item.id}.pdf`);
    try {
      const bytes = await item.make(file);
      if (bytes) fs.writeFileSync(file, bytes);
      index.set(item.id, { id: item.id, title: item.title, features: item.features, levels: item.levels, expect: item.expect, file: path.basename(file) });
      made++;
      log("made", item.id, `${fs.statSync(file).size} bytes`);
    } catch (error) {
      log("FAILED", item.id, error.stack || error);
      process.exitCode = 1;
    }
  }
  const files = fixtures.map((item) => index.get(item.id)).filter(Boolean);
  writeJson(path.join(OUT, "index.json"), { set: "a", made: new Date().toISOString(), files });
  log(`set A: made ${made}; ${files.length} of ${fixtures.length} in the set`);
}

await main();
