#pragma once

#include <map>
#include <string>
#include <vector>

namespace fynepdf::pdfa {

struct PdfaOptions {
  int part = 2;  // PDF/A-1b, 2b or 3b
  // A folder with the substitute fonts (this repository's fonts/): a font the PDF names without
  // embedding gets the one there with its widths embedded. Empty: none are.
  std::string fonts;
  // PDF/A-1 (which has no transparency): pages that blend drawn as pictures at this resolution,
  // their text kept over them, invisible, so it can still be found and copied. Needs PDFium
  // (FYNEPDF_PDFA_WITH_PDFIUM); 0, or without it, such pages are a problem instead.
  int flatten_dpi = 300;
};

struct PdfaResult {
  bool ok = false;
  // When not ok: "unreadable", "password", "part" (not 1, 2 or 3) or "write".
  std::string error;
  // Written as PDF/A (it says so in its metadata). When false the file is still written, as
  // close to PDF/A as it could be made but without the label, and |problems| say why.
  bool conforms = false;
  int pages = 0;
  // Why the file isn't PDF/A: "code" or "code: detail", such as "font-not-embedded: Arial".
  std::vector<std::string> problems;
  // What was changed to make it PDF/A, by kind, and how many: "javascript", "annotations-removed",
  // "attachments-removed", "appearances-made", "cmyk-colours", ...
  std::map<std::string, int> changes;
  // Signatures removed: a signed file can't be changed without breaking its signatures.
  int signatures = 0;
};

// A PDF made PDF/A in place: what PDF/A forbids is removed or changed (JavaScript and other
// actions, encryption, LZW, transfer functions, comments without appearances, device CMYK with
// an RGB output intent, unsupported attachments, ...), the sRGB output intent and XMP metadata
// are added, and everything PDF/A allows (text, pictures, bookmarks, links, forms, tags, layers)
// stays as it is. Fonts are checked; one that isn't embedded gets a free substitute from
// |options.fonts| at the document's widths, or is a problem.
PdfaResult ConvertToPdfa(const std::string &input, const std::string &output, const PdfaOptions &options = {});

// The result as one line of JSON.
std::string ToJson(const PdfaResult &result);

} // namespace fynepdf::pdfa
