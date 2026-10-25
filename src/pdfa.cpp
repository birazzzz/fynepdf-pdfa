#include "fynepdf/pdfa/pdfa.h"

#include "appearance.h"
#include "colour.h"
#include "common.h"
#include "content.h"
#include "font_check.h"
#include "font_cmaps.h"
#include "font_embed.h"
#include "font_fix.h"
#include "icc.h"
#include "flatten.h"
#include "jpeg.h"
#include "jpx.h"
#include "pdf_function.h"
#include "srgb.h"
#include "xmp.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <qpdf/Buffer.hh>
#include <qpdf/Pl_Buffer.hh>
#include <qpdf/Pl_Flate.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFAcroFormDocumentHelper.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFNameTreeObjectHelper.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

namespace fynepdf::pdfa {
namespace {

using namespace content;

// Every operator PDF 1.7 defines (ISO 32000-1 Annex A); content may use no others.
const std::set<std::string> kOperators = {
    "b",  "B",  "b*", "B*", "BDC", "BI", "BMC", "BT", "BX", "c",  "cm", "CS", "cs", "d",  "d0", "d1", "Do", "DP", "EI",
    "EMC", "ET", "EX", "f",  "F",  "f*", "G",  "g",  "gs", "h",  "i",  "ID", "j",  "J",  "K",  "k",  "l",  "m",  "M",
    "MP", "n",  "q",  "Q",  "re", "RG", "rg", "ri", "s",  "S",  "SC", "sc", "SCN", "scn", "sh", "T*", "Tc", "Td", "TD",
    "Tf", "Tj", "TJ", "TL", "Tm", "Tr", "Ts", "Tw", "Tz", "v",  "w",  "W",  "W*", "y",  "'",  "\""};

const std::set<std::string> kIntents = {"/AbsoluteColorimetric", "/RelativeColorimetric", "/Saturation", "/Perceptual"};

const std::set<std::string> kBlendModes = {"/Normal", "/Compatible", "/Multiply", "/Screen", "/Overlay", "/Darken",
                                           "/Lighten", "/ColorDodge", "/ColorBurn", "/HardLight", "/SoftLight", "/Difference",
                                           "/Exclusion", "/Hue", "/Saturation", "/Color", "/Luminosity"};

// Comment types each part allows (PDF 1.4's for PDF/A-1, ISO 32000-1's for 2 and 3, without the
// multimedia ones and, for 1, attachments).
const std::set<std::string> kAnnotations1 = {"/Text",      "/Link",      "/FreeText", "/Line",  "/Square", "/Circle",
                                             "/Highlight", "/Underline", "/Squiggly", "/StrikeOut", "/Stamp", "/Ink",
                                             "/Popup",     "/Widget",    "/PrinterMark", "/TrapNet"};
const std::set<std::string> kAnnotations2 = {"/Text",     "/Link",      "/FreeText", "/Line",   "/Square",    "/Circle",
                                             "/Polygon",  "/PolyLine",  "/Highlight", "/Underline", "/Squiggly", "/StrikeOut",
                                             "/Stamp",    "/Caret",     "/Ink",       "/Popup",  "/FileAttachment", "/Widget",
                                             "/PrinterMark", "/TrapNet", "/Watermark", "/Redact"};

// The CMaps every reader has (ISO 32000-1 Table 118), with the character collection each is for.
const std::map<std::string, std::string> kCMaps = {
    {"GB-EUC-H", "GB1"},          {"GB-EUC-V", "GB1"},          {"GBpc-EUC-H", "GB1"},        {"GBpc-EUC-V", "GB1"},
    {"GBK-EUC-H", "GB1"},         {"GBK-EUC-V", "GB1"},         {"GBKp-EUC-H", "GB1"},        {"GBKp-EUC-V", "GB1"},
    {"GBK2K-H", "GB1"},           {"GBK2K-V", "GB1"},           {"UniGB-UCS2-H", "GB1"},      {"UniGB-UCS2-V", "GB1"},
    {"UniGB-UTF16-H", "GB1"},     {"UniGB-UTF16-V", "GB1"},     {"B5pc-H", "CNS1"},           {"B5pc-V", "CNS1"},
    {"HKscs-B5-H", "CNS1"},       {"HKscs-B5-V", "CNS1"},       {"ETen-B5-H", "CNS1"},        {"ETen-B5-V", "CNS1"},
    {"ETenms-B5-H", "CNS1"},      {"ETenms-B5-V", "CNS1"},      {"CNS-EUC-H", "CNS1"},        {"CNS-EUC-V", "CNS1"},
    {"UniCNS-UCS2-H", "CNS1"},    {"UniCNS-UCS2-V", "CNS1"},    {"UniCNS-UTF16-H", "CNS1"},   {"UniCNS-UTF16-V", "CNS1"},
    {"83pv-RKSJ-H", "Japan1"},    {"90ms-RKSJ-H", "Japan1"},    {"90ms-RKSJ-V", "Japan1"},    {"90msp-RKSJ-H", "Japan1"},
    {"90msp-RKSJ-V", "Japan1"},   {"90pv-RKSJ-H", "Japan1"},    {"Add-RKSJ-H", "Japan1"},     {"Add-RKSJ-V", "Japan1"},
    {"EUC-H", "Japan1"},          {"EUC-V", "Japan1"},          {"Ext-RKSJ-H", "Japan1"},     {"Ext-RKSJ-V", "Japan1"},
    {"H", "Japan1"},              {"V", "Japan1"},              {"UniJIS-UCS2-H", "Japan1"},  {"UniJIS-UCS2-V", "Japan1"},
    {"UniJIS-UCS2-HW-H", "Japan1"}, {"UniJIS-UCS2-HW-V", "Japan1"}, {"UniJIS-UTF16-H", "Japan1"}, {"UniJIS-UTF16-V", "Japan1"},
    {"KSC-EUC-H", "Korea1"},      {"KSC-EUC-V", "Korea1"},      {"KSCms-UHC-H", "Korea1"},    {"KSCms-UHC-V", "Korea1"},
    {"KSCms-UHC-HW-H", "Korea1"}, {"KSCms-UHC-HW-V", "Korea1"}, {"KSCpc-EUC-H", "Korea1"},    {"UniKS-UCS2-H", "Korea1"},
    {"UniKS-UCS2-V", "Korea1"},   {"UniKS-UTF16-H", "Korea1"},  {"UniKS-UTF16-V", "Korea1"},  {"Identity-H", ""},
    {"Identity-V", ""}};

const std::set<std::string> kStandard14 = {"/Times-Roman", "/Times-Bold", "/Times-Italic", "/Times-BoldItalic", "/Helvetica",
                                           "/Helvetica-Bold", "/Helvetica-Oblique", "/Helvetica-BoldOblique", "/Courier",
                                           "/Courier-Bold", "/Courier-Oblique", "/Courier-BoldOblique", "/Symbol", "/ZapfDingbats"};

// The resource kinds whose names content refers to.
const char *const kResourceKinds[] = {"/Font", "/XObject", "/ExtGState", "/ColorSpace", "/Pattern", "/Shading", "/Properties"};

std::string Name(QPDFObjectHandle handle) { return handle.isName() ? handle.getName() : ""; }

bool ValidUtf8(const std::string &text) {
  for (size_t at = 0; at < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[at]);
    const size_t length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (length == 0 || at + length > text.size())
      return false;
    unsigned long code = length == 1 ? c : length == 2 ? c & 0x1f : length == 3 ? c & 0x0f : c & 0x07;
    for (size_t k = 1; k < length; ++k) {
      const unsigned char next = static_cast<unsigned char>(text[at + k]);
      if ((next >> 6) != 2)
        return false;
      code = code << 6 | (next & 0x3f);
    }
    if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) || (length == 4 && code < 0x10000) || (code >= 0xd800 && code < 0xe000) ||
        code >= 0x110000)
      return false;
    at += length;
  }
  return true;
}

// A name PDF/A can carry: valid UTF-8, at most 127 bytes (with or without its slash, which is
// kept). One that isn't UTF-8 gets each byte past ASCII written as two hex digits; one too long
// is cut, a hash of the whole after it. Either way it maps to the same new name wherever it is.
// (veraPDF reads a name's characters back as bytes: non-ASCII UTF-8 made from them still fails,
// so the new name is plain ASCII.)
std::string FixedName(const std::string &name) {
  if (!name.empty() && name[0] == '/')
    return "/" + FixedName(name.substr(1));
  static const char *hex = "0123456789ABCDEF";
  std::string out;
  if (ValidUtf8(name)) {
    out = name;
  } else {
    for (const char c : name) {
      const unsigned char byte = static_cast<unsigned char>(c);
      if (byte < 0x80) {
        out += c;
      } else {
        out += hex[byte >> 4];
        out += hex[byte & 15];
      }
    }
  }
  if (out.size() > 127) {
    uint32_t hash = 2166136261u;  // FNV-1a
    for (const char c : name)
      hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
    std::string tail = "_";
    for (int shift = 28; shift >= 0; shift -= 4)
      tail += hex[(hash >> shift) & 15];
    size_t cut = 127 - tail.size();
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xc0) == 0x80)
      --cut;  // not inside a UTF-8 character
    out = out.substr(0, cut) + tail;
  }
  return out;
}

uint32_t Read32(const std::string &data, size_t at) {
  return at + 4 > data.size() ? 0
                              : static_cast<uint32_t>(static_cast<unsigned char>(data[at])) << 24 | static_cast<uint32_t>(static_cast<unsigned char>(data[at + 1])) << 16 |
                                    static_cast<uint32_t>(static_cast<unsigned char>(data[at + 2])) << 8 | static_cast<unsigned char>(data[at + 3]);
}

unsigned Read16(const std::string &data, size_t at) {
  return at + 2 > data.size() ? 0 : static_cast<unsigned>(static_cast<unsigned char>(data[at])) << 8 | static_cast<unsigned char>(data[at + 1]);
}

// The (platform, encoding) pairs of a TrueType font program's cmap table; false when there's
// no cmap table to read.
bool CmapEncodings(const std::string &font, std::vector<std::pair<unsigned, unsigned>> &out) {
  const unsigned tables = Read16(font, 4);
  for (unsigned index = 0; index < tables && index < 512; ++index) {
    const size_t record = 12 + static_cast<size_t>(index) * 16;
    if (font.compare(record, 4, "cmap") != 0)
      continue;
    const size_t at = Read32(font, record + 8);
    const unsigned count = Read16(font, at + 2);
    if (at + 4 + static_cast<size_t>(count) * 8 > font.size())
      return false;
    for (unsigned k = 0; k < count; ++k)
      out.push_back({Read16(font, at + 4 + k * 8), Read16(font, at + 6 + k * 8)});
    return true;
  }
  return false;
}

// What PDF/A-2 doesn't allow in a JPEG 2000 picture (6.2.8.3), "" when it's fine.
std::string JpxTrouble(const std::string &data, bool colours_count) {
  if (data.size() >= 4 && static_cast<unsigned char>(data[0]) == 0xff && static_cast<unsigned char>(data[1]) == 0x4f) {
    // A bare codestream: its SIZ marker.
    if (Read16(data, 2) != 0xff51)
      return "unreadable";
    const unsigned components = Read16(data, 40);
    if (components != 1 && components != 3 && components != 4)
      return "channels";
    for (unsigned k = 1; k < components; ++k)
      if ((static_cast<unsigned char>(data[42 + k * 3]) & 0x7f) != (static_cast<unsigned char>(data[42]) & 0x7f))
        return "bit depths";
    return "";
  }
  int colours = 0, approximate = 0;
  std::string trouble;
  std::function<void(size_t, size_t, int)> boxes = [&](size_t at, size_t end, int depth) {
    while (at + 8 <= end && depth < 4) {
      uint64_t length = Read32(data, at);
      const std::string type = data.substr(at + 4, 4);
      size_t header = 8;
      if (length == 1) {
        length = static_cast<uint64_t>(Read32(data, at + 8)) << 32 | Read32(data, at + 12);
        header = 16;
      } else if (length == 0) {
        length = end - at;
      }
      if (length < header || at + length > end)
        return;
      const size_t body = at + header;
      if (type == "jp2h") {
        boxes(body, at + static_cast<size_t>(length), depth + 1);
      } else if (type == "ihdr" && body + 14 <= data.size()) {
        const unsigned components = Read16(data, body + 8);
        if (components != 1 && components != 3 && components != 4)
          trouble = "channels";
        const unsigned depth_byte = static_cast<unsigned char>(data[body + 10]);
        if (depth_byte != 255 && (depth_byte & 0x7f) + 1 > 38)
          trouble = "bit depth";
      } else if (type == "bpcc") {
        for (size_t k = body + 1; k < at + length; ++k)
          if (data[k] != data[body])
            trouble = "bit depths";
      } else if (type == "colr" && body + 3 <= data.size()) {
        ++colours;
        const unsigned method = static_cast<unsigned char>(data[body]), approx = static_cast<unsigned char>(data[body + 2]);
        if (approx == 1)
          ++approximate;
        if (method < 1 || method > 3)
          trouble = "colour method";
        if (method == 1 && Read32(data, body + 3) == 19)
          trouble = "CIEJab";
      } else if (type == "jp2c") {
        return;
      }
      at += static_cast<size_t>(length);
    }
  };
  boxes(0, data.size(), 0);
  if (colours > 1 && approximate != 1)
    trouble = "colour specifications";
  if (!colours_count && (trouble == "colour specifications" || trouble == "colour method" || trouble == "CIEJab"))
    trouble.clear();
  return trouble;
}

// A name token as written in content.
std::string NameText(const std::string &name) { return QPDFObjectHandle::newName("/" + name).unparse(); }

std::string Text(QPDFObjectHandle handle) { return handle.isString() ? handle.getUTF8Value() : ""; }

