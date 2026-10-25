# Benchmarks

fynepdf-pdfa against the open-source ways of making PDF/A, on the same 2,053 files, every result
checked by [veraPDF](https://verapdf.org/), the PDF Association's reference validator.

**In short, at PDF/A-2b:** 99.3% of the files come out valid PDF/A (the next best, LibreOffice,
96.0%; Ghostscript 10.08, 93.5%). No file is labelled PDF/A that isn't (the others: 62 to 192).
Text, bookmarks, links, form fields and tags are kept. And it takes about 8 to 43 times less
time in total, with at most 215 MB of memory where the others reach 1.4 to 5.6 GB.

## What was tested

| Set | Files | What | Shared |
| --- | --- | --- | --- |
| A. Feature set | 56 | Small PDFs made for the bench, one PDF/A problem each: fonts not embedded, CMYK, transparency, JavaScript, layers, attachments, a damaged cross-reference... (`bench/scripts/build-set-a.mjs`, also this project's `tests/fixtures`) | yes |
| B. Rule set | 1,771 | The [veraPDF corpus](https://github.com/veraPDF/veraPDF-corpus) (PDF/A-1b, 2b, 3b and Isartor), each file converted to its own level | yes (CC BY 4.0) |
| C. Real world | 1,000 | The first 1,000 PDFs of [Digital Corpora's SafeDocs crawl](https://digitalcorpora.org/cc-main-2021-31-pdf-untruncated/) (zip 0000): whatever the web had in 2021 | results and file numbers only |
| E. Office and print | 11 | The same documents printed by Chrome, LibreOffice 25.2 and LaTeX | yes |

Each file is converted to the PDF/A level its set calls for: everything to PDF/A-2b; sets A, E
and the corpus's 1b files to PDF/A-1b; sets A, E and the corpus's 3b files to PDF/A-3b. A fifth
set of 93 private files was tested too and is left out of every number here; its results agree.

## The tools

| Tool | Version | Command |
| --- | --- | --- |
| **fynepdf-pdfa** | 0.1.0 | `fynepdf-pdfa in.pdf out.pdf --part N --fonts fonts` (PDFium loaded for PDF/A-1b pages that blend) |
| Ghostscript | 10.08.0 (latest) and 10.00.0 (Debian bookworm) | `gs -dPDFA=N -dBATCH -dNOPAUSE -sColorConversionStrategy=RGB -sDEVICE=pdfwrite -dPDFACompatibilityPolicy=1 -sOutputFile=out.pdf PDFA_def.ps in.pdf`, the prefix declaring Ghostscript's sRGB profile as output intent, as its documentation shows |
| OCRmyPDF | 17.13.0, on Ghostscript 10.08 | `ocrmypdf --skip-text --tesseract-timeout 0 --jobs 1 --output-type pdfa-N in.pdf out.pdf` (no OCR) |
| LibreOffice | 25.2.3 | `soffice --headless --infilter=draw_pdf_import --convert-to 'pdf:draw_pdf_Export:{"SelectPdfVersion":…}'`: the PDF opened in Draw and exported, as Gotenberg does |

## What was measured

- **Valid PDF/A:** veraPDF 1.31 passes the file at the level asked for. A tool that writes a
  blank one-page PDF/A for a file it couldn't read gets no credit for it ("blank stand-ins").
- **False claim:** the file says it is PDF/A and veraPDF fails it. The worst result: an archive,
  a court or a portal accepts the label and the file isn't what it says.
- **Text lost:** [pdf.js](https://mozilla.github.io/pdf.js/) finds less than 90% of the input's
  text in the result (of the files that have text).
- **Pages look different:** Poppler draws the first 10 pages of input and result at 30 dpi and
  they differ beyond antialiasing (moved or missing content, wrong colours, different glyphs).
- **Lost:** bookmarks, links, form fields, tags, comments, attachments, signatures or pages the
  input had and the result doesn't.
- **Size, time, memory:** the result's size over the input's (median); each conversion's wall
  time and peak memory from GNU time, 4 conversions at once in a Docker container with 8 CPUs
  and 8 GB on an x86-64 desktop.

## Results

### PDF/A-2b, sets A, B, C and E

| Tool | Valid PDF/A | False claims | Text lost | Pages look different | Files that lost something | Median size | Time, summed | Slowest file | Most memory |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **fynepdf-pdfa** | 2,038 of 2,053 (99.3%) | 0 | 1 of 1,037 | 49 | 22 | 1.21× | 96 s | 4.5 s | 215 MB |
| Ghostscript 10.08 | 1,919 of 2,053 (93.5%), +3 blank stand-ins | 100 | 38 of 1,036 | 186 | 550 | 1.03× | 740 s | 116 s | 1376 MB |
| Ghostscript 10.00 | 1,900 of 2,053 (92.5%), +3 blank stand-ins | 137 | 83 of 1,036 | 186 | 1,447 | 1.03× | 1,374 s | 230 s | 1379 MB |
| OCRmyPDF 17.13 | 1,737 of 2,053 (84.6%) | 192 | 11 of 941 | 109 | 341 | 1.22× | 1,806 s | 180 s | 5176 MB |
| LibreOffice 25.2 | 1,970 of 2,053 (96.0%) | 62 | 90 of 1,023 | 965 | 1,359 | 2.72× | 4,113 s | 293 s | 5613 MB |

### PDF/A-2b by set

| Set | **fynepdf-pdfa** | Ghostscript 10.08 | Ghostscript 10.00 | OCRmyPDF 17.13 | LibreOffice 25.2 |
| --- | --- | --- | --- | --- | --- |
| A. Feature set | 55 / 56, 0 false | 49 / 56, 6 false | 49 / 56, 6 false | 40 / 56, 10 false | 54 / 56, 0 false |
| B. Rule set (veraPDF corpus) | 979 / 986, 0 false | 931 / 986, 50 false | 934 / 986, 49 false | 893 / 986, 69 false | 982 / 986, 2 false |
| C. Real world (1,000 web PDFs) | 993 / 1,000, 0 false | 928 / 1,000, 44 false | 906 / 1,000, 82 false | 793 / 1,000, 113 false | 923 / 1,000, 60 false |
| E. Office and print | 11 / 11, 0 false | 11 / 11, 0 false | 11 / 11, 0 false | 11 / 11, 0 false | 11 / 11, 0 false |

### PDF/A-2b: files that lost each kind of thing

| Tool | bookmarks | links | fields | tags | comments | attachments | signatures | pages |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **fynepdf-pdfa** | 0 | 0 | 0 | 0 | 16 | 6 | 0 | 0 |
| Ghostscript 10.08 | 3 | 134 | 38 | 378 | 41 | 5 | 12 | 0 |
| Ghostscript 10.00 | 978 | 134 | 38 | 378 | 41 | 6 | 12 | 1 |
| OCRmyPDF 17.13 | 3 | 0 | 25 | 299 | 16 | 4 | 0 | 0 |
| LibreOffice 25.2 | 1,031 | 335 | 38 | 0 | 84 | 6 | 12 | 0 |

fynepdf-pdfa's losses are what PDF/A itself forbids: comments that can't have a valid appearance
(sound, movie and 3D comments, hidden ones) and attachments that aren't PDF/A (PDF/A-2 allows
only those; PDF/A-3 keeps them all).

### PDF/A-1b, sets A, B and E

| Tool | Valid PDF/A | False claims | Text lost | Pages look different | Files that lost something | Median size | Time, summed | Slowest file | Most memory |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **fynepdf-pdfa** | 818 of 840 (97.4%) | 0 | 0 of 184 | 9 | 21 | 2.16× | 7.6 s | 0.6 s | 88 MB |
| Ghostscript 10.08 | 795 of 840 (94.6%), +1 blank stand-ins | 43 | 22 of 184 | 13 | 48 | 1.81× | 56 s | 10 s | 292 MB |
| Ghostscript 10.00 | 795 of 840 (94.6%), +1 blank stand-ins | 43 | 23 of 184 | 15 | 651 | 1.41× | 83 s | 22 s | 404 MB |
| OCRmyPDF 17.13 | 761 of 840 (90.6%) | 57 | 21 of 175 | 11 | 43 | 1.63× | 393 s | 5.7 s | 258 MB |
| LibreOffice 25.2 | 0 of 840 (0.0%) | 836 | 10 of 182 | 36 | 788 | 4.45× | 173 s | 13 s | 436 MB |

PDF/A-1 has no transparency: fynepdf-pdfa draws the pages that blend as pictures (with PDFium,
text kept over them, invisible and searchable), which is why its files grow more here.
LibreOffice's PDF/A-1 export is labelled PDF/A-1 but fails veraPDF on nearly every file.

### PDF/A-3b, sets A, B and E

| Tool | Valid PDF/A | False claims | Text lost | Pages look different | Files that lost something | Median size | Time, summed | Slowest file | Most memory |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **fynepdf-pdfa** | 78 of 79 (98.7%) | 0 | 0 of 65 | 0 | 1 | 1.46× | 0.7 s | 0.1 s | 35 MB |
| Ghostscript 10.08 | 60 of 79 (75.9%), +1 blank stand-ins | 18 | 5 of 65 | 1 | 17 | 1.19× | 4.8 s | 1.3 s | 43 MB |
| Ghostscript 10.00 | 71 of 79 (89.9%), +1 blank stand-ins | 7 | 6 of 65 | 1 | 30 | 1.10× | 27 s | 22 s | 56 MB |
| OCRmyPDF 17.13 | 54 of 79 (68.4%) | 19 | 4 of 60 | 1 | 12 | 1.16× | 40 s | 5.8 s | 177 MB |
| LibreOffice 25.2 | 77 of 79 (97.5%) | 0 | 8 of 64 | 22 | 30 | 2.16× | 24 s | 4.5 s | 406 MB |

## Real files

Every number above comes from files like these.

- **Labelled PDF/A, isn't (real world, c-0000130, made by Microsoft Word 2016).** The file has
  layers. Ghostscript 10.08 keeps them but not the settings PDF/A requires for them (rule 6.9:
  every layer configuration needs a name, no `/AS`, a complete `/Order`), and writes the PDF/A
  label anyway. Most of Ghostscript's false claims on real files are this. fynepdf-pdfa fixes the
  configurations; veraPDF passes the file.
- **Valid PDF/A, text gone (c-0000203, made by cairo).** veraPDF passes Ghostscript's result, but
  pdf.js finds 0.9% of the text in it: the archived file looks right and can't be searched or
  copied from. fynepdf-pdfa keeps all of it. Same story for c-0000316
  (Word through Acrobat PDFMaker: 7% of the text left) and c-0000319 (55%).
- **Bookmarks and links (c-0000364).** The input has 586 bookmarks and 509 links. Ghostscript
  10.08 keeps 504 bookmarks and no links, and its result still fails veraPDF. fynepdf-pdfa keeps
  all 586 and all 509, and passes.
- **Refused, honestly (c-0000849).** The file names a Japanese font, KozMinPr6N, without
  embedding it or saying which characters its codes are. No free font can stand in without
  guessing, so fynepdf-pdfa writes no label and reports `font-not-embedded: KozMinPr6N-Regular`.
  Ghostscript labels the file PDF/A-2b; veraPDF fails it.

## Where fynepdf-pdfa doesn't make it

Of the 2,053 files at PDF/A-2b, 15 aren't valid PDF/A, each written without the label and with
the reason:

- **Real world (7):** two Chinese files using characters Droid Sans Fallback doesn't have
  (U+3010, U+6500), the Japanese font above, a password-protected file, a download cut off half
  way, and two HTML pages saved with a `.pdf` name.
- **veraPDF corpus (7):** tests of real limits (character IDs and numbers past what PDF allows,
  strings over 32 KB), test fonts that can't be read, an XFA-only form, and CMap names that
  aren't standard.
- **Set A (1):** the password-protected file.

**Text lost, once:** `b-2b-verapdf-test-suite-6-2-11-4-1-t02-fail-c` from the veraPDF corpus is
built to fail: its text points at a glyph the font doesn't have. fynepdf-pdfa takes those codes
out (PDF/A forbids them), so about a sixth of the characters pdf.js read before aren't there
after.

## Run it yourself

Everything runs in one Docker image with pinned versions (`bench/Dockerfile`):

```sh
cd bench
docker build -t fynepdf-pdfa-bench:0 .
../scripts/build-linux.sh                                 # our command line, into build/linux
./bench.sh node scripts/build-set-a.mjs                   # set A
node host/make-chrome-pdfs.mjs && ./bench.sh node scripts/build-set-e.mjs   # set E
./bench.sh node scripts/import-sets.mjs b c               # the corpus and the SafeDocs files (bench/README.md)
BENCH_WORKERS=4 ./bench.sh node scripts/run.mjs --run mine --sets a,b,c,e
./bench.sh node scripts/report.mjs --run mine             # report.html with every file
./bench.sh node scripts/docs-tables.mjs --run mine        # the tables on this page
```

For Grayscale too, check out [fynepdf-grayscale](https://github.com/birazzzz/fynepdf-grayscale)
next to this repository and build it with its own `scripts/build-linux.sh`. A full run takes a
few hours, nearly all of it in the other tools and in veraPDF. `bench/README.md` has the details.
