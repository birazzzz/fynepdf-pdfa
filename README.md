# fynepdf-pdfa

Makes a PDF a valid PDF/A (1b, 2b or 3b) by fixing it in place: only what PDF/A forbids is
changed, missing fonts get free stand-ins with the same widths, and a file that can't be made
PDF/A is never labelled PDF/A. A C++17 library on [qpdf](https://github.com/qpdf/qpdf), with a
command line, and no Ghostscript. Made by [FynePDF](https://fynepdf.com).

> Work in progress: the full README (how it works, benchmarks against Ghostscript and others,
> install) is being written.

```sh
fynepdf-pdfa input.pdf output.pdf            # PDF/A-2b
fynepdf-pdfa input.pdf output.pdf --part 1   # PDF/A-1b
```

```cpp
#include <fynepdf/pdfa/pdfa.h>

fynepdf::pdfa::PdfaOptions options;
options.fonts = "fonts";  // this repository's fonts/ folder
fynepdf::pdfa::PdfaResult result = fynepdf::pdfa::ConvertToPdfa("in.pdf", "out.pdf", options);
// result.conforms: written as PDF/A; otherwise result.problems say why
```

Build and test: `cmake --preset default && cmake --build --preset default && ctest --preset default`
(qpdf, libjpeg-turbo, OpenJPEG and Little CMS from the system), or `--preset vcpkg`.

MIT licence; the fonts keep their own (`THIRD-PARTY-NOTICES.md`). Sister project:
[fynepdf-grayscale](https://github.com/birazzzz/fynepdf-grayscale).
