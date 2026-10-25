#pragma once

#include <string>

namespace fynepdf::pdfa::jpx {

// A JPEG 2000 picture (JP2 file or bare codestream) decoded as 8-bit grey: luma of RGB, the Y
// channel of YCC, grey from CMYK. |alpha| gets its alpha channel when it has one (else empty).
// False without OpenJPEG (FYNEPDF_PDFA_WITH_OPENJPEG), or when the picture can't be decoded.
bool DecodeGray(const std::string &bytes, std::string &gray, std::string &alpha, int &width, int &height);

// A JPEG 2000 picture decoded to 8-bit samples, interleaved: grey (1 component), RGB (3; YCC
// converted) or CMYK (4); |alpha| its alpha channel when it has one.
struct Picture {
  int width = 0, height = 0, components = 0;
  std::string samples, alpha;
};
// False without OpenJPEG, when it can't be decoded, or when it has more than |most| samples.
bool Decode(const std::string &bytes, Picture &picture, unsigned long long most);

} // namespace fynepdf::pdfa::jpx
