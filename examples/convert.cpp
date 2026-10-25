// Makes a PDF PDF/A-2b, or says why it can't be:
//   convert <input.pdf> <output.pdf> <fonts folder>
#include <fynepdf/pdfa/pdfa.h>

#include <iostream>

int main(int argc, char **argv) {
  if (argc != 4) {
    std::cerr << "usage: convert <input.pdf> <output.pdf> <fonts folder>\n";
    return 64;
  }
  fynepdf::pdfa::PdfaOptions options;
  options.part = 2;         // PDF/A-2b; 1 and 3 work too
  options.fonts = argv[3];  // free stand-ins for fonts the PDF doesn't embed

  const fynepdf::pdfa::PdfaResult result = fynepdf::pdfa::ConvertToPdfa(argv[1], argv[2], options);
  if (result.ok && result.conforms) {
    std::cout << "PDF/A-2b, " << result.pages << " pages\n";
    return 0;
  }
  // Never labelled PDF/A when it isn't: the reason is in error (the file can't be read) or
  // problems (what PDF/A forbids and couldn't be fixed).
  std::cerr << "not PDF/A: " << (result.ok ? result.problems.front() : result.error) << "\n";
  return 1;
}
