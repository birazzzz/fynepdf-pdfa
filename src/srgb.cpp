#include "srgb.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace fynepdf::pdfa {
namespace {

void Put32(std::string &out, size_t at, uint32_t value) {
  for (int k = 0; k < 4; ++k)
    out[at + static_cast<size_t>(k)] = static_cast<char>(value >> (24 - 8 * k));
}

void Append32(std::string &out, uint32_t value) {
  out.append(4, '\0');
  Put32(out, out.size() - 4, value);
}

void Append16(std::string &out, uint16_t value) {
  out += static_cast<char>(value >> 8);
  out += static_cast<char>(value & 0xff);
}

uint32_t Signature(const char *text) {
  return static_cast<uint32_t>(text[0]) << 24 | static_cast<uint32_t>(text[1]) << 16 | static_cast<uint32_t>(text[2]) << 8 |
         static_cast<uint32_t>(text[3]);
}

// s15Fixed16Number
uint32_t Fixed(double value) { return static_cast<uint32_t>(static_cast<int32_t>(std::lround(value * 65536))); }

std::string Xyz(double x, double y, double z) {
  std::string tag;
  Append32(tag, Signature("XYZ "));
  Append32(tag, 0);
  Append32(tag, Fixed(x));
  Append32(tag, Fixed(y));
  Append32(tag, Fixed(z));
  return tag;
}

std::string Build() {
  constexpr const char *kDescription = "sRGB IEC61966-2.1";
  // textDescriptionType: ASCII, then empty Unicode and ScriptCode parts.
  std::string desc;
  Append32(desc, Signature("desc"));
  Append32(desc, 0);
  Append32(desc, static_cast<uint32_t>(std::char_traits<char>::length(kDescription) + 1));
  desc += kDescription;
  desc += '\0';
  Append32(desc, 0);  // Unicode language
  Append32(desc, 0);  // Unicode count
  Append16(desc, 0);  // ScriptCode code
  desc += '\0';       // ScriptCode count
  desc.append(67, '\0');
  std::string cprt;
  Append32(cprt, Signature("text"));
  Append32(cprt, 0);
  cprt += "No copyright, use freely";
  cprt += '\0';
  // The sRGB curve, 1024 samples.
  std::string curve;
  Append32(curve, Signature("curv"));
  Append32(curve, 0);
  Append32(curve, 1024);
  for (int i = 0; i < 1024; ++i) {
    const double v = i / 1023.0;
    const double linear = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    Append16(curve, static_cast<uint16_t>(std::lround(linear * 65535)));
  }
  // The sRGB primaries adapted to D50 (Bradford), as the ICC's own sRGB profiles have them.
  struct Tag {
    const char *signature;
    std::string data;
    int shared;  // the index of the tag whose data this one reuses, or -1
  };
  const std::vector<Tag> tags = {
      {"desc", desc, -1},
      {"cprt", cprt, -1},
      {"wtpt", Xyz(0.9642, 1.0, 0.8249), -1},
      {"rXYZ", Xyz(0.4360747, 0.2225045, 0.0139322), -1},
      {"gXYZ", Xyz(0.3850649, 0.7168786, 0.0971045), -1},
      {"bXYZ", Xyz(0.1430804, 0.0606169, 0.7141733), -1},
      {"rTRC", curve, -1},
      {"gTRC", std::string(), 6},
      {"bTRC", std::string(), 6},
  };
  std::string profile(128, '\0');
  Put32(profile, 8, 0x02100000);  // version 2.1.0
  Put32(profile, 12, Signature("mntr"));
  Put32(profile, 16, Signature("RGB "));
  Put32(profile, 20, Signature("XYZ "));
  // 2026-10-24 00:00:00, fixed so the profile never changes.
  const uint16_t date[6] = {2026, 10, 24, 0, 0, 0};
  for (int k = 0; k < 6; ++k) {
    profile[24 + static_cast<size_t>(k) * 2] = static_cast<char>(date[k] >> 8);
    profile[25 + static_cast<size_t>(k) * 2] = static_cast<char>(date[k] & 0xff);
  }
  Put32(profile, 36, Signature("acsp"));
  Put32(profile, 68, Fixed(0.9642));  // the PCS illuminant, D50
  Put32(profile, 72, Fixed(1.0));
  Put32(profile, 76, Fixed(0.8249));
  Append32(profile, static_cast<uint32_t>(tags.size()));
  const size_t table = profile.size();
  profile.append(tags.size() * 12, '\0');
  std::vector<uint32_t> offsets(tags.size()), sizes(tags.size());
  for (size_t i = 0; i < tags.size(); ++i) {
    if (tags[i].shared >= 0) {
      offsets[i] = offsets[static_cast<size_t>(tags[i].shared)];
      sizes[i] = sizes[static_cast<size_t>(tags[i].shared)];
      continue;
    }
    while (profile.size() % 4)
      profile += '\0';
    offsets[i] = static_cast<uint32_t>(profile.size());
    sizes[i] = static_cast<uint32_t>(tags[i].data.size());
    profile += tags[i].data;
  }
  while (profile.size() % 4)
    profile += '\0';
  for (size_t i = 0; i < tags.size(); ++i) {
    Put32(profile, table + i * 12, Signature(tags[i].signature));
    Put32(profile, table + i * 12 + 4, offsets[i]);
    Put32(profile, table + i * 12 + 8, sizes[i]);
  }
  Put32(profile, 0, static_cast<uint32_t>(profile.size()));
  return profile;
}

} // namespace

const std::string &SrgbProfile() {
  static const std::string profile = Build();
  return profile;
}

} // namespace fynepdf::pdfa
