# How fynepdf-pdfa works

PDF/A is ordinary PDF with rules taken away: no JavaScript, no encryption, no transparency in
PDF/A-1, every font embedded, colours tied to a profile, metadata that agrees with itself. Most
of a PDF already follows them. fynepdf-pdfa finds what doesn't, changes that, and leaves the rest
of the file as it was.

## The idea: fix in place, don't rebuild

The usual open-source way to make PDF/A is Ghostscript's: read the PDF into its PostScript
interpreter and write a new PDF from what it draws. Every object is rebuilt, so things the writer
doesn't carry over are lost along the way: bookmarks, links, form fields, tags, sometimes the
text itself (it comes out as shapes, or with characters that no longer map to letters).

fynepdf-pdfa opens the file with [qpdf](https://github.com/qpdf/qpdf), which reads and writes
PDF objects without interpreting them, and works object by object:

- what PDF/A allows stays byte for byte: text, pictures, bookmarks, links, forms, tags, layers;
- what PDF/A forbids is removed or rewritten: an action, a filter, a colour, a font table;
- what PDF/A requires is added: the output intent, the XMP metadata, missing appearances.

That is also why it is fast and light: nothing is drawn, so a typical file takes a few
milliseconds and a few megabytes ([benchmarks](benchmarks.md)).

## The rule: never claim what the file isn't

A file is labelled PDF/A (the `pdfaid` entries in its XMP metadata) only when every check passed.
When something can't be fixed, the file is still written, as close to PDF/A as it could be made,
but without the label, and `PdfaResult::problems` says why:

```json
{"ok":true,"conforms":false,"pages":3,"problems":["font-not-embedded: KozMinPr6N-Regular"],"changes":{"javascript":2}}
```

On the bench, files labelled PDF/A that fail veraPDF ("false claims") number 0 for fynepdf-pdfa
and 100 to 192 for the other tools at PDF/A-2b ([benchmarks](benchmarks.md)). An app built on it
can be honest with its users: say why, rather than hand over a file that isn't what it says.

## The passes, in order

| Pass | What it does | Examples of `changes` |
| --- | --- | --- |
| Open and repair | Opens the file with qpdf; a damaged file (broken cross-reference, cut-off end) gets its page tree rebuilt from the pages that are there. Encryption is removed (PDF/A forbids it) when no user password is needed. | `damaged-file-repaired`, `encryption` |
| Catalog | The document's own settings: forbidden entries (`/AA`, `/OpenAction` with JavaScript, `/Perms`, `/Requirements`), names that aren't ASCII, page labels. | `actions-removed`, `names` |
| Colours | With an sRGB output intent, device CMYK can't be used: CMYK colours, pictures and gradients become RGB. ICC profiles PDF/A can't use (version 4 in PDF/A-1, wrong class or components) are replaced. | `cmyk-colours`, `cmyk-images`, `icc-profiles-replaced` |
| Forms | Fields get appearances (PDF/A forbids `NeedAppearances`), XFA is removed when the AcroForm can stand alone, field actions go. | `appearances-made`, `xfa` |
| Pages | Page boxes inside the media box and within PDF/A's size limits (larger pages are drawn in other units, same size on paper), comments without appearances get one or go, flags fixed, hidden comments and forbidden kinds (sound, movie, 3D) removed. | `page-boxes`, `annotation-flags`, `annotations-removed` |
| Layers | Optional content configurations made valid (`/Name`, no `/AS`, a complete `/Order`). In PDF/A-1, which has no layers, hidden content is removed. | `layer-names`, `layer-order`, `hidden-content-removed` |
| Attachments | PDF/A-2 keeps attachments that are PDF/A themselves; PDF/A-3 keeps any, with a MIME type and an `/AFRelationship`. | `attachments-removed`, `attachment-types` |
| Objects | Every object the file will hold: LZW to Flate, `Crypt` and external streams, transfer functions and halftones, PostScript, rendering intents, JPEG 2000 pictures PDF/A-1 can't hold (decoded and re-encoded as JPEG), 16-bit pictures, `/Interpolate`, numbers past PDF's limits. | `lzw`, `transfer-functions`, `jpeg2000-decoded`, `interpolation` |
| Contents | Page and form content read operator by operator: operators PDF doesn't know, numbers written wrong, inline pictures PDF/A forbids or that readers end in different places. | `unknown-operators`, `inline-pictures-hex`, `numbers-clamped` |
| Fonts | The heart of it (below): every font the pages use is read, checked against what the pages show, fixed in place, or replaced by a free font with the same widths. | `font-widths-written`, `font-encodings`, `cmaps-embedded` |
| Text | Widths a font program can't take become the program's, with the difference added back after the glyph, so every glyph stays where it was. Codes that draw an empty `.notdef` are taken out, their advance kept. | `text-widths-kept`, `notdef-boxes-removed` |
| Transparency (PDF/A-1 only) | Pages that blend (soft masks, opacity, blend modes) are drawn as pictures with PDFium, at 300 dpi; the page's text is kept over the picture, invisible, so it can still be found and copied. | `pages-flattened`, `transparency-groups` |
| Output intent and metadata | An sRGB output intent (the profile is built in code from the IEC 61966-2-1 parameters), XMP metadata made to agree with the Info dictionary, the `pdfaid` part and conformance written last, only if nothing above left a problem. | `output-intents-replaced`, `info` |

The file is then written by qpdf: a cross-reference table rather than streams for PDF/A-1, a
document ID, nothing the catalog doesn't reach.

## Fonts

Fonts cause most PDF/A failures in real files, because PDF/A asks for more than readers need: the
font program embedded, a glyph for every code the page uses, widths in the font that agree with
the PDF's, an encoding that maps to Unicode, CMaps embedded or predefined.

- **Read.** TrueType, OpenType (CFF), Type 1 and CID fonts are parsed: their glyph names,
  `cmap` tables, widths and glyph outlines (enough to tell an empty glyph from a real one).
  Programs that several fonts share are parsed once.
- **Check.** Every code the pages show, with the font, size and text mode it's shown in, is
  matched to a glyph. Missing glyphs, `.notdef`, widths that disagree and encodings PDF/A
  forbids are found per code, not per font.
- **Fix in place.** Widths are written from the program, a `cmap` table is added or reduced to
  the one PDF/A-1 allows, `Differences` arrays get names that map to Unicode, predefined CMaps
  get their supplement right, and a `ToUnicode` map keeps the text copyable when an encoding has
  to change.
- **Replace.** A font that isn't embedded gets a free substitute, cut down to the glyphs the
  document uses and given the document's own widths, so no line moves:

  | The PDF names | It gets | Same widths |
  | --- | --- | --- |
  | Arial, Helvetica | Arimo | yes |
  | Times New Roman, Times | Tinos | yes |
  | Courier New, Courier | Cousine | yes |
  | Calibri | Carlito | yes |
  | Cambria | Caladea | yes |
  | Symbol, ZapfDingbats characters | DejaVu Sans | close |
  | anything else | Noto Sans or Noto Serif, by style | close |
  | Chinese, Japanese, Korean | Droid Sans Fallback with Adobe's CMaps | close |

- **Report.** A font no free font covers (a CJK font using characters Droid Sans Fallback
  lacks) is a problem: `font-not-embedded: <name> (no free font has U+3010)`.

## Colour

PDF/A wants every colour tied to the output intent. fynepdf-pdfa declares sRGB, so device RGB
and grey stand as they are, and device CMYK becomes RGB: colour operators by formula, while
CMYK pictures and gradients keep their data byte for byte under a colour space of the four
process inks with an RGB alternate, so nothing is re-compressed and separations still work. ICC-based colours keep their profiles when PDF/A accepts them (version 4 sRGB
profiles are swapped for the version 2 one PDF/A-1 can read); with Little CMS, colours that must
leave their profile are converted through it. Spot inks get an alternate PDF/A can draw.

## What it doesn't do

- **Only the "b" conformance levels** (1b, 2b, 3b): the file looks right and its text is
  copyable. The "a" levels also need tags describing the document's structure, and the "u"
  levels need Unicode for every character, which this doesn't add. PDF/A-4 isn't written yet.
- **No OCR.** A scan becomes a valid PDF/A of pictures; run OCR first if you need its text.
- **A user password** (needed to open the file) isn't asked for: such files are refused.
- **XFA-only forms**, fonts no free font covers, and content past PDF's own limits are reported
  as problems rather than guessed at.

## Memory and speed

- Content streams are read operator by operator, never as one parsed tree; new stream data over
  32 MB is kept compressed.
- Font programs shared by several fonts are parsed once; tables are read when first needed; the
  CJK fallback font is opened only when nothing else fits.
- PDFium is loaded only when a PDF/A-1b page has to be flattened, so other runs don't pay for it.

On the 2,053 public bench files at PDF/A-2b the conversions took 96 s in total, the slowest
4.5 s, and no file used more than 215 MB ([benchmarks](benchmarks.md)).

## How it's checked

- `ctest` converts the 64 files in `tests/fixtures` (one PDF/A problem each, plus exports from
  Chrome, LibreOffice and LaTeX) at 1b, 2b and 3b; with `-DFYNEPDF_PDFA_VERAPDF=docker` veraPDF
  then checks every file labelled PDF/A.
- The [bench](../bench) runs the same files and thousands more (the veraPDF corpus and 1,000 PDFs
  from the web) through fynepdf-pdfa and the other tools, and checks each result with veraPDF.
