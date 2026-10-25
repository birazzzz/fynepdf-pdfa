// Converts every PDF in tests/fixtures and checks the results:
//
//   fynepdf_pdfa_smoke <1|2|3> <fixtures> <fonts> <out> <pdfium: 0|1>
//
// Every file comes out labelled PDF/A, except the password-protected one (refused, with
// "password") and, without PDFium, PDF/A-1b files whose pages use transparency or that have a
// highlight without an appearance (a highlight is drawn with a multiply blend, which PDF/A-1
// allows only on a page PDFium draws): those are written without the label, with "transparency"
// or "no-appearance: Highlight" among the problems. A file written without the label is
// deleted, as an app would, so <out> holds only files that say they're PDF/A: run veraPDF over
// it (tests/verapdf.cmake) to check that every one is.

#include "fynepdf/pdfa/pdfa.h"

#include <qpdf/QPDF.hh>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace fynepdf::pdfa;

namespace {

const std::string kPassword = "a-encrypted-user-password.pdf";

int Pages(const fs::path &file) {
  try {
    QPDF pdf;
    pdf.processFile(file.string().c_str());
    return static_cast<int>(pdf.getAllPages().size());
  } catch (...) {
    return -1;
  }
}

std::vector<fs::path> Fixtures(const fs::path &dir) {
  std::vector<fs::path> files;
  for (const auto &entry : fs::directory_iterator(dir))
    if (entry.path().extension() == ".pdf")
      files.push_back(entry.path());
  std::sort(files.begin(), files.end());
  return files;
}

bool Has(const std::vector<std::string> &problems, const std::string &code) {
  return std::any_of(problems.begin(), problems.end(), [&](const std::string &p) { return p.rfind(code, 0) == 0; });
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 6)
    return 64;
  int failures = 0, converted = 0, total = 0;
  const auto fail = [&](const fs::path &file, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", file.filename().string().c_str(), why.c_str());
    ++failures;
  };
  PdfaOptions options;
  options.part = std::atoi(argv[1]);
  options.fonts = argv[3];
  const fs::path out = argv[4];
  const bool pdfium = std::string(argv[5]) == "1";
  fs::create_directories(out);
  for (const fs::path &input : Fixtures(argv[2])) {
    ++total;
    const fs::path output = out / input.filename();
    const PdfaResult result = ConvertToPdfa(input.string(), output.string(), options);
    if (input.filename() == kPassword) {
      if (result.ok || result.error != "password")
        fail(input, "expected a password refusal, got " + ToJson(result));
    } else if (!result.ok) {
      fail(input, "not converted: " + ToJson(result));
    } else if (!result.conforms) {
      if (pdfium || options.part != 1 || !(Has(result.problems, "transparency") || Has(result.problems, "no-appearance: Highlight")))
        fail(input, "not PDF/A: " + ToJson(result));
    } else if (Pages(output) != Pages(input)) {
      fail(input, "pages changed or the result doesn't open");
    } else {
      ++converted;
    }
    if (!result.conforms)
      fs::remove(output);
  }
  std::printf("PDF/A-%db: %d of %d labelled PDF/A, %d failures\n", options.part, converted, total, failures);
  return failures == 0 ? 0 : 1;
}
