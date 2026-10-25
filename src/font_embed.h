#pragma once

// Fonts a PDF names without embedding them, embedded from free fonts with the same width for
// every character (fonts/SOURCES.md): the document's widths stay, so no line moves.

#include "font_check.h"

#include <functional>
#include <map>
#include <set>
#include <string>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>

namespace fynepdf::pdfa::font {

// The file of the substitute for a font a PDF names (its BaseFont; the descriptor's flags and
// angle when the name doesn't say the style), "" when none has its widths: Arimo for Arial and
// Helvetica, Tinos for Times New Roman and Times, Cousine for Courier New and Courier.
std::string SubstituteFile(const std::string &base_font, QPDFObjectHandle descriptor);

// The font to embed for one a PDF names without embedding it: its twin (|twin| true) when there
// is one; otherwise, from the close fonts in its style (sans, serif, monospaced or symbols; bold,
// italic), the one whose widths come closest to the document's for the codes the pages use,
// with a glyph for every drawn one. "" when none has. |read| gives a font file's program (null
// when it isn't there).
// |lacking|, when no font fits: a drawn character the last one tried lacks.
std::string ChooseSubstitute(QPDFObjectHandle font, const Checker &checker, const std::function<const Program *(const std::string &)> &read, bool &twin,
                             unsigned *lacking = nullptr);

// A simple font that isn't embedded made into a TrueType font embedding |program| (the
// substitute's file) cut down to the glyphs the pages use; its codes keep their glyph names
// (WinAnsi with differences), its widths stay. With |plain| (PDF/A-1, which allows no
// differences in such a font) a font that needs differences is made symbolic instead: its own
// cmap maps the codes, a ToUnicode map keeps their text. False, with |why|, when a drawn code
// has no glyph in the substitute; nothing is changed then.
// With |shared|, a font that needs no codes of its own (not symbolic) uses the subset made once
// for all the fonts the same substitute stands in for.
struct SharedProgram {
  std::set<unsigned> characters;                         // every character those fonts need
  std::set<unsigned> codes;                              // and every code (one encoding for all)
  std::set<unsigned> covered;                            // the ones the subset made has
  QPDFObjectHandle file = QPDFObjectHandle::newNull();   // that subset, once made
};
bool Embed(QPDF &q, QPDFObjectHandle font, const std::string &program, const Checker &checker, bool plain, std::string &why, SharedProgram *shared = nullptr);

// A simple font that isn't embedded and needn't be (its text isn't drawn) given the FirstChar,
// LastChar, Widths and descriptor its dictionary lacks, from |program|: the widths of the free
// font a reader would place it by. False when it has them all.
bool DescribeFont(QPDF &q, QPDFObjectHandle font, const std::string &program, const Checker &checker);

// A ToUnicode map's characters by code: those that map to one (ligatures, fi to ffl, to theirs).
std::map<unsigned, unsigned> Characters(QPDFObjectHandle to_unicode);

// ChooseSubstitute for a Type 0 font whose CIDFont isn't embedded: each CID stands for the
// character |characters| (its ToUnicode map's) gives it, and its width is the W array's.
std::string ChooseCompositeSubstitute(QPDFObjectHandle font, const Checker &checker, const std::map<unsigned, unsigned> &characters,
                                      const std::function<const Program *(const std::string &)> &read, bool &twin, unsigned *lacking = nullptr);

// A Type 0 font with Identity-H or a predefined horizontal CMap whose CIDFont isn't embedded made into one embedding
// |program| as a CIDFontType2 font: each CID the pages use draws the substitute's glyph for its
// character (by a CIDToGIDMap), its widths (W, DW) and text (ToUnicode) stay. False, with
// |why|, when a drawn CID has no character or no glyph; nothing is changed then.
bool EmbedComposite(QPDF &q, QPDFObjectHandle font, const std::string &program, const Checker &checker, const std::map<unsigned, unsigned> &characters,
                    std::string &why);

} // namespace fynepdf::pdfa::font