// A stream's bytes through all its filters (nothing for lossy or image-only ones); false when
// they can't be decoded.
bool Decoded(QPDFObjectHandle stream, std::string &out) {
  try {
    Pl_Buffer buffer("pdfa");
    if (!stream.pipeStreamData(&buffer, 0, qpdf_dl_generalized))
      return false;
    std::unique_ptr<Buffer> data(buffer.getBuffer());
    out = data ? std::string(reinterpret_cast<const char *>(data->getBuffer()), data->getSize()) : std::string();
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

// The MIME type an attachment's name suggests.
std::string MimeType(const std::string &file) {
  static const std::map<std::string, std::string> types{
      {"pdf", "application/pdf"},  {"xml", "text/xml"},   {"txt", "text/plain"},  {"csv", "text/csv"},
      {"json", "application/json"}, {"html", "text/html"}, {"htm", "text/html"},   {"png", "image/png"},
      {"jpg", "image/jpeg"},        {"jpeg", "image/jpeg"}, {"gif", "image/gif"},  {"tif", "image/tiff"},
      {"tiff", "image/tiff"},       {"zip", "application/zip"}, {"doc", "application/msword"},
      {"xls", "application/vnd.ms-excel"}, {"ppt", "application/vnd.ms-powerpoint"},
      {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
      {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
      {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"}};
  const size_t dot = file.rfind('.');
  std::string extension = dot == std::string::npos ? "" : file.substr(dot + 1);
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const auto found = types.find(extension);
  return found == types.end() ? "application/octet-stream" : found->second;
}

class Pdfa {
public:
  Pdfa(QPDF &q, PdfaResult &result, const PdfaOptions &options)
      : q_(q), result_(result), part_(options.part), flatten_dpi_(options.flatten_dpi), fonts_dir_(options.fonts), root_(q.getRoot()) {}

  void Run() {
    // Predefined CMaps fonts name, read from the fonts folder's cmaps (for checking their codes).
    font::SetCMapFolder(fonts_dir_.empty() ? "" : fonts_dir_ + "/cmaps");
    pages_ = QPDFPageDocumentHelper(q_).getAllPages();
    result_.pages = static_cast<int>(pages_.size());
    Catalog();
    // FYNEPDF_PDFA_TRACE=1: each pass's time and the peak memory after it, on stderr (Linux).
    const bool trace = std::getenv("FYNEPDF_PDFA_TRACE") != nullptr;
    auto last = std::chrono::steady_clock::now();
    const auto stage = [&](const char *name) {
      if (!trace)
        return;
      const auto now = std::chrono::steady_clock::now();
      const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
      last = now;
      std::ifstream status("/proc/self/status");
      for (std::string line; std::getline(status, line);)
        if (line.rfind("VmHWM:", 0) == 0)
          std::fprintf(stderr, "%-12s %6lld ms %s\n", name, ms, line.substr(6).c_str());
    };
    stage("open");
    Colours();
    stage("colours");
    Forms();
    Pages();
    stage("pages");
    Layers();
    Attachments();
    Objects();
    stage("objects");
    Associate();
    Contents();
    stage("contents");
    Fonts();
    stage("fonts");
    AdjustText();
    if (part_ == 1)
      Transparency();
    OutputIntent();
    Metadata();
    stage("done");
  }

private:
  void Change(const std::string &kind, int count = 1) {
    if (count > 0)
      result_.changes[kind] += count;
  }

  void Problem(const std::string &code, const std::string &detail = "") {
    if (code == "content-unreadable")
      content_unreadable_ = true;
    const std::string text = detail.empty() ? code : code + ": " + detail;
    if (result_.problems.size() < 40 && std::find(result_.problems.begin(), result_.problems.end(), text) == result_.problems.end())
      result_.problems.push_back(text);
  }

  // Whether |object| (indirect) is seen for the first time under |tag|.
  bool First(QPDFObjectHandle object, int tag = 0) {
    return !object.isIndirect() || seen_.insert({object.getObjectID(), object.getGeneration() * 16 + tag}).second;
  }

  // -----------------------------------------------------------------------------------------
  // The catalog

  void Catalog() {
    for (const char *key : {"/AA", "/Requirements", "/Version"})
      if (root_.hasKey(key)) {
        root_.removeKey(key);
        Change(std::string("catalog-") + (key + 1));
      }
    QPDFObjectHandle names = root_.getKey("/Names");
    if (names.isDictionary()) {
      if (names.hasKey("/JavaScript")) {
        names.removeKey("/JavaScript");
        Change("javascript");
      }
      if (names.hasKey("/AlternatePresentations")) {
        names.removeKey("/AlternatePresentations");
        Change("alternate-presentations");
      }
    }
    QPDFObjectHandle open = root_.getKey("/OpenAction");
    if (open.isDictionary() && !Allowed(open)) {
      root_.removeKey("/OpenAction");
      Change("actions-removed");
    }
    // Permissions other than the two PDF/A-2 names.
    QPDFObjectHandle perms = root_.getKey("/Perms");
    if (perms.isDictionary() && part_ != 1)
      for (const std::string &key : perms.getKeys())
        if (key != "/DocMDP" && key != "/UR3") {
          perms.removeKey(key);
          Change("permissions");
        }
  }

  // -----------------------------------------------------------------------------------------
  // Actions

  bool Allowed(QPDFObjectHandle action, int depth = 0) {
    if (!action.isDictionary() || depth > 16)
      return true;
    const std::string type = Name(action.getKey("/S"));
    static const std::set<std::string> allowed1 = {"/GoTo", "/GoToR", "/Thread", "/URI", "/Named", "/SubmitForm"};
    static const std::set<std::string> allowed2 = {"/GoTo", "/GoToR", "/GoToE", "/Thread", "/URI", "/Named", "/SubmitForm"};
    if (!(part_ == 1 ? allowed1 : allowed2).count(type))
      return false;
    if (type == "/Named") {
      const std::string name = Name(action.getKey("/N"));
      if (name != "/NextPage" && name != "/PrevPage" && name != "/FirstPage" && name != "/LastPage")
        return false;
    }
    // Actions that follow: the forbidden ones dropped from the chain.
    QPDFObjectHandle next = action.getKey("/Next");
    if (next.isDictionary() && !Allowed(next, depth + 1)) {
      action.removeKey("/Next");
      Change("actions-removed");
    } else if (next.isArray()) {
      std::vector<QPDFObjectHandle> kept;
      for (QPDFObjectHandle item : next.getArrayAsVector())
        if (Allowed(item, depth + 1))
          kept.push_back(item);
      if (kept.size() != static_cast<size_t>(next.getArrayNItems())) {
        Change("actions-removed", next.getArrayNItems() - static_cast<int>(kept.size()));
        action.replaceKey("/Next", QPDFObjectHandle::newArray(kept));
      }
    }
    return true;
  }

  // -----------------------------------------------------------------------------------------
  // Colours: device CMYK can't be used with an sRGB output intent, so it becomes RGB.

  void Colours() {
    ColourStats stats;
    ColourOptions options;
    options.target = Target::Rgb;
    ColourConverter converter(q_, stats, options);
    for (QPDFPageObjectHelper &page : pages_)
      converter.Page(page);
    converter.FormDefaults();
    Change("cmyk-colours", stats.colours);
    Change("cmyk-images", stats.images + stats.inlineImages);
    Change("cmyk-gradients", stats.shadings);
    Change("spot-inks", stats.spots);
    if (stats.cmykLeft)
      Problem("cmyk", std::to_string(stats.cmykLeft) + " left");
    if (stats.contentKept)
      Problem("content-unreadable");
  }

  // -----------------------------------------------------------------------------------------
  // Forms

  void Forms() {
    QPDFObjectHandle form = root_.getKey("/AcroForm");
    const bool dynamic = root_.getKey("/NeedsRendering").isBool() && root_.getKey("/NeedsRendering").getBoolValue();
    root_.removeKey("/NeedsRendering");
    if (dynamic)
      Problem("xfa", "the form is only drawn by XFA");
    if (!form.isDictionary())
      return;
    if (form.hasKey("/XFA")) {
      form.removeKey("/XFA");
      Change("xfa");
    }
    // Appearances a reader would draw afresh: made again from the fields' values (Annotation).
    need_appearances_ = form.getKey("/NeedAppearances").isBool() && form.getKey("/NeedAppearances").getBoolValue();
    form.removeKey("/NeedAppearances");
    // Signed fields: the file can't stay signed once it's changed.
    std::vector<QPDFObjectHandle> fields;
    QPDFObjectHandle top = form.getKey("/Fields");
    for (int index = 0; top.isArray() && index < top.getArrayNItems(); ++index)
      fields.push_back(top.getArrayItem(index));
    std::set<QPDFObjGen> visited;
    while (!fields.empty()) {
      QPDFObjectHandle field = fields.back();
      fields.pop_back();
      if (!field.isDictionary() || (field.isIndirect() && !visited.insert(field.getObjGen()).second) || visited.size() > 100000)
        continue;
      if (IsName(field.getKey("/FT"), "/Sig") && field.getKey("/V").isDictionary()) {
        field.removeKey("/V");
        ++result_.signatures;
      }
      // PDF/A-1: fields have no actions.
      if (part_ == 1 && field.hasKey("/A")) {
        field.removeKey("/A");
        Change("actions-removed");
      }
      QPDFObjectHandle kids = field.getKey("/Kids");
      for (int index = 0; kids.isArray() && index < kids.getArrayNItems(); ++index)
        fields.push_back(kids.getArrayItem(index));
    }
    if (result_.signatures) {
      root_.removeKey("/Perms");
      Change("signatures", result_.signatures);
    }
  }

  // A widget's field type, inherited from its parents.
  static std::string FieldType(QPDFObjectHandle annotation) {
    QPDFObjectHandle at = annotation;
    for (int depth = 0; depth < 32 && at.isDictionary(); ++depth, at = at.getKey("/Parent"))
      if (at.getKey("/FT").isName())
        return at.getKey("/FT").getName();
    return "";
  }

  // -----------------------------------------------------------------------------------------
  // Pages and their comments

  // PDF/A-2's page sizes: 3 to 14,400 units each way. Boxes past the media box are cut to it (as
  // readers show them); a page still out of range is drawn in larger or smaller units (UserUnit),
  // its boxes and content scaled to them, so its size on paper stays. A page with comments, whose
  // places would have to move too, stays a problem.
  void PageSize(QPDFObjectHandle page) {
    static const char *const keys[] = {"/MediaBox", "/CropBox", "/BleedBox", "/TrimBox", "/ArtBox"};
    const auto normal = [](std::vector<double> box) {
      return std::vector<double>{std::min(box[0], box[2]), std::min(box[1], box[3]), std::max(box[0], box[2]), std::max(box[1], box[3])};
    };
    std::vector<double> media = Numbers(page.getKey("/MediaBox"));
    if (media.size() != 4)
      return;
    media = normal(media);
    for (const char *key : keys) {
      std::vector<double> box = Numbers(page.getKey(key));
      if (key == keys[0] || box.size() != 4)
        continue;
      box = normal(box);
      const std::vector<double> cut = {std::max(box[0], media[0]), std::max(box[1], media[1]), std::min(box[2], media[2]), std::min(box[3], media[3])};
      if (cut != box && cut[2] > cut[0] && cut[3] > cut[1]) {
        page.replaceKey(key, NumberArray(cut));
        Change("page-boxes");
      }
    }
    double smallest = 1e300, largest = 0;
    for (const char *key : keys) {
      const std::vector<double> box = Numbers(page.getKey(key));
      if (box.size() != 4)
        continue;
      for (const double side : {std::abs(box[2] - box[0]), std::abs(box[3] - box[1])}) {
        smallest = std::min(smallest, side);
        largest = std::max(largest, side);
      }
    }
    if (smallest >= 3 && largest <= 14400)
      return;
    // The scale that brings every side in: k units of the page make one of the new.
    const double scale = largest > 14400 ? largest / 14400 * 1.0001 : smallest / 3 / 1.0001;
    QPDFObjectHandle annotations = page.getKey("/Annots");
    if (smallest <= 0 || largest / scale > 14400 || smallest / scale < 3 || (annotations.isArray() && annotations.getArrayNItems() > 0)) {
      Problem("page-size");
      return;
    }
    for (const char *key : keys) {
      std::vector<double> box = Numbers(page.getKey(key));
      if (box.size() != 4)
        continue;
      for (double &value : box)
        value /= scale;
      page.replaceKey(key, NumberArray(box));
    }
    const double unit = page.getKey("/UserUnit").isNumber() ? page.getKey("/UserUnit").getNumericValue() : 1;
    page.replaceKey("/UserUnit", QPDFObjectHandle::newReal(unit * scale, 6, true));
    std::vector<QPDFObjectHandle> contents;
    char factor[32];
    std::snprintf(factor, sizeof factor, "%.8f", 1 / scale);
    contents.push_back(QPDFObjectHandle::newStream(&q_, std::string("q ") + factor + " 0 0 " + factor + " 0 0 cm\n"));
    QPDFObjectHandle old = page.getKey("/Contents");
    if (old.isArray())
      for (QPDFObjectHandle item : old.getArrayAsVector())
        contents.push_back(item);
    else if (old.isStream())
      contents.push_back(old);
    contents.push_back(QPDFObjectHandle::newStream(&q_, "\nQ\n"));
    page.replaceKey("/Contents", QPDFObjectHandle::newArray(contents));
    Change("page-units");
  }

  void Pages() {
    for (QPDFPageObjectHelper &helper : pages_) {
      QPDFObjectHandle page = helper.getObjectHandle();
      if (page.hasKey("/PresSteps")) {
        page.removeKey("/PresSteps");
        Change("presentation-steps");
      }
      if (part_ != 1)
        PageSize(page);
      QPDFObjectHandle annotations = page.getKey("/Annots");
      if (!annotations.isArray())
        continue;
      std::vector<QPDFObjectHandle> kept;
      std::set<QPDFObjGen> dropped;
      bool changed = false;
      for (QPDFObjectHandle annotation : annotations.getArrayAsVector()) {
        if (!annotation.isDictionary()) {
          changed = true;
          continue;
        }
        if (!Annotation(annotation)) {
          changed = true;
          QPDFObjectHandle popup = annotation.getKey("/Popup");
          if (popup.isIndirect())
            dropped.insert(popup.getObjGen());
          continue;
        }
        kept.push_back(annotation);
      }
      if (!dropped.empty()) {
        std::vector<QPDFObjectHandle> remaining;
        for (QPDFObjectHandle annotation : kept)
          if (!annotation.isIndirect() || !dropped.count(annotation.getObjGen()))
            remaining.push_back(annotation);
        kept.swap(remaining);
      }
      if (changed || !dropped.empty())
        page.replaceKey("/Annots", QPDFObjectHandle::newArray(kept));
    }
  }

  // A comment made PDF/A; false to drop it.
  bool Annotation(QPDFObjectHandle annotation) {
    const std::string subtype = Name(annotation.getKey("/Subtype"));
    if (!(part_ == 1 ? kAnnotations1 : kAnnotations2).count(subtype)) {
      Change("annotations-removed");
      return false;
    }
    if (subtype == "/FileAttachment" && !Attachment(annotation.getKey("/FS"))) {
      Change("attachments-removed");
      return false;
    }
    // A link's action; form fields have none (and no actions on events, which nothing has).
    QPDFObjectHandle action = annotation.getKey("/A");
    if (action.isDictionary() && (!Allowed(action) || subtype == "/Widget")) {
      annotation.removeKey("/A");
      Change("actions-removed");
    }
    // Flags: printed, never hidden. A hidden comment keeps being hidden by drawing nothing.
    QPDFObjectHandle flags_object = annotation.getKey("/F");
    const int flags = flags_object.isInteger() ? flags_object.getIntValueAsInt() : 0;
    const bool hidden = (flags & (2 | 32)) != 0;
    const int fixed = (flags | 4) & ~(1 | 2 | 32 | 256);
    if (subtype != "/Popup" || flags_object.isInteger()) {
      if (!flags_object.isInteger() || fixed != flags) {
        annotation.replaceKey("/F", QPDFObjectHandle::newInteger(fixed));
        Change("annotation-flags");
      }
    }
    if (part_ == 1) {
      QPDFObjectHandle opacity = annotation.getKey("/CA");
      if (opacity.isNumber() && opacity.getNumericValue() < 1)
        transparent_ = true;
    }
    // Appearances: only the normal one; buttons by state, everything else a stream.
    QPDFObjectHandle appearance = annotation.getKey("/AP");
    if (appearance.isDictionary()) {
      for (const char *key : {"/R", "/D"})
        if (appearance.hasKey(key)) {
          appearance.removeKey(key);
          Change("appearance-states");
        }
      for (const std::string &key : appearance.getKeys())
        if (key != "/N")
          appearance.removeKey(key);
    }
    if (subtype == "/Popup")
      return true;
    const bool button = subtype == "/Widget" && FieldType(annotation) == "/Btn";
    QPDFObjectHandle normal = appearance.isDictionary() ? appearance.getKey("/N") : QPDFObjectHandle::newNull();
    const std::vector<double> rect = Numbers(annotation.getKey("/Rect"));
    const bool empty = rect.size() == 4 && rect[0] == rect[2] && rect[1] == rect[3];
    if (hidden) {
      SetAppearance(annotation, EmptyAppearance(q_), button);
      Change("hidden-annotations");
      return true;
    }
    if (normal.isDictionary() && !button) {
      // A state dictionary on something that isn't a button: its current state's appearance.
      QPDFObjectHandle chosen = normal.getKey(Name(annotation.getKey("/AS")));
      for (const std::string &key : normal.getKeys())
        if (!chosen.isStream())
          chosen = normal.getKey(key);
      normal = chosen.isStream() ? chosen : QPDFObjectHandle::newNull();
      if (normal.isStream()) {
        SetAppearance(annotation, normal, false);
        Change("appearance-states");
      }
    } else if (normal.isStream() && button) {
      SetAppearance(annotation, normal, true);
      Change("appearance-states");
    }
    const bool has = button ? normal.isDictionary() || normal.isStream() : normal.isStream();
    // A form that asks for its fields' appearances to be drawn: text and choices drawn again
    // from their values (the ones it has may be out of date; kept when one can't be drawn).
    const std::string field = subtype == "/Widget" ? FieldType(annotation) : "";
    const bool redraw = need_appearances_ && (field == "/Tx" || field == "/Ch");
    if ((!has || redraw) && subtype != "/Link" && !empty) {
      // (PDF/A-1 drawing pages that blend as pictures can take a highlight's multiply too.)
      QPDFObjectHandle made = MakeAppearance(q_, annotation, root_.getKey("/AcroForm"), part_ != 1 || Flattening(), [&](const std::string &base) { return FieldFont(base); });
      if (made.isNull()) {
        if (!has)
          Problem("no-appearance", subtype.substr(1));
      } else if (made.isDictionary()) {
        QPDFObjectHandle states = QPDFObjectHandle::newDictionary();
        states.replaceKey("/N", made);
        annotation.replaceKey("/AP", states);
        Change(field.empty() ? "appearances-made" : "field-appearances");
      } else {
        SetAppearance(annotation, made, button);
        Change(field.empty() ? "appearances-made" : "field-appearances");
      }
    }
    return true;
  }

  // The font form fields and comments drawn here show their text in: the free font of the same
  // widths as the one they name (as a font dictionary naming it, which Fonts() embeds), or
  // Helvetica, Times or Courier in its style. None without the substitute fonts.
  TextFont FieldFont(const std::string &base) {
    TextFont out;
    if (fonts_dir_.empty())
      return out;
    std::string name = base.size() > 7 && base[6] == '+' ? base.substr(7) : base;
    std::string file = font::SubstituteFile("/" + name, QPDFObjectHandle::newNull());
    if (file.empty()) {
      std::string lower;
      for (const char c : name)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      const auto has = [&](const char *word) { return lower.find(word) != std::string::npos; };
      const bool bold = has("bold") || has("black") || has("heavy"), italic = has("italic") || has("oblique");
      if (has("courier") || has("mono"))
        name = std::string("Courier") + (bold && italic ? "-BoldOblique" : bold ? "-Bold" : italic ? "-Oblique" : "");
      else if ((has("times") || has("serif")) && !has("sans"))
        name = bold && italic ? "Times-BoldItalic" : bold ? "Times-Bold" : italic ? "Times-Italic" : "Times-Roman";
      else
        name = std::string("Helvetica") + (bold && italic ? "-BoldOblique" : bold ? "-Bold" : italic ? "-Oblique" : "");
      file = font::SubstituteFile("/" + name, QPDFObjectHandle::newNull());
    }
    const font::Program *program = SubstituteProgram(file);
    if (!program)
      return out;
    QPDFObjectHandle &made = field_fonts_[name];
    if (!made.isInitialized() || made.isNull()) {
      QPDFObjectHandle font = QPDFObjectHandle::newDictionary();
      font.replaceKey("/Type", QPDFObjectHandle::newName("/Font"));
      font.replaceKey("/Subtype", QPDFObjectHandle::newName("/Type1"));
      font.replaceKey("/BaseFont", QPDFObjectHandle::newName("/" + name));
      font.replaceKey("/Encoding", QPDFObjectHandle::newName("/WinAnsiEncoding"));
      made = q_.makeIndirectObject(font);
    }
    out.font = made;
    out.width = [program](unsigned code) {
      const char *glyph_name = font::Encoding("/WinAnsiEncoding")[code & 0xff];
      const unsigned character = glyph_name ? font::UnicodeOf(glyph_name) : 0;
      const int glyph = character ? program->ByCmap(3, 1, character) : 0;
      return glyph > 0 ? program->Width(glyph) : -1.0;
    };
    return out;
  }

  // The normal appearance; a button's as its current state (or "Off").
  void SetAppearance(QPDFObjectHandle annotation, QPDFObjectHandle stream, bool button) {
    QPDFObjectHandle appearance = QPDFObjectHandle::newDictionary();
    if (button) {
      std::string state = Name(annotation.getKey("/AS"));
      if (state.empty()) {
        state = "/Off";
        annotation.replaceKey("/AS", QPDFObjectHandle::newName(state));
      }
      QPDFObjectHandle states = QPDFObjectHandle::newDictionary();
      states.replaceKey(state, stream);
      appearance.replaceKey("/N", states);
    } else {
      appearance.replaceKey("/N", stream);
    }
    annotation.replaceKey("/AP", appearance);
  }

  // -----------------------------------------------------------------------------------------
  // Layers (optional content)

  // PDF/A-1: whether a layer (or a membership of layers) is hidden in the default view.
  bool Hidden(QPDFObjectHandle oc) {
    if (!oc.isDictionary() || hidden_.empty())
      return false;
    if (!IsName(oc.getKey("/Type"), "/OCMD"))
      return oc.isIndirect() && hidden_.count(oc.getObjGen());
    QPDFObjectHandle groups = oc.getKey("/OCGs");
    std::vector<QPDFObjectHandle> list = groups.isArray() ? groups.getArrayAsVector() : std::vector<QPDFObjectHandle>{groups};
    int shown = 0, total = 0;
    for (QPDFObjectHandle group : list)
      if (group.isDictionary()) {
        ++total;
        shown += group.isIndirect() && hidden_.count(group.getObjGen()) ? 0 : 1;
      }
    if (total == 0)
      return false;
    const std::string policy = Name(oc.getKey("/P"));
    if (policy == "/AllOn")
      return shown < total;
    if (policy == "/AnyOff")
      return shown == total;
    if (policy == "/AllOff")
      return shown > 0;
    return shown == 0;  // AnyOn, the default
  }

  void Layers() {
    QPDFObjectHandle properties = root_.getKey("/OCProperties");
    if (!properties.isDictionary())
      return;
    QPDFObjectHandle defaults = properties.getKey("/D");
    if (part_ == 1) {
      // PDF 1.4 has no layers: everything would show. What the default view hides is taken out
      // instead (content in Contents, comments here).
      QPDFObjectHandle all = properties.getKey("/OCGs");
      const bool base_off = defaults.isDictionary() && IsName(defaults.getKey("/BaseState"), "/OFF");
      const auto list = [&](const char *key) {
        std::set<QPDFObjGen> out;
        QPDFObjectHandle array = defaults.isDictionary() ? defaults.getKey(key) : QPDFObjectHandle::newNull();
        for (int index = 0; array.isArray() && index < array.getArrayNItems(); ++index)
          if (array.getArrayItem(index).isIndirect())
            out.insert(array.getArrayItem(index).getObjGen());
        return out;
      };
      const std::set<QPDFObjGen> on = list("/ON");
      hidden_ = list("/OFF");
      for (int index = 0; base_off && all.isArray() && index < all.getArrayNItems(); ++index)
        if (all.getArrayItem(index).isIndirect() && !on.count(all.getArrayItem(index).getObjGen()))
          hidden_.insert(all.getArrayItem(index).getObjGen());
      for (QPDFPageObjectHelper &helper : pages_) {
        QPDFObjectHandle page = helper.getObjectHandle();
        QPDFObjectHandle annotations = page.getKey("/Annots");
        if (!annotations.isArray())
          continue;
        std::vector<QPDFObjectHandle> kept;
        for (QPDFObjectHandle annotation : annotations.getArrayAsVector())
          if (!annotation.isDictionary() || !Hidden(annotation.getKey("/OC")))
            kept.push_back(annotation);
        if (kept.size() != static_cast<size_t>(annotations.getArrayNItems())) {
          Change("hidden-comments-removed", annotations.getArrayNItems() - static_cast<int>(kept.size()));
          page.replaceKey("/Annots", QPDFObjectHandle::newArray(kept));
        }
      }
      root_.removeKey("/OCProperties");
      Change("layers");
      return;
    }
    QPDFObjectHandle groups = properties.getKey("/OCGs");
    std::vector<QPDFObjectHandle> all = groups.isArray() ? groups.getArrayAsVector() : std::vector<QPDFObjectHandle>{};
    int number = 0;
    for (QPDFObjectHandle group : all) {
      ++number;
      if (group.isDictionary() && !group.getKey("/Name").isString()) {
        group.replaceKey("/Name", QPDFObjectHandle::newUnicodeString("Layer " + std::to_string(number)));
        Change("layer-names");
      }
    }
    std::vector<QPDFObjectHandle> configs;
    if (!defaults.isDictionary()) {
      defaults = QPDFObjectHandle::newDictionary();
      properties.replaceKey("/D", defaults);
    }
    configs.push_back(defaults);
    QPDFObjectHandle more = properties.getKey("/Configs");
    for (int index = 0; more.isArray() && index < more.getArrayNItems(); ++index)
      if (more.getArrayItem(index).isDictionary())
        configs.push_back(more.getArrayItem(index));
    std::set<std::string> names;
    for (size_t at = 0; at < configs.size(); ++at) {
      QPDFObjectHandle config = configs[at];
      std::string name = Text(config.getKey("/Name"));
      if (name.empty() || names.count(name)) {
        std::string made = at == 0 ? "Default" : "Configuration " + std::to_string(at);
        while (names.count(made))
          made += " (" + std::to_string(at) + ")";
        config.replaceKey("/Name", QPDFObjectHandle::newUnicodeString(made));
        name = made;
        Change("layer-names");
      }
      names.insert(name);
      if (config.hasKey("/AS")) {
        config.removeKey("/AS");
        Change("layer-usage");
      }
      // A layer order lists every layer.
      QPDFObjectHandle order = config.getKey("/Order");
      if (!order.isArray())
        continue;
      std::set<QPDFObjGen> listed;
      std::function<void(QPDFObjectHandle, int)> collect = [&](QPDFObjectHandle item, int depth) {
        if (depth > 32)
          return;
        if (item.isArray())
          for (QPDFObjectHandle child : item.getArrayAsVector())
            collect(child, depth + 1);
        else if (item.isIndirect())
          listed.insert(item.getObjGen());
      };
      collect(order, 0);
      for (QPDFObjectHandle group : all)
        if (group.isIndirect() && !listed.count(group.getObjGen())) {
          order.appendItem(group);
          Change("layer-order");
        }
    }
  }

  // -----------------------------------------------------------------------------------------
  // Attachments

  // Whether the file another file specification embeds can stay (made fit for PDF/A-3 in
  // place); false to drop it.
  bool Attachment(QPDFObjectHandle spec) {
    if (spec.isIndirect()) {
      const auto found = verdicts_.find(spec.getObjGen());
      if (found != verdicts_.end())
        return found->second;
      const bool verdict = Judge(spec);
      verdicts_.emplace(spec.getObjGen(), verdict);
      return verdict;
    }
    return Judge(spec);
  }

  bool Judge(QPDFObjectHandle spec) {
    if (!spec.isDictionary())
      return part_ != 1;
    QPDFObjectHandle embedded = spec.getKey("/EF");
    QPDFObjectHandle stream = embedded.isDictionary() ? embedded.getKey("/F") : QPDFObjectHandle::newNull();
    if (!stream.isStream() && embedded.isDictionary())
      stream = embedded.getKey("/UF");
    if (!stream.isStream())
      return part_ != 1;  // a reference to a file outside, nothing embedded
    if (part_ == 1)
      return false;
    if (part_ == 2) {
      std::string data;
      if (!Decoded(stream, data))
        return false;
      // A PDF/A-1 or 2 file, by what it says it is.
      if (data.compare(0, 5, "%PDF-") != 0)
        return false;
      for (size_t at = data.find("pdfaid:part"); at != std::string::npos; at = data.find("pdfaid:part", at + 1)) {
        size_t pos = at + 11;
        while (pos < data.size() && (data[pos] == '>' || data[pos] == '=' || data[pos] == '"' || data[pos] == '\'' || std::isspace(static_cast<unsigned char>(data[pos]))))
          ++pos;
        if (pos < data.size() && (data[pos] == '1' || data[pos] == '2'))
          return true;
      }
      return false;
    }
    // PDF/A-3: any file, with a name (F and UF), a type and its relationship to the document.
    std::string name = Text(spec.getKey("/UF"));
    if (name.empty())
      name = Text(spec.getKey("/F"));
    if (name.empty())
      name = "attachment";
    for (const char *key : {"/F", "/UF"})
      if (!spec.getKey(key).isString()) {
        spec.replaceKey(key, QPDFObjectHandle::newUnicodeString(name));
        Change("attachment-names");
      }
    if (!spec.getKey("/AFRelationship").isName()) {
      spec.replaceKey("/AFRelationship", QPDFObjectHandle::newName("/Unspecified"));
      Change("attachment-relationships");
    }
    QPDFObjectHandle dict = stream.getDict();
    if (!dict.getKey("/Subtype").isName()) {
      dict.replaceKey("/Subtype", QPDFObjectHandle::newName("/" + MimeType(name)));
      Change("attachment-types");
    }
    if (spec.isIndirect())
      associated_.push_back(spec);
    return true;
  }

  void Attachments() {
    QPDFObjectHandle names = root_.getKey("/Names");
    QPDFObjectHandle tree = names.isDictionary() ? names.getKey("/EmbeddedFiles") : QPDFObjectHandle::newNull();
    if (tree.isDictionary()) {
      if (part_ == 1) {
        names.removeKey("/EmbeddedFiles");
        Change("attachments-removed");
      } else {
        // The tree read as it is (its keys kept exactly) and written back flat, without the
        // attachments that can't stay.
        std::vector<QPDFObjectHandle> kept;
        int dropped = 0;
        std::set<QPDFObjGen> visited;
        std::function<void(QPDFObjectHandle, int)> walk = [&](QPDFObjectHandle node, int depth) {
          if (!node.isDictionary() || depth > 32 || (node.isIndirect() && !visited.insert(node.getObjGen()).second))
            return;
          QPDFObjectHandle pairs = node.getKey("/Names");
          for (int at = 0; pairs.isArray() && at + 1 < pairs.getArrayNItems(); at += 2) {
            if (Attachment(pairs.getArrayItem(at + 1))) {
              kept.push_back(pairs.getArrayItem(at));
              kept.push_back(pairs.getArrayItem(at + 1));
            } else {
              ++dropped;
            }
          }
          QPDFObjectHandle kids = node.getKey("/Kids");
          for (int at = 0; kids.isArray() && at < kids.getArrayNItems(); ++at)
            walk(kids.getArrayItem(at), depth + 1);
        };
        walk(tree, 0);
        if (dropped) {
          QPDFObjectHandle flat = QPDFObjectHandle::newDictionary();
          flat.replaceKey("/Names", QPDFObjectHandle::newArray(kept));
          names.replaceKey("/EmbeddedFiles", flat);
          Change("attachments-removed", dropped);
        }
      }
    }
  }

  // -----------------------------------------------------------------------------------------
  // Every object: streams, pictures, forms, graphics states, font descriptors, actions on events

  // What the file will hold: the objects the catalog and Info dictionary reach (the writer
  // leaves out the rest, such as signatures just removed).
  void Objects() {
    Queue(root_);
    QPDFObjectHandle info = q_.getTrailer().getKey("/Info");
    if (info.isIndirect())
      Queue(info);
    else if (info.isDictionary())
      Scan(info, 0);
    while (!pending_.empty()) {
      QPDFObjectHandle object = pending_.back();
      pending_.pop_back();
      Scan(object, 0);
    }
  }

  void Queue(QPDFObjectHandle object) {
    if (object.isIndirect() && scanned_.insert(object.getObjGen()).second)
      pending_.push_back(object);
  }

  // PDF/A-3: the attachments kept, in the catalog's list of associated files.
  void Associate() {
    if (part_ != 3 || associated_.empty())
      return;
    QPDFObjectHandle list = root_.getKey("/AF");
    std::set<QPDFObjGen> present;
    if (!list.isArray()) {
      list = QPDFObjectHandle::newArray();
      root_.replaceKey("/AF", list);
    }
    for (QPDFObjectHandle item : list.getArrayAsVector())
      if (item.isIndirect())
        present.insert(item.getObjGen());
    for (QPDFObjectHandle spec : associated_)
      if (present.insert(spec.getObjGen()).second)
        list.appendItem(spec);
  }

  // A number, string or name within PDF/A's limits (PDF/A-1 6.1.12, PDF/A-2 6.1.13): what
  // replaces it (an integer past them clamped, which no reader can use as it is; a name too long
  // or not UTF-8 renamed, as its uses are), null to keep it.
  QPDFObjectHandle Limits(QPDFObjectHandle value) {
    if (value.isInteger()) {
      const long long number = value.getIntValue();
      if (number > 2147483647LL || number < -2147483648LL) {
        Change("numbers-clamped");
        return QPDFObjectHandle::newInteger(number > 0 ? 2147483647LL : -2147483648LL);
      }
    } else if (value.isReal()) {
      if (part_ == 1 && std::abs(value.getNumericValue()) > 32767)
        Problem("limits", "real");
    } else if (value.isString()) {
      if (value.getStringValue().size() > (part_ == 1 ? 65535u : 32767u))
        Problem("limits", "string");
    } else if (value.isName()) {
      const std::string fixed = FixedName(value.getName());
      if (fixed != value.getName()) {
        Change("names");
        return QPDFObjectHandle::newName(fixed);
      }
    }
    return QPDFObjectHandle::newNull();
  }

  // An ink name, a structure type or a font's name as UTF-8 (PDF/A-2 6.1.8).
  void Utf8Name(QPDFObjectHandle holder, const std::string &key) {
    QPDFObjectHandle value = holder.getKey(key);
    if (value.isName() && FixedName(value.getName()) != value.getName()) {
      holder.replaceKey(key, QPDFObjectHandle::newName(FixedName(value.getName())));
      Change("names");
    }
  }

  void Scan(QPDFObjectHandle object, int depth) {
    if (depth > 64)
      return;
    if (object.isArray()) {
      const int count = object.getArrayNItems();
      if (part_ == 1 && count > 8191)
        Problem("limits", "array");
      if (count >= 2 && IsName(object.getArrayItem(0), "/ICCBased") && object.getArrayItem(1).isStream() && !Profile(object.getArrayItem(1)))
        RepairProfile(object);
      if (count == 4 && IsName(object.getArrayItem(0), "/Separation"))
        Separation(object);
      if (count >= 4 && IsName(object.getArrayItem(0), "/DeviceN"))
        DeviceN(object);
      for (int index = 0; index < count; ++index) {
        QPDFObjectHandle item = object.getArrayItem(index);
        if (item.isIndirect()) {
          Queue(item);
          continue;
        }
        if (item.isArray() || item.isDictionary())
          Scan(item, depth + 1);
        else if (QPDFObjectHandle limited = Limits(item); !limited.isNull())
          object.setArrayItem(index, limited);
      }
      return;
    }
    if (!object.isDictionary() && !object.isStream())
      return;
    QPDFObjectHandle dict = object.isStream() ? object.getDict() : object;
    if (object.isStream())
      Stream(object);
    if (part_ == 1 && dict.getKeys().size() > 4095)
      Problem("limits", "dictionary");
    if (dict.hasKey("/AA")) {
      dict.removeKey("/AA");
      Change("event-actions");
    }
    // A file specification anywhere (a list of associated files, a link) embeds only what the
    // part allows.
    if (!object.isStream() && dict.getKey("/EF").isDictionary() && !Attachment(dict)) {
      dict.removeKey("/EF");
      Change("attachments-removed");
    }
    // Metadata of parts of the document (a picture's from Photoshop, a page's from InDesign)
    // uses schemas PDF/A would need described; it goes, the document's own stays.
    if (dict.getKey("/Metadata").isStream() && !(dict.isIndirect() && dict.getObjGen() == root_.getObjGen())) {
      dict.removeKey("/Metadata");
      Change("object-metadata");
    }
    // Fonts written into resources directly: indirect (identical ones shared), so a fix to one
    // reaches every page that has it.
    QPDFObjectHandle font_group = dict.getKey("/Font");
    if (font_group.isDictionary() && !object.isStream())
      for (const std::string &key : font_group.getKeys()) {
        QPDFObjectHandle value = font_group.getKey(key);
        if (value.isIndirect() || !value.isDictionary())
          continue;
        const std::string text = value.unparse();
        auto shared = direct_fonts_.find(text);
        if (shared == direct_fonts_.end())
          shared = direct_fonts_.emplace(text, q_.makeIndirectObject(value)).first;
        font_group.replaceKey(key, shared->second);
      }
    // Resource names content uses, font names and structure types: UTF-8.
    for (const char *kind : kResourceKinds) {
      QPDFObjectHandle group = dict.getKey(kind);
      if (!group.isDictionary())
        continue;
      for (const std::string &key : group.getKeys())
        if (FixedName(key) != key) {
          QPDFObjectHandle value = group.getKey(key);
          group.removeKey(key);
          group.replaceKey(FixedName(key), value);
          Change("names");
        }
    }
    const std::string type = Name(dict.getKey("/Type"));
    if (type == "/Font" || IsName(dict.getKey("/Subtype"), "/CIDFontType2") || IsName(dict.getKey("/Subtype"), "/CIDFontType0"))
      Utf8Name(dict, "/BaseFont");
    if (type == "/FontDescriptor")
      Utf8Name(dict, "/FontName");
    if (type == "/StructElem" || (dict.hasKey("/S") && dict.hasKey("/P") && dict.hasKey("/K")))
      Utf8Name(dict, "/S");
    QPDFObjectHandle roles = dict.getKey("/RoleMap");
    for (const std::string &key : roles.isDictionary() ? roles.getKeys() : std::set<std::string>{}) {
      Utf8Name(roles, key);
      if (FixedName(key) != key) {
        QPDFObjectHandle value = roles.getKey(key);
        roles.removeKey(key);
        roles.replaceKey(FixedName(key), value);
        Change("names");
      }
    }
    QPDFObjectHandle action = dict.getKey("/A");
    if (action.isDictionary() && action.hasKey("/S") && !Allowed(action)) {
      dict.removeKey("/A");
      Change("actions-removed");
    }
    QPDFObjectHandle states = dict.getKey("/ExtGState");
    for (const std::string &key : states.isDictionary() ? states.getKeys() : std::set<std::string>{})
      State(states.getKey(key));
    if (IsName(dict.getKey("/Type"), "/FontDescriptor") && part_ != 1)
      for (const char *key : {"/CIDSet", "/CharSet"})
        if (dict.hasKey(key)) {
          // Optional, and often wrong about the font they describe.
          dict.removeKey(key);
          Change("font-glyph-lists");
        }
    if (IsName(dict.getKey("/Subtype"), "/CIDFontType2") && !dict.hasKey("/CIDToGIDMap")) {
      dict.replaceKey("/CIDToGIDMap", QPDFObjectHandle::newName("/Identity"));  // the default, written out
      Change("cid-maps");
    }
    for (std::string key : dict.getKeys()) {
      QPDFObjectHandle value = dict.getKey(key);
      if (key.size() > 128) {
        // No key PDF defines is this long: renamed, as a resource's would be.
        dict.removeKey(key);
        key = FixedName(key);
        dict.replaceKey(key, value);
        Change("names");
      }
      if (value.isIndirect()) {
        Queue(value);
        continue;
      }
      if (value.isArray() || value.isDictionary())
        Scan(value, depth + 1);
      else if (QPDFObjectHandle limited = Limits(value); !limited.isNull())
        dict.replaceKey(key, limited);
    }
  }

  // Inks of the same name: one alternate and tint transform, the first's (PDF/A-2 6.2.4.4).
  void Separation(QPDFObjectHandle space) {
    QPDFObjectHandle name = space.getArrayItem(1);
    if (!name.isName())
      return;
    if (!ValidUtf8(name.getName())) {
      space.setArrayItem(1, QPDFObjectHandle::newName(FixedName(name.getName())));
      Change("names");
    }
    const std::string ink = FixedName(name.getName());
    const auto found = inks_.find(ink);
    if (found == inks_.end()) {
      inks_.emplace(ink, std::make_pair(space.getArrayItem(2), space.getArrayItem(3)));
      return;
    }
    if (part_ != 1 && (space.getArrayItem(2).unparse() != found->second.first.unparse() ||
                       space.getArrayItem(3).unparse() != found->second.second.unparse())) {
      space.setArrayItem(2, found->second.first);
      space.setArrayItem(3, found->second.second);
      Change("spot-inks-unified");
    }
  }

  void DeviceN(QPDFObjectHandle space) {
    QPDFObjectHandle names = space.getArrayItem(1);
    if (!names.isArray())
      return;
    if (names.getArrayNItems() > (part_ == 1 ? 8 : 32))
      Problem("too-many-inks");
    for (int index = 0; index < names.getArrayNItems(); ++index)
      if (names.getArrayItem(index).isName() && !ValidUtf8(names.getArrayItem(index).getName())) {
        names.setArrayItem(index, QPDFObjectHandle::newName(FixedName(names.getArrayItem(index).getName())));
        Change("names");
      }
    QPDFObjectHandle attributes = space.getArrayNItems() > 4 ? space.getArrayItem(4) : QPDFObjectHandle::newNull();
    QPDFObjectHandle colorants = attributes.isDictionary() ? attributes.getKey("/Colorants") : QPDFObjectHandle::newNull();
    QPDFObjectHandle process = attributes.isDictionary() ? attributes.getKey("/Process") : QPDFObjectHandle::newNull();
    if (colorants.isDictionary())
      for (const std::string &key : colorants.getKeys())
        if (!ValidUtf8(key)) {
          QPDFObjectHandle value = colorants.getKey(key);
          colorants.removeKey(key);
          colorants.replaceKey(FixedName(key), value);
          Change("names");
        }
    // Process inks in device CMYK (NChannel) can't stay with an RGB output intent: they are
    // drawn through the alternate, which is RGB by now, like the other inks.
    if (process.isDictionary() && IsName(process.getKey("/ColorSpace"), "/DeviceCMYK") && part_ != 1) {
      attributes.removeKey("/Process");
      if (IsName(attributes.getKey("/Subtype"), "/NChannel"))
        attributes.replaceKey("/Subtype", QPDFObjectHandle::newName("/DeviceN"));
      Change("process-inks");
      process = QPDFObjectHandle::newNull();
    }
    if (part_ == 1)
      return;
    // Every spot ink described (PDF/A-2 6.2.4.4).
    std::set<std::string> known = {"/Cyan", "/Magenta", "/Yellow", "/Black", "/None", "/All"};
    QPDFObjectHandle components = process.isDictionary() ? process.getKey("/Components") : QPDFObjectHandle::newNull();
    for (int index = 0; components.isArray() && index < components.getArrayNItems(); ++index)
      known.insert(Name(components.getArrayItem(index)));
    for (int index = 0; index < names.getArrayNItems(); ++index) {
      const std::string ink = Name(names.getArrayItem(index));
      if (known.count(ink) || (colorants.isDictionary() && colorants.hasKey(ink)))
        continue;
      // Described from the space itself: the ink alone through its tint transform.
      QPDFObjectHandle made = InkSeparation(space, index, ink);
      if (!made.isArray()) {
        Problem("spot-colours", "undescribed ink");
        continue;
      }
      if (!attributes.isDictionary()) {
        attributes = QPDFObjectHandle::newDictionary();
        while (space.getArrayNItems() < 4)
          space.appendItem(QPDFObjectHandle::newNull());
        if (space.getArrayNItems() == 4)
          space.appendItem(attributes);
        else
          space.setArrayItem(4, attributes);
      }
      if (!colorants.isDictionary()) {
        colorants = QPDFObjectHandle::newDictionary();
        attributes.replaceKey("/Colorants", colorants);
      }
      colorants.replaceKey(ink, made);
      Change("inks-described");
    }
  }

  // One ink of a DeviceN space as a Separation space: the DeviceN's tint transform with that ink
  // alone, sampled (256 steps) into its alternate, a device space. Null when it can't be.
  QPDFObjectHandle InkSeparation(QPDFObjectHandle space, int index, const std::string &ink) {
    QPDFObjectHandle alternate = space.getArrayItem(2);
    const int components = IsName(alternate, "/DeviceGray") ? 1 : IsName(alternate, "/DeviceRGB") ? 3 : IsName(alternate, "/DeviceCMYK") ? 4 : 0;
    const std::unique_ptr<PdfFunction> transform = PdfFunction::Read(space.getArrayItem(3));
    if (!components || !transform)
      return QPDFObjectHandle::newNull();
    std::vector<double> in(static_cast<size_t>(space.getArrayItem(1).getArrayNItems()), 0.0), out;
    std::string data;
    for (int step = 0; step < 256; ++step) {
      in[static_cast<size_t>(index)] = step / 255.0;
      if (!transform->Evaluate(in, out) || out.size() != static_cast<size_t>(components))
        return QPDFObjectHandle::newNull();
      for (const double value : out) {
        const long sample = std::lround(std::clamp(value, 0.0, 1.0) * 65535);
        data += static_cast<char>(sample >> 8);
        data += static_cast<char>(sample & 0xff);
      }
    }
    QPDFObjectHandle function = QPDFObjectHandle::newStream(&q_, data);
    QPDFObjectHandle dict = function.getDict();
    dict.replaceKey("/FunctionType", QPDFObjectHandle::newInteger(0));
    dict.replaceKey("/Domain", QPDFObjectHandle::parse("[0 1]"));
    std::vector<double> range;
    for (int k = 0; k < components; ++k)
      range.insert(range.end(), {0, 1});
    dict.replaceKey("/Range", NumberArray(range));
    dict.replaceKey("/Size", QPDFObjectHandle::parse("[256]"));
    dict.replaceKey("/BitsPerSample", QPDFObjectHandle::newInteger(16));
    return QPDFObjectHandle::newArray({QPDFObjectHandle::newName("/Separation"), QPDFObjectHandle::newName(ink), alternate, function});
  }

  void Stream(QPDFObjectHandle stream) {
    QPDFObjectHandle dict = stream.getDict();
    for (const char *key : {"/F", "/FFilter", "/FDecodeParms"})
      if (dict.hasKey(key)) {
        dict.removeKey(key);
        Change("external-streams");
      }
    // Filter names in full (the abbreviations are for inline pictures only), and only ones PDF
    // defines.
    static const std::map<std::string, std::string> full = {{"/A85", "/ASCII85Decode"}, {"/AHx", "/ASCIIHexDecode"}, {"/LZW", "/LZWDecode"},
                                                             {"/Fl", "/FlateDecode"},    {"/RL", "/RunLengthDecode"},  {"/CCF", "/CCITTFaxDecode"},
                                                             {"/DCT", "/DCTDecode"}};
    static const std::set<std::string> known = {"/ASCIIHexDecode", "/ASCII85Decode", "/LZWDecode", "/FlateDecode", "/RunLengthDecode",
                                                 "/CCITTFaxDecode", "/JBIG2Decode", "/DCTDecode", "/JPXDecode", "/Crypt"};
    QPDFObjectHandle filter = dict.getKey("/Filter");
    std::vector<QPDFObjectHandle> filters = filter.isArray() ? filter.getArrayAsVector() : std::vector<QPDFObjectHandle>{filter};
    // The Identity crypt filter changes nothing: it goes, with its parameters.
    QPDFObjectHandle parms = dict.getKey("/DecodeParms");
    std::vector<QPDFObjectHandle> parameters = parms.isArray() ? parms.getArrayAsVector() : std::vector<QPDFObjectHandle>{parms};
    parameters.resize(filters.size(), QPDFObjectHandle::newNull());
    for (size_t at = filters.size(); at-- > 0;) {
      QPDFObjectHandle name = parameters[at].isDictionary() ? parameters[at].getKey("/Name") : QPDFObjectHandle::newNull();
      if (IsName(filters[at], "/Crypt") && (name.isNull() || IsName(name, "/Identity"))) {
        filters.erase(filters.begin() + static_cast<long>(at));
        parameters.erase(parameters.begin() + static_cast<long>(at));
        const bool any = std::any_of(parameters.begin(), parameters.end(), [](QPDFObjectHandle p) { return !p.isNull(); });
        dict.replaceKey("/Filter", filters.empty() ? QPDFObjectHandle::newNull() : filters.size() == 1 ? filters[0] : QPDFObjectHandle::newArray(filters));
        dict.replaceKey("/DecodeParms", !any ? QPDFObjectHandle::newNull() : parameters.size() == 1 ? parameters[0] : QPDFObjectHandle::newArray(parameters));
        filter = dict.getKey("/Filter");
        Change("crypt-filters");
      }
    }
    if (filter.isNull())
      dict.removeKey("/Filter");
    if (dict.getKey("/DecodeParms").isNull())
      dict.removeKey("/DecodeParms");
    bool renamed = false;
    for (QPDFObjectHandle &item : filters) {
      const auto found = full.find(Name(item));
      if (found != full.end()) {
        item = QPDFObjectHandle::newName(found->second);
        renamed = true;
      } else if (item.isName() && !known.count(item.getName())) {
        Problem("filter", item.getName().substr(1));
      } else if (IsName(item, "/Crypt")) {
        Problem("filter", "Crypt");
      }
    }
    if (renamed && !filters.empty()) {
      dict.replaceKey("/Filter", filter.isArray() ? QPDFObjectHandle::newArray(filters) : filters[0]);
      Change("filter-names");
    }
    Lzw(stream);
    const std::string subtype = Name(dict.getKey("/Subtype"));
    const std::string type = Name(dict.getKey("/Type"));
    if (subtype == "/Image" && (type.empty() || type == "/XObject"))
      Image(stream);
    else if (subtype == "/Form" && (type.empty() || type == "/XObject"))
      Form(stream);
    else if (subtype == "/PS" && (type.empty() || type == "/XObject")) {
      // PostScript for printers only: a form that draws nothing in its place.
      stream.replaceStreamData("", QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
      for (const std::string &key : dict.getKeys())
        if (key != "/Length")
          dict.removeKey(key);
      dict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
      dict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
      dict.replaceKey("/BBox", QPDFObjectHandle::parse("[0 0 1 1]"));
      Change("postscript");
    }
  }

  // A stream's bytes through its first |count| filters (lossless ones); false when they can't
  // be decoded.
  bool DecodeFirst(QPDFObjectHandle stream, size_t count, std::string &out) {
    QPDFObjectHandle dict = stream.getDict();
    QPDFObjectHandle filter = dict.getKey("/Filter"), parms = dict.getKey("/DecodeParms");
    std::vector<QPDFObjectHandle> filters = filter.isArray() ? filter.getArrayAsVector() : std::vector<QPDFObjectHandle>{filter};
    std::vector<QPDFObjectHandle> parameters = parms.isArray() ? parms.getArrayAsVector() : std::vector<QPDFObjectHandle>{parms};
    parameters.resize(filters.size(), QPDFObjectHandle::newNull());
    const std::shared_ptr<Buffer> raw = stream.getRawStreamData();
    if (!raw)
      return false;
    std::string bytes(reinterpret_cast<const char *>(raw->getBuffer()), raw->getSize());
    if (count == 0) {
      out = std::move(bytes);
      return true;
    }
    count = std::min(count, filters.size());
    QPDFObjectHandle scratch = QPDFObjectHandle::newStream(&q_);
    scratch.replaceStreamData(bytes, QPDFObjectHandle::newArray(std::vector<QPDFObjectHandle>(filters.begin(), filters.begin() + static_cast<long>(count))),
                              QPDFObjectHandle::newArray(std::vector<QPDFObjectHandle>(parameters.begin(), parameters.begin() + static_cast<long>(count))));
    return Decoded(scratch, out);
  }

  // LZW, which PDF/A doesn't allow: decoded (with the lossless filters before it) and left for
  // the writer to compress with Flate.
  void Lzw(QPDFObjectHandle stream) {
    QPDFObjectHandle dict = stream.getDict();
    QPDFObjectHandle filter = dict.getKey("/Filter"), parms = dict.getKey("/DecodeParms");
    std::vector<QPDFObjectHandle> filters = filter.isArray() ? filter.getArrayAsVector() : std::vector<QPDFObjectHandle>{filter};
    std::vector<QPDFObjectHandle> parameters = parms.isArray() ? parms.getArrayAsVector() : std::vector<QPDFObjectHandle>{parms};
    parameters.resize(filters.size(), QPDFObjectHandle::newNull());
    size_t last = filters.size();
    for (size_t at = 0; at < filters.size(); ++at)
      if (IsName(filters[at], "/LZWDecode") || IsName(filters[at], "/LZW"))
        last = at;
    if (last == filters.size())
      return;
    std::string data;
    if (!DecodeFirst(stream, last + 1, data)) {
      Problem("lzw");
      return;
    }
    std::vector<QPDFObjectHandle> rest_filters(filters.begin() + static_cast<long>(last) + 1, filters.end());
    std::vector<QPDFObjectHandle> rest_parms(parameters.begin() + static_cast<long>(last) + 1, parameters.end());
    stream.replaceStreamData(data, rest_filters.empty() ? QPDFObjectHandle::newNull() : QPDFObjectHandle::newArray(rest_filters),
                             rest_parms.empty() ? QPDFObjectHandle::newNull() : QPDFObjectHandle::newArray(rest_parms));
    Change("lzw");
  }

  void Image(QPDFObjectHandle image) {
    QPDFObjectHandle dict = image.getDict();
    for (const char *key : {"/Alternates", "/OPI"})
      if (dict.hasKey(key)) {
        dict.removeKey(key);
        Change("image-alternates");
      }
    if (dict.getKey("/Interpolate").isBool() && dict.getKey("/Interpolate").getBoolValue()) {
      dict.removeKey("/Interpolate");
      Change("interpolation");
    }
    if (dict.hasKey("/Intent") && !kIntents.count(Name(dict.getKey("/Intent")))) {
      dict.replaceKey("/Intent", QPDFObjectHandle::newName("/RelativeColorimetric"));
      Change("rendering-intents");
    }
    QPDFObjectHandle filter = dict.getKey("/Filter");
    const bool jpx = IsName(filter, "/JPXDecode") ||
                     (filter.isArray() && filter.getArrayNItems() > 0 && IsName(filter.getArrayItem(filter.getArrayNItems() - 1), "/JPXDecode"));
    if (jpx && part_ != 1) {
      std::string data;
      const size_t before = filter.isArray() ? static_cast<size_t>(filter.getArrayNItems()) - 1 : 0;
      // The colour specifications count only when the picture has no colour space of its own.
      const bool read = DecodeFirst(image, before, data);
      const std::string trouble = read ? JpxTrouble(data, !dict.hasKey("/ColorSpace")) : "unreadable";
      if (!trouble.empty()) {
        if (read && Unpacked(image, data))
          Change("jpeg2000-decoded");
        else
          Problem("jpeg2000", trouble);
      }
    }
    if (part_ == 1) {
      // PDF/A-1 takes neither JPEG 2000 nor 16-bit pictures: decoded and written again.
      if (jpx) {
        std::string data;
        const size_t before = filter.isArray() ? static_cast<size_t>(filter.getArrayNItems()) - 1 : 0;
        if (DecodeFirst(image, before, data) && Unpacked(image, data))
          Change("jpeg2000-decoded");
        else
          Problem("jpeg2000");
      }
      if (Integer(dict, "/BitsPerComponent", 8) == 16) {
        if (EightBits(image))
          Change("16-bit-images");
        else
          Problem("16-bit-image");
      }
      if (dict.getKey("/SMask").isStream() || Integer(dict, "/SMaskInData", 0) != 0)
        transparent_ = true;
    }
  }

  // A 16-bit picture as 8 bits a sample (each sample's high byte), losslessly compressed by the
  // writer; false when its data can't be decoded.
  bool EightBits(QPDFObjectHandle image) {
    QPDFObjectHandle dict = image.getDict();
    QPDFObjectHandle filter = dict.getKey("/Filter");
    const size_t count = filter.isArray() ? static_cast<size_t>(filter.getArrayNItems()) : filter.isNull() ? 0 : 1;
    std::string data;
    if (!DecodeFirst(image, count, data))
      return false;
    std::string eight(data.size() / 2, '\0');
    for (size_t at = 0; at < eight.size(); ++at)
      eight[at] = data[at * 2];
    image.replaceStreamData(eight, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
    dict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
    return true;
  }

  // A JPEG 2000 picture PDF/A can't take as it is, decoded and written again: as a JPEG (quality
  // 92, about the size it was; CMYK as RGB, CmykToRgb's), or, keeping a colour space of its own
  // with the components decoded, losslessly (Flate, by the writer); its alpha, when it says to use
  // it, as a soft mask. False when it can't be decoded (or is too large to here).
  bool Unpacked(QPDFObjectHandle image, const std::string &data) {
    jpx::Picture picture;
    if (!jpx::Decode(data, picture, 32ULL * 1024 * 1024))
      return false;
    QPDFObjectHandle dict = image.getDict();
    QPDFObjectHandle space = dict.getKey("/ColorSpace");
    const int own = space.isNull() ? 0 : IsName(space, "/DeviceGray") ? 1 : IsName(space, "/DeviceRGB") ? 3 : -1;
    if (own != 0 && own != picture.components) {
      // A colour space of its own (ICC, indexed, ...) whose components the picture may not have.
      if (own > 0 || !space.isArray() || !IsName(space.getArrayItem(0), "/ICCBased") || Integer(space.getArrayItem(1).getDict(), "/N", 0) != picture.components)
        return false;
    }
    std::string jpeg;
    if (own == 0) {
      if (picture.components == 4) {
        std::string rgb(picture.samples.size() / 4 * 3, '\0');
        for (size_t at = 0, to = 0; at + 3 < picture.samples.size(); at += 4, to += 3) {
          double r, g, b;
          const auto ink = [&](size_t k) { return static_cast<unsigned char>(picture.samples[at + k]) / 255.0; };
          CmykToRgb(ink(0), ink(1), ink(2), ink(3), r, g, b);
          rgb[to] = static_cast<char>(std::lround(r * 255));
          rgb[to + 1] = static_cast<char>(std::lround(g * 255));
          rgb[to + 2] = static_cast<char>(std::lround(b * 255));
        }
        picture.samples.swap(rgb);
        picture.components = 3;
      }
      space = QPDFObjectHandle::newName(picture.components == 3 ? "/DeviceRGB" : "/DeviceGray");
      dict.removeKey("/Decode");
      if (!jpeg::Encode(reinterpret_cast<const unsigned char *>(picture.samples.data()), picture.width, picture.height, picture.components, 92, jpeg))
        jpeg.clear();
    }
    const bool use_alpha = Integer(dict, "/SMaskInData", 0) != 0 && !picture.alpha.empty() && !dict.getKey("/SMask").isStream();
    if (jpeg.empty())
      image.replaceStreamData(picture.samples, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
    else
      image.replaceStreamData(jpeg, QPDFObjectHandle::newName("/DCTDecode"), QPDFObjectHandle::newNull());
    dict.replaceKey("/Width", QPDFObjectHandle::newInteger(picture.width));
    dict.replaceKey("/Height", QPDFObjectHandle::newInteger(picture.height));
    dict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
    dict.replaceKey("/ColorSpace", space);
    dict.removeKey("/SMaskInData");
    if (use_alpha) {
      QPDFObjectHandle mask = QPDFObjectHandle::newStream(&q_, picture.alpha);
      QPDFObjectHandle mask_dict = mask.getDict();
      mask_dict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
      mask_dict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
      mask_dict.replaceKey("/Width", QPDFObjectHandle::newInteger(picture.width));
      mask_dict.replaceKey("/Height", QPDFObjectHandle::newInteger(picture.height));
      mask_dict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
      mask_dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceGray"));
      dict.replaceKey("/SMask", mask);
    }
    return true;
  }

  void Form(QPDFObjectHandle form) {
    QPDFObjectHandle dict = form.getDict();
    for (const char *key : {"/OPI", "/PS", "/Ref"})
      if (dict.hasKey(key)) {
        dict.removeKey(key);
        Change("form-postscript");
      }
    if (IsName(dict.getKey("/Subtype2"), "/PS")) {
      dict.removeKey("/Subtype2");
      Change("form-postscript");
    }
    if (part_ == 1 && dict.getKey("/Group").isDictionary() && IsName(dict.getKey("/Group").getKey("/S"), "/Transparency"))
      groups_.push_back(dict);
  }

  // A graphics state: no transfer functions or halftone details (they're for printers), known
  // rendering intents and blend modes.
  void State(QPDFObjectHandle state) {
    if (!state.isDictionary() || !First(state, 1))
      return;
    if (state.hasKey("/TR")) {
      state.removeKey("/TR");
      Change("transfer-functions");
    }
    if (state.hasKey("/TR2") && !IsName(state.getKey("/TR2"), "/Default")) {
      state.removeKey("/TR2");
      Change("transfer-functions");
    }
    if (state.hasKey("/HTP")) {
      state.removeKey("/HTP");
      Change("halftones");
    }
    if (state.hasKey("/HT") && !IsName(state.getKey("/HT"), "/Default")) {
      state.removeKey("/HT");
      Change("halftones");
    }
    if (state.hasKey("/RI") && !kIntents.count(Name(state.getKey("/RI")))) {
      state.replaceKey("/RI", QPDFObjectHandle::newName("/RelativeColorimetric"));
      Change("rendering-intents");
    }
    // Overprint mode 1 with overprinting on (PDF/A-2 6.2.4.2 forbids it for ICC-based CMYK):
    // mode 0, which differs only where a printer overprints.
    const auto on = [&](const char *key) { return state.getKey(key).isBool() && state.getKey(key).getBoolValue(); };
    if (part_ != 1 && Integer(state, "/OPM", 0) == 1 && (on("/OP") || on("/op"))) {
      state.replaceKey("/OPM", QPDFObjectHandle::newInteger(0));
      Change("overprint-mode");
    }
    QPDFObjectHandle blend = state.getKey("/BM");
    if (blend.isArray()) {
      // A list to choose from (PDF 1.4): the first known one.
      QPDFObjectHandle chosen = QPDFObjectHandle::newName("/Normal");
      for (QPDFObjectHandle item : blend.getArrayAsVector())
        if (kBlendModes.count(Name(item))) {
          chosen = item;
          break;
        }
      state.replaceKey("/BM", chosen);
      blend = chosen;
      Change("blend-modes");
    } else if (blend.isName() && !kBlendModes.count(blend.getName())) {
      blend = QPDFObjectHandle::newName("/Normal");
      state.replaceKey("/BM", blend);
      Change("blend-modes");
    }
    if (part_ == 1) {
      const std::string mode = Name(blend);
      QPDFObjectHandle mask = state.getKey("/SMask");
      const auto below_one = [&](const char *key) {
        QPDFObjectHandle value = state.getKey(key);
        return value.isNumber() && value.getNumericValue() < 1;
      };
      if ((!mode.empty() && mode != "/Normal" && mode != "/Compatible") || (mask.isDictionary()) || below_one("/CA") || below_one("/ca"))
        transparent_ = true;
    }
  }

  // An ICC profile a colour space embeds: one PDF/A can use. False when it isn't a profile at all.
  bool Profile(QPDFObjectHandle stream) {
    std::string data;
    const int n = Integer(stream.getDict(), "/N", 0);
    if (!Decoded(stream, data) || data.size() < 132 || data.compare(36, 4, "acsp") != 0)
      return false;
    if (!First(stream, 2))
      return true;
    const std::string space = data.substr(16, 4), device = data.substr(12, 4);
    const int expected = space == "GRAY" ? 1 : space == "RGB " || space == "Lab " ? 3 : space == "CMYK" ? 4 : 0;
    const int major = static_cast<unsigned char>(data[8]);
    if (expected != n)
      Problem("icc-profile", "components");
    else if (major > (part_ == 1 ? 2 : 4) && icc::IsSrgb(data)) {
      // sRGB in a newer version (Chrome embeds a v4 one): the same colours as the v2 sRGB profile.
      stream.replaceStreamData(SrgbProfile(), QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
      Change("icc-profiles-srgb");
    } else if (major > (part_ == 1 ? 2 : 4)) {
      Problem("icc-profile", "version " + std::to_string(major));
    }
    else if (device != "scnr" && device != "mntr" && device != "prtr" && device != "spac")
      Problem("icc-profile", "class");
    return true;
  }

  // A colour space whose profile isn't one, made what readers draw it as, its alternate (the
  // device space of its components when it names none), in a space PDF/A takes: grey as
  // CalGray, RGB through the sRGB profile, CMYK as its process inks with an RGB alternate.
  void RepairProfile(QPDFObjectHandle space) {
    QPDFObjectHandle dict = space.getArrayItem(1).getDict();
    const int n = Integer(dict, "/N", 0);
    QPDFObjectHandle alternate = dict.getKey("/Alternate");
    const int components = IsName(alternate, "/DeviceGray") ? 1 : IsName(alternate, "/DeviceRGB") ? 3 : IsName(alternate, "/DeviceCMYK") ? 4 : n;
    if (components != n || (n != 1 && n != 3 && n != 4)) {
      Problem("icc-profile", "unreadable");
      return;
    }
    std::vector<QPDFObjectHandle> made;
    if (n == 1) {
      made = {QPDFObjectHandle::newName("/CalGray"), QPDFObjectHandle::parse("<< /WhitePoint [0.9505 1 1.089] /Gamma 2.2 >>")};
    } else if (n == 3) {
      if (srgb_.isNull()) {
        srgb_ = QPDFObjectHandle::newStream(&q_, SrgbProfile());
        srgb_.getDict().replaceKey("/N", QPDFObjectHandle::newInteger(3));
      }
      made = {QPDFObjectHandle::newName("/ICCBased"), srgb_};
    } else {
      made = CmykAsRgb(q_);
    }
    while (space.getArrayNItems() > 0)
      space.eraseItem(0);
    for (const QPDFObjectHandle &item : made)
      space.appendItem(item);
    Change("icc-profiles-replaced");
  }

  // -----------------------------------------------------------------------------------------
  // Content: what fonts draw text, inline pictures, operators

  struct TextState {
    QPDFObjectHandle font = QPDFObjectHandle::newNull();
    int mode = 0;
    double size = 0, tc = 0, tw = 0;  // font size, character and word spacing
  };

  using Names = std::set<std::pair<std::string, std::string>>;  // resources by kind and name

  struct FontUse {
    QPDFObjectHandle font;
    bool drawn = false;  // used with a text rendering mode other than 3 (invisible)
  };

  void Contents() {
    for (QPDFPageObjectHelper &helper : pages_) {
      QPDFObjectHandle page = helper.getObjectHandle();
      QPDFObjectHandle resources = page.getKey("/Resources");
      bool ok = true;
      const std::string content = PageContent(page, ok);
      std::string rewritten;
      if (!ok)
        Problem("content-unreadable");
      Names used;
      if (ok && Walk(content, resources, TextState{}, rewritten, 0, used))
        page.replaceKey("/Contents", QPDFObjectHandle::newStream(&q_, rewritten));
      ResourcesContent(resources, 0);
      QPDFObjectHandle annotations = page.getKey("/Annots");
      for (int index = 0; annotations.isArray() && index < annotations.getArrayNItems(); ++index) {
        QPDFObjectHandle appearance = annotations.getArrayItem(index).getKey("/AP");
        QPDFObjectHandle normal = appearance.isDictionary() ? appearance.getKey("/N") : QPDFObjectHandle::newNull();
        if (normal.isStream())
          StreamContent(normal, resources, TextState{}, 0);
        else if (normal.isDictionary())
          for (const std::string &key : normal.getKeys())
            StreamContent(normal.getKey(key), resources, TextState{}, 0);
      }
    }
  }

  // A form, pattern or glyph's content; |inherited| resources when it has none of its own.
  // |glyphs|: a Type 3 font's, collecting the resources its glyphs name (the font holds them).
  void StreamContent(QPDFObjectHandle stream, QPDFObjectHandle inherited, const TextState &state, int depth, Names *glyphs = nullptr) {
    if (!stream.isStream() || depth > 32 || !First(stream, state.mode == 3 ? 3 : 0))
      return;
    QPDFObjectHandle own = stream.getDict().getKey("/Resources");
    QPDFObjectHandle resources = own.isDictionary() ? own : inherited;
    Names used;
    bool ok = true;
    const std::string content = StreamData(stream, ok);
    std::string rewritten;
    if (!ok)
      Problem("content-unreadable");
    else if (Walk(content, resources, state, rewritten, depth, glyphs ? *glyphs : used))
      stream.replaceStreamData(rewritten, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
    // Content must name its resources itself (PDF/A-2 6.2.2): the ones it uses of those it
    // inherited.
    if (!own.isDictionary() && !glyphs)
      Adopt(stream.getDict(), inherited, used, stream);
    ResourcesContent(resources, depth);
  }

  // A resources dictionary for |holder| with just the |used| ones of |inherited| (never
  // |self|, which would make a loop).
  void Adopt(QPDFObjectHandle holder, QPDFObjectHandle inherited, const Names &used, QPDFObjectHandle self) {
    if (used.empty() || !inherited.isDictionary())
      return;
    QPDFObjectHandle made = QPDFObjectHandle::newDictionary();
    for (const auto &[kind, name] : used) {
      QPDFObjectHandle group = inherited.getKey(kind);
      QPDFObjectHandle value = group.isDictionary() ? group.getKey(name) : QPDFObjectHandle::newNull();
      if (value.isNull() || (value.isIndirect() && self.isIndirect() && value.getObjGen() == self.getObjGen()))
        continue;
      if (!made.getKey(kind).isDictionary())
        made.replaceKey(kind, QPDFObjectHandle::newDictionary());
      made.getKey(kind).replaceKey(name, value);
    }
    if (made.getKeys().empty())
      return;
    holder.replaceKey("/Resources", made);
    Change("resources");
  }

  // Content that resources hold besides forms: tiling patterns, soft masks' groups.
  void ResourcesContent(QPDFObjectHandle resources, int depth) {
    if (!resources.isDictionary() || !First(resources, 3))
      return;
    QPDFObjectHandle patterns = resources.getKey("/Pattern");
    for (const std::string &key : patterns.isDictionary() ? patterns.getKeys() : std::set<std::string>{})
      StreamContent(patterns.getKey(key), resources, TextState{}, depth + 1);
    QPDFObjectHandle states = resources.getKey("/ExtGState");
    for (const std::string &key : states.isDictionary() ? states.getKeys() : std::set<std::string>{}) {
      QPDFObjectHandle mask = states.getKey(key).isDictionary() ? states.getKey(key).getKey("/SMask") : QPDFObjectHandle::newNull();
      if (mask.isDictionary())
        StreamContent(mask.getKey("/G"), resources, TextState{}, depth + 1);
    }
  }

  // A shown string's codes, checked against the embedded font program (a Type 3 font's glyph
  // procedures).
  void Glyphs(QPDFObjectHandle font, const std::string &bytes, bool drawn) {
    if (!font.isDictionary())
      return;
    const std::string key = font.isIndirect() ? std::to_string(font.getObjectID()) + " " + std::to_string(font.getGeneration()) : font.unparse();
    auto found = checkers_.find(key);
    if (found == checkers_.end()) {
      found = checkers_.emplace(key, std::make_unique<font::Checker>(font)).first;
    }
    found->second->Show(bytes, drawn);
  }

  void Use(QPDFObjectHandle font, int mode, QPDFObjectHandle resources, int depth) {
    if (!font.isDictionary())
      return;
    const std::string key = font.isIndirect() ? std::to_string(font.getObjectID()) + " " + std::to_string(font.getGeneration()) : font.unparse();
    FontUse &use = fonts_[key];
    use.font = font;
    use.drawn = use.drawn || mode != 3 || part_ == 1;
    // A Type 3 font's glyphs are content of their own.
    if (IsName(font.getKey("/Subtype"), "/Type3") && First(font, 1)) {
      QPDFObjectHandle own = font.getKey("/Resources");
      QPDFObjectHandle glyph_resources = own.isDictionary() ? own : resources;
      Names used;
      QPDFObjectHandle procs = font.getKey("/CharProcs");
      for (const std::string &name : procs.isDictionary() ? procs.getKeys() : std::set<std::string>{})
        StreamContent(procs.getKey(name), glyph_resources, TextState{}, depth + 1, &used);
      if (!own.isDictionary())
        Adopt(font, resources, used, font);
    }
  }

  // Reads |content|; true with |out| when it had to change (inline pictures, operators PDF/A
  // doesn't allow).
  bool Walk(const std::string &content, QPDFObjectHandle resources, TextState state, std::string &out, int depth, Names &used) {
    const auto resource = [&](const char *kind, const std::string &name) {
      // Names that aren't UTF-8 were renamed in the resources already (Scan).
      used.insert({kind, "/" + FixedName(name)});
      QPDFObjectHandle group = resources.isDictionary() ? resources.getKey(kind) : QPDFObjectHandle::newNull();
      return group.isDictionary() ? group.getKey("/" + FixedName(name)) : QPDFObjectHandle::newNull();
    };
    // The other operators that name a resource.
    const auto names = [&](const Op &op) {
      const auto last_name = [&](const char *kind) {
        if (!op.operands.empty() && op.operands.back().kind == Token::Name)
          used.insert({kind, "/" + FixedName(op.operands.back().text)});
      };
      if (op.name == "cs" || op.name == "CS") {
        const std::string name = op.operands.empty() ? "" : op.operands[0].text;
        if (name != "DeviceGray" && name != "DeviceRGB" && name != "DeviceCMYK" && name != "Pattern")
          last_name("/ColorSpace");
      } else if (op.name == "scn" || op.name == "SCN") {
        last_name("/Pattern");
      } else if (op.name == "sh") {
        last_name("/Shading");
      } else if ((op.name == "BDC" || op.name == "DP") && op.operands.size() == 2 && op.operands[1].kind == Token::Name) {
        used.insert({"/Properties", "/" + FixedName(op.operands[1].text)});
      } else if (op.name == "BI") {
        // An inline picture's named colour space.
        static const std::regex space(R"(/(?:CS|ColorSpace)\s*/([^\s/\[\]<>()]+))");
        std::smatch match;
        const std::string text = content.substr(op.begin, op.end - op.begin);
        const std::string head = text.substr(0, text.find("ID"));
        if (std::regex_search(head, match, space)) {
          const std::string name = match[1].str();
          if (name != "G" && name != "RGB" && name != "CMYK" && name != "I" && name != "DeviceGray" && name != "DeviceRGB" && name != "DeviceCMYK" && name != "Indexed")
            used.insert({"/ColorSpace", "/" + name});
        }
      }
    };
    std::vector<TextState> stack;
    bool changed = false;
    std::string rewritten;
    rewritten.reserve(content.size() + 16);
    // Operator by operator: a whole stream's list of them takes about 19 times its size.
    int hidden = 0;  // PDF/A-1: inside marked content of a hidden layer (its depth)
    const bool well_formed = ForEachOp(content, [&](const Op &op) {
      if (hidden > 0) {
        // Taken out, but for q and Q (keeping the graphics state stack as it was).
        if (op.name == "BDC" || op.name == "BMC")
          ++hidden;
        else if (op.name == "EMC")
          --hidden;
        if (op.name == "q" || op.name == "Q") {
          rewritten.append(content, op.begin, op.end - op.begin);
          rewritten += '\n';
        }
        changed = true;
        return;
      }
      if (part_ == 1 && !hidden_.empty() &&
          ((op.name == "BDC" && op.operands.size() == 2 && op.operands[0].kind == Token::Name && op.operands[0].text == "OC" &&
            op.operands[1].kind == Token::Name && Hidden(resource("/Properties", op.operands[1].text))) ||
           (op.name == "Do" && op.operands.size() == 1 && op.operands[0].kind == Token::Name &&
            Hidden(resource("/XObject", op.operands[0].text).isStream() ? resource("/XObject", op.operands[0].text).getDict().getKey("/OC")
                                                                         : QPDFObjectHandle::newNull())))) {
        if (op.name == "BDC")
          hidden = 1;
        Change("hidden-content-removed");
        changed = true;
        return;
      }
      std::string replaced;
      bool drop = false;
      names(op);
      if (!kOperators.count(op.name)) {
        // The tokenizer also stops at true, false and null inside a dictionary (BDC's): only
        // a word with whole operands is an operator, dropped (readers ignore it anyway).
        int open = 0;
        for (const Token &token : op.operands)
          open += token.kind == Token::ArrayOpen || token.kind == Token::DictOpen ? 1 : token.kind == Token::ArrayClose || token.kind == Token::DictClose ? -1 : 0;
        const bool word = std::all_of(op.name.begin(), op.name.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '*'; });
        if (open == 0 && word && op.name != "true" && op.name != "false" && op.name != "null") {
          drop = true;
          Change("unknown-operators");
        } else if (!op.name.empty() && std::strchr("+-.0123456789", op.name[0])) {
          // A number written wrong (2.1.1, 12.-797): readers read it differently (veraPDF as the
          // largest number there is), so what the content draws isn't known.
          if (!numbers_reported_)
            Problem("content-numbers", op.name.substr(0, 20));
          numbers_reported_ = true;
        }
      } else if (op.name == "q") {
        stack.push_back(state);
      } else if (op.name == "Q") {
        if (!stack.empty()) {
          state = stack.back();
          stack.pop_back();
        }
      } else if (op.name == "Tf" && !op.operands.empty() && op.operands[0].kind == Token::Name) {
        state.font = resource("/Font", op.operands[0].text);
        if (!state.font.isDictionary())
          Problem("font-missing", op.operands[0].text);
      } else if (op.name == "Tr" && op.operands.size() == 1 && op.operands[0].kind == Token::Number) {
        state.mode = static_cast<int>(op.operands[0].number);
      } else if (op.name == "gs" && op.operands.size() == 1 && op.operands[0].kind == Token::Name) {
        QPDFObjectHandle gs = resource("/ExtGState", op.operands[0].text);
        QPDFObjectHandle font = gs.isDictionary() ? gs.getKey("/Font") : QPDFObjectHandle::newNull();
        if (font.isArray() && font.getArrayNItems() >= 1)
          state.font = font.getArrayItem(0);
      } else if (op.name == "Tj" || op.name == "TJ" || op.name == "'" || op.name == "\"") {
        // Any text operator uses its font, even with nothing to show (veraPDF counts it).
        {
          Use(state.font, state.mode, resources, depth);
          // Each code checked against the font program: TJ's strings, the others' last one.
          const bool two_byte = TwoByteCodes(state.font);
          for (size_t at = 0; at < op.operands.size(); ++at)
            if (op.operands[at].kind == Token::String && (op.name == "TJ" || at + 1 == op.operands.size()))
              Glyphs(state.font, Shown(op.operands[at].text, two_byte), state.mode != 3);
        }
        replaced = LongText(op, state.font);
      } else if (op.name == "ri" && op.operands.size() == 1 && op.operands[0].kind == Token::Name &&
                 !kIntents.count("/" + op.operands[0].text)) {
        replaced = "/RelativeColorimetric ri";
        Change("rendering-intents");
      } else if (op.name == "BI") {
        replaced = InlineImage(content.substr(op.begin, op.end - op.begin));
      } else if (op.name == "Do" && op.operands.size() == 1 && op.operands[0].kind == Token::Name) {
        QPDFObjectHandle xobject = resource("/XObject", op.operands[0].text);
        if (xobject.isStream() && IsName(xobject.getDict().getKey("/Subtype"), "/Form"))
          StreamContent(xobject, resources, state, depth + 1);
      }
      if (drop) {
        changed = true;
        return;
      }
      if (op.name == "q" && stack.size() > 28)
        Problem("limits", "graphics state nesting");
      if (replaced.empty())
        replaced = Operands(content, op);
      if (!replaced.empty()) {
        rewritten += replaced;
        changed = true;
      } else {
        rewritten.append(content, op.begin, op.end - op.begin);
      }
      rewritten += '\n';
    });
    if (!well_formed) {
      Problem("content-unreadable");
      return false;
    }
    if (changed)
      out.swap(rewritten);
    return changed;
  }

  // Whether a font's codes are two bytes each (Identity): a string of odd length ends in half a
  // code, which readers leave out.
  static bool TwoByteCodes(QPDFObjectHandle font) {
    return font.isDictionary() && IsName(font.getKey("/Subtype"), "/Type0") &&
           (IsName(font.getKey("/Encoding"), "/Identity-H") || IsName(font.getKey("/Encoding"), "/Identity-V"));
  }

  // A shown string as readers read it: without the half code an Identity font's odd one ends in.
  static std::string Shown(const std::string &text, bool two_byte) { return two_byte && text.size() % 2 ? text.substr(0, text.size() - 1) : text; }

  // A text operator showing a string longer than PDF/A allows, or ending in half a code, as one
  // TJ of strings short enough, split between codes, the half code left out (what it shows is the
  // same); "" when there's neither.
  std::string LongText(const Op &op, QPDFObjectHandle font) {
    const size_t limit = part_ == 1 ? 65535 : 32767;
    const bool two_byte = TwoByteCodes(font);
    bool long_one = false, half = false;
    for (size_t at = 0; at < op.operands.size(); ++at) {
      const Token &token = op.operands[at];
      if (token.kind != Token::String || (op.name != "TJ" && at + 1 != op.operands.size()))
        continue;
      long_one = long_one || token.text.size() > limit;
      half = half || (two_byte && token.text.size() % 2);
    }
    if ((!long_one && !half) || !font.isDictionary())
      return "";
    const std::string key = font.isIndirect() ? std::to_string(font.getObjectID()) + " " + std::to_string(font.getGeneration()) : font.unparse();
    const auto checker = checkers_.find(key);
    if (checker == checkers_.end())
      return "";
    static const char *hex = "0123456789abcdef";
    const auto pieces = [&](const std::string &text) {
      std::string out;
      size_t start = 0, end = 0;
      const auto write = [&]() {
        out += "<";
        for (size_t at = start; at < end; ++at) {
          out += hex[static_cast<unsigned char>(text[at]) >> 4];
          out += hex[static_cast<unsigned char>(text[at]) & 15];
        }
        out += ">";
        start = end;
      };
      for (const font::Checker::Piece &piece : checker->second->Pieces(text)) {
        if (piece.begin + piece.length - start > limit && end > start)
          write();
        end = piece.begin + piece.length;
      }
      end = text.size();
      while (end - start > limit) {  // no codes found to split at: the rest as it is, in parts
        const size_t stop = end;
        end = start + limit;
        write();
        end = stop;
      }
      write();
      return out;
    };
    std::string prefix, items;
    if (op.name == "TJ") {
      for (const Token &token : op.operands)
        if (token.kind == Token::String)
          items += pieces(Shown(token.text, two_byte)) + " ";
        else if (token.kind == Token::Number)
          items += Num(token.number) + " ";
    } else {
      if (op.name == "'")
        prefix = "T*\n";
      else if (op.name == "\"" && op.operands.size() == 3)
        prefix = Num(op.operands[0].number) + " Tw\n" + Num(op.operands[1].number) + " Tc\nT*\n";
      items = pieces(Shown(op.operands.back().text, two_byte));
    }
    Change(half ? "half-codes-removed" : "long-strings");
    return prefix + "[" + items + "] TJ";
  }

  // An operator's operands within PDF/A's limits; names that aren't UTF-8 renamed as their
  // resources were, hexadecimal strings written out cleanly. "" when nothing changed.
  std::string Operands(const std::string &content, const Op &op) {
    std::string out;
    size_t from = op.begin;
    bool changed = false;
    for (const Token &token : op.operands) {
      std::string replacement;
      if (token.kind == Token::Number) {
        const std::string text = content.substr(token.begin, token.end - token.begin);
        const bool integer = text.find('.') == std::string::npos;
        if (integer && (token.number > 2147483647.0 || token.number < -2147483648.0)) {
          replacement = token.number > 0 ? "2147483647" : "-2147483648";
          Change("numbers-clamped");
        }
        // PDF/A-1 reads content numbers as reals where reals go (coordinates): its real limit.
        if (part_ == 1 && std::abs(token.number) > 32767)
          Problem("limits", "real");
      } else if (token.kind == Token::String) {
        if (token.text.size() > (part_ == 1 ? 65535u : 32767u))
          Problem("limits", "string");
        if (content[token.begin] == '<') {
          size_t digits = 0;
          bool clean = true;
          for (size_t at = token.begin + 1; at + 1 < token.end; ++at) {
            const unsigned char c = static_cast<unsigned char>(content[at]);
            if (std::isxdigit(c))
              ++digits;
            else if (!IsWhite(c))
              clean = false;
          }
          if (!clean || digits % 2) {
            static const char *hex = "0123456789abcdef";
            replacement = "<";
            for (const char c : token.text) {
              replacement += hex[static_cast<unsigned char>(c) >> 4];
              replacement += hex[static_cast<unsigned char>(c) & 15];
            }
            replacement += ">";
            Change("hex-strings");
          }
        }
      } else if (token.kind == Token::Name) {
        if (FixedName(token.text) != token.text)
          replacement = NameText(FixedName(token.text));
      }
      if (replacement.empty())
        continue;
      out.append(content, from, token.begin - from);
      out += replacement;
      from = token.end;
      changed = true;
    }
    if (!changed)
      return "";
    out.append(content, from, op.end - from);
    return out;
  }

  // An inline picture: no interpolation, a known intent; "" to keep it as it is.
  std::string InlineImage(const std::string &text) {
    const size_t data = text.find("ID");
    if (data == std::string::npos)
      return "";
    std::string head = text.substr(0, data);
    bool changed = false;
    for (const char *key : {"/I ", "/Interpolate "}) {
      for (size_t at = head.find(key); at != std::string::npos; at = head.find(key, at + 1)) {
        const size_t value = head.find_first_not_of(" \t\r\n", at + std::char_traits<char>::length(key));
        if (value != std::string::npos && head.compare(value, 4, "true") == 0) {
          head.replace(value, 4, "false");
          changed = true;
          Change("interpolation");
        }
      }
    }
    static const std::regex intent(R"(/Intent\s*/([A-Za-z#0-9]+))");
    std::smatch match;
    if (std::regex_search(head, match, intent) && !kIntents.count("/" + match[1].str())) {
      head.replace(static_cast<size_t>(match.position(0)), static_cast<size_t>(match.length(0)), "/Intent /RelativeColorimetric");
      changed = true;
      Change("rendering-intents");
    }
    if (std::regex_search(head, std::regex(R"(/(LZW|LZWDecode)\b)"))) {
      std::string rewritten;
      if (InlineLzw(head, text.substr(data), rewritten)) {
        Change("lzw");
        return rewritten;
      }
      Problem("lzw", "inline picture");
    }
    if (part_ == 1 && std::regex_search(head, std::regex(R"(/(BPC|BitsPerComponent)\s+16\b)")))
      Problem("16-bit-image");
    // Data readers find differently ends of: "EI" anywhere in it (veraPDF ends a picture at an
    // EI that white space follows, others at one between white space), white space at its start
    // or end (taken for the space after ID or before EI). Written as hex, which can hold
    // neither, every reader finds the same data.
    const std::string body = text.substr(data);
    const std::string raw = InlineData(body);
    if (raw.find("EI") != std::string::npos ||
        (!raw.empty() && (IsWhite(static_cast<unsigned char>(raw.front())) || IsWhite(static_cast<unsigned char>(raw.back()))))) {
      std::string rewritten;
      if (InlineHex(head, body, rewritten)) {
        Change("inline-pictures-hex");
        return rewritten;
      }
      Problem("content-unreadable");
      return "";
    }
    return changed ? head + body : "";
  }

  // An inline picture's entries (|head| from BI to ID) but its filters and their parameters, as
  // they are written (|kept|), and those (long names; parameters one a filter).
  static bool InlineHead(const std::string &head, std::string &kept, std::vector<QPDFObjectHandle> &list, std::vector<QPDFObjectHandle> &parameters) {
    size_t pos = 0;
    Token token;
    if (!NextToken(head, pos, token) || token.text != "BI")
      return false;
    kept = "BI";
    QPDFObjectHandle filters = QPDFObjectHandle::newNull(), parms = QPDFObjectHandle::newNull();
    try {
      while (NextToken(head, pos, token)) {
        if (token.kind != Token::Name)
          return false;
        const std::string key = token.text;
        const size_t from = token.begin;
        if (!NextToken(head, pos, token))
          return false;
        const size_t value_begin = token.begin;
        if (token.kind == Token::ArrayOpen || token.kind == Token::DictOpen) {
          const Token::Kind open = token.kind, close = open == Token::ArrayOpen ? Token::ArrayClose : Token::DictClose;
          for (int depth = 1; depth > 0 && NextToken(head, pos, token);)
            depth += token.kind == open ? 1 : token.kind == close ? -1 : 0;
        }
        const std::string value = head.substr(value_begin, token.end - value_begin);
        if (key == "F" || key == "Filter")
          filters = QPDFObjectHandle::parse(value);
        else if (key == "DP" || key == "DecodeParms")
          parms = QPDFObjectHandle::parse(value);
        else
          kept += " " + head.substr(from, token.end - from);
      }
    } catch (const std::exception &) {
      return false;
    }
    list = filters.isArray() ? filters.getArrayAsVector() : filters.isNull() ? std::vector<QPDFObjectHandle>{} : std::vector<QPDFObjectHandle>{filters};
    parameters = parms.isArray() ? parms.getArrayAsVector() : std::vector<QPDFObjectHandle>{parms};
    parameters.resize(list.size(), QPDFObjectHandle::newNull());
    for (QPDFObjectHandle &item : list) {
      if (!item.isName())
        return false;
      item = QPDFObjectHandle::newName(FilterName(item.getName(), false));
    }
    return true;
  }

  // An inline picture's data (|body| from ID to EI): after ID and the white space that follows
  // it, before the white space before EI.
  static std::string InlineData(const std::string &body) {
    size_t end = body.size() - 2;
    if (end > 3 && IsWhite(static_cast<unsigned char>(body[end - 1])))
      --end;
    return body.substr(3, end > 3 ? end - 3 : 0);
  }

  // Bytes as an ASCIIHex filter's data, its end marker included.
  static std::string HexData(const unsigned char *bytes, size_t size) {
    static const char *hex = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2 + size / 32 + 4);
    for (size_t at = 0; at < size; ++at) {
      out += hex[bytes[at] >> 4];
      out += hex[bytes[at] & 15];
      if (at % 32 == 31)
        out += '\n';
    }
    return out + ">";
  }

  // An inline picture (|head| from BI, |body| from ID to EI) with its data as it is behind an
  // ASCIIHex filter.
  bool InlineHex(const std::string &head, const std::string &body, std::string &out) {
    std::string kept;
    std::vector<QPDFObjectHandle> list, parameters;
    if (body.size() < 5 || !InlineHead(head, kept, list, parameters))
      return false;
    const std::string raw = InlineData(body);
    std::string names = "/AHx", parameter_list = "null";
    bool any = false;
    for (size_t at = 0; at < list.size(); ++at) {
      names += " " + FilterName(list[at].getName(), true);
      parameter_list += " " + parameters[at].unparse();
      any = any || !parameters[at].isNull();
    }
    out = kept + " /F [" + names + "]" + (any ? " /DP [" + parameter_list + "]" : "") + " ID\n" +
          HexData(reinterpret_cast<const unsigned char *>(raw.data()), raw.size()) + "\nEI";
    return true;
  }

  // An inline picture's filters by their short names (what inline pictures use) or long ones.
  static std::string FilterName(const std::string &name, bool abbreviated) {
    static const std::vector<std::pair<std::string, std::string>> names = {
        {"/AHx", "/ASCIIHexDecode"}, {"/A85", "/ASCII85Decode"}, {"/LZW", "/LZWDecode"}, {"/Fl", "/FlateDecode"},
        {"/RL", "/RunLengthDecode"}, {"/CCF", "/CCITTFaxDecode"}, {"/DCT", "/DCTDecode"}};
    for (const auto &[short_name, long_name] : names)
      if (name == short_name || name == long_name)
        return abbreviated ? short_name : long_name;
    return name;
  }

  // An inline picture with LZW (|head| from BI, |body| from ID to EI) decoded through its
  // filters up to the LZW one and written again with Flate, in hex as inline data must be.
  bool InlineLzw(const std::string &head, const std::string &body, std::string &out) {
    if (body.size() < 5)
      return false;
    const std::string raw = InlineData(body);
    std::string kept;
    std::vector<QPDFObjectHandle> list, parameters;
    if (!InlineHead(head, kept, list, parameters))
      return false;
    size_t last = list.size();
    for (size_t at = 0; at < list.size(); ++at)
      if (list[at].getName() == "/LZWDecode")
        last = at;
    if (last == list.size())
      return false;
    QPDFObjectHandle scratch = QPDFObjectHandle::newStream(&q_);
    scratch.replaceStreamData(raw, QPDFObjectHandle::newArray(std::vector<QPDFObjectHandle>(list.begin(), list.begin() + static_cast<long>(last) + 1)),
                              QPDFObjectHandle::newArray(std::vector<QPDFObjectHandle>(parameters.begin(), parameters.begin() + static_cast<long>(last) + 1)));
    std::string decoded;
    if (!Decoded(scratch, decoded))
      return false;
    Pl_Buffer buffer("inline");
    Pl_Flate flate("inline", &buffer, Pl_Flate::a_deflate);
    flate.write(reinterpret_cast<const unsigned char *>(decoded.data()), decoded.size());
    flate.finish();
    std::unique_ptr<Buffer> compressed(buffer.getBuffer());
    // The filters now: hex, Flate, then those after LZW.
    std::string names = "/AHx /Fl", parameter_list = "null null";
    bool any = false;
    for (size_t at = last + 1; at < list.size(); ++at) {
      names += " " + FilterName(list[at].getName(), true);
      parameter_list += " " + parameters[at].unparse();
      any = any || !parameters[at].isNull();
    }
    static const char *hex = "0123456789abcdef";
    out = kept + " /F [" + names + "]" + (any ? " /DP [" + parameter_list + "]" : "") + " ID\n";
    const unsigned char *bytes = compressed ? compressed->getBuffer() : nullptr;
    for (size_t at = 0; bytes && at < compressed->getSize(); ++at) {
      out += hex[bytes[at] >> 4];
      out += hex[bytes[at] & 15];
      if (at % 32 == 31)
        out += '\n';
    }
    out += ">\nEI";
    return true;
  }

  // -----------------------------------------------------------------------------------------
  // Fonts: checked, not changed (yet)

  // A Type 0 font's CIDFont, or the font itself.
  static QPDFObjectHandle Described(QPDFObjectHandle font) {
    if (!IsName(font.getKey("/Subtype"), "/Type0"))
      return font;
    QPDFObjectHandle descendants = font.getKey("/DescendantFonts");
    return descendants.isArray() && descendants.getArrayNItems() > 0 ? descendants.getArrayItem(0) : QPDFObjectHandle::newNull();
  }

  // The font program embedded the way the font's type allows (a TrueType font in FontFile2, a
  // compact one in FontFile3 of its own subtype, ...), and not empty; null when there's none.
  static QPDFObjectHandle Program(QPDFObjectHandle font) {
    QPDFObjectHandle described = Described(font);
    const std::string subtype = Name(described.getKey("/Subtype"));
    QPDFObjectHandle descriptor = described.isDictionary() ? described.getKey("/FontDescriptor") : QPDFObjectHandle::newNull();
    if (!descriptor.isDictionary())
      return QPDFObjectHandle::newNull();
    const auto stream = [&](const char *key, std::set<std::string> subtypes) {
      QPDFObjectHandle program = descriptor.getKey(key);
      if (!program.isStream() || (!subtypes.empty() && !subtypes.count(Name(program.getDict().getKey("/Subtype")))))
        return QPDFObjectHandle::newNull();
      const std::shared_ptr<Buffer> raw = program.getRawStreamData();
      return raw && raw->getSize() > 0 ? program : QPDFObjectHandle::newNull();
    };
    QPDFObjectHandle found = QPDFObjectHandle::newNull();
    if (subtype == "/Type1" || subtype == "/MMType1") {
      found = stream("/FontFile", {});
      if (found.isNull())
        found = stream("/FontFile3", {"/Type1C", "/OpenType"});
    } else if (subtype == "/TrueType" || subtype == "/CIDFontType2") {
      found = stream("/FontFile2", {});
      if (found.isNull())
        found = stream("/FontFile3", {"/OpenType"});
    } else if (subtype == "/CIDFontType0") {
      found = stream("/FontFile3", {"/CIDFontType0C", "/OpenType"});
    }
    return found;
  }

  // A subset font's name: six capital letters and a plus sign before it.
  static bool Subset(QPDFObjectHandle font) {
    const std::string name = Name(font.getKey("/BaseFont"));
    if (name.size() < 8 || name[7] != '+')
      return false;
    return std::all_of(name.begin() + 1, name.begin() + 7, [](char c) { return c >= 'A' && c <= 'Z'; });
  }

  void Fonts() {
    // The substitute each font that isn't embedded gets, and the characters each substitute
    // must have for all the fonts of one encoding it stands in for: one subset serves them all.
    // (Only fonts of one encoding: veraPDF reads a shared program once, with the first font's
    // encoding, for all of them.)
    struct Choice {
      std::string file, group;
      bool twin = false;
    };
    std::map<std::string, Choice> chosen;              // by font key
    std::map<std::string, font::SharedProgram> shared;  // by group: file and encoding
    std::map<std::string, unsigned> lacks;              // fonts no free font fits: a character none has
    for (auto &[key, use] : fonts_) {
      const std::string subtype = Name(use.font.getKey("/Subtype"));
      const auto found = checkers_.find(key);
      if ((subtype != "/Type1" && subtype != "/MMType1" && subtype != "/TrueType") || !Program(use.font).isNull() || fonts_dir_.empty() ||
          !(use.drawn || part_ == 1) || found == checkers_.end() || !found->second->Ready())
        continue;
      bool twin = false;
      unsigned lacking = 0;
      const std::string file = font::ChooseSubstitute(use.font, *found->second, [&](const std::string &name) { return SubstituteProgram(name); }, twin, &lacking);
      if (file.empty()) {
        lacks[key] = lacking;
        continue;
      }
      const std::string group = file + " " + use.font.getKey("/Encoding").unparseResolved();
      chosen[key] = {file, group, twin};
      for (const auto &[code, drawn] : found->second->Codes()) {
        shared[group].codes.insert(code);  // named or not: an unnamed one makes the font symbolic
        if (const unsigned character = font::UnicodeOf(found->second->NameFor(code)))
          shared[group].characters.insert(character);
      }
    }
    for (auto &[key, use] : fonts_) {
      QPDFObjectHandle font = use.font;
      const std::string subtype = Name(font.getKey("/Subtype"));
      std::string name = Name(font.getKey("/BaseFont"));
      name = name.empty() ? "unnamed" : name.substr(1);
      const bool type3 = subtype == "/Type3", simple = subtype == "/Type1" || subtype == "/MMType1" || subtype == "/TrueType";
      if (!type3 && !simple && subtype != "/Type0") {
        Problem("font-dictionary", name);
        continue;
      }
      if (!IsName(font.getKey("/Type"), "/Font")) {
        font.replaceKey("/Type", QPDFObjectHandle::newName("/Font"));
        Change("font-dictionaries");
      }
      // Not embedded: embedded from a free font of the same widths, when one is at hand.
      std::string why;  // why it couldn't be, for the problem
      if (simple && Program(font).isNull() && !fonts_dir_.empty() && (use.drawn || part_ == 1) && !chosen.count(key))
        why = Lacking(lacks.count(key) ? lacks[key] : 0);
      if (const auto plan = chosen.find(key); simple && plan != chosen.end()) {
        const auto found = checkers_.find(key);
        const auto &[file, group, twin] = plan->second;
        {
          const std::string &data = Substitute(file);
          if (!data.empty() && font::Embed(q_, font, data, *found->second, part_ == 1, why, &shared[group])) {
            // A close font, not the one named: the app says so.
            Change(twin ? "fonts-embedded" : "fonts-substituted");
            auto fresh = font::Recheck(font, *found->second);
            found->second = std::move(fresh);
          }
        }
      }
      // Not embedded and not drawn (invisible text): its dictionary as PDF requires it all the
      // same, with the widths of the free font readers would place it by.
      if (simple && Program(font).isNull() && !fonts_dir_.empty() && !kStandard14.count(Name(font.getKey("/BaseFont")))) {
        const auto found = checkers_.find(key);
        bool twin = false;
        const std::string file = found != checkers_.end() && found->second->Ready()
                                     ? font::ChooseSubstitute(font, *found->second, [&](const std::string &name) { return SubstituteProgram(name); }, twin)
                                     : std::string();
        const std::string &data = file.empty() ? file : Substitute(file);
        if (!data.empty() && font::DescribeFont(q_, font, data, *found->second))
          Change("font-widths-written");
      }
      // The same for a Type 0 font's CIDFont, its CIDs read as the characters its ToUnicode map
      // gives them (the original font's glyph numbers, which a substitute doesn't share).
      // A predefined Unicode CMap (UniJIS-UCS2-H, ...) gives the characters too: its codes are.
      const std::string cmap = Name(font.getKey("/Encoding"));
      const bool unicode_cmap = cmap.rfind("/Uni", 0) == 0 && cmap.find("-UCS2-") != std::string::npos && cmap.compare(cmap.size() - 2, 2, "-H") == 0;
      if (subtype == "/Type0" && Program(font).isNull() && !fonts_dir_.empty() && (use.drawn || part_ == 1) &&
          ((cmap == "/Identity-H" && font.getKey("/ToUnicode").isStream()) || unicode_cmap)) {
        const auto found = checkers_.find(key);
        // By CID: the ToUnicode map's character for its code, or the code itself in a Unicode CMap.
        std::map<unsigned, unsigned> characters;
        const std::map<unsigned, unsigned> by_code = font::Characters(font.getKey("/ToUnicode"));
        if (cmap == "/Identity-H") {
          characters = by_code;
        } else if (found != checkers_.end() && found->second->Ready()) {
          for (const auto &[cid, code] : found->second->ShownCodes()) {
            const auto mapped = by_code.find(code);
            characters.emplace(cid, mapped != by_code.end() ? mapped->second : code);
          }
        }
        bool twin = false;
        unsigned lacking = 0;
        const std::string file = found != checkers_.end() && found->second->Ready() && !characters.empty()
                                     ? font::ChooseCompositeSubstitute(font, *found->second, characters, [&](const std::string &name) { return SubstituteProgram(name); }, twin, &lacking)
                                     : std::string();
        if (file.empty())
          why = found == checkers_.end() ? "codes unreadable" : !found->second->Ready() ? "codes unreadable: " + found->second->Why()
                : characters.empty()                                ? "no characters for its codes"
                                                                    : Lacking(lacking);
        if (!file.empty()) {
          const std::string &data = Substitute(file);
          if (!data.empty() && font::EmbedComposite(q_, font, data, *found->second, characters, why)) {
            Change(twin ? "fonts-embedded" : "fonts-substituted");
            // PDF/A-1 lets only Identity go unembedded: the predefined CMap goes in.
            if (part_ == 1 && unicode_cmap)
              EmbedCMap(font, *found->second);
            auto fresh = font::Recheck(font, *found->second);
            found->second = std::move(fresh);
          }
        }
      }
      // A font whose program another font's embedding gave it (a shared CIDFont) is checked
      // against it.
      if (const auto stale = checkers_.find(key); stale != checkers_.end() && stale->second->Ready() && !stale->second->Font() && !Program(font).isNull()) {
        auto fresh = font::Recheck(font, *stale->second);
        stale->second = std::move(fresh);
      }
      // A Type 3 font's glyph procedures given the dictionary's widths (which place its text).
      if (const auto found = checkers_.find(key); type3 && found != checkers_.end() && found->second->Ready() && found->second->Widths() &&
                                                  font::Type3Widths(q_, font, *found->second)) {
        Change("type3-widths");
        auto fresh = font::Recheck(font, *found->second);
        found->second = std::move(fresh);
      }
      // Fixes that move nothing on the page, each checked by finding the font's codes again.
      const auto fixing = checkers_.find(key);
      if (fixing != checkers_.end() && fixing->second->Ready() && !Program(font).isNull()) {
        bool changed = false;
        if (font::FillWidths(font, *fixing->second)) {
          Change("font-widths-written");
          changed = true;
        }
        if (part_ == 1 && font::OneCmap(font, *fixing->second)) {
          Change("font-cmaps");
          auto fresh = font::Recheck(font, *fixing->second);
          fixing->second = std::move(fresh);
        }
        if (font::AddCmap(font, *fixing->second)) {
          Change("font-cmaps");
          auto fresh = font::Recheck(font, *fixing->second);
          fixing->second = std::move(fresh);
        }
        if (font::TrueTypeEncoding(q_, font, *fixing->second)) {
          Change("font-encodings");
          changed = true;
        }
        if (changed) {
          auto fresh = font::Recheck(font, *fixing->second);
          fixing->second = std::move(fresh);
        }
        if (fixing->second->Widths() && font::SetProgramWidths(font, *fixing->second)) {
          Change("font-program-widths");
          auto fresh = font::Recheck(font, *fixing->second);
          fixing->second = std::move(fresh);
        }
        // PDF/A-1: a subset's list of its glyphs, written from the program.
        QPDFObjectHandle own = Described(font).getKey("/FontDescriptor");
        const bool listed = own.isDictionary() && (own.hasKey("/CharSet") || own.hasKey("/CIDSet"));
        if (part_ == 1 && (Subset(font) || Subset(Described(font)) || listed) && font::GlyphLists(q_, font, *fixing->second))
          Change("font-glyph-lists-written");
      }
      // The font dictionary as PDF requires it (PDF/A-2 6.2.11.2), whatever the text mode.
      if (type3 || (simple && !(subtype == "/Type1" && kStandard14.count(Name(font.getKey("/BaseFont"))) && Program(font).isNull()))) {
        const int first = Integer(font, "/FirstChar", -1), last = Integer(font, "/LastChar", -1);
        QPDFObjectHandle widths = font.getKey("/Widths");
        if (first < 0 || last < first || !widths.isArray() || widths.getArrayNItems() != last - first + 1)
          Problem("font-dictionary", name);
      }
      QPDFObjectHandle program = Program(font);
      // Its glyphs as the pages show them (PDF/A-2 6.2.11.4.1, 6.2.11.5, 6.2.11.8); for a font
      // that isn't embedded, only the .notdef rule, which holds for invisible text too.
      const auto checker = checkers_.find(key);
      if (checker != checkers_.end()) {
        const font::Checker &c = *checker->second;
        if (!c.Ready()) {
          if (!program.isNull() || (part_ != 1 && !use.drawn))
            Problem("font-unchecked", name + " (" + c.Why() + ")");
        } else if ((c.Missing() || c.Widths() || c.Notdef()) && !PlanText(key, font, c)) {
          Problem(c.Missing() ? "font-glyphs" : c.Widths() ? "font-widths" : "font-notdef", name + " (" + c.Example() + ")");
        }
        if (c.BigCids())
          Problem("limits", "CID");
      }
      if (type3)
        continue;
      if (program.isNull()) {
        if (use.drawn)
          Problem("font-not-embedded", why.empty() ? name : name + " (" + why + ")");
        continue;
      }
      QPDFObjectHandle described = Described(font);
      QPDFObjectHandle descriptor = described.getKey("/FontDescriptor");
      if (subtype == "/TrueType" && descriptor.getKey("/FontFile2").isStream())
        TrueType(font, descriptor, program, name);
      if (subtype == "/Type0")
        Composite(font, described, name);
      // PDF/A-1 wants subsets to list their glyphs (and the lists right: checked later).
      if (part_ == 1 && Subset(font) && simple && subtype != "/TrueType" && !descriptor.getKey("/CharSet").isString())
        Problem("font-glyph-list", name);
      if (part_ == 1 && subtype == "/Type0" && Subset(described) && !descriptor.getKey("/CIDSet").isStream())
        Problem("font-glyph-list", name);
    }
  }

  // PDF/A-1: a predefined CMap a font names embedded, cut down to the codes the pages show (its
  // code spaces and collection as they are, each code with the CID it had).
  void EmbedCMap(QPDFObjectHandle font, const font::Checker &checker) {
    const std::string name = Name(font.getKey("/Encoding")).substr(1);
    const std::shared_ptr<const std::string> program = font::PredefinedCMap(name);
    if (!program)
      return;
    static const std::regex registry(R"(/Registry\s*\(([^)]*)\))"), ordering(R"(/Ordering\s*\(([^)]*)\))"), supplement(R"(/Supplement\s+(\d+))"),
        spaces(R"(\d+\s+begincodespacerange[\s\S]*?endcodespacerange)");
    std::smatch r, o, s, c;
    if (!std::regex_search(*program, r, registry) || !std::regex_search(*program, o, ordering) || !std::regex_search(*program, s, supplement) ||
        !std::regex_search(*program, c, spaces))
      return;
    static const char *hex = "0123456789ABCDEF";
    std::vector<std::string> entries;
    for (const auto &[code, cid] : checker.ShownCids()) {
      std::string text = "<";
      for (const char byte : code) {
        text += hex[static_cast<unsigned char>(byte) >> 4];
        text += hex[static_cast<unsigned char>(byte) & 15];
      }
      entries.push_back(text + "> " + std::to_string(cid) + "\n");
    }
    const std::string own = name + "-FynePDF";
    std::string text = "%!PS-Adobe-3.0 Resource-CMap\n/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n/CIDSystemInfo 3 dict dup begin\n/Registry (" +
                       r[1].str() + ") def\n/Ordering (" + o[1].str() + ") def\n/Supplement " + s[1].str() + " def\nend def\n/CMapName /" + own +
                       " def\n/CMapVersion 1.0 def\n/CMapType 1 def\n/WMode 0 def\n" + c[0].str() + "\n";
    for (size_t at = 0; at < entries.size(); at += 100) {
      const size_t count = std::min<size_t>(100, entries.size() - at);
      text += std::to_string(count) + " begincidchar\n";
      for (size_t k = 0; k < count; ++k)
        text += entries[at + k];
      text += "endcidchar\n";
    }
    text += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    QPDFObjectHandle info = QPDFObjectHandle::newDictionary();
    info.replaceKey("/Registry", QPDFObjectHandle::newString(r[1].str()));
    info.replaceKey("/Ordering", QPDFObjectHandle::newString(o[1].str()));
    info.replaceKey("/Supplement", QPDFObjectHandle::newInteger(std::stoi(s[1].str())));
    QPDFObjectHandle stream = QPDFObjectHandle::newStream(&q_, text);
    QPDFObjectHandle dict = stream.getDict();
    dict.replaceKey("/Type", QPDFObjectHandle::newName("/CMap"));
    dict.replaceKey("/CMapName", QPDFObjectHandle::newName("/" + own));
    dict.replaceKey("/CIDSystemInfo", info);
    dict.replaceKey("/WMode", QPDFObjectHandle::newInteger(0));
    font.replaceKey("/Encoding", stream);
    Change("cmaps-embedded");
  }

  // Why no free font stands in for one: a character none has (U+XXXX), or a code with none.
  static std::string Lacking(unsigned character) {
    if (!character)
      return "a code with no character";
    char text[32];
    std::snprintf(text, sizeof text, "no free font has U+%04X", character);
    return text;
  }

  // A substitute font's program, read once; null when it isn't there.
  const font::Program *SubstituteProgram(const std::string &file) {
    auto found = substitute_programs_.find(file);
    if (found == substitute_programs_.end()) {
      const std::string &data = Substitute(file);
      found = substitute_programs_.emplace(file, data.empty() ? nullptr : font::Program::Read(data, font::Container::TrueType)).first;
    }
    return found->second.get();
  }

  // A substitute font's file, read once; "" when it isn't there.
  const std::string &Substitute(const std::string &file) {
    auto found = substitutes_.find(file);
    if (found == substitutes_.end()) {
      std::ifstream in(fonts_dir_ + "/" + file, std::ios::binary);
      std::ostringstream data;
      if (in)
        data << in.rdbuf();
      found = substitutes_.emplace(file, data.str()).first;
    }
    return found->second;
  }

  // -----------------------------------------------------------------------------------------
  // Text kept in place: widths a font program can't take become the program's, with the
  // difference added back after the glyph (a TJ shift); codes that draw a blank .notdef are
  // taken out, their whole advance added back. Every glyph after them stays where it was.

  struct Plan {
    QPDFObjectHandle font;
    const font::Checker *checker = nullptr;  // as the pages were first read
    std::set<unsigned> remove;                // codes (CIDs) drawing .notdef
    std::map<unsigned, std::pair<double, double>> widths;  // code: old dictionary width, the program's
    std::unique_ptr<font::Checker> verify;    // the text as rewritten, checked again
    bool blank = false;                       // its .notdef draws nothing
    bool boxes = false;                       // a .notdef that drew something was taken out
  };

  // A font whose glyph problems the text can fix: true when it will be (its problems then come
  // from checking the rewritten text).
  bool PlanText(const std::string &key, QPDFObjectHandle font, const font::Checker &c) {
    if (!c.Ready() || content_unreadable_ || IsName(font.getKey("/Subtype"), "/Type3"))
      return false;
    // Vertical writing moves down, not across: left as it is.
    if (IsName(font.getKey("/Encoding"), "/Identity-V"))
      return false;
    Plan plan;
    plan.font = font;
    plan.checker = &c;
    plan.blank = c.NotdefBlank();
    const font::Program *program = c.Font();
    for (const auto &[code, drawn] : c.Codes()) {
      if (c.DrawsNotdef(code)) {
        // PDF/A-1 has no .notdef rule as such, but veraPDF counts a code that finds no glyph (and
        // so draws .notdef) as one the font lacks: it goes at every level.
        plan.remove.insert(code);
        continue;
      }
      if (!drawn || !program)
        continue;
      const int glyph = c.GlyphFor(code);
      const double width = glyph >= 0 ? program->Width(glyph) : -1, dictionary = c.DictionaryWidth(code);
      if (width >= 0 && std::fabs(width - dictionary) > 1)
        plan.widths.emplace(code, std::make_pair(dictionary, std::round(width * 1000) / 1000));
    }
    if (plan.remove.empty() && plan.widths.empty())
      return false;
    plans_.emplace(key, std::move(plan));
    return true;
  }

  void AdjustText() {
    if (plans_.empty())
      return;
    // The dictionaries take the programs' widths.
    for (auto &[key, plan] : plans_) {
      if (plan.widths.empty())
        continue;
      QPDFObjectHandle font = plan.font;
      if (plan.checker->Composite()) {
        QPDFObjectHandle descendants = font.getKey("/DescendantFonts");
        QPDFObjectHandle descendant = descendants.isArray() && descendants.getArrayNItems() > 0 ? descendants.getArrayItem(0) : QPDFObjectHandle::newNull();
        // The widths as they are now (another Type 0 font sharing this CIDFont may have changed
        // some), with this one's changes.
        std::map<unsigned, double> widths;
        const double fallback = descendant.getKey("/DW").isNumber() ? descendant.getKey("/DW").getNumericValue() : 1000;
        QPDFObjectHandle current = descendant.isDictionary() ? descendant.getKey("/W") : QPDFObjectHandle::newNull();
        for (int index = 0; current.isArray() && index < current.getArrayNItems();) {
          QPDFObjectHandle first = current.getArrayItem(index);
          QPDFObjectHandle next = index + 1 < current.getArrayNItems() ? current.getArrayItem(index + 1) : QPDFObjectHandle::newNull();
          if (!first.isInteger())
            break;
          const long long start = first.getIntValue();
          if (next.isArray()) {
            for (int k = 0; k < next.getArrayNItems(); ++k)
              widths.emplace(static_cast<unsigned>(start + k), next.getArrayItem(k).isNumber() ? next.getArrayItem(k).getNumericValue() : fallback);
            index += 2;
          } else if (next.isInteger() && index + 2 < current.getArrayNItems()) {
            const long long stop = next.getIntValue();
            QPDFObjectHandle value = current.getArrayItem(index + 2);
            for (long long cid = start; cid <= stop && cid - start < 65536; ++cid)
              widths.emplace(static_cast<unsigned>(cid), value.isNumber() ? value.getNumericValue() : fallback);
            index += 3;
          } else {
            break;
          }
        }
        for (const auto &[cid, change] : plan.widths)
          widths[cid] = change.second;
        // W written out again: runs of consecutive CIDs.
        std::vector<QPDFObjectHandle> list;
        for (auto it = widths.begin(); it != widths.end();) {
          const unsigned first = it->first;
          std::vector<double> run;
          unsigned next = first;
          while (it != widths.end() && it->first == next) {
            run.push_back(it->second);
            ++it;
            ++next;
          }
          list.push_back(QPDFObjectHandle::newInteger(first));
          list.push_back(NumberArray(run));
        }
        if (descendant.isDictionary())
          descendant.replaceKey("/W", QPDFObjectHandle::newArray(list));
      } else {
        int first = Integer(font, "/FirstChar", 0), last = Integer(font, "/LastChar", -1);
        QPDFObjectHandle old = font.getKey("/Widths");
        std::vector<double> widths = Numbers(old);
        for (const auto &[code, change] : plan.widths) {
          // Codes past the dictionary's range had the missing width; the range grows to them.
          while (static_cast<int>(code) < first) {
            widths.insert(widths.begin(), plan.checker->DictionaryWidth(static_cast<unsigned>(first - 1)));
            --first;
          }
          while (static_cast<int>(code) > last) {
            widths.push_back(plan.checker->DictionaryWidth(static_cast<unsigned>(last + 1)));
            ++last;
          }
          widths[static_cast<size_t>(static_cast<int>(code) - first)] = change.second;
        }
        font.replaceKey("/FirstChar", QPDFObjectHandle::newInteger(first));
        font.replaceKey("/LastChar", QPDFObjectHandle::newInteger(last));
        font.replaceKey("/Widths", NumberArray(widths));
      }
    }
    for (auto &[key, plan] : plans_)
      plan.verify = std::make_unique<font::Checker>(plan.font);
    // Every content stream that shows text, rewritten where these fonts are used.
    for (QPDFPageObjectHelper &helper : pages_) {
      QPDFObjectHandle page = helper.getObjectHandle();
      QPDFObjectHandle resources = page.getKey("/Resources");
      bool ok = true;
      const std::string content = PageContent(page, ok);
      std::string rewritten;
      if (ok && AdjustWalk(content, resources, TextState{}, rewritten, 0))
        page.replaceKey("/Contents", QPDFObjectHandle::newStream(&q_, rewritten));
      AdjustResources(resources, 0);
      QPDFObjectHandle annotations = page.getKey("/Annots");
      for (int index = 0; annotations.isArray() && index < annotations.getArrayNItems(); ++index) {
        QPDFObjectHandle appearance = annotations.getArrayItem(index).getKey("/AP");
        QPDFObjectHandle normal = appearance.isDictionary() ? appearance.getKey("/N") : QPDFObjectHandle::newNull();
        if (normal.isStream())
          AdjustStream(normal, resources, TextState{}, 0);
        else if (normal.isDictionary())
          for (const std::string &state : normal.getKeys())
            AdjustStream(normal.getKey(state), resources, TextState{}, 0);
      }
    }
    // What the rewritten text shows, checked like the first time.
    for (auto &[key, plan] : plans_) {
      const font::Checker &c = *plan.verify;
      std::string name = Name(plan.font.getKey("/BaseFont"));
      name = name.empty() ? "unnamed" : name.substr(1);
      if (c.Missing() || c.Widths() || c.Notdef()) {
        Problem(c.Missing() ? "font-glyphs" : c.Widths() ? "font-widths" : "font-notdef", name + " (" + c.Example() + ")");
      } else {
        if (!plan.widths.empty())
          Change("text-widths-kept");
        if (!plan.remove.empty())
          Change(plan.boxes ? "notdef-boxes-removed" : "notdef-removed");
      }
    }
  }

  void AdjustStream(QPDFObjectHandle stream, QPDFObjectHandle inherited, const TextState &state, int depth) {
    if (!stream.isStream() || depth > 32 || !First(stream, 8 + (state.mode == 3 ? 1 : 0)))
      return;
    QPDFObjectHandle own = stream.getDict().getKey("/Resources");
    QPDFObjectHandle resources = own.isDictionary() ? own : inherited;
    bool ok = true;
    const std::string content = StreamData(stream, ok);
    std::string rewritten;
    if (ok && AdjustWalk(content, resources, state, rewritten, depth))
      stream.replaceStreamData(rewritten, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
    AdjustResources(resources, depth);
  }

  void AdjustResources(QPDFObjectHandle resources, int depth) {
    if (!resources.isDictionary() || !First(resources, 10))
      return;
    QPDFObjectHandle patterns = resources.getKey("/Pattern");
    for (const std::string &key : patterns.isDictionary() ? patterns.getKeys() : std::set<std::string>{})
      AdjustStream(patterns.getKey(key), resources, TextState{}, depth + 1);
    QPDFObjectHandle states = resources.getKey("/ExtGState");
    for (const std::string &key : states.isDictionary() ? states.getKeys() : std::set<std::string>{}) {
      QPDFObjectHandle mask = states.getKey(key).isDictionary() ? states.getKey(key).getKey("/SMask") : QPDFObjectHandle::newNull();
      if (mask.isDictionary())
        AdjustStream(mask.getKey("/G"), resources, TextState{}, depth + 1);
    }
  }

  bool AdjustWalk(const std::string &content, QPDFObjectHandle resources, TextState state, std::string &out, int depth) {
    const auto resource = [&](const char *kind, const std::string &name) {
      QPDFObjectHandle group = resources.isDictionary() ? resources.getKey(kind) : QPDFObjectHandle::newNull();
      return group.isDictionary() ? group.getKey("/" + FixedName(name)) : QPDFObjectHandle::newNull();
    };
    std::vector<TextState> stack;
    bool changed = false;
    std::string rewritten;
    rewritten.reserve(content.size() + 64);
    const bool well_formed = ForEachOp(content, [&](const Op &op) {
      std::string replaced;
      const auto number = [&](size_t at) { return at < op.operands.size() && op.operands[at].kind == Token::Number ? op.operands[at].number : 0.0; };
      if (op.name == "q") {
        stack.push_back(state);
      } else if (op.name == "Q") {
        if (!stack.empty()) {
          state = stack.back();
          stack.pop_back();
        }
      } else if (op.name == "Tf" && op.operands.size() == 2 && op.operands[0].kind == Token::Name) {
        state.font = resource("/Font", op.operands[0].text);
        state.size = number(1);
      } else if (op.name == "Tc") {
        state.tc = number(0);
      } else if (op.name == "Tw") {
        state.tw = number(0);
      } else if (op.name == "Tr") {
        state.mode = static_cast<int>(number(0));
      } else if (op.name == "gs" && op.operands.size() == 1 && op.operands[0].kind == Token::Name) {
        QPDFObjectHandle gs = resource("/ExtGState", op.operands[0].text);
        QPDFObjectHandle font = gs.isDictionary() ? gs.getKey("/Font") : QPDFObjectHandle::newNull();
        if (font.isArray() && font.getArrayNItems() >= 2) {
          state.font = font.getArrayItem(0);
          state.size = font.getArrayItem(1).isNumber() ? font.getArrayItem(1).getNumericValue() : state.size;
        }
      } else if (op.name == "Tj" || op.name == "TJ" || op.name == "'" || op.name == "\"") {
        replaced = AdjustShow(op, state);
      } else if (op.name == "Do" && op.operands.size() == 1 && op.operands[0].kind == Token::Name) {
        QPDFObjectHandle xobject = resource("/XObject", op.operands[0].text);
        if (xobject.isStream() && IsName(xobject.getDict().getKey("/Subtype"), "/Form"))
          AdjustStream(xobject, resources, state, depth + 1);
      }
      if (!replaced.empty()) {
        rewritten += replaced;
        changed = true;
      } else {
        rewritten.append(content, op.begin, op.end - op.begin);
      }
      rewritten += '\n';
    });
    if (!well_formed)
      return false;
    if (changed)
      out.swap(rewritten);
    return changed;
  }

  // A text operator rewritten as one TJ for a planned font; "" to leave it.
  std::string AdjustShow(const Op &op, TextState &state) {
    std::string prefix;
    if (op.name == "\"" && op.operands.size() == 3) {
      // aw ac (string) ": the spacing it sets holds for this string too.
      state.tw = op.operands[0].kind == Token::Number ? op.operands[0].number : state.tw;
      state.tc = op.operands[1].kind == Token::Number ? op.operands[1].number : state.tc;
    }
    if (!state.font.isDictionary())
      return "";
    const std::string key = state.font.isIndirect() ? std::to_string(state.font.getObjectID()) + " " + std::to_string(state.font.getGeneration()) : state.font.unparse();
    const auto found = plans_.find(key);
    if (found == plans_.end())
      return "";
    Plan &plan = found->second;
    const bool invisible = state.mode == 3 || state.mode == 7;  // nothing drawn (may go)
    const bool drawn = state.mode != 3;                          // what veraPDF checks widths for
    const bool simple = !plan.checker->Composite();
    static const char *hex = "0123456789abcdef";
    std::vector<std::string> items;
    std::string current;
    bool shifted = false;
    const auto flush = [&]() {
      if (current.empty())
        return;
      std::string text = "<";
      for (const char c : current) {
        text += hex[static_cast<unsigned char>(c) >> 4];
        text += hex[static_cast<unsigned char>(c) & 15];
      }
      items.push_back(text + ">");
      current.clear();
    };
    for (size_t at = 0; at < op.operands.size(); ++at) {
      const Token &token = op.operands[at];
      if (token.kind == Token::Number && op.name == "TJ") {
        flush();
        items.push_back(Num(token.number));
        continue;
      }
      if (token.kind != Token::String || (op.name != "TJ" && at + 1 != op.operands.size()))
        continue;
      size_t used = 0;
      for (const font::Checker::Piece &piece : plan.checker->Pieces(token.text)) {
        const std::string bytes = token.text.substr(piece.begin, piece.length);
        used = piece.begin + piece.length;
        if (plan.remove.count(piece.code) && state.size != 0) {
          // Taken out, its advance (width and spacing) added back. PDF/A allows no .notdef at
          // all, so one readers drew as a box goes too (said apart: the page shows less).
          if (!invisible && !plan.blank && plan.checker->Font())
            plan.boxes = true;
          const double spacing = state.tc + (simple && piece.code == 32 ? state.tw : 0);
          flush();
          items.push_back(Num(-(plan.checker->DictionaryWidth(piece.code) + spacing * 1000 / state.size)));
          shifted = true;
          continue;
        }
        current += bytes;
        plan.verify->Show(bytes, drawn);
        const auto change = plan.widths.find(piece.code);
        if (change != plan.widths.end()) {
          flush();
          items.push_back(Num(-(change->second.first - change->second.second)));
          shifted = true;
        }
      }
      // A half code left at the end stays (and is checked: it's a problem of its own).
      const std::string rest = token.text.substr(std::min(used, token.text.size()));
      current += rest;
      if (!rest.empty())
        plan.verify->Show(rest, drawn);
    }
    flush();
    if (!shifted)
      return "";
    if (op.name == "'")
      prefix = "T*\n";
    else if (op.name == "\"")
      prefix = Num(state.tw) + " Tw " + Num(state.tc) + " Tc T*\n";
    std::string array = "[";
    for (size_t at = 0; at < items.size(); ++at)
      array += (at ? " " : "") + items[at];
    return prefix + array + "] TJ";
  }

  // A TrueType font's encoding and cmap table as PDF/A requires them (PDF/A-2 6.2.11.6).
  void TrueType(QPDFObjectHandle font, QPDFObjectHandle descriptor, QPDFObjectHandle program, const std::string &name) {
    const bool symbolic = (Integer(descriptor, "/Flags", 0) & 4) != 0;
    QPDFObjectHandle encoding = font.getKey("/Encoding");
    std::string data;
    std::vector<std::pair<unsigned, unsigned>> cmaps;
    if (!Decoded(program, data) || !CmapEncodings(data, cmaps)) {
      Problem("font-cmap", name + " (no cmap table)");
      return;
    }
    const auto has = [&](unsigned platform, unsigned id) { return std::find(cmaps.begin(), cmaps.end(), std::make_pair(platform, id)) != cmaps.end(); };
    const bool unicode = std::any_of(cmaps.begin(), cmaps.end(), [](const std::pair<unsigned, unsigned> &c) { return c.first == 0; });
    if (symbolic) {
      if (!encoding.isNull())
        Problem("font-encoding", name);
      // PDF/A-1: exactly one; PDF/A-2 also takes several with (3,0) among them.
      if (cmaps.size() != 1 && (part_ == 1 || !has(3, 0)))
        Problem("font-cmap", name);
      return;
    }
    const std::string base = encoding.isName() ? encoding.getName() : encoding.isDictionary() ? Name(encoding.getKey("/BaseEncoding")) : "";
    if (base != "/MacRomanEncoding" && base != "/WinAnsiEncoding")
      Problem("font-encoding", name);
    if (encoding.isDictionary() && encoding.hasKey("/Differences")) {
      // Names it changes must be in the Adobe Glyph List, and the program's Unicode cmap must
      // have each one's character (veraPDF: "Differences are Unicode compliant").
      const auto read = font::Program::Read(data, font::Container::TrueType);
      bool listed = read != nullptr;
      QPDFObjectHandle differences = encoding.getKey("/Differences");
      for (int index = 0; listed && differences.isArray() && index < differences.getArrayNItems(); ++index) {
        QPDFObjectHandle item = differences.getArrayItem(index);
        if (!item.isName())
          continue;
        const std::string glyph = item.getName().substr(1);
        listed = font::InGlyphList(glyph) && read->ByUnicode(font::UnicodeOf(glyph)) > 0;
      }
      if (!listed || (!has(3, 1) && !unicode))
        Problem("font-encoding", name);
    }
    if (!has(3, 1) && !has(1, 0) && !unicode)
      Problem("font-cmap", name);
  }

  // An embedded CIDFont's collection named as its CMap's: the CMap gives the CIDs, the font's
  // program finds their glyphs itself, so the name changes no glyph.
  void Relabel(QPDFObjectHandle described, const std::string &registry, const std::string &ordering) {
    QPDFObjectHandle info = described.getKey("/CIDSystemInfo");
    QPDFObjectHandle made = info.isDictionary() ? info.shallowCopy() : QPDFObjectHandle::newDictionary();
    made.replaceKey("/Registry", QPDFObjectHandle::newString(registry));
    made.replaceKey("/Ordering", QPDFObjectHandle::newString(ordering));
    if (!made.getKey("/Supplement").isInteger())
      made.replaceKey("/Supplement", QPDFObjectHandle::newInteger(0));
    described.replaceKey("/CIDSystemInfo", made);
    Change("cid-collections");
  }

  // A CIDFont's collection supplement no newer than its predefined CMap's (which the CMap's text
  // says; veraPDF compares them): lowered when it is. The CMap only gives CIDs of its own
  // supplement, so no code finds another glyph.
  void Supplement(QPDFObjectHandle described, const std::shared_ptr<const std::string> &cmap) {
    static const std::regex supplement(R"(/Supplement\s+(\d+))");
    std::smatch match;
    QPDFObjectHandle info = described.getKey("/CIDSystemInfo");
    if (!cmap || !info.isDictionary() || !std::regex_search(*cmap, match, supplement))
      return;
    const int own = std::stoi(match[1].str());
    if (Integer(info, "/Supplement", 0) <= own)
      return;
    QPDFObjectHandle lowered = info.shallowCopy();
    lowered.replaceKey("/Supplement", QPDFObjectHandle::newInteger(own));
    described.replaceKey("/CIDSystemInfo", lowered);
    Change("cid-supplements");
  }

  // A composite font's CMap and character collection (PDF/A-2 6.2.11.3).
  void Composite(QPDFObjectHandle font, QPDFObjectHandle described, const std::string &name) {
    if (IsName(described.getKey("/Subtype"), "/CIDFontType2")) {
      QPDFObjectHandle map = described.getKey("/CIDToGIDMap");
      if (!map.isStream() && !IsName(map, "/Identity")) {
        described.replaceKey("/CIDToGIDMap", QPDFObjectHandle::newName("/Identity"));  // what readers do with anything else
        Change("cid-maps");
      }
    }
    QPDFObjectHandle info = described.getKey("/CIDSystemInfo");
    const std::string registry = info.isDictionary() ? Text(info.getKey("/Registry")) : "";
    const std::string ordering = info.isDictionary() ? Text(info.getKey("/Ordering")) : "";
    QPDFObjectHandle encoding = font.getKey("/Encoding");
    if (encoding.isName()) {
      const auto found = kCMaps.find(encoding.getName().substr(1));
      // PDF/A-1 lets only Identity-H and V go unembedded.
      if (found == kCMaps.end() || (part_ == 1 && !found->second.empty()))
        Problem("cmap-not-embedded", name);
      else if (!found->second.empty()) {
        if (registry != "Adobe" || ordering != found->second)
          Relabel(described, "Adobe", found->second);
        Supplement(described, font::PredefinedCMap(found->first));
      }
      return;
    }
    if (!encoding.isStream()) {
      Problem("cmap-not-embedded", name);
      return;
    }
    QPDFObjectHandle dict = encoding.getDict();
    QPDFObjectHandle own = dict.getKey("/CIDSystemInfo");
    if (own.isArray() && own.getArrayNItems() > 0)
      own = own.getArrayItem(0);
    // The same collection, and the CMap at least as recent a supplement of it (the font's
    // lowered when it's newer: the CMap gives only CIDs of its own).
    if (own.isDictionary() && (Text(own.getKey("/Registry")) != registry || Text(own.getKey("/Ordering")) != ordering))
      Relabel(described, Text(own.getKey("/Registry")), Text(own.getKey("/Ordering")));
    if (own.isDictionary() && Integer(own, "/Supplement", 0) < Integer(described.getKey("/CIDSystemInfo"), "/Supplement", 0))
      Supplement(described, std::make_shared<const std::string>("/Supplement " + std::to_string(Integer(own, "/Supplement", 0))));
    // The CMap's writing mode, said twice, the same; and no other CMap used but the standard ones.
    std::string program;
    if (!Decoded(encoding, program)) {
      Problem("cmap", name);
      return;
    }
    static const std::regex wmode(R"(/WMode\s+(\d+)\s+def)"), usecmap(R"(/([^\s/\[\]()<>{}%]+)\s+usecmap)");
    std::smatch match;
    // The dictionary takes the program's writing mode.
    const int said = std::regex_search(program, match, wmode) ? std::stoi(match[1].str()) : 0;
    if (said != Integer(dict, "/WMode", 0)) {
      dict.replaceKey("/WMode", QPDFObjectHandle::newInteger(said));
      Change("cmap-modes");
    }
    // Another CMap it uses: a standard one stays; an embedded one is merged in (its mappings
    // first, so the CMap's own still win); one that's neither, readers can't find, and it goes.
    std::string merged;
    bool rewrite = false;
    QPDFObjectHandle use = dict.getKey("/UseCMap");
    if (use.isStream()) {
      std::string used;
      QPDFObjectHandle deeper = use.getDict().getKey("/UseCMap");
      if (!Decoded(use, used) || deeper.isStream() || (deeper.isName() && !kCMaps.count(deeper.getName().substr(1)))) {
        Problem("cmap", name);
        return;
      }
      static const std::regex blocks(R"(\d+\s+begin(codespacerange|cidrange|cidchar|notdefrange|notdefchar)\b[\s\S]*?\bend\1)");
      for (auto it = std::sregex_iterator(used.begin(), used.end(), blocks); it != std::sregex_iterator(); ++it)
        merged += it->str() + "\n";
      if (deeper.isName())
        dict.replaceKey("/UseCMap", deeper);
      else
        dict.removeKey("/UseCMap");
      rewrite = true;
    } else if (use.isName() && !kCMaps.count(use.getName().substr(1))) {
      dict.removeKey("/UseCMap");
      rewrite = true;
    }
    if (std::regex_search(program, match, usecmap) && (!kCMaps.count(match[1].str()) || rewrite)) {
      QPDFObjectHandle kept = dict.getKey("/UseCMap");
      program.replace(static_cast<size_t>(match.position(0)), static_cast<size_t>(match.length(0)),
                      kept.isName() ? "/" + kept.getName().substr(1) + " usecmap" : "");
      rewrite = true;
    }
    if (rewrite) {
      const size_t begin = program.find("begincmap");
      if (begin == std::string::npos) {
        Problem("cmap", name);
        return;
      }
      program.insert(begin + 9, "\n" + merged);
      encoding.replaceStreamData(program, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
      Change("cmaps-merged");
    }
  }

  // -----------------------------------------------------------------------------------------
  // PDF/A-1 transparency: pages that blend drawn as pictures

  bool Flattening() const { return part_ == 1 && flatten_dpi_ > 0 && flatten::Available(); }

  // Whether a graphics state blends: a soft mask, an alpha below 1, a blend mode other than Normal.
  static bool StateBlends(QPDFObjectHandle state) {
    if (!state.isDictionary())
      return false;
    if (state.getKey("/SMask").isDictionary())
      return true;
    for (const char *key : {"/CA", "/ca"}) {
      QPDFObjectHandle value = state.getKey(key);
      if (value.isNumber() && value.getNumericValue() < 1)
        return true;
    }
    QPDFObjectHandle mode = state.getKey("/BM");
    if (mode.isArray() && mode.getArrayNItems() > 0)
      mode = mode.getArrayItem(0);
    return mode.isName() && mode.getName() != "/Normal" && mode.getName() != "/Compatible";
  }

  // Whether drawing with |resources| can blend: a state that does, a picture with a soft mask (or
  // alpha of its own), or forms, patterns and Type 3 glyphs that do. Each indirect object is
  // answered once.
  bool Blends(QPDFObjectHandle resources, int depth) {
    if (!resources.isDictionary() || depth > 24)
      return false;
    const auto answer = [&](QPDFObjectHandle object, const std::function<bool()> &compute) {
      if (!object.isIndirect())
        return compute();
      const auto [at, fresh] = blends_memo_.emplace(object.getObjGen(), 0);
      if (!fresh)
        return at->second == 2;  // 0: being answered (a loop), 1: no, 2: yes
      const bool result = compute();
      blends_memo_[object.getObjGen()] = result ? 2 : 1;
      return result;
    };
    return answer(resources, [&]() {
      QPDFObjectHandle states = resources.getKey("/ExtGState");
      for (const std::string &key : states.isDictionary() ? states.getKeys() : std::set<std::string>{})
        if (StateBlends(states.getKey(key)))
          return true;
      // A form's or tiling pattern's own resources (inherited ones are being answered here).
      const auto stream_blends = [&](QPDFObjectHandle stream) {
        return answer(stream, [&]() {
          QPDFObjectHandle own = stream.getDict().getKey("/Resources");
          return own.isDictionary() && Blends(own, depth + 1);
        });
      };
      QPDFObjectHandle xobjects = resources.getKey("/XObject");
      for (const std::string &key : xobjects.isDictionary() ? xobjects.getKeys() : std::set<std::string>{}) {
        QPDFObjectHandle xobject = xobjects.getKey(key);
        if (!xobject.isStream())
          continue;
        QPDFObjectHandle dict = xobject.getDict();
        if (IsName(dict.getKey("/Subtype"), "/Image") && (dict.getKey("/SMask").isStream() || Integer(dict, "/SMaskInData", 0) != 0))
          return true;
        if (IsName(dict.getKey("/Subtype"), "/Form") && stream_blends(xobject))
          return true;
      }
      QPDFObjectHandle patterns = resources.getKey("/Pattern");
      for (const std::string &key : patterns.isDictionary() ? patterns.getKeys() : std::set<std::string>{}) {
        QPDFObjectHandle pattern = patterns.getKey(key);
        if ((pattern.isStream() && stream_blends(pattern)) || (pattern.isDictionary() && StateBlends(pattern.getKey("/ExtGState"))))
          return true;
      }
      QPDFObjectHandle fonts = resources.getKey("/Font");
      for (const std::string &key : fonts.isDictionary() ? fonts.getKeys() : std::set<std::string>{}) {
        QPDFObjectHandle font = fonts.getKey(key);
        if (font.isDictionary() && IsName(font.getKey("/Subtype"), "/Type3") && Blends(font.getKey("/Resources"), depth + 1))
          return true;
      }
      return false;
    });
  }

  // Whether a comment blends: an opacity below 1, a blend mode, or an appearance that does.
  bool AnnotationBlends(QPDFObjectHandle annotation) {
    if (!annotation.isDictionary())
      return false;
    if (StateBlends(annotation))
      return true;
    QPDFObjectHandle appearance = annotation.getKey("/AP");
    QPDFObjectHandle normal = appearance.isDictionary() ? appearance.getKey("/N") : QPDFObjectHandle::newNull();
    std::vector<QPDFObjectHandle> streams;
    if (normal.isStream())
      streams.push_back(normal);
    for (const std::string &key : normal.isDictionary() ? normal.getKeys() : std::set<std::string>{})
      streams.push_back(normal.getKey(key));
    for (QPDFObjectHandle stream : streams)
      if (stream.isStream() && Blends(stream.getDict().getKey("/Resources"), 0))
        return true;
    return false;
  }

  // A content stream's text alone, invisible: what stays of a page drawn as a picture, so its
  // text can still be found and copied. Positions (q, Q, cm), text and marked content stay, in
  // rendering mode 3; paths, painting, colours, pictures and graphics states go; forms are
  // followed, made text-only too, under their names in |xobjects|.
  std::string TextOnly(const std::string &content, QPDFObjectHandle resources, QPDFObjectHandle xobjects, int depth) {
    static const std::set<std::string> kept = {"q",  "Q",  "cm", "BT", "ET", "Tc", "Tw", "Tz",  "TL",  "Tf",  "Ts", "Td",
                                               "TD", "Tm", "T*", "Tj", "TJ", "'",  "\"", "BMC", "BDC", "EMC", "MP", "DP"};
    std::string out;
    ForEachOp(content, [&](const Op &op) {
      if (op.name == "BT") {
        out += "BT\n3 Tr\n";
      } else if (op.name == "Tr") {
        out += "3 Tr\n";
      } else if (op.name == "Do" && op.operands.size() == 1 && op.operands[0].kind == Token::Name) {
        QPDFObjectHandle group = resources.isDictionary() ? resources.getKey("/XObject") : QPDFObjectHandle::newNull();
        const std::string key = "/" + FixedName(op.operands[0].text);
        QPDFObjectHandle form = group.isDictionary() ? group.getKey(key) : QPDFObjectHandle::newNull();
        if (depth < 16 && form.isStream() && IsName(form.getDict().getKey("/Subtype"), "/Form")) {
          QPDFObjectHandle made = TextForm(form, resources, depth + 1);
          if (made.isStream()) {
            xobjects.replaceKey(key, made);
            out.append(content, op.begin, op.end - op.begin);
            out += '\n';
          }
        }
      } else if (kept.count(op.name)) {
        out.append(content, op.begin, op.end - op.begin);
        out += '\n';
      }
    });
    return out;
  }

  // A form made text-only (TextOnly), once for each form.
  QPDFObjectHandle TextForm(QPDFObjectHandle form, QPDFObjectHandle inherited, int depth) {
    const auto found = text_forms_.find(form.getObjGen());
    if (found != text_forms_.end())
      return found->second;
    text_forms_[form.getObjGen()] = QPDFObjectHandle::newNull();  // a form that draws itself
    QPDFObjectHandle dict = form.getDict();
    QPDFObjectHandle own = dict.getKey("/Resources");
    QPDFObjectHandle resources = own.isDictionary() ? own : inherited;
    bool ok = true;
    const std::string content = StreamData(form, ok);
    if (!ok)
      return QPDFObjectHandle::newNull();
    QPDFObjectHandle xobjects = QPDFObjectHandle::newDictionary();
    const std::string text = TextOnly(content, resources, xobjects, depth);
    QPDFObjectHandle made = QPDFObjectHandle::newStream(&q_, text);
    QPDFObjectHandle made_dict = made.getDict();
    made_dict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
    made_dict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
    for (const char *key : {"/BBox", "/Matrix"})
      if (dict.hasKey(key))
        made_dict.replaceKey(key, dict.getKey(key));
    made_dict.replaceKey("/Resources", TextResources(resources, xobjects));
    text_forms_[form.getObjGen()] = made;
    return made;
  }

  // The resources text-only content needs: fonts (a Type 3 one that blends made quiet),
  // marked content's properties, the forms in it.
  QPDFObjectHandle TextResources(QPDFObjectHandle resources, QPDFObjectHandle xobjects) {
    QPDFObjectHandle made = QPDFObjectHandle::newDictionary();
    if (resources.isDictionary() && resources.getKey("/Properties").isDictionary())
      made.replaceKey("/Properties", resources.getKey("/Properties"));
    QPDFObjectHandle fonts = resources.isDictionary() ? resources.getKey("/Font") : QPDFObjectHandle::newNull();
    if (fonts.isDictionary()) {
      QPDFObjectHandle kept = QPDFObjectHandle::newDictionary();
      for (const std::string &key : fonts.getKeys()) {
        QPDFObjectHandle font = fonts.getKey(key);
        const bool blends = font.isDictionary() && IsName(font.getKey("/Subtype"), "/Type3") && Blends(font.getKey("/Resources"), 0);
        kept.replaceKey(key, blends ? QuietType3(font) : font);
      }
      made.replaceKey("/Font", kept);
    }
    if (!xobjects.getKeys().empty())
      made.replaceKey("/XObject", xobjects);
    return made;
  }

  // A Type 3 font whose glyphs draw nothing (each keeps only its width), for invisible text: its
  // codes and widths as they were, nothing in it that could blend. Once for each font.
  QPDFObjectHandle QuietType3(QPDFObjectHandle font) {
    if (font.isIndirect())
      if (const auto found = quiet_fonts_.find(font.getObjGen()); found != quiet_fonts_.end())
        return found->second;
    QPDFObjectHandle made = font.shallowCopy();
    made.removeKey("/Resources");
    QPDFObjectHandle procs = font.getKey("/CharProcs");
    QPDFObjectHandle quiet = QPDFObjectHandle::newDictionary();
    for (const std::string &name : procs.isDictionary() ? procs.getKeys() : std::set<std::string>{}) {
      bool ok = true;
      const std::string content = StreamData(procs.getKey(name), ok);
      std::string width = "0 0 d0";
      ForEachOp(ok ? content : std::string(), [&](const Op &op) {
        if ((op.name == "d0" || op.name == "d1") && width == "0 0 d0" && op.operands.size() >= 2)
          width = content.substr(op.begin, op.end - op.begin);
      });
      quiet.replaceKey(name, QPDFObjectHandle::newStream(&q_, width + "\n"));
    }
    made.replaceKey("/CharProcs", quiet);
    QPDFObjectHandle indirect = q_.makeIndirectObject(made);
    if (font.isIndirect())
      quiet_fonts_[font.getObjGen()] = indirect;
    Change("type3-fonts-quietened");
    return indirect;
  }

  // A page drawn as a picture: the picture over its visible area, its text over that, invisible.
  void DrawnPage(QPDFObjectHandle page, flatten::Picture &picture) {
    std::string jpeg;
    if (!jpeg::Encode(reinterpret_cast<const unsigned char *>(picture.rgb.data()), picture.width, picture.height, 3, 90, jpeg))
      return;
    std::string().swap(picture.rgb);
    QPDFObjectHandle image = QPDFObjectHandle::newStream(&q_);
    image.replaceStreamData(jpeg, QPDFObjectHandle::newName("/DCTDecode"), QPDFObjectHandle::newNull());
    QPDFObjectHandle image_dict = image.getDict();
    image_dict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
    image_dict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
    image_dict.replaceKey("/Width", QPDFObjectHandle::newInteger(picture.width));
    image_dict.replaceKey("/Height", QPDFObjectHandle::newInteger(picture.height));
    image_dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
    image_dict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
    // The visible area: the crop box within the media box.
    std::vector<double> media = Numbers(page.getKey("/MediaBox")), crop = Numbers(page.getKey("/CropBox"));
    if (media.size() != 4)
      media = {0, 0, 612, 792};
    if (crop.size() != 4)
      crop = media;
    const double x1 = std::max(std::min(media[0], media[2]), std::min(crop[0], crop[2])), y1 = std::max(std::min(media[1], media[3]), std::min(crop[1], crop[3]));
    const double x2 = std::min(std::max(media[0], media[2]), std::max(crop[0], crop[2])), y2 = std::min(std::max(media[1], media[3]), std::max(crop[1], crop[3]));
    QPDFObjectHandle resources = page.getKey("/Resources");
    bool ok = true;
    const std::string content = PageContent(page, ok);
    QPDFObjectHandle xobjects = QPDFObjectHandle::newDictionary();
    const std::string text = ok ? TextOnly(content, resources, xobjects, 0) : std::string();
    xobjects.replaceKey("/FynePDFPage", image);
    page.replaceKey("/Contents", QPDFObjectHandle::newStream(&q_, "q " + Num(x2 - x1) + " 0 0 " + Num(y2 - y1) + " " + Num(x1) + " " + Num(y1) +
                                                                    " cm /FynePDFPage Do Q\nq\n" + text + "Q\n"));
    page.replaceKey("/Resources", TextResources(resources, xobjects));
    page.removeKey("/Group");
    Change("pages-flattened");
  }

  // Pages that blend drawn as pictures by PDFium, with their comments that blend (which go); their
  // other comments stay. False when one couldn't be drawn.
  bool Flatten(const std::vector<size_t> &indexes) {
    // What PDFium draws: these pages, unrotated, with only their comments that blend.
    struct Saved {
      QPDFObjectHandle page, annots, rotate;
      std::vector<QPDFObjectHandle> drawn;
    };
    std::vector<Saved> saved;
    for (const size_t index : indexes) {
      QPDFObjectHandle page = pages_[index].getObjectHandle();
      Saved entry{page, page.getKey("/Annots"), page.getKey("/Rotate"), {}};
      for (int at = 0; entry.annots.isArray() && at < entry.annots.getArrayNItems(); ++at)
        if (AnnotationBlends(entry.annots.getArrayItem(at)))
          entry.drawn.push_back(entry.annots.getArrayItem(at));
      page.replaceKey("/Annots", QPDFObjectHandle::newArray(entry.drawn));
      page.removeKey("/Rotate");
      saved.push_back(entry);
    }
    std::string pdf;
    try {
      if (!q_.getTrailer().hasKey("/Size"))
        q_.getTrailer().replaceKey("/Size", QPDFObjectHandle::newInteger(1));
      QPDFWriter writer(q_);
      writer.setOutputMemory();
      writer.setPreserveEncryption(false);
      writer.write();
      std::shared_ptr<Buffer> buffer = writer.getBufferSharedPointer();
      pdf.assign(reinterpret_cast<const char *>(buffer->getBuffer()), buffer->getSize());
    } catch (const std::exception &) {
      pdf.clear();
    }
    for (Saved &entry : saved) {
      if (entry.annots.isNull())
        entry.page.removeKey("/Annots");
      else
        entry.page.replaceKey("/Annots", entry.annots);
      if (!entry.rotate.isNull())
        entry.page.replaceKey("/Rotate", entry.rotate);
    }
    if (pdf.empty())
      return false;
    std::vector<int> wanted;
    for (const size_t index : indexes)
      wanted.push_back(static_cast<int>(index));
    std::set<int> drawn;
    flatten::Render(pdf, wanted, flatten_dpi_, 16e6, [&](int index, flatten::Picture &picture) {
      DrawnPage(pages_[static_cast<size_t>(index)].getObjectHandle(), picture);
      drawn.insert(index);
    });
    std::string().swap(pdf);
    // The comments drawn into the pictures go.
    for (size_t at = 0; at < indexes.size(); ++at) {
      if (!drawn.count(static_cast<int>(indexes[at])) || saved[at].drawn.empty() || !saved[at].annots.isArray())
        continue;
      std::set<QPDFObjGen> gone;
      for (QPDFObjectHandle annotation : saved[at].drawn)
        if (annotation.isIndirect())
          gone.insert(annotation.getObjGen());
      std::vector<QPDFObjectHandle> left;
      for (QPDFObjectHandle annotation : saved[at].annots.getArrayAsVector())
        if (!annotation.isIndirect() || !gone.count(annotation.getObjGen()))
          left.push_back(annotation);
      saved[at].page.replaceKey("/Annots", QPDFObjectHandle::newArray(left));
      Change("comments-flattened", static_cast<int>(saved[at].drawn.size()));
    }
    return drawn.size() == indexes.size();
  }

  // PDF/A-1 has no transparency: pages that blend are drawn as pictures (with PDFium; else they
  // are a problem), and groups that then blend nothing go.
  void Transparency() {
    std::vector<size_t> blending;
    for (size_t index = 0; index < pages_.size(); ++index) {
      QPDFObjectHandle page = pages_[index].getObjectHandle();
      bool blends = Blends(page.getKey("/Resources"), 0);
      QPDFObjectHandle annotations = page.getKey("/Annots");
      for (int at = 0; !blends && annotations.isArray() && at < annotations.getArrayNItems(); ++at)
        blends = AnnotationBlends(annotations.getArrayItem(at));
      if (blends)
        blending.push_back(index);
    }
    bool left = !blending.empty();
    if (left && Flattening())
      left = !Flatten(blending);
    for (QPDFPageObjectHelper &helper : pages_) {
      QPDFObjectHandle page = helper.getObjectHandle();
      if (page.getKey("/Group").isDictionary() && IsName(page.getKey("/Group").getKey("/S"), "/Transparency"))
        groups_.push_back(page);
    }
    if (left) {
      Problem("transparency");
      return;
    }
    for (QPDFObjectHandle holder : groups_) {
      holder.removeKey("/Group");
      Change("transparency-groups");
    }
  }

  // -----------------------------------------------------------------------------------------
  // Output intent and metadata

  void OutputIntent() {
    QPDFObjectHandle profile = QPDFObjectHandle::newStream(&q_, SrgbProfile());
    profile.getDict().replaceKey("/N", QPDFObjectHandle::newInteger(3));
    QPDFObjectHandle intent = QPDFObjectHandle::newDictionary();
    intent.replaceKey("/Type", QPDFObjectHandle::newName("/OutputIntent"));
    intent.replaceKey("/S", QPDFObjectHandle::newName("/GTS_PDFA1"));
    intent.replaceKey("/OutputConditionIdentifier", QPDFObjectHandle::newString("sRGB IEC61966-2.1"));
    intent.replaceKey("/RegistryName", QPDFObjectHandle::newString("http://www.color.org"));
    intent.replaceKey("/Info", QPDFObjectHandle::newString("sRGB IEC61966-2.1"));
    intent.replaceKey("/DestOutputProfile", profile);
    if (root_.hasKey("/OutputIntents"))
      Change("output-intents-replaced");
    root_.replaceKey("/OutputIntents", QPDFObjectHandle::newArray({q_.makeIndirectObject(intent)}));
  }

  void Metadata(bool label = true) {
    QPDFObjectHandle trailer = q_.getTrailer();
    QPDFObjectHandle info = trailer.getKey("/Info");
    if (!info.isDictionary()) {
      info = q_.makeIndirectObject(QPDFObjectHandle::newDictionary());
      trailer.replaceKey("/Info", info);
    }
    std::string old;
    QPDFObjectHandle stream = root_.getKey("/Metadata");
    if (stream.isStream())
      Decoded(stream, old);
    xmp::Description d;
    // Each text: the Info dictionary's, else the old packet's; written back to both, cleaned.
    const auto text = [&](const char *key, const char *property, std::string &value) {
      QPDFObjectHandle entry = info.getKey(key);
      value = entry.isString() ? xmp::Clean(entry.getUTF8Value()) : "";
      if (value.empty() && !old.empty())
        value = xmp::Find(old, property);
      if (value.empty()) {
        if (info.hasKey(key)) {
          info.removeKey(key);
          Change("info");
        }
        return;
      }
      if (!entry.isString() || entry.getUTF8Value() != value) {
        info.replaceKey(key, QPDFObjectHandle::newUnicodeString(value));
        Change("info");
      }
    };
    text("/Title", "dc:title", d.title);
    text("/Author", "dc:creator", d.author);
    text("/Subject", "dc:description", d.subject);
    text("/Keywords", "pdf:Keywords", d.keywords);
    text("/Creator", "xmp:CreatorTool", d.creator);
    text("/Producer", "pdf:Producer", d.producer);
    const auto date = [&](const char *key, const char *property, std::string &value) {
      QPDFObjectHandle entry = info.getKey(key);
      std::string pdf;
      if (entry.isString() && xmp::PdfDateToXmp(entry.getUTF8Value(), value)) {
        xmp::XmpDateToPdf(value, pdf);
      } else if (!old.empty() && xmp::XmpDateToXmp(xmp::Find(old, property), value)) {
        xmp::XmpDateToPdf(value, pdf);
      } else {
        value.clear();
        if (info.hasKey(key)) {
          info.removeKey(key);
          Change("info");
        }
        return;
      }
      if (!entry.isString() || entry.getUTF8Value() != pdf) {
        info.replaceKey(key, QPDFObjectHandle::newString(pdf));
        Change("info");
      }
    };
    date("/CreationDate", "xmp:CreateDate", d.created);
    date("/ModDate", "xmp:ModifyDate", d.modified);
    if (info.hasKey("/Trapped")) {
      info.removeKey("/Trapped");
      Change("info");
    }
    const std::string packet = xmp::Packet(d, label && result_.problems.empty() ? part_ : 0);
    if (!stream.isStream()) {
      stream = QPDFObjectHandle::newStream(&q_);
      root_.replaceKey("/Metadata", stream);
    }
    stream.replaceStreamData(packet, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
    stream.getDict().replaceKey("/Type", QPDFObjectHandle::newName("/Metadata"));
    stream.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/XML"));
    result_.conforms = label && result_.problems.empty();
  }

  QPDF &q_;
  PdfaResult &result_;
  const int part_;
  const int flatten_dpi_;                                  // PDF/A-1 pages that blend drawn at this (0: never)
  std::map<QPDFObjGen, char> blends_memo_;                 // Blends(), by object
  std::map<QPDFObjGen, QPDFObjectHandle> text_forms_;      // forms made text-only, by the form
  std::map<QPDFObjGen, QPDFObjectHandle> quiet_fonts_;     // Type 3 fonts drawing nothing, by the font
  const std::string fonts_dir_;
  std::map<std::string, std::string> substitutes_;  // font files read, by name
  std::map<std::string, QPDFObjectHandle> field_fonts_;  // fonts drawn appearances use, by name
  bool need_appearances_ = false;                         // the form's NeedAppearances
  bool numbers_reported_ = false;                         // a number written wrong, said once
  std::set<QPDFObjGen> hidden_;                            // PDF/A-1: layers the default view hides
  QPDFObjectHandle srgb_ = QPDFObjectHandle::newNull();   // the sRGB profile for repaired spaces
  std::map<std::string, std::unique_ptr<font::Program>> substitute_programs_;
  std::map<std::string, QPDFObjectHandle> direct_fonts_;  // fonts that were written in place, made indirect
  QPDFObjectHandle root_;
  std::vector<QPDFPageObjectHelper> pages_;
  std::set<std::pair<int, int>> seen_;
  std::map<std::string, FontUse> fonts_;
  std::map<std::string, std::unique_ptr<font::Checker>> checkers_;  // by font, as fonts_
  std::map<std::string, Plan> plans_;    // fonts whose text is rewritten to keep it in place
  bool content_unreadable_ = false;      // some content couldn't be read: no font's widths change
  std::vector<QPDFObjectHandle> associated_;  // PDF/A-3: attachments for the catalog's /AF
  std::map<QPDFObjGen, bool> verdicts_;       // attachments judged, kept or not
  std::vector<QPDFObjectHandle> pending_;     // objects still to scan
  std::set<QPDFObjGen> scanned_;
  std::vector<QPDFObjectHandle> groups_;      // PDF/A-1: holders of transparency groups
  bool transparent_ = false;                  // PDF/A-1: something blends
  std::map<std::string, std::pair<QPDFObjectHandle, QPDFObjectHandle>> inks_;  // each spot ink's first alternate and tint transform
};

std::string Escape(const std::string &text) {
  std::string out;
  for (const char c : text) {
    if (c == '"' || c == '\\')
      out += '\\';
    if (static_cast<unsigned char>(c) < 0x20)
      continue;
    out += c;
  }
  return out;
}

// A file qpdf can't open for want of a trailer (cut off, as a download that stopped is): its
// last catalog found in |bytes| and a trailer naming it appended, for qpdf to rebuild the rest
// from the objects that are whole. False when there's no catalog.
bool WithTrailer(std::string &bytes) {
  for (size_t at = bytes.rfind("/Catalog"); at != std::string::npos && at > 0; at = bytes.rfind("/Catalog", at - 1)) {
    // The object it is in: "N G obj" before it, with no "endobj" between.
    const size_t from = at > 4096 ? at - 4096 : 0;
    const size_t obj = bytes.rfind("obj", at);
    if (obj == std::string::npos || obj < from || bytes.find("endobj", obj) < at)
      continue;
    size_t end = obj;
    std::string numbers[2];
    for (int k = 1; k >= 0; --k) {
      while (end > 0 && std::isspace(static_cast<unsigned char>(bytes[end - 1])))
        --end;
      size_t start = end;
      while (start > 0 && std::isdigit(static_cast<unsigned char>(bytes[start - 1])))
        --start;
      numbers[k] = bytes.substr(start, end - start);
      end = start;
    }
    if (numbers[0].empty() || numbers[1].empty() || numbers[0].size() > 9 || numbers[1].size() > 5)
      continue;
    bytes += "\ntrailer\n<< /Root " + numbers[0] + " " + numbers[1] + " R /Size 1 >>\n";
    return true;
  }
  return false;
}

} // namespace

PdfaResult ConvertToPdfa(const std::string &input, const std::string &output, const PdfaOptions &options) {
  PdfaResult result;
  if (options.part < 1 || options.part > 3) {
    result.error = "part";
    return result;
  }
  std::string repaired;  // a damaged file's bytes with a trailer, which qpdf reads in place
  auto q_holder = std::make_unique<QPDF>();
  try {
    Open(*q_holder, input);
  } catch (const QPDFExc &error) {
    result.error = error.getErrorCode() == qpdf_e_password ? "password" : "unreadable";
  } catch (const std::exception &) {
    result.error = "unreadable";
  }
  if (result.error == "unreadable") {
    std::ifstream in(input, std::ios::binary);
    std::ostringstream data;
    data << in.rdbuf();
    repaired = data.str();
    if (repaired.compare(0, 5, "%PDF-") != 0)
      return result;
    // Read from memory: true with pages.
    const auto open = [&]() {
      try {
        q_holder = std::make_unique<QPDF>();
        q_holder->setSuppressWarnings(true);
        q_holder->processMemoryFile(input.c_str(), repaired.data(), repaired.size());
        QPDFPageDocumentHelper(*q_holder).pushInheritedAttributesToPage();
        return !QPDFPageDocumentHelper(*q_holder).getAllPages().empty();
      } catch (const std::exception &) {
        return false;
      }
    };
    // Something after its end (readers look for the cross-reference table's place in the last
    // kilobyte): cut; else a trailer naming its catalog.
    const size_t eof = repaired.rfind("%%EOF");
    bool opened = false;
    if (eof != std::string::npos && repaired.size() - eof > 1024) {
      repaired.resize(eof + 5);
      repaired += "\n";
      opened = open();
    }
    if (!opened && (!WithTrailer(repaired) || !open()))
      return result;
    // The pages and content streams that are there (a cut file names some that aren't).
    try {
      QPDF &q = *q_holder;
      std::vector<QPDFObjectHandle> pages;
      std::set<QPDFObjGen> seen;
      std::function<void(QPDFObjectHandle, int)> walk = [&](QPDFObjectHandle node, int depth) {
        QPDFObjectHandle kids = node.getKey("/Kids");
        for (int index = 0; depth < 64 && kids.isArray() && index < kids.getArrayNItems(); ++index) {
          QPDFObjectHandle kid = kids.getArrayItem(index);
          if (!kid.isDictionary() || (kid.isIndirect() && !seen.insert(kid.getObjGen()).second))
            continue;
          if (kid.getKey("/Kids").isArray())
            walk(kid, depth + 1);
          else
            pages.push_back(kid);
        }
      };
      walk(q.getRoot().getKey("/Pages"), 0);
      if (pages.empty())
        return result;
      QPDFObjectHandle tree = q.makeIndirectObject(QPDFObjectHandle::newDictionary());
      tree.replaceKey("/Type", QPDFObjectHandle::newName("/Pages"));
      tree.replaceKey("/Kids", QPDFObjectHandle::newArray(pages));
      tree.replaceKey("/Count", QPDFObjectHandle::newInteger(static_cast<long long>(pages.size())));
      for (QPDFObjectHandle page : pages) {
        page.replaceKey("/Parent", tree);
        QPDFObjectHandle contents = page.getKey("/Contents");
        if (contents.isArray()) {
          std::vector<QPDFObjectHandle> streams;
          for (QPDFObjectHandle item : contents.getArrayAsVector())
            if (item.isStream())
              streams.push_back(item);
          page.replaceKey("/Contents", QPDFObjectHandle::newArray(streams));
        } else if (!contents.isStream()) {
          page.removeKey("/Contents");
        }
      }
      q.getRoot().replaceKey("/Pages", tree);
      q.updateAllPagesCache();
    } catch (const std::exception &) {
      return result;
    }
    result.error.clear();
    result.changes["damaged-file-repaired"] = 1;
  } else if (!result.error.empty()) {
    return result;
  }
  QPDF &q = *q_holder;
  try {
    if (q.isEncrypted())
      result.changes["encryption"] = 1;
    Pdfa(q, result, options).Run();
  } catch (const std::exception &) {
    result = PdfaResult{};
    result.error = "unreadable";
    return result;
  }
  try {
    // qpdf writes the trailer's /Size only in place of one it has (a rebuilt file may not).
    if (!q.getTrailer().hasKey("/Size"))
      q.getTrailer().replaceKey("/Size", QPDFObjectHandle::newInteger(1));
    QPDFWriter writer(q, output.c_str());
    writer.setPreserveEncryption(false);
    // PDF/A-1 is PDF 1.4: no object or cross-reference streams.
    writer.setObjectStreamMode(options.part == 1 ? qpdf_o_disable : qpdf_o_preserve);
    writer.setCompressStreams(true);
    writer.setNewlineBeforeEndstream(true);
    writer.forcePDFVersion(options.part == 1 ? "1.4" : "1.7");
    writer.setDeterministicID(true);
    writer.write();
  } catch (const std::exception &) {
    result.error = "write";
    result.conforms = false;
    return result;
  }
  result.ok = true;
  return result;
}

std::string ToJson(const PdfaResult &r) {
  std::string json = "{\"ok\":" + std::string(r.ok ? "true" : "false");
  if (!r.error.empty())
    json += ",\"error\":\"" + r.error + "\"";
  json += ",\"conforms\":" + std::string(r.conforms ? "true" : "false");
  json += ",\"pages\":" + std::to_string(r.pages) + ",\"signatures\":" + std::to_string(r.signatures);
  json += ",\"problems\":[";
  for (size_t at = 0; at < r.problems.size(); ++at)
    json += (at ? ",\"" : "\"") + Escape(r.problems[at]) + "\"";
  json += "],\"changes\":{";
  bool first = true;
  for (const auto &[kind, count] : r.changes) {
    json += (first ? "\"" : ",\"") + Escape(kind) + "\":" + std::to_string(count);
    first = false;
  }
  return json + "}}";
}

} // namespace fynepdf::pdfa
