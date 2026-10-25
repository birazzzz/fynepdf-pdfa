#include "flatten.h"

#ifdef FYNEPDF_PDFA_WITH_PDFIUM

#include <algorithm>
#include <cmath>
#include <mutex>

#include <fpdfview.h>

#ifdef FYNEPDF_PDFA_PDFIUM_DLOPEN
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstdlib>
#include <string>
#endif

namespace fynepdf::pdfa::flatten {
namespace {

// PDFium isn't thread-safe: one caller at a time, the library set up once.
std::mutex &Lock() {
  static std::mutex mutex;
  return mutex;
}

// The PDFium calls used. Linked in, or (FYNEPDF_PDFA_PDFIUM_DLOPEN) loaded the first time a page
// is drawn, so a run that draws none doesn't pay for loading it: the file FYNEPDF_PDFA_PDFIUM
// names, else libpdfium.so (libpdfium.dylib, pdfium.dll) beside this program, else wherever the
// system finds it.
struct Api {
  decltype(&FPDF_InitLibraryWithConfig) init = nullptr;
  decltype(&FPDF_LoadMemDocument64) load = nullptr;
  decltype(&FPDF_CloseDocument) close = nullptr;
  decltype(&FPDF_LoadPage) load_page = nullptr;
  decltype(&FPDF_ClosePage) close_page = nullptr;
  decltype(&FPDF_GetPageWidthF) width = nullptr;
  decltype(&FPDF_GetPageHeightF) height = nullptr;
  decltype(&FPDFBitmap_Create) bitmap = nullptr;
  decltype(&FPDFBitmap_FillRect) fill = nullptr;
  decltype(&FPDF_RenderPageBitmap) render = nullptr;
  decltype(&FPDFBitmap_GetBuffer) buffer = nullptr;
  decltype(&FPDFBitmap_GetStride) stride = nullptr;
  decltype(&FPDFBitmap_Destroy) destroy = nullptr;
  bool ready = false;
};

#ifdef FYNEPDF_PDFA_PDFIUM_DLOPEN
#if defined(_WIN32)
constexpr char kLibrary[] = "pdfium.dll";
#elif defined(__APPLE__)
constexpr char kLibrary[] = "libpdfium.dylib";
#else
constexpr char kLibrary[] = "libpdfium.so";
#endif

#ifdef _WIN32
using Library = HMODULE;
Library OpenFile(const std::string &path) { return LoadLibraryA(path.c_str()); }
void *Symbol(Library library, const char *name) { return reinterpret_cast<void *>(GetProcAddress(library, name)); }
std::string Here() {
  char path[MAX_PATH];
  const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
  std::string here(path, length < MAX_PATH ? length : 0);
  return here.substr(0, here.find_last_of("\\/") + 1);
}
#else
using Library = void *;
Library OpenFile(const std::string &path) { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); }
void *Symbol(Library library, const char *name) { return dlsym(library, name); }
std::string Here() {
  Dl_info self{};
  if (!dladdr(reinterpret_cast<void *>(&Here), &self) || !self.dli_fname)
    return "";
  const std::string here = self.dli_fname;
  return here.substr(0, here.rfind('/') + 1);
}
#endif

Library Open() {
  if (const char *path = std::getenv("FYNEPDF_PDFA_PDFIUM"))
    if (Library library = OpenFile(path))
      return library;
  const std::string here = Here();
  if (!here.empty())
    if (Library library = OpenFile(here + kLibrary))
      return library;
  return OpenFile(kLibrary);
}
#endif

const Api &Load() {
  static Api api;
  static bool tried = false;
  if (tried)
    return api;
  tried = true;
#ifdef FYNEPDF_PDFA_PDFIUM_DLOPEN
  Library library = Open();
  if (!library)
    return api;
  const auto get = [&](auto &slot, const char *name) {
    slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(Symbol(library, name));
    return slot != nullptr;
  };
  api.ready = get(api.init, "FPDF_InitLibraryWithConfig") && get(api.load, "FPDF_LoadMemDocument64") && get(api.close, "FPDF_CloseDocument") &&
              get(api.load_page, "FPDF_LoadPage") && get(api.close_page, "FPDF_ClosePage") && get(api.width, "FPDF_GetPageWidthF") &&
              get(api.height, "FPDF_GetPageHeightF") && get(api.bitmap, "FPDFBitmap_Create") && get(api.fill, "FPDFBitmap_FillRect") &&
              get(api.render, "FPDF_RenderPageBitmap") && get(api.buffer, "FPDFBitmap_GetBuffer") && get(api.stride, "FPDFBitmap_GetStride") &&
              get(api.destroy, "FPDFBitmap_Destroy");
#else
  api = Api{&FPDF_InitLibraryWithConfig, &FPDF_LoadMemDocument64, &FPDF_CloseDocument, &FPDF_LoadPage, &FPDF_ClosePage,
            &FPDF_GetPageWidthF,         &FPDF_GetPageHeightF,     &FPDFBitmap_Create,  &FPDFBitmap_FillRect, &FPDF_RenderPageBitmap,
            &FPDFBitmap_GetBuffer,       &FPDFBitmap_GetStride,    &FPDFBitmap_Destroy, true};
#endif
  if (api.ready) {
    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    api.init(&config);
  }
  return api;
}

} // namespace

