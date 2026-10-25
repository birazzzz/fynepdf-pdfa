#pragma once

// Pages drawn as pictures, for PDF/A-1 (which has no transparency), by PDFium. Without it
// (FYNEPDF_PDFA_WITH_PDFIUM unset) nothing is drawn.

#include <functional>
#include <string>
#include <vector>

namespace fynepdf::pdfa::flatten {

// Whether pages can be drawn (the library was built with PDFium).
bool Available();

struct Picture {
  int width = 0, height = 0;
  std::string rgb;  // rows top to bottom, 3 bytes a pixel
};

// Pages (0-based) of a PDF held in memory, each drawn on white with its annotations' appearances,
// as it shows (its crop box, unrotated), at |dpi|; at fewer when a page would take more than
// |most_pixels|. Each picture goes to |done| as it's drawn (one held at a time). False when the
// PDF can't be read; a page that can't be drawn is left out.
bool Render(const std::string &pdf, const std::vector<int> &pages, double dpi, double most_pixels, const std::function<void(int, Picture &)> &done);

} // namespace fynepdf::pdfa::flatten
