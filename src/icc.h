#pragma once

#include <memory>
#include <string>

namespace fynepdf::pdfa::icc {

// Colours through an ICC profile to sRGB, by Little CMS, when the library was built with it
// (FYNEPDF_PDFA_WITH_LCMS): an ICC-based space's own profile instead of a formula, so a CMYK
// photo turns the grey it shows as. Relative colorimetric with black point compensation, as
// Acrobat converts by default. Transforms are made once per profile (|key|).
class Profiles {
public:
  Profiles();
  ~Profiles();
  // Whether |profile| (the bytes of an ICC profile with |components| channels, 3 or 4) can be
  // used; remembered under |key|.
  bool Usable(const std::string &key, const std::string &profile, int components);
  // |count| pixels of 16-bit samples (0 to 65535; CMYK as ink) through the profile |key| (made
  // usable first) to 16-bit sRGB.
  void ToRgb(const std::string &key, const unsigned short *in, unsigned short *out, size_t count);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Whether an RGB display profile describes sRGB: its colorants and white point sRGB's (D50, as
// profiles store them) and each tone curve the sRGB curve, as a parametric curve or a table. Any
// version; the version is what differs between such profiles (Chrome embeds a v4 one).
bool IsSrgb(const std::string &profile);

} // namespace fynepdf::pdfa::icc
