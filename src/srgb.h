#pragma once

#include <string>

namespace fynepdf::pdfa {

// An sRGB ICC profile (version 2.1, a display profile with matrix and curves, D50 white), made
// here byte for byte so every file gets the same one and no colour library is needed: the
// PDF/A output intent's destination profile.
const std::string &SrgbProfile();

} // namespace fynepdf::pdfa
