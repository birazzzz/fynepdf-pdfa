#pragma once

// The predefined CMaps (ISO 32000-1 9.7.5.2) a font may name instead of embedding one, read from a
// folder (Adobe's cmap-resources; this repository has the Unicode ones in fonts/cmaps).

#include <memory>
#include <string>

namespace fynepdf::pdfa::font {

// The folder predefined CMaps are read from ("" for none). One for the whole process.
void SetCMapFolder(const std::string &folder);

// A predefined CMap's program by its name (UniJIS-UCS2-H, ...), read once; null when the folder
// doesn't have it or the name isn't one (only letters, digits and hyphens).
std::shared_ptr<const std::string> PredefinedCMap(const std::string &name);

} // namespace fynepdf::pdfa::font
