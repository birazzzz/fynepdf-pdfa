# PDF/A converter bench

Converts the same PDFs with every tool, then checks each result:

- **Valid:** [veraPDF](https://verapdf.org/) at the PDF/A level asked for (1b, 2b, 3b).
- **Honest:** a file that says it's PDF/A but fails veraPDF is a *false claim*, the worst outcome.
- **Looks the same:** Poppler draws the first 10 pages of input and output, which are compared pixel by pixel.
- **Text kept:** pdf.js extracts the text before and after.
- **Kept:** bookmarks, links, form fields, signatures, comments, attachments, tags, pages.
- **Cost:** output size, time, peak memory.

Grayscale is measured the same way (pages compared with the input drawn in grey).

Everything runs in one Docker image with fixed versions, so anyone can re-run it. Untrusted PDFs are only opened inside the container.

## File sets

| Set | What | Can be shared |
|---|---|---|
| A. Feature set | 56 small PDFs made by `scripts/build-set-a.mjs`, one PDF/A problem each | Yes (made here) |
| B. Rule set | [veraPDF corpus](https://github.com/veraPDF/veraPDF-corpus) PDF/A-1b, 2b, 3b and Isartor: 1,771 files | Yes, CC BY 4.0 |
| C. Real world | 1,000 PDFs from [Digital Corpora's SafeDocs crawl](https://digitalcorpora.org/cc-main-2021-31-pdf-untruncated/), zip 0000 | Results and file numbers only |
| D. Private | Your own files, mounted read-only (`BENCH_PRIVATE`) | No |
| E. Office and print | `set-e/`: the same documents exported by Chrome, LibreOffice 25.2 and LaTeX | Yes (made here) |

## Tools

| Name | What runs |
|---|---|
| `ghostscript` | Debian bookworm's Ghostscript 10.00.0, with the command FynePDF's servers used before fynepdf-pdfa |
| `ghostscript-latest` | Ghostscript 10.08.0 built from source, same command |
| `ocrmypdf` | OCRmyPDF 17.13.0, OCR off, on Ghostscript 10.08.0 (it refuses 10.0 to 10.02) |
| `libreoffice` | LibreOffice 25.2 opening the PDF in Draw and exporting PDF/A (as Gotenberg does) |
| `picture-copy` | Grayscale only: pages drawn at 180 dpi and saved as grey JPEGs, as many apps make Grayscale |
| `fynepdf-pdfa` | Ours, PDF/A: this repository's command line, built by `../scripts/build-linux.sh` and mounted by `bench.sh` |
| `fynepdf-grayscale` | Ours, Grayscale: the sister project [fynepdf-grayscale](https://github.com/birazzzz/fynepdf-grayscale), when it's checked out next to this repository and built (its `scripts/build-linux.sh`) |

Commands are in `scripts/tools.mjs`.

## Running it

From this folder, with Docker (Git Bash on Windows):

```sh
docker build -t fynepdf-pdfa-bench:0 .
./bench.sh node scripts/build-set-a.mjs              # set A
node host/make-chrome-pdfs.mjs                       # set E's Chrome prints (on the host)
./bench.sh node scripts/build-set-e.mjs              # set E
./bench.sh node scripts/import-sets.mjs b c          # sets B and C (see downloads/ below)
BENCH_PRIVATE="/path/to/pdfs" ./bench.sh node scripts/import-sets.mjs d
BENCH_WORKERS=4 ./bench.sh node scripts/run.mjs --run baseline --sets a,b,c,e
./bench.sh node scripts/report.mjs --run baseline    # reports/baseline/{public,internal}/report.html
```

`downloads/` (not committed) holds:

- `verapdf-corpus-src/corpus`: `git clone --depth 1 https://github.com/veraPDF/veraPDF-corpus.git`
- `safedocs-0000/0000.zip`: `https://digitalcorpora.s3.amazonaws.com/corpora/files/CC-MAIN-2021-31-PDF-UNTRUNCATED/zipfiles/0000-0999/0000.zip` (1.27 GB, SHA-256 `a6b28286550f6dcfdebec0454d57cb9797ca4c5792c85a09cb89b207f3f407fb`)

A run is resumable: start it again and finished steps are skipped. `--limit N` takes the first N files of each set, `--tools` and `--tasks` narrow it down.

## Limits

- Times are measured under load (several conversions at once in a container with 8 CPUs and 8 GB).
- Page comparison is at 30 dpi on the first 10 pages: it catches lost or moved content and wrong colours, not fine detail.
- The picture copy stands for Grayscale made by drawing pages as pictures; apps that write RGB JPEGs of grey pages get slightly bigger files.
