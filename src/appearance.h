#pragma once

#include <functional>
#include <string>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>

namespace fynepdf::pdfa {

// What drawing text in an appearance needs: a simple font with WinAnsi codes to show it with
// (null when there's none) and each code's width per 1,000 (below 0 when it has no glyph).
struct TextFont {
  QPDFObjectHandle font = QPDFObjectHandle::newNull();
  std::function<double(unsigned)> width;
};
// The font to draw text in a font named |base_font| (a BaseFont, or the resource's name when
// the form doesn't have it) with.
using FontFor = std::function<TextFont(const std::string &base_font)>;

// An appearance for a comment or form field that has none, drawn as readers draw them: shapes,
// lines, ink, text markup, notes, carets and attachments from their geometry and colours; form
// fields (text, choice, check boxes, radio buttons, push buttons) from their value and default
// appearance (|form| the AcroForm, for its defaults), free text from its contents, stamps from
// their name, in fonts from |font_for|. A check box's or radio button's is a dictionary of its
// states' appearances. Null for what can't be drawn (text in characters WinAnsi doesn't have,
// no font). Without |transparency| (PDF/A-1), highlights can't be drawn (they would hide the
// text) and opacity is left out.
QPDFObjectHandle MakeAppearance(QPDF &q, QPDFObjectHandle annotation, QPDFObjectHandle form, bool transparency, const FontFor &font_for);

// An appearance that draws nothing, for comments that are hidden.
QPDFObjectHandle EmptyAppearance(QPDF &q);

} // namespace fynepdf::pdfa
