#pragma once

#include <string>

namespace fynepdf::pdfa::jpeg {

// A colour JPEG made grey without decoding it: its brightness channel (Y of YCbCr) is kept as it
// was compressed and the colour channels are dropped, as jpegtran -grayscale does. Only for
// YCbCr JPEGs whose brightness is at full resolution; false otherwise (RGB, CMYK, YCCK, damaged).
bool GrayFromYCbCr(const std::string &jpeg, std::string &out);

// Grey (|components| 1) or RGB (3) pixels, a byte each, rows top to bottom, as a baseline JPEG.
bool Encode(const unsigned char *pixels, int width, int height, int components, int quality, std::string &out);

} // namespace fynepdf::pdfa::jpeg
