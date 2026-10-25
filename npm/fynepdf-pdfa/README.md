# fynepdf-pdfa

**Turn PDFs into valid PDF/A by fixing them in place, not rebuilding them.** The
[fynepdf-pdfa](https://github.com/birazzzz/fynepdf-pdfa) command line and a Node API, with no
Ghostscript. Made by [FynePDF](https://fynepdf.com) for its web and desktop apps.

- **Valid:** 99.3% of 2,053 test files come out valid PDF/A-2b, checked by veraPDF (Ghostscript:
  93.5%, LibreOffice: 96.0%).
- **Honest:** a file it can't make PDF/A is never labelled PDF/A, and you're told why.
- **Keeps the document:** text, bookmarks, links, form fields, tags and layers stay as they were.
- **Fast:** a typical file takes 0.01 s; 2,053 files took 96 s, against 740 s for Ghostscript.

## Install

```sh
npm install fynepdf-pdfa
```

It brings the command line for your platform (Linux x64 and arm64 with glibc 2.28+, Windows x64,
macOS 11+ on Apple silicon and Intel) with its fonts and PDFium. Nothing is downloaded at install
time and no install script runs.

## Command line

```sh
npx fynepdf-pdfa input.pdf output.pdf             # PDF/A-2b
npx fynepdf-pdfa input.pdf output.pdf --part 1    # PDF/A-1b (or --part 3)
```

It prints one line of JSON:

```json
{"ok":true,"conforms":true,"pages":12,"signatures":0,"problems":[],"changes":{"cmyk-colours":31,"javascript":1}}
```

## Node

```js
const { toPdfa } = require("fynepdf-pdfa"); // or: import { toPdfa } from "fynepdf-pdfa";

const result = await toPdfa("input.pdf", "output.pdf", { part: 2 });
if (result.conforms) {
  // output.pdf is PDF/A-2b
} else {
  // output.pdf is written without the PDF/A label; result.problems says why,
  // for example "font-not-embedded: KozMinPr6N-Regular"
}
```

`toPdfa` rejects when the file can't be read (`error.code` `"unreadable"`), needs a password
(`"password"`) or can't be written (`"write"`). Options: `part` (1, 2 or 3), `fonts` (another
fonts folder), `flattenDpi` (PDF/A-1b pages with transparency, default 300), `signal`. Types
are included.

On a platform without a package, build the command line from the repository and set
`FYNEPDF_PDFA_BINARY` to it.

## More

How it works, the full benchmark against Ghostscript, LibreOffice and OCRmyPDF, and the C++
library: [github.com/birazzzz/fynepdf-pdfa](https://github.com/birazzzz/fynepdf-pdfa).

MIT, © 2026 FynePDF. The libraries inside each platform package keep their own licences
(`licenses/`), as do the fonts (`fonts/`).