// (Loaded on demand, whether it loads is only known by loading it: Render says.)
bool Available() { return true; }

bool Render(const std::string &pdf, const std::vector<int> &pages, double dpi, double most_pixels, const std::function<void(int, Picture &)> &done) {
  std::lock_guard<std::mutex> lock(Lock());
  const Api &api = Load();
  if (!api.ready)
    return false;
  FPDF_DOCUMENT document = api.load(pdf.data(), pdf.size(), nullptr);
  if (!document)
    return false;
  for (const int index : pages) {
    FPDF_PAGE page = api.load_page(document, index);
    if (!page)
      continue;
    const double width = api.width(page), height = api.height(page);
    double scale = dpi / 72;
    if (width * height * scale * scale > most_pixels)
      scale = std::sqrt(most_pixels / (width * height));
    const int w = std::max(1, static_cast<int>(std::lround(width * scale))), h = std::max(1, static_cast<int>(std::lround(height * scale)));
    FPDF_BITMAP bitmap = api.bitmap(w, h, 0);
    if (bitmap) {
      api.fill(bitmap, 0, 0, w, h, 0xFFFFFFFF);
      api.render(bitmap, page, 0, 0, w, h, 0, FPDF_ANNOT);
      const unsigned char *pixels = static_cast<const unsigned char *>(api.buffer(bitmap));
      const int stride = api.stride(bitmap);
      Picture picture;
      picture.width = w;
      picture.height = h;
      picture.rgb.resize(static_cast<size_t>(w) * h * 3);
      for (int y = 0; y < h; ++y) {
        const unsigned char *row = pixels + static_cast<size_t>(y) * stride;
        char *to = &picture.rgb[static_cast<size_t>(y) * w * 3];
        for (int x = 0; x < w; ++x) {  // BGRx
          to[x * 3] = static_cast<char>(row[x * 4 + 2]);
          to[x * 3 + 1] = static_cast<char>(row[x * 4 + 1]);
          to[x * 3 + 2] = static_cast<char>(row[x * 4]);
        }
      }
      api.destroy(bitmap);
      done(index, picture);
    }
    api.close_page(page);
  }
  api.close(document);
  return true;
}

} // namespace fynepdf::pdfa::flatten

#else

namespace fynepdf::pdfa::flatten {
bool Available() { return false; }
bool Render(const std::string &, const std::vector<int> &, double, double, const std::function<void(int, Picture &)> &) { return false; }
} // namespace fynepdf::pdfa::flatten

#endif
