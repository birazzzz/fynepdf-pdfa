// fynepdf-pdfa: the library's command line.
//
//   fynepdf-pdfa <input.pdf> <output.pdf> [--part 1|2|3] [--fonts <folder>] [--flatten-dpi 300]
//
// Prints the result as one line of JSON. Exit codes: 0 done ("conforms" says whether the file is
// PDF/A or was written without the label, with "problems" saying why), 2 needs a password, 3 can't
// be read, 5 can't be written, 64 wrong arguments.
//
// Without --fonts, the substitute fonts are looked for beside the program (fonts/) and where an
// install puts them (../share/fynepdf-pdfa/fonts).

#include "fynepdf/pdfa/pdfa.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace {

// This program's own file, wherever it was started from.
std::filesystem::path Self(const char *program) {
#if defined(_WIN32)
  wchar_t path[32768];
  const DWORD length = GetModuleFileNameW(nullptr, path, 32768);
  if (length > 0 && length < 32768)
    return std::filesystem::path(std::wstring(path, length));
#elif defined(__APPLE__)
  char path[4096];
  uint32_t size = sizeof path;
  if (_NSGetExecutablePath(path, &size) == 0)
    return std::filesystem::path(path);
#else
  std::error_code error;
  const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
  if (!error)
    return self;
#endif
  return std::filesystem::path(program);
}

int Usage() {
  std::cerr << "usage: fynepdf-pdfa <input.pdf> <output.pdf> [--part 1|2|3] [--fonts <folder>] [--flatten-dpi 300]\n"
               "  --part         PDF/A-1b, 2b (the default) or 3b\n"
               "  --fonts        the substitute fonts, for fonts the PDF names without embedding\n"
               "  --flatten-dpi  resolution of PDF/A-1b pages that use transparency, drawn as pictures\n";
  return 64;
}

// The fonts folder next to the program, or where `cmake --install` puts it.
std::string DefaultFonts(const char *program) {
  std::error_code error;
  const std::filesystem::path here = std::filesystem::weakly_canonical(Self(program), error).parent_path();
  for (const auto &candidate : {here / "fonts", here.parent_path() / "share" / "fynepdf-pdfa" / "fonts"})
    if (std::filesystem::is_directory(candidate, error))
      return candidate.string();
  return "";
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3)
    return Usage();
  fynepdf::pdfa::PdfaOptions options;
  bool fonts_given = false;
  for (int index = 3; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--part" && index + 1 < argc) {
      options.part = std::atoi(argv[++index]);
      if (options.part < 1 || options.part > 3)
        return Usage();
    } else if (arg == "--fonts" && index + 1 < argc) {
      options.fonts = argv[++index];
      fonts_given = true;
    } else if (arg == "--flatten-dpi" && index + 1 < argc) {
      options.flatten_dpi = std::atoi(argv[++index]);
    } else {
      return Usage();
    }
  }
  if (!fonts_given)
    options.fonts = DefaultFonts(argv[0]);
  const fynepdf::pdfa::PdfaResult result = fynepdf::pdfa::ConvertToPdfa(argv[1], argv[2], options);
  std::cout << fynepdf::pdfa::ToJson(result) << "\n";
  if (result.ok)
    return 0;
  return result.error == "password" ? 2 : result.error == "write" ? 5 : 3;
}
