#pragma once

// The colour converter both Grayscale and PDF/A use: a document's colours rewritten in place, to
// grey (Grayscale) or, for PDF/A, device CMYK to RGB (an sRGB output intent can't cover CMYK).

#include <memory>
#include <vector>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

namespace fynepdf::pdfa {

enum class Target {
  Gray,  // every colour to DeviceGray
  Rgb,   // device CMYK to DeviceRGB, and spot inks' CMYK alternates to RGB; the rest stays
};

struct ColourOptions {
  Target target = Target::Gray;
  bool drawings = true;  // text and drawings, gradients, patterns, comments and fields
  bool images = true;    // pictures
  int jpegQuality = 90;  // for photos that must be re-compressed
};

struct ColourStats {
  int colours = 0, images = 0, jpegLossless = 0, shadings = 0, inlineImages = 0, spots = 0, wrapped = 0;
  int imagesKept = 0, shadingsKept = 0, inlineKept = 0, contentKept = 0;
  // Target::Rgb: device CMYK that couldn't be converted (a spot ink of more than eight inks or
  // whose tint transform can't be read, an inline picture whose resources can't take the wrapper).
  int cmykLeft = 0;
};

// Device CMYK as PDF/A takes it beside an sRGB output intent: DeviceN of the four process inks
// with a DeviceRGB alternate, sampled at the sixteen ink combinations (CmykToRgb between them).
// The colour space array's items.
std::vector<QPDFObjectHandle> CmykAsRgb(QPDF &q);

class Converter;

class ColourConverter {
public:
  ColourConverter(QPDF &q, ColourStats &stats, const ColourOptions &options);
  ~ColourConverter();
  // A page's content, resources, transparency group and annotations.
  void Page(QPDFPageObjectHelper &page);
  // The form's and its fields' default appearances (/DA).
  void FormDefaults();

private:
  std::unique_ptr<Converter> impl_;
};

} // namespace fynepdf::pdfa
