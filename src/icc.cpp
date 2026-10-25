#include "icc.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

#ifdef FYNEPDF_PDFA_WITH_LCMS
#include <lcms2.h>
#endif

namespace fynepdf::pdfa::icc {
namespace {

uint32_t Read32(const std::string &d, size_t at) {
  if (at + 4 > d.size())
    return 0;
  return static_cast<uint32_t>(static_cast<unsigned char>(d[at])) << 24 | static_cast<uint32_t>(static_cast<unsigned char>(d[at + 1])) << 16 |
         static_cast<uint32_t>(static_cast<unsigned char>(d[at + 2])) << 8 | static_cast<unsigned char>(d[at + 3]);
}

double Fixed(const std::string &d, size_t at) { return static_cast<int32_t>(Read32(d, at)) / 65536.0; }

// The sRGB curve, from a signal 0..1.
double SrgbCurve(double x) { return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4); }

// Whether a tone curve tag ('curv' or 'para') is the sRGB curve.
bool SrgbTrc(const std::string &tag) {
  if (tag.compare(0, 4, "para") == 0 && tag.size() >= 14) {
    const unsigned type = static_cast<unsigned char>(tag[8]) << 8 | static_cast<unsigned char>(tag[9]);
    static const int counts[] = {1, 3, 4, 5, 7};
    if (type > 4 || tag.size() < 12 + static_cast<size_t>(counts[type]) * 4)
      return false;
    double p[7] = {0, 1, 0, 0, 0, 0, 0};
    for (int k = 0; k < counts[type]; ++k)
      p[k] = Fixed(tag, 12 + static_cast<size_t>(k) * 4);
    // g a b c d (e f): Y = (aX+b)^g for X >= d, else cX (+ offsets that must be nothing).
    const auto curve = [&](double x) {
      switch (type) {
      case 0: return std::pow(x, p[0]);
      case 3: return x >= p[4] ? std::pow(p[1] * x + p[2], p[0]) : p[3] * x;
      case 4: return x >= p[4] ? std::pow(p[1] * x + p[2], p[0]) + p[5] : p[3] * x + p[6];
      default: return -1.0;
      }
    };
    for (int k = 0; k <= 20; ++k)
      if (std::fabs(curve(k / 20.0) - SrgbCurve(k / 20.0)) > 0.002)
        return false;
    return true;
  }
  if (tag.compare(0, 4, "curv") == 0) {
    const uint32_t count = Read32(tag, 8);
    if (count < 2 || tag.size() < 12 + static_cast<size_t>(count) * 2)
      return false;
    for (uint32_t k = 0; k < count; ++k) {
      const double y = (static_cast<unsigned char>(tag[12 + k * 2]) << 8 | static_cast<unsigned char>(tag[13 + k * 2])) / 65535.0;
      if (std::fabs(y - SrgbCurve(static_cast<double>(k) / (count - 1))) > 0.003)
        return false;
    }
    return true;
  }
  return false;
}

} // namespace

bool IsSrgb(const std::string &d) {
  if (d.size() < 132 || d.compare(36, 4, "acsp") != 0 || d.compare(16, 4, "RGB ") != 0 || d.compare(20, 4, "XYZ ") != 0)
    return false;
  std::map<std::string, std::string> tags;
  const uint32_t count = Read32(d, 128);
  for (uint32_t k = 0; k < count && k < 100; ++k) {
    const size_t entry = 132 + static_cast<size_t>(k) * 12;
    const uint32_t offset = Read32(d, entry + 4), size = Read32(d, entry + 8);
    if (entry + 12 > d.size() || offset > d.size() || size > d.size() - offset)
      return false;
    tags[d.substr(entry, 4)] = d.substr(offset, size);
  }
  // sRGB's colorants and white point, adapted to D50.
  static const std::map<std::string, std::vector<double>> xyz = {
      {"rXYZ", {0.4361, 0.2225, 0.0139}}, {"gXYZ", {0.3851, 0.7169, 0.0971}}, {"bXYZ", {0.1431, 0.0606, 0.7141}}, {"wtpt", {0.9642, 1.0, 0.8249}}};
  for (const auto &[name, want] : xyz) {
    const auto found = tags.find(name);
    if (found == tags.end() || found->second.size() < 20 || found->second.compare(0, 4, "XYZ ") != 0)
      return false;
    for (size_t k = 0; k < 3; ++k)
      if (std::fabs(Fixed(found->second, 8 + k * 4) - want[k]) > 0.0015)
        return false;
  }
  for (const char *name : {"rTRC", "gTRC", "bTRC"}) {
    const auto found = tags.find(name);
    if (found == tags.end() || !SrgbTrc(found->second))
      return false;
  }
  return true;
}

#ifdef FYNEPDF_PDFA_WITH_LCMS

struct Profiles::Impl {
  cmsHPROFILE srgb = cmsCreate_sRGBProfile();
  std::map<std::string, cmsHTRANSFORM> transforms;  // null for a profile that can't be used
  ~Impl() {
    for (const auto &[key, transform] : transforms)
      if (transform)
        cmsDeleteTransform(transform);
    if (srgb)
      cmsCloseProfile(srgb);
  }
};

Profiles::Profiles() : impl_(std::make_unique<Impl>()) {}
Profiles::~Profiles() = default;

bool Profiles::Usable(const std::string &key, const std::string &profile, int components) {
  const auto found = impl_->transforms.find(key);
  if (found != impl_->transforms.end())
    return found->second != nullptr;
  cmsHTRANSFORM transform = nullptr;
  if (impl_->srgb && (components == 3 || components == 4) && !profile.empty()) {
    cmsHPROFILE source = cmsOpenProfileFromMem(profile.data(), static_cast<cmsUInt32Number>(profile.size()));
    if (source) {
      const cmsColorSpaceSignature space = cmsGetColorSpace(source);
      if ((components == 4 && space == cmsSigCmykData) || (components == 3 && space == cmsSigRgbData))
        transform = cmsCreateTransform(source, components == 4 ? TYPE_CMYK_16 : TYPE_RGB_16, impl_->srgb, TYPE_RGB_16,
                                       INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION);
      cmsCloseProfile(source);
    }
  }
  impl_->transforms.emplace(key, transform);
  return transform != nullptr;
}

void Profiles::ToRgb(const std::string &key, const unsigned short *in, unsigned short *out, size_t count) {
  const auto found = impl_->transforms.find(key);
  if (found != impl_->transforms.end() && found->second)
    cmsDoTransform(found->second, in, out, static_cast<cmsUInt32Number>(count));
}

#else

struct Profiles::Impl {};
Profiles::Profiles() = default;
Profiles::~Profiles() = default;
bool Profiles::Usable(const std::string &, const std::string &, int) { return false; }
void Profiles::ToRgb(const std::string &, const unsigned short *, unsigned short *, size_t) {}

#endif

} // namespace fynepdf::pdfa::icc
