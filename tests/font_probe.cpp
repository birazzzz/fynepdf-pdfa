// font_probe: what the library reads of one font in a PDF, for working on the font checks.
//
//   font_probe <file.pdf> <part of a BaseFont name> [codes...]
//
// Prints the program's kind and glyph count and, for each code given (or 0-255 for simple
// fonts), the glyph it finds, its name and width against the dictionary's.

#include "font_check.h"
#include "font_program.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>

using namespace fynepdf::pdfa;

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: font_probe <file.pdf> <font name part> [codes...]\n");
    return 64;
  }
  QPDF q;
  bool found = false;
  q.setSuppressWarnings(true);
  q.processFile(argv[1]);
  for (QPDFObjectHandle object : q.getAllObjects()) {
    if (!object.isDictionary() || !object.getKey("/Type").isName() || object.getKey("/Type").getName() != "/Font")
      continue;
    QPDFObjectHandle base = object.getKey("/BaseFont");
    if (!base.isName() || base.getName().find(argv[2]) == std::string::npos)
      continue;
    font::Checker checker(object);
    std::printf("%d %s %s ready=%d why=%s\n", object.getObjectID(), object.getKey("/Subtype").unparse().c_str(), base.getName().c_str(), checker.Ready(),
                checker.Why().c_str());
    const font::Program *program = checker.Font();
    if (program)
      std::printf("  glyphs=%d cid=%d sfnt=%d own-encoding=%d %s\n", program->Glyphs(), program->CidKeyed(), program->Sfnt(), program->OwnEncoding(),
                  program->Describe().c_str());
    if (!checker.Ready())
      continue;
    const auto probe = [&](unsigned code) {
      const int glyph = checker.GlyphFor(code);
      std::printf("  code %u: named '%s' encoded '%s' glyph %d '%s' width %.3f (dictionary %.3f)\n", code, checker.NameFor(code).c_str(),
                  program->Encoded(code).c_str(), glyph, program->GlyphName(glyph).c_str(), program->Width(glyph), checker.DictionaryWidth(code));
    };
    if (argc > 3)
      for (int i = 3; i < argc; ++i)
        probe(static_cast<unsigned>(std::strtoul(argv[i], nullptr, 0)));
    else
      for (unsigned code = 0; code < 256; ++code)
        if (checker.GlyphFor(code) > 0)
          probe(code);
    found = true;
  }
  if (!found)
    std::fprintf(stderr, "no such font\n");
  return found ? 0 : 1;
}
