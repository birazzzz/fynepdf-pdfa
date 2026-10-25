#pragma once

// Fonts changed in place so PDF/A's checks pass with nothing on the page moving: each fix is
// checked by looking every code the pages use up again in the changed font, and undone when
// any would find another glyph.

#include "font_check.h"

#include <map>
#include <memory>
#include <string>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>

namespace fynepdf::pdfa::font {

// A simple font without FirstChar, LastChar and Widths that fit gets them from its program:
// what readers use when they're missing.
bool FillWidths(QPDFObjectHandle font, const Checker &checker);

// A TrueType program's advance widths set to the font dictionary's for the glyphs the pages
// draw (readers place glyphs by the dictionary's; their outlines stay as they are). False
// when a glyph is drawn at two widths or the program has no metrics to change.
bool SetProgramWidths(QPDFObjectHandle font, const Checker &checker);

// A Type 3 font's glyph procedures given the widths its Widths array has for them (in their d0 or
// d1), as readers place its glyphs by the array. False when a glyph is drawn at two widths.
bool Type3Widths(QPDF &q, QPDFObjectHandle font, const Checker &checker);

// PDF/A-1: a symbolic TrueType program's cmap as exactly one table, (3,0), finding the glyph each
// code finds now (into a copy of the program).
bool OneCmap(QPDFObjectHandle font, const Checker &checker);

// A TrueType program without a cmap table (its glyphs found by their post table names) given
// one that finds the same glyphs: Unicode for a non-symbolic font, the codes for a symbolic one.
bool AddCmap(QPDFObjectHandle font, const Checker &checker);

// A TrueType font's symbolic flag and encoding as PDF/A wants them (PDF/A-2 6.2.11.6,
// PDF/A-1 6.3.7), when every code the pages use still finds the same glyph.
// A font without a ToUnicode map gets one from its old encoding's names, so its text stays.
bool TrueTypeEncoding(QPDF &q, QPDFObjectHandle font, const Checker &checker);

// PDF/A-1: a subset's CharSet (Type 1) or CIDSet (CIDFont) written from its program.
bool GlyphLists(QPDF &q, QPDFObjectHandle font, const Checker &checker);

// A ToUnicode map's entries: code to its text (UTF-16BE bytes).
std::map<unsigned, std::string> ReadToUnicode(QPDFObjectHandle stream);

// A new checker for |font| that has seen what |old| saw.
std::unique_ptr<Checker> Recheck(QPDFObjectHandle font, const Checker &old);

} // namespace fynepdf::pdfa::font
