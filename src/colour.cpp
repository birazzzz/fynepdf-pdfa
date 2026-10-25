#include "colour.h"

#include "common.h"
#include "content.h"
#include "icc.h"
#include "jpeg.h"
#include "jpx.h"
#include "pdf_function.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <qpdf/Buffer.hh>
#include <qpdf/Pl_Flate.hh>
#include <qpdf/Pl_String.hh>
#include <qpdf/QPDFObjectHandle.hh>

namespace fynepdf::pdfa {
namespace {

using namespace content;

// The most a picture may take decoded (256 MB): larger ones are left as they are.
constexpr unsigned long long kMaximumPixelBytes = 256ULL * 1024 * 1024;

// Stream data the converter makes (page content, forms, pictures) stays in memory until the
// file is written, for the whole file at once. Past kRawHeld bytes of it, the rest is held
// Flate-compressed and inflated again when the writer asks for it; the writer then compresses
// it as it always did, so the file comes out the same, byte for byte. Most files make less than
// that and cost no extra work.
constexpr size_t kRawHeld = 32 * 1024 * 1024;

class HeldData : public QPDFObjectHandle::StreamDataProvider {
public:
  explicit HeldData(const std::string &data) {
    Pl_String sink("held", nullptr, compressed_);
    Pl_Flate deflate("held", &sink, Pl_Flate::a_deflate);
    deflate.write(reinterpret_cast<const unsigned char *>(data.data()), data.size());
    deflate.finish();
  }
  void provideStreamData(QPDFObjGen const &, Pipeline *pipeline) override {
    Pl_Flate inflate("held", pipeline, Pl_Flate::a_inflate);
    inflate.write(reinterpret_cast<const unsigned char *>(compressed_.data()), compressed_.size());
    inflate.finish();
  }

private:
  std::string compressed_;
};

enum class Kind { Gray, Rgb, Cmyk, Lab, Indexed, Spot, Pattern, Other };

bool Convertible(Kind kind) { return kind == Kind::Rgb || kind == Kind::Cmyk || kind == Kind::Lab || kind == Kind::Spot; }

// A device space (a name, or a name alone in an array), not an ICC-based or calibrated one.
} // namespace

std::vector<QPDFObjectHandle> CmykAsRgb(QPDF &q) {
  std::string data;
  for (int at = 0; at < 16; ++at) {  // cyan varies fastest
    double rgb[3];
    CmykToRgb(at & 1, (at >> 1) & 1, (at >> 2) & 1, (at >> 3) & 1, rgb[0], rgb[1], rgb[2]);
    for (const double value : rgb) {
      const long sample = std::lround(value * 65535);
      data += static_cast<char>(sample >> 8);
      data += static_cast<char>(sample & 0xff);
    }
  }
  QPDFObjectHandle function = QPDFObjectHandle::newStream(&q, data);
  QPDFObjectHandle dict = function.getDict();
  dict.replaceKey("/FunctionType", QPDFObjectHandle::newInteger(0));
  dict.replaceKey("/Domain", QPDFObjectHandle::parse("[0 1 0 1 0 1 0 1]"));
  dict.replaceKey("/Range", QPDFObjectHandle::parse("[0 1 0 1 0 1]"));
  dict.replaceKey("/Size", QPDFObjectHandle::parse("[2 2 2 2]"));
  dict.replaceKey("/BitsPerSample", QPDFObjectHandle::newInteger(16));
  return {QPDFObjectHandle::newName("/DeviceN"), QPDFObjectHandle::parse("[/Cyan /Magenta /Yellow /Black]"), QPDFObjectHandle::newName("/DeviceRGB"), function};
}

namespace {

bool Device(QPDFObjectHandle space) { return space.isName() || (space.isArray() && space.getArrayNItems() == 1); }

std::string Join(const std::vector<double> &values) {
  std::string out;
  for (const double value : values)
    out += (out.empty() ? "" : " ") + Num(value);
  return out;
}

} // namespace

class Converter {
public:
  Converter(QPDF &q, ColourStats &stats, const ColourOptions &options)
      : q_(q), stats_(stats), gray_(options.target == Target::Gray), drawings_(options.drawings), images_(options.images),
        quality_(std::clamp(options.jpegQuality, 10, 100)) {}

  void Page(QPDFPageObjectHelper &page) {
    QPDFObjectHandle object = page.getObjectHandle();
    QPDFObjectHandle resources = object.getKey("/Resources");
    bool ok = true;
    const std::string content = PageContent(object, ok);
    if (ok) {
      bool changed = false, parsed = true;
      const std::string rewritten = Content(content, resources, changed, parsed);
      if (!parsed)
        ++stats_.contentKept;
      else if (changed) {
        QPDFObjectHandle stream = QPDFObjectHandle::newStream(&q_);
        Hold(stream, rewritten);
        object.replaceKey("/Contents", stream);
      }
    } else {
      ++stats_.contentKept;
    }
    Resources(resources);
    Group(object);
    if (drawings_)
      Annotations(object, resources);
  }

  // Default appearances (/DA) of the form and its fields, as readers redraw fields from them.
  void FormDefaults() {
    if (!drawings_)
      return;
    QPDFObjectHandle form = q_.getRoot().getKey("/AcroForm");
    if (!form.isDictionary())
      return;
    DefaultAppearance(form);
    std::vector<QPDFObjectHandle> fields;
    QPDFObjectHandle top = form.getKey("/Fields");
    for (int index = 0; top.isArray() && index < top.getArrayNItems(); ++index)
      fields.push_back(top.getArrayItem(index));
    std::set<QPDFObjGen> seen;
    while (!fields.empty()) {
      QPDFObjectHandle field = fields.back();
      fields.pop_back();
      if (!field.isDictionary() || (field.isIndirect() && !seen.insert(field.getObjGen()).second) || seen.size() > 100000)
        continue;
      DefaultAppearance(field);
      QPDFObjectHandle kids = field.getKey("/Kids");
      for (int index = 0; kids.isArray() && index < kids.getArrayNItems(); ++index)
        fields.push_back(kids.getArrayItem(index));
    }
  }

private:
  // ---------------------------------------------------------------------------------------------
  // The target

  // How many components a target colour has, and its space.
  size_t Out() const { return gray_ ? 1 : 3; }
  const char *OutSpace() const { return gray_ ? "/DeviceGray" : "/DeviceRGB"; }

  // Whether a colour of |kind| in |space| (resolved) is converted: for Grayscale everything with
  // colour; for PDF/A only device CMYK (spot inks keep their names; their CMYK alternates are
  // replaced in their colour spaces, see Spot()).
  bool Wanted(Kind kind, QPDFObjectHandle space) const { return gray_ ? Convertible(kind) : kind == Kind::Cmyk && Device(space); }

  // A colour of |space| in the target: grey, or RGB from device CMYK.
  bool ToTarget(Kind kind, QPDFObjectHandle space, const std::vector<double> &values, std::vector<double> &out, QPDFObjectHandle resources) {
    if (gray_) {
      double gray = 0;
      if (!ToGray(kind, space, values, gray, resources))
        return false;
      out = {gray};
      return true;
    }
    if (kind != Kind::Cmyk || values.size() < 4)
      return false;
    double r, g, b;
    CmykToRgb(values[0], values[1], values[2], values[3], r, g, b);
    out = {r, g, b};
    return true;
  }

  // ---------------------------------------------------------------------------------------------
  // Colour spaces

  Kind KindOf(QPDFObjectHandle space, QPDFObjectHandle resources, int depth = 0) {
    if (depth > 8)
      return Kind::Other;
    if (space.isName()) {
      const std::string name = space.getName();
      if (name == "/DeviceRGB" || name == "/RGB")
        return Kind::Rgb;
      if (name == "/DeviceGray" || name == "/G")
        return Kind::Gray;
      if (name == "/DeviceCMYK" || name == "/CMYK")
        return Kind::Cmyk;
      if (name == "/Pattern")
        return Kind::Pattern;
      if (name == "/Indexed" || name == "/I")
        return Kind::Indexed;
      QPDFObjectHandle spaces = resources.isDictionary() ? resources.getKey("/ColorSpace") : QPDFObjectHandle::newNull();
      return spaces.isDictionary() && spaces.hasKey(name) ? KindOf(spaces.getKey(name), resources, depth + 1) : Kind::Other;
    }
    if (!space.isArray() || space.getArrayNItems() == 0 || !space.getArrayItem(0).isName())
      return Kind::Other;
    // Some writers put a device space in an array of its own: [/DeviceRGB].
    if (space.getArrayNItems() == 1)
      return KindOf(space.getArrayItem(0), resources, depth + 1);
    const std::string family = space.getArrayItem(0).getName();
    if (family == "/ICCBased" && space.getArrayNItems() > 1 && space.getArrayItem(1).isStream()) {
      const int n = Integer(space.getArrayItem(1).getDict(), "/N", 0);
      return n == 3 ? Kind::Rgb : n == 1 ? Kind::Gray : n == 4 ? Kind::Cmyk : Kind::Other;
    }
    if (family == "/CalRGB")
      return Kind::Rgb;
    if (family == "/CalGray")
      return Kind::Gray;
    if (family == "/Lab")
      return Kind::Lab;
    if (family == "/Indexed" || family == "/I")
      return Kind::Indexed;
    if (family == "/Pattern")
      return Kind::Pattern;
    // Spot inks, but never "no ink" (None), which paints nothing.
    if (family == "/Separation" && space.getArrayNItems() == 4 && space.getArrayItem(1).isName())
      return space.getArrayItem(1).getName() == "/None" ? Kind::Other : Kind::Spot;
    if (family == "/DeviceN" && space.getArrayNItems() >= 4 && space.getArrayItem(1).isArray()) {
      QPDFObjectHandle names = space.getArrayItem(1);
      for (int index = 0; index < names.getArrayNItems(); ++index)
        if (names.getArrayItem(index).isName() && names.getArrayItem(index).getName() != "/None")
          return Kind::Spot;
    }
    return Kind::Other;
  }

  // A colour space by its name in the resources, or as it is.
  QPDFObjectHandle Resolve(QPDFObjectHandle space, QPDFObjectHandle resources) {
    for (int depth = 0; depth < 8 && space.isName(); ++depth) {
      QPDFObjectHandle spaces = resources.isDictionary() ? resources.getKey("/ColorSpace") : QPDFObjectHandle::newNull();
      if (!spaces.isDictionary() || !spaces.hasKey(space.getName()))
        break;
      space = spaces.getKey(space.getName());
    }
    return space;
  }

  static size_t SpotComponents(QPDFObjectHandle space) {
    return IsName(space.getArrayItem(0), "/Separation") ? 1 : static_cast<size_t>(space.getArrayItem(1).getArrayNItems());
  }

  size_t Components(Kind kind, QPDFObjectHandle space) {
    switch (kind) {
    case Kind::Gray:
    case Kind::Indexed:
      return 1;
    case Kind::Rgb:
    case Kind::Lab:
      return 3;
    case Kind::Cmyk:
      return 4;
    case Kind::Spot:
      return SpotComponents(space);
    default:
      return 0;
    }
  }

  // A Lab space's a* and b* ranges (default -100 to 100).
  static std::vector<double> LabRange(QPDFObjectHandle space) {
    QPDFObjectHandle dict = space.isArray() && space.getArrayNItems() > 1 ? space.getArrayItem(1) : QPDFObjectHandle::newNull();
    std::vector<double> range = dict.isDictionary() ? Numbers(dict.getKey("/Range")) : std::vector<double>{};
    return range.size() == 4 ? range : std::vector<double>{-100, 100, -100, 100};
  }

  // The key of an ICC-based space (RGB or CMYK) whose own profile Little CMS can use; "" when
  // there is none (or no Little CMS), and the formulas apply.
  std::string IccKey(QPDFObjectHandle space) {
    if (!space.isArray() || space.getArrayNItems() < 2 || !IsName(space.getArrayItem(0), "/ICCBased") || !space.getArrayItem(1).isStream())
      return "";
    QPDFObjectHandle stream = space.getArrayItem(1);
    const int n = Integer(stream.getDict(), "/N", 0);
    if (n != 3 && n != 4)
      return "";
    const std::string key = stream.isIndirect() ? std::to_string(stream.getObjectID()) + " " + std::to_string(stream.getGeneration()) : stream.unparse();
    if (profiles_seen_.insert(key).second) {
      bool ok = true;
      const std::string bytes = StreamData(stream, ok);
      profiles_.Usable(key, ok ? bytes : std::string(), n);
    }
    return profiles_.Usable(key, std::string(), n) ? key : "";
  }

  // A colour of an ICC-based space as grey through its own profile; false when it has none usable.
  bool IccGray(QPDFObjectHandle space, const std::vector<double> &values, double &gray) {
    const std::string key = IccKey(space);
    if (key.empty() || values.size() < 3)
      return false;
    unsigned short in[4] = {}, out[3] = {};
    for (size_t k = 0; k < std::min<size_t>(values.size(), 4); ++k)
      in[k] = static_cast<unsigned short>(std::lround(std::clamp(values[k], 0.0, 1.0) * 65535));
    profiles_.ToRgb(key, in, out, 1);
    gray = Luma(out[0] / 65535.0, out[1] / 65535.0, out[2] / 65535.0);
    return true;
  }

  // A colour of |space| as grey, 0 (black) to 1 (white).
  bool ToGray(Kind kind, QPDFObjectHandle space, const std::vector<double> &values, double &gray, QPDFObjectHandle resources, int depth = 0) {
    if (depth > 8)
      return false;
    switch (kind) {
    case Kind::Gray:
      if (values.empty())
        return false;
      gray = std::clamp(values[0], 0.0, 1.0);
      return true;
    case Kind::Rgb:
      if (values.size() < 3)
        return false;
      if (!IccGray(space, values, gray))
        gray = Luma(values[0], values[1], values[2]);
      return true;
    case Kind::Cmyk:
      if (values.size() < 4)
        return false;
      if (!IccGray(space, values, gray))
        gray = CmykGray(values[0], values[1], values[2], values[3]);
      return true;
    case Kind::Lab: {
      if (values.size() < 3)
        return false;
      double r, g, b;
      LabToRgb(values[0], values[1], values[2], r, g, b);
      gray = Luma(r, g, b);
      return true;
    }
    case Kind::Spot:
      return SpotGray(space, values, gray, resources, depth);
    default:
      return false;
    }
  }

  // A spot ink's tint transform, read once.
  PdfFunction *TintTransform(QPDFObjectHandle space) {
    const std::string key = space.unparse();
    auto found = functions_.find(key);
    if (found == functions_.end())
      found = functions_.emplace(key, std::shared_ptr<PdfFunction>(PdfFunction::Read(space.getArrayItem(3)))).first;
    return found->second.get();
  }

  // A spot ink's tints as the grey its alternate colours make; the registration colour (All)
  // as black.
  bool SpotGray(QPDFObjectHandle space, const std::vector<double> &tints, double &gray, QPDFObjectHandle resources, int depth) {
    if (IsName(space.getArrayItem(0), "/Separation") && IsName(space.getArrayItem(1), "/All")) {
      if (tints.empty())
        return false;
      gray = 1 - std::clamp(tints[0], 0.0, 1.0);
      return true;
    }
    PdfFunction *transform = TintTransform(space);
    std::vector<double> alternate;
    if (!transform || !transform->Evaluate(tints, alternate))
      return false;
    QPDFObjectHandle base = Resolve(space.getArrayItem(2), resources);
    return ToGray(KindOf(base, resources), base, alternate, gray, resources, depth + 1);
  }

  // The colour a space starts with when a content stream selects it, in the target: black for
  // device spaces, no ink for ICC-based CMYK, full tint for spot inks.
  bool InitialTarget(Kind kind, QPDFObjectHandle space, std::vector<double> &out, QPDFObjectHandle resources) {
    if (!gray_)
      return ToTarget(kind, space, {0, 0, 0, 1}, out, resources);
    double gray = 0;
    switch (kind) {
    case Kind::Rgb:
    case Kind::Lab:
      gray = 0;
      break;
    case Kind::Cmyk:
      gray = space.isArray() ? 1 : 0;
      break;
    case Kind::Spot:
      if (!SpotGray(space, std::vector<double>(SpotComponents(space), 1.0), gray, resources, 0))
        return false;
      break;
    default:
      return false;
    }
    out = {gray};
    return true;
  }

  // An indexed space's palette in the target, in place (idempotent: a palette already in the
  // target is left alone).
  bool Palette(QPDFObjectHandle space, QPDFObjectHandle resources) {
    if (!space.isArray() || space.getArrayNItems() != 4)
      return false;
    QPDFObjectHandle base = Resolve(space.getArrayItem(1), resources);
    const Kind kind = KindOf(base, resources);
    if (!Wanted(kind, base)) {
      if (!gray_ && kind == Kind::Spot)
        Spot(base, resources);
      return false;
    }
    const size_t components = Components(kind, base);
    QPDFObjectHandle lookup = space.getArrayItem(3);
    std::string bytes;
    if (lookup.isString()) {
      bytes = lookup.getStringValue();
    } else if (lookup.isStream()) {
      bool ok = true;
      bytes = StreamData(lookup, ok);
      if (!ok)
        return false;
    } else {
      return false;
    }
    const size_t entries = components ? bytes.size() / components : 0;
    if (entries == 0)
      return false;
    const std::vector<double> lab = kind == Kind::Lab ? LabRange(base) : std::vector<double>{};
    std::string out(entries * Out(), '\0');
    std::vector<double> target;
    for (size_t at = 0; at < entries; ++at) {
      std::vector<double> values(components);
      for (size_t k = 0; k < components; ++k) {
        const double sample = static_cast<unsigned char>(bytes[at * components + k]) / 255.0;
        values[k] = kind != Kind::Lab ? sample : k == 0 ? sample * 100 : lab[(k - 1) * 2] + sample * (lab[(k - 1) * 2 + 1] - lab[(k - 1) * 2]);
      }
      if (!ToTarget(kind, base, values, target, resources))
        return false;
      for (size_t k = 0; k < Out(); ++k)
        out[at * Out() + k] = static_cast<char>(std::lround(std::clamp(target[k], 0.0, 1.0) * 255));
    }
    space.setArrayItem(1, QPDFObjectHandle::newName(OutSpace()));
    space.setArrayItem(3, QPDFObjectHandle::newString(out));
    stats_.colours += static_cast<int>(entries);
    return true;
  }

  // PDF/A: a spot ink whose alternate is device CMYK gets an RGB alternate, through a sampled tint
  // transform (the old transform, then CMYK to RGB). The ink keeps its name; content using it is
  // unchanged. DeviceN's colorants too. Idempotent.
  void Spot(QPDFObjectHandle space, QPDFObjectHandle resources) {
    if (gray_ || !space.isArray() || space.getArrayNItems() < 4 || !space.getArrayItem(0).isName())
      return;
    const std::string family = space.getArrayItem(0).getName();
    if (family != "/Separation" && family != "/DeviceN")
      return;
    if (family == "/DeviceN" && space.getArrayNItems() > 4) {
      QPDFObjectHandle colorants = space.getArrayItem(4).isDictionary() ? space.getArrayItem(4).getKey("/Colorants") : QPDFObjectHandle::newNull();
      for (const std::string &key : colorants.isDictionary() ? colorants.getKeys() : std::set<std::string>{})
        Spot(Resolve(colorants.getKey(key), resources), resources);
    }
    QPDFObjectHandle alternate = Resolve(space.getArrayItem(2), resources);
    if (!(KindOf(alternate, resources) == Kind::Cmyk && Device(alternate)))
      return;
    // Inks of the same name must keep the same alternate and tint transform (PDF/A-2 6.2.4.4).
    const std::string key = space.unparse();
    if (const auto made = spots_made_.find(key); made != spots_made_.end()) {
      space.setArrayItem(2, QPDFObjectHandle::newName("/DeviceRGB"));
      space.setArrayItem(3, made->second);
      return;
    }
    const size_t inputs = family == "/Separation" ? 1 : static_cast<size_t>(space.getArrayItem(1).isArray() ? space.getArrayItem(1).getArrayNItems() : 0);
    PdfFunction *transform = TintTransform(space);
    // A process ink's own separation is that ink, whatever its transform says (some writers give
    // it one that doesn't work).
    int process = -1;
    if (family == "/Separation" && space.getArrayItem(1).isName()) {
      const std::string ink = space.getArrayItem(1).getName();
      process = ink == "/Cyan" ? 0 : ink == "/Magenta" ? 1 : ink == "/Yellow" ? 2 : ink == "/Black" ? 3 : -1;
    }
    if (inputs == 0 || inputs > 8 || (!transform && process < 0)) {
      ++stats_.cmykLeft;
      return;
    }
    const int size = inputs == 1 ? 256 : inputs == 2 ? 33 : inputs == 3 ? 17 : inputs == 4 ? 9 : 5;
    size_t count = 1;
    for (size_t i = 0; i < inputs; ++i)
      count *= static_cast<size_t>(size);
    std::string data(count * 3 * 2, '\0');
    std::vector<double> tints(inputs), cmyk;
    for (size_t at = 0; at < count; ++at) {
      size_t rest = at;
      for (size_t i = 0; i < inputs; ++i) {  // the first input varies fastest
        tints[i] = static_cast<double>(rest % static_cast<size_t>(size)) / (size - 1);
        rest /= static_cast<size_t>(size);
      }
      if (!transform || !transform->Evaluate(tints, cmyk) || cmyk.size() < 4) {
        if (process < 0) {
          ++stats_.cmykLeft;
          return;
        }
        cmyk.assign(4, 0.0);
        cmyk[static_cast<size_t>(process)] = tints[0];
      }
      double rgb[3];
      CmykToRgb(cmyk[0], cmyk[1], cmyk[2], cmyk[3], rgb[0], rgb[1], rgb[2]);
      for (int k = 0; k < 3; ++k) {
        const long value = std::lround(rgb[k] * 65535);
        data[(at * 3 + static_cast<size_t>(k)) * 2] = static_cast<char>(value >> 8);
        data[(at * 3 + static_cast<size_t>(k)) * 2 + 1] = static_cast<char>(value & 0xff);
      }
    }
    QPDFObjectHandle sampled = QPDFObjectHandle::newStream(&q_, data);
    QPDFObjectHandle dict = sampled.getDict();
    dict.replaceKey("/FunctionType", QPDFObjectHandle::newInteger(0));
    std::vector<double> domain;
    for (size_t i = 0; i < inputs; ++i)
      domain.insert(domain.end(), {0, 1});
    dict.replaceKey("/Domain", NumberArray(domain));
    dict.replaceKey("/Range", QPDFObjectHandle::parse("[0 1 0 1 0 1]"));
    std::vector<QPDFObjectHandle> sizes(inputs, QPDFObjectHandle::newInteger(size));
    dict.replaceKey("/Size", QPDFObjectHandle::newArray(sizes));
    dict.replaceKey("/BitsPerSample", QPDFObjectHandle::newInteger(16));
    space.setArrayItem(2, QPDFObjectHandle::newName("/DeviceRGB"));
    space.setArrayItem(3, sampled);
    spots_made_.emplace(key, sampled);
    ++stats_.spots;
  }

  // ---------------------------------------------------------------------------------------------
  // Content

  // The colour spaces a content stream has selected, as they were (sc/scn values arrive in them).
  struct State {
    Kind fill = Kind::Gray, stroke = Kind::Gray;
    QPDFObjectHandle fillSpace, strokeSpace;
  };

  // The content with its colours in the target; |changed| when any was, |parsed| false when the
  // content couldn't be read (it is then left as it was). |initial|: the spaces it starts in (a
  // form inherits those of the content that draws it).
  std::string Content(const std::string &content, QPDFObjectHandle resources, bool &changed, bool &parsed) {
    return Content(content, resources, changed, parsed, State());
  }
  std::string Content(const std::string &content, QPDFObjectHandle resources, bool &changed, bool &parsed, const State &initial) {
    std::vector<State> stack;
    State state = initial;
    std::string out;
    out.reserve(content.size() + 64);
    const bool read = ForEachOp(content, [&](const Op &op) {
      const auto numbers = [&](size_t count) {
        if (op.operands.size() != count)
          return false;
        for (const Token &token : op.operands)
          if (token.kind != Token::Number)
            return false;
        return true;
      };
      const bool fill = op.name == "rg" || op.name == "k" || op.name == "g" || op.name == "cs" || op.name == "sc" || op.name == "scn";
      Kind &kind = fill ? state.fill : state.stroke;
      QPDFObjectHandle &space = fill ? state.fillSpace : state.strokeSpace;
      std::string replaced;
      const bool colour_op = op.name == "rg" || op.name == "RG" || op.name == "k" || op.name == "K" || op.name == "cs" || op.name == "CS" ||
                             op.name == "sc" || op.name == "scn" || op.name == "SC" || op.name == "SCN";
      if (colour_op && !drawings_) {
        // Colours stay; only the pictures change.
      } else if (op.name == "q") {
        stack.push_back(state);
      } else if (op.name == "Q") {
        if (!stack.empty()) {
          state = stack.back();
          stack.pop_back();
        }
      } else if ((op.name == "rg" || op.name == "RG") && numbers(3)) {
        if (gray_) {
          replaced = Num(Luma(op.operands[0].number, op.operands[1].number, op.operands[2].number)) + (fill ? " g" : " G");
          ++stats_.colours;
        }
        // rg selects DeviceRGB too: later sc/scn values come in RGB.
        kind = Kind::Rgb;
        space = QPDFObjectHandle::newName("/DeviceRGB");
      } else if ((op.name == "k" || op.name == "K") && numbers(4)) {
        std::vector<double> target;
        ToTarget(Kind::Cmyk, QPDFObjectHandle::newName("/DeviceCMYK"),
                 {op.operands[0].number, op.operands[1].number, op.operands[2].number, op.operands[3].number}, target, resources);
        replaced = Join(target) + (gray_ ? (fill ? " g" : " G") : (fill ? " rg" : " RG"));
        kind = Kind::Cmyk;
        space = QPDFObjectHandle::newName("/DeviceCMYK");
        ++stats_.colours;
      } else if (op.name == "g" || op.name == "G") {
        kind = Kind::Gray;
      } else if ((op.name == "cs" || op.name == "CS") && op.operands.size() == 1 && op.operands[0].kind == Token::Name) {
        QPDFObjectHandle named = QPDFObjectHandle::newName("/" + op.operands[0].text);
        QPDFObjectHandle resolved = Resolve(named, resources);
        kind = KindOf(named, resources);
        space = resolved;
        std::vector<double> initial_values;
        if (Wanted(kind, resolved) && InitialTarget(kind, resolved, initial_values, resources)) {
          replaced = std::string(OutSpace()) + " " + op.name;
          if (std::any_of(initial_values.begin(), initial_values.end(), [](double v) { return v > 0.0005; }))
            replaced += "\n" + Join(initial_values) + (fill ? " sc" : " SC");
        } else if (kind == Kind::Pattern && resolved.isArray() && resolved.getArrayNItems() == 2) {
          // An uncoloured pattern: its colours come with it, in its base space.
          QPDFObjectHandle base = Resolve(resolved.getArrayItem(1), resources);
          const std::string pattern = PatternSpace(resources);
          if (!pattern.empty() && Wanted(KindOf(base, resources), base))
            replaced = "/" + pattern + " " + op.name;
        } else if (Wanted(kind, resolved)) {
          kind = Kind::Other;  // a spot ink whose tint transform can't be worked out stays
        }
      } else if ((op.name == "sc" || op.name == "scn" || op.name == "SC" || op.name == "SCN") && !op.operands.empty()) {
        std::vector<double> values, target;
        for (const Token &token : op.operands)
          if (token.kind == Token::Number)
            values.push_back(token.number);
        const bool pattern_name = op.operands.back().kind == Token::Name;
        if (Wanted(kind, space) && numbers(Components(kind, space)) && ToTarget(kind, space, values, target, resources)) {
          replaced = Join(target) + " " + op.name;
          ++(kind == Kind::Spot ? stats_.spots : stats_.colours);
        } else if (kind == Kind::Pattern && pattern_name && space.isArray() && space.getArrayNItems() == 2 &&
                   values.size() + 1 == op.operands.size()) {
          QPDFObjectHandle base = Resolve(space.getArrayItem(1), resources);
          const Kind base_kind = KindOf(base, resources);
          if (Wanted(base_kind, base) && values.size() == Components(base_kind, base) && ToTarget(base_kind, base, values, target, resources)) {
            replaced = Join(target) + " /" + op.operands.back().text + " " + op.name;
            ++stats_.colours;
          }
        }
      } else if (op.name == "Do" && op.operands.size() == 1 && op.operands[0].kind == Token::Name) {
        // A form starts in the colour spaces of the content that draws it (the first such).
        QPDFObjectHandle xobjects = resources.isDictionary() ? resources.getKey("/XObject") : QPDFObjectHandle::newNull();
        QPDFObjectHandle form = xobjects.isDictionary() ? xobjects.getKey("/" + op.operands[0].text) : QPDFObjectHandle::newNull();
        if (form.isStream() && form.isIndirect() && IsName(form.getDict().getKey("/Subtype"), "/Form"))
          inherited_.emplace(form.getObjGen(), state);
      } else if (op.name == "BI" && images_) {
        replaced = InlineImage(content.substr(op.begin, op.end - op.begin), resources);
      }
      if (!replaced.empty()) {
        out += replaced;
        changed = true;
      } else {
        out.append(content, op.begin, op.end - op.begin);
      }
      out += '\n';
    });
    if (!read) {
      parsed = false;
      return content;
    }
    return out;
  }

  // The name of a resource [/Pattern <target space>] for uncoloured patterns, added to
  // |resources| when missing; "" when there are no resources to add it to.
  std::string PatternSpace(QPDFObjectHandle resources) {
    if (!resources.isDictionary())
      return "";
    if (!resources.getKey("/ColorSpace").isDictionary())
      resources.replaceKey("/ColorSpace", QPDFObjectHandle::newDictionary());
    QPDFObjectHandle spaces = resources.getKey("/ColorSpace");
    const std::string name = gray_ ? "FyneGrayPattern" : "FyneRgbPattern";
    if (!spaces.hasKey("/" + name))
      spaces.replaceKey("/" + name, QPDFObjectHandle::parse(gray_ ? "[/Pattern /DeviceGray]" : "[/Pattern /DeviceRGB]"));
    return name;
  }

  // An inline picture's abbreviations (PDF 32000 8.9.7) in full.
  static std::string FullName(const std::string &name) {
    static const std::map<std::string, std::string> names{
        {"AHx", "ASCIIHexDecode"}, {"A85", "ASCII85Decode"},  {"LZW", "LZWDecode"},     {"Fl", "FlateDecode"},
        {"RL", "RunLengthDecode"},  {"CCF", "CCITTFaxDecode"}, {"DCT", "DCTDecode"},     {"G", "DeviceGray"},
        {"RGB", "DeviceRGB"},       {"CMYK", "DeviceCMYK"},    {"I", "Indexed"}};
    const auto found = names.find(name);
    return found == names.end() ? name : found->second;
  }

  // An inline object's value as a PDF object, abbreviations spelt out.
  static QPDFObjectHandle InlineValue(const std::string &text) {
    QPDFObjectHandle value = QPDFObjectHandle::parse(text);
    const auto expand = [](QPDFObjectHandle item) { return item.isName() ? QPDFObjectHandle::newName("/" + FullName(item.getName().substr(1))) : item; };
    if (value.isArray())
      for (int index = 0; index < value.getArrayNItems(); ++index)
        value.setArrayItem(index, expand(value.getArrayItem(index)));
    return expand(value);
  }

  // An inline picture (BI ... ID ... EI) in the target: decoded through its filters (qpdf's, by
  // way of the scratch stream) and written back as hex; a palette converted where it stands.
  // When it can't be decoded, Grayscale draws it grey through the wrapper. "" to keep it.
  std::string InlineImage(const std::string &text, QPDFObjectHandle resources) {
    size_t pos = 0;
    Token token;
    if (!NextToken(text, pos, token) || token.text != "BI")
      return "";
    // Key, value pairs up to ID; a value is one token, or an array or dictionary to its close.
    // Each value keeps where its text starts and ends.
    struct Value {
      std::vector<Token> tokens;
      size_t begin = 0, end = 0;
    };
    std::map<std::string, Value> dict;
    size_t data_start = std::string::npos;
    const auto is_id = [](const Token &t) { return t.kind == Token::Operator && t.text == "ID"; };
    while (NextToken(text, pos, token)) {
      if (is_id(token)) {
        data_start = pos + 1;
        break;
      }
      if (token.kind != Token::Name)
        continue;
      const std::string key = token.text;
      if (!NextToken(text, pos, token))
        break;
      if (is_id(token)) {
        data_start = pos + 1;
        break;
      }
      Value value;
      value.tokens.push_back(token);
      value.begin = token.begin;
      if (token.kind == Token::ArrayOpen || token.kind == Token::DictOpen) {
        const Token::Kind open = token.kind, close = open == Token::ArrayOpen ? Token::ArrayClose : Token::DictClose;
        for (int depth = 1; depth > 0 && NextToken(text, pos, token);) {
          value.tokens.push_back(token);
          depth += token.kind == open ? 1 : token.kind == close ? -1 : 0;
        }
      }
      value.end = value.tokens.back().end;
      dict[key] = value;
    }
    const auto find = [&](const char *a, const char *b) -> const Value * {
      auto found = dict.find(a);
      if (found == dict.end())
        found = dict.find(b);
      return found == dict.end() ? nullptr : &found->second;
    };
    const auto source = [&](const Value *value) { return text.substr(value->begin, value->end - value->begin); };
    const Value *mask = find("IM", "ImageMask"), *space_value = find("CS", "ColorSpace");
    // A stencil mask paints in the fill colour, which the content converted already.
    if (data_start == std::string::npos || !space_value || (mask && mask->tokens.front().text == "true"))
      return "";
    QPDFObjectHandle space;
    try {
      space = InlineValue(source(space_value));
    } catch (const std::exception &) {
      ++stats_.inlineKept;
      return "";
    }
    const Kind kind = KindOf(space, resources);
    // The text with the colour space's value replaced, the data untouched.
    const auto with_space = [&](const std::string &replacement) {
      return text.substr(0, space_value->begin) + replacement + text.substr(space_value->end);
    };
    if (kind == Kind::Indexed) {
      QPDFObjectHandle palette = Resolve(space, resources);
      // A palette already in the target (or converted through the resources) has nothing to do.
      if (palette.isArray() && palette.getArrayNItems() == 4) {
        QPDFObjectHandle base = Resolve(palette.getArrayItem(1), resources);
        if (!Wanted(KindOf(base, resources), base)) {
          if (!gray_)
            Spot(base, resources);
          return "";
        }
      }
      if (!Palette(palette, resources)) {
        ++stats_.inlineKept;
        return "";
      }
      ++stats_.inlineImages;
      // A named palette was changed in the resources; one written inline is written back.
      return space.isName() ? "" : with_space(palette.unparse());
    }
    QPDFObjectHandle resolved = Resolve(space, resources);
    if (!Wanted(kind, resolved)) {
      if (gray_ && kind != Kind::Gray)
        ++stats_.inlineKept;
      if (!gray_ && kind == Kind::Spot)
        Spot(resolved, resources);
      return "";
    }
    const Value *width = find("W", "Width"), *height = find("H", "Height"), *bits = find("BPC", "BitsPerComponent");
    const Value *filter = find("F", "Filter"), *parms = find("DP", "DecodeParms"), *decode_value = find("D", "Decode");
    const int w = width ? static_cast<int>(width->tokens.front().number) : 0, h = height ? static_cast<int>(height->tokens.front().number) : 0;
    const int depth = bits ? static_cast<int>(bits->tokens.front().number) : 8;
    const size_t components = Components(kind, resolved);
    std::string pixels, converted;
    bool decoded = false;
    if (w > 0 && h > 0 && static_cast<unsigned long long>(w) * h * components <= 64ULL * 1024 * 1024) {
      try {
        const size_t end = text.size() >= 2 ? text.size() - 2 : 0;  // where EI starts
        const std::string raw = data_start < end ? text.substr(data_start, end - data_start) : std::string();
        QPDFObjectHandle filters = filter ? InlineValue(source(filter)) : QPDFObjectHandle::newNull();
        QPDFObjectHandle decode_parms = parms ? QPDFObjectHandle::parse(source(parms)) : QPDFObjectHandle::newNull();
        decoded = Decode(raw, filters, decode_parms, pixels);
        std::vector<double> decode = decode_value ? Numbers(QPDFObjectHandle::parse(source(decode_value))) : std::vector<double>{};
        decoded = decoded && ToTargetPixels(kind, resolved, decode, depth, components, w, h, pixels, converted, resources);
      } catch (const std::exception &) {
        decoded = false;
      }
    }
    if (!decoded) {
      const std::string wrapper = WrapperName(kind, resources);
      if (wrapper.empty()) {
        ++(gray_ ? stats_.inlineKept : stats_.cmykLeft);
        return "";
      }
      ++stats_.inlineImages;
      ++stats_.wrapped;
      return with_space("/" + wrapper);
    }
    static const char *hex = "0123456789abcdef";
    std::string out = "BI /W " + std::to_string(w) + " /H " + std::to_string(h) + " /CS " + (gray_ ? "/G" : "/RGB") + " /BPC 8 /F /AHx ID\n";
    out.reserve(out.size() + converted.size() * 2 + converted.size() / 32 + 8);
    for (size_t at = 0; at < converted.size(); ++at) {
      const unsigned char byte = static_cast<unsigned char>(converted[at]);
      out += hex[byte >> 4];
      out += hex[byte & 15];
      if (at % 32 == 31)
        out += '\n';
    }
    out += ">\nEI";
    ++stats_.inlineImages;
    return out;
  }

  // Bytes through filters, by qpdf, in the scratch stream; DCT's parameters dropped (qpdf takes
  // none, and the JPEG's own markers say how its colours are stored).
  bool Decode(const std::string &raw, QPDFObjectHandle filters, QPDFObjectHandle parms, std::string &out) {
    if (filters.isNull()) {
      out = raw;
      return true;
    }
    if (filters.isName())
      filters = QPDFObjectHandle::newArray({filters});
    if (parms.isDictionary() || parms.isNull())
      parms = QPDFObjectHandle::newArray({parms});
    if (!filters.isArray() || !parms.isArray())
      return false;
    std::vector<QPDFObjectHandle> kept;
    for (int index = 0; index < filters.getArrayNItems(); ++index)
      kept.push_back(IsName(filters.getArrayItem(index), "/DCTDecode") || index >= parms.getArrayNItems() ? QPDFObjectHandle::newNull()
                                                                                                         : parms.getArrayItem(index));
    if (scratch_.isNull())
      scratch_ = QPDFObjectHandle::newStream(&q_);
    scratch_.replaceStreamData(raw, filters, QPDFObjectHandle::newArray(kept));
    out.clear();
    Pl_String sink("scratch", nullptr, out);
    bool ok = false;
    try {
      ok = scratch_.pipeStreamData(&sink, 0, qpdf_dl_all);
    } catch (const std::exception &) {
      ok = false;
    }
    if (!ok)
      std::string().swap(out);
    scratch_.replaceStreamData("", QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
    return ok;
  }

  // Pixels of |kind| (|components| samples of |bits| each, rows padded to bytes) as 8-bit target
  // pixels (grey, or RGB), through /Decode (default 0 to 1, Lab's own ranges).
  bool ToTargetPixels(Kind kind, QPDFObjectHandle space, std::vector<double> decode, int bits, size_t components, int width, int height,
                      const std::string &data, std::string &out, QPDFObjectHandle resources) {
    if ((bits != 1 && bits != 2 && bits != 4 && bits != 8 && bits != 16) || components == 0 || width <= 0 || height <= 0)
      return false;
    const size_t row_bytes = (static_cast<size_t>(width) * components * bits + 7) / 8;
    if (data.size() < row_bytes * height)
      return false;
    if (decode.size() != components * 2) {
      decode.clear();
      const std::vector<double> lab = kind == Kind::Lab ? LabRange(space) : std::vector<double>{};
      for (size_t k = 0; k < components; ++k) {
        if (kind == Kind::Lab) {
          decode.push_back(k == 0 ? 0 : lab[(k - 1) * 2]);
          decode.push_back(k == 0 ? 100 : lab[(k - 1) * 2 + 1]);
        } else {
          decode.push_back(0);
          decode.push_back(1);
        }
      }
    }
    const double top = (1 << bits) - 1;
    const unsigned char *in = reinterpret_cast<const unsigned char *>(data.data());
    const auto sample = [&](size_t row, size_t index) -> unsigned {
      const unsigned char *line = in + row * row_bytes;
      if (bits == 8)
        return line[index];
      if (bits == 16)
        return static_cast<unsigned>(line[index * 2] << 8 | line[index * 2 + 1]);
      const size_t bit = index * static_cast<size_t>(bits);
      return (line[bit / 8] >> (8 - bits - bit % 8)) & ((1u << bits) - 1);
    };
    const size_t n = Out();
    out.assign(static_cast<size_t>(width) * height * n, '\0');
    // An ICC-based picture, a row at a time through its own profile (Grayscale only: PDF/A keeps
    // ICC-based colours).
    if (const std::string key = gray_ && (kind == Kind::Rgb || kind == Kind::Cmyk) ? IccKey(space) : std::string(); !key.empty()) {
      std::vector<unsigned short> row_in(static_cast<size_t>(width) * components), row_rgb(static_cast<size_t>(width) * 3);
      for (size_t row = 0; row < static_cast<size_t>(height); ++row) {
        for (size_t x = 0; x < static_cast<size_t>(width); ++x)
          for (size_t k = 0; k < components; ++k) {
            const double value = decode[k * 2] + sample(row, x * components + k) / top * (decode[k * 2 + 1] - decode[k * 2]);
            row_in[x * components + k] = static_cast<unsigned short>(std::lround(std::clamp(value, 0.0, 1.0) * 65535));
          }
        profiles_.ToRgb(key, row_in.data(), row_rgb.data(), static_cast<size_t>(width));
        for (size_t x = 0; x < static_cast<size_t>(width); ++x) {
          const double g = Luma(row_rgb[x * 3] / 65535.0, row_rgb[x * 3 + 1] / 65535.0, row_rgb[x * 3 + 2] / 65535.0);
          out[row * width + x] = static_cast<char>(std::lround(g * 255));
        }
      }
      return true;
    }
    std::unordered_map<std::string, std::string> cache;  // for spot inks and Lab
    std::vector<double> values(components), target;
    std::string key(components * 2, '\0');
    for (size_t row = 0; row < static_cast<size_t>(height); ++row) {
      for (size_t x = 0; x < static_cast<size_t>(width); ++x) {
        for (size_t k = 0; k < components; ++k) {
          const unsigned raw = sample(row, x * components + k);
          values[k] = decode[k * 2] + raw / top * (decode[k * 2 + 1] - decode[k * 2]);
          key[k * 2] = static_cast<char>(raw >> 8);
          key[k * 2 + 1] = static_cast<char>(raw & 0xff);
        }
        char *pixel = &out[(row * width + x) * n];
        if (gray_ && kind == Kind::Rgb) {
          pixel[0] = static_cast<char>(std::lround(Luma(values[0], values[1], values[2]) * 255));
        } else if (gray_ && kind == Kind::Cmyk) {
          pixel[0] = static_cast<char>(std::lround(CmykGray(values[0], values[1], values[2], values[3]) * 255));
        } else if (!gray_ && kind == Kind::Cmyk) {
          double r, g, b;
          CmykToRgb(values[0], values[1], values[2], values[3], r, g, b);
          pixel[0] = static_cast<char>(std::lround(r * 255));
          pixel[1] = static_cast<char>(std::lround(g * 255));
          pixel[2] = static_cast<char>(std::lround(b * 255));
        } else {
          auto found = cache.find(key);
          if (found == cache.end()) {
            if (!ToTarget(kind, space, values, target, resources))
              return false;
            std::string bytes(n, '\0');
            for (size_t k = 0; k < n; ++k)
              bytes[k] = static_cast<char>(std::lround(std::clamp(target[k], 0.0, 1.0) * 255));
            found = cache.emplace(key, bytes).first;
          }
          std::copy(found->second.begin(), found->second.end(), pixel);
        }
      }
    }
    return true;
  }

  // A colour space that draws its samples in the target by itself, so data that can't be (or
  // needn't be) rewritten stays byte for byte: mesh gradients, pictures without a decoder, and
  // for PDF/A every device CMYK picture and gradient.
  // Grayscale: RGB or CMYK as DeviceN with inks no device has, so readers draw it through its
  // alternate, DeviceGray, by a calculator function computing the grey.
  // PDF/A: device CMYK as DeviceN of the four process inks with a DeviceRGB alternate, sampled at
  // the sixteen ink combinations: drawn between them exactly as CmykToRgb converts.
  // Null for other kinds.
  QPDFObjectHandle Wrapper(Kind kind) {
    if (!gray_) {
      if (kind != Kind::Cmyk)
        return QPDFObjectHandle::newNull();
      if (cmyk_wrapper_.isNull())
        cmyk_wrapper_ = q_.makeIndirectObject(QPDFObjectHandle::newArray(CmykAsRgb(q_)));
      return cmyk_wrapper_;
    }
    if (kind != Kind::Rgb && kind != Kind::Cmyk)
      return QPDFObjectHandle::newNull();
    QPDFObjectHandle &made = kind == Kind::Rgb ? rgb_wrapper_ : cmyk_wrapper_;
    if (!made.isNull())
      return made;
    const bool rgb = kind == Kind::Rgb;
    // Inputs on the stack in order (the last on top); out comes the grey.
    QPDFObjectHandle function = QPDFObjectHandle::newStream(
        &q_, rgb ? "{0.114 mul exch 0.587 mul add exch 0.299 mul add}" : "{exch 0.11 mul add exch 0.59 mul add exch 0.3 mul add 1 exch sub dup 0 lt {pop 0} if}");
    QPDFObjectHandle dict = function.getDict();
    dict.replaceKey("/FunctionType", QPDFObjectHandle::newInteger(4));
    dict.replaceKey("/Domain", QPDFObjectHandle::parse(rgb ? "[0 1 0 1 0 1]" : "[0 1 0 1 0 1 0 1]"));
    dict.replaceKey("/Range", QPDFObjectHandle::parse("[0 1]"));
    made = q_.makeIndirectObject(QPDFObjectHandle::newArray(
        {QPDFObjectHandle::newName("/DeviceN"),
         QPDFObjectHandle::parse(rgb ? "[/FyneGrayRed /FyneGrayGreen /FyneGrayBlue]" : "[/FyneGrayCyan /FyneGrayMagenta /FyneGrayYellow /FyneGrayBlack]"),
         QPDFObjectHandle::newName("/DeviceGray"), function}));
    return made;
  }

  // The wrapper's name in |resources| (added when missing), for inline pictures; "" when it
  // can't be added.
  std::string WrapperName(Kind kind, QPDFObjectHandle resources) {
    QPDFObjectHandle wrapper = Wrapper(kind);
    if (wrapper.isNull() || !resources.isDictionary())
      return "";
    if (!resources.getKey("/ColorSpace").isDictionary())
      resources.replaceKey("/ColorSpace", QPDFObjectHandle::newDictionary());
    const std::string name = !gray_ ? "FyneCMYK" : kind == Kind::Rgb ? "FyneGrayRGB" : "FyneGrayCMYK";
    resources.getKey("/ColorSpace").replaceKey("/" + name, wrapper);
    return name;
  }

  // ---------------------------------------------------------------------------------------------
  // Resources, forms, groups

  void Resources(QPDFObjectHandle resources) {
    if (!resources.isDictionary() || (resources.isIndirect() && !done_.insert(resources.getObjGen()).second))
      return;
    QPDFObjectHandle xobjects = resources.getKey("/XObject");
    for (const std::string &key : xobjects.isDictionary() ? xobjects.getKeys() : std::set<std::string>{}) {
      QPDFObjectHandle xobject = xobjects.getKey(key);
      if (!xobject.isStream())
        continue;
      QPDFObjectHandle subtype = xobject.getDict().getKey("/Subtype");
      if (IsName(subtype, "/Image")) {
        if (images_)
          Image(xobject, resources);
      } else if (IsName(subtype, "/Form")) {
        Form(xobject, resources);
      }
    }
    if (!drawings_)
      return;
    QPDFObjectHandle shadings = resources.getKey("/Shading");
    for (const std::string &key : shadings.isDictionary() ? shadings.getKeys() : std::set<std::string>{})
      Shading(shadings.getKey(key), resources);
    QPDFObjectHandle patterns = resources.getKey("/Pattern");
    for (const std::string &key : patterns.isDictionary() ? patterns.getKeys() : std::set<std::string>{}) {
      QPDFObjectHandle pattern = patterns.getKey(key);
      if (pattern.isStream())
        Form(pattern, resources);  // a tiling pattern: content of its own
      else if (pattern.isDictionary())
        Shading(pattern.getKey("/Shading"), resources);
    }
    // Palettes named here may serve fills and pictures alike, so only when both are converted.
    QPDFObjectHandle spaces = images_ ? resources.getKey("/ColorSpace") : QPDFObjectHandle::newNull();
    for (const std::string &key : spaces.isDictionary() ? spaces.getKeys() : std::set<std::string>{}) {
      QPDFObjectHandle space = Resolve(spaces.getKey(key), resources);
      const Kind kind = KindOf(space, resources);
      if (kind == Kind::Indexed)
        Palette(space, resources);
      else if (kind == Kind::Spot)
        Spot(space, resources);
    }
    // With everything grey, the device spaces' calibrated stand-ins go: DefaultRGB and
    // DefaultCMYK have nothing left to apply to, and DefaultGray (often CalGray with a D50 white)
    // would tint the new greys in some readers.
    if (gray_)
      for (const char *key : {"/DefaultGray", "/DefaultRGB", "/DefaultCMYK"})
        if (spaces.isDictionary())
          spaces.removeKey(key);
    QPDFObjectHandle states = resources.getKey("/ExtGState");
    for (const std::string &key : states.isDictionary() ? states.getKeys() : std::set<std::string>{})
      SoftMask(states.getKey(key), resources);
    QPDFObjectHandle fonts = resources.getKey("/Font");
    for (const std::string &key : fonts.isDictionary() ? fonts.getKeys() : std::set<std::string>{})
      Type3(fonts.getKey(key), resources);
  }

  void Form(QPDFObjectHandle form, QPDFObjectHandle inherited) {
    if (!form.isStream() || (form.isIndirect() && !done_.insert(form.getObjGen()).second))
      return;
    QPDFObjectHandle dict = form.getDict();
    QPDFObjectHandle resources = dict.getKey("/Resources").isDictionary() ? dict.getKey("/Resources") : inherited;
    bool ok = true;
    const std::string content = StreamData(form, ok);
    if (ok) {
      bool changed = false, parsed = true;
      const auto from = form.isIndirect() ? inherited_.find(form.getObjGen()) : inherited_.end();
      const std::string rewritten = Content(content, resources, changed, parsed, from != inherited_.end() ? from->second : State{});
      if (!parsed)
        ++stats_.contentKept;
      else if (changed)
        Hold(form, rewritten);
    } else {
      ++stats_.contentKept;
    }
    Resources(resources);
    Group(dict);
  }

  // A transparency group blends in the target (when everything is converted).
  void Group(QPDFObjectHandle dict) {
    if (!drawings_ || !images_)
      return;
    QPDFObjectHandle group = dict.isDictionary() ? dict.getKey("/Group") : QPDFObjectHandle::newNull();
    if (!group.isDictionary() || !group.hasKey("/CS"))
      return;
    QPDFObjectHandle space = group.getKey("/CS");
    if (Wanted(KindOf(space, QPDFObjectHandle::newNull()), space))
      group.replaceKey("/CS", QPDFObjectHandle::newName(OutSpace()));
  }

  // A soft mask's group, and its backdrop colour (in the group's space, so taken first).
  void SoftMask(QPDFObjectHandle state, QPDFObjectHandle resources) {
    QPDFObjectHandle mask = state.isDictionary() ? state.getKey("/SMask") : QPDFObjectHandle::newNull();
    if (!mask.isDictionary())
      return;
    QPDFObjectHandle group = mask.getKey("/G");
    if (!group.isStream())
      return;
    QPDFObjectHandle group_dict = group.getDict().getKey("/Group");
    QPDFObjectHandle space = group_dict.isDictionary() ? Resolve(group_dict.getKey("/CS"), resources) : QPDFObjectHandle::newNull();
    const Kind kind = KindOf(space, resources);
    std::vector<double> backdrop = Numbers(mask.getKey("/BC")), target;
    if (!backdrop.empty() && Wanted(kind, space) && ToTarget(kind, space, backdrop, target, resources))
      mask.replaceKey("/BC", NumberArray(target));
    Form(group, resources);
  }

  // A Type 3 font's glyphs are content streams of their own.
  void Type3(QPDFObjectHandle font, QPDFObjectHandle inherited) {
    if (!font.isDictionary() || !IsName(font.getKey("/Subtype"), "/Type3") || (font.isIndirect() && !done_.insert(font.getObjGen()).second))
      return;
    QPDFObjectHandle resources = font.getKey("/Resources").isDictionary() ? font.getKey("/Resources") : inherited;
    QPDFObjectHandle procs = font.getKey("/CharProcs");
    for (const std::string &key : procs.isDictionary() ? procs.getKeys() : std::set<std::string>{}) {
      QPDFObjectHandle glyph = procs.getKey(key);
      if (!glyph.isStream() || (glyph.isIndirect() && !done_.insert(glyph.getObjGen()).second))
        continue;
      bool ok = true;
      const std::string content = StreamData(glyph, ok);
      bool changed = false, parsed = true;
      const std::string rewritten = ok ? Content(content, resources, changed, parsed) : content;
      if (ok && parsed && changed)
        Hold(glyph, rewritten);
    }
    Resources(resources);
  }

  // ---------------------------------------------------------------------------------------------
  // Pictures

  // A stream's filters and their parameters as two lists of the same length.
  static void Filters(QPDFObjectHandle dict, std::vector<QPDFObjectHandle> &filters, std::vector<QPDFObjectHandle> &parms) {
    QPDFObjectHandle filter = dict.getKey("/Filter"), parm = dict.getKey("/DecodeParms");
    if (filter.isName())
      filters.push_back(filter);
    for (int index = 0; filter.isArray() && index < filter.getArrayNItems(); ++index)
      filters.push_back(filter.getArrayItem(index));
    if (parm.isDictionary())
      parms.push_back(parm);
    for (int index = 0; parm.isArray() && index < parm.getArrayNItems(); ++index)
      parms.push_back(parm.getArrayItem(index));
    parms.resize(filters.size(), QPDFObjectHandle::newNull());
  }

  // A stream's bytes through its first |count| filters (all of them when |count| is past the
  // end; a lossy one, DCT, only with |lossy|). When qpdf refuses the stream as it is (writers put
  // DCT parameters there it won't take, such as /Quality), the bytes go through Decode. False
  // when a filter can't be decoded. |expected| is the decoded size when known: the bytes are
  // decoded straight into |out|, made that big once, instead of into a buffer and then copied.
  bool Filtered(QPDFObjectHandle stream, size_t count, bool lossy, std::string &out, size_t expected = 0) {
    std::vector<QPDFObjectHandle> filters, parms;
    Filters(stream.getDict(), filters, parms);
    count = std::min(count, filters.size());
    if (count == filters.size()) {
      out.clear();
      out.reserve(expected);
      Pl_String sink("image", nullptr, out);
      try {
        if (stream.pipeStreamData(&sink, 0, lossy ? qpdf_dl_all : qpdf_dl_generalized))
          return true;
      } catch (const std::exception &) {
      }
      std::string().swap(out);
    }
    const std::shared_ptr<Buffer> raw = stream.getRawStreamData();
    if (!raw)
      return false;
    std::string bytes(reinterpret_cast<const char *>(raw->getBuffer()), raw->getSize());
    if (count == 0) {
      out = std::move(bytes);
      return true;
    }
    for (size_t index = 0; index < count && !lossy; ++index)
      if (IsName(filters[index], "/DCTDecode") || IsName(filters[index], "/JPXDecode"))
        return false;
    return Decode(bytes, QPDFObjectHandle::newArray(std::vector<QPDFObjectHandle>(filters.begin(), filters.begin() + static_cast<long>(count))),
                  QPDFObjectHandle::newArray(std::vector<QPDFObjectHandle>(parms.begin(), parms.begin() + static_cast<long>(count))), out);
  }

  void Image(QPDFObjectHandle image, QPDFObjectHandle resources) {
    if (image.isIndirect() && !done_.insert(image.getObjGen()).second)
      return;
    QPDFObjectHandle dict = image.getDict();
    if (dict.getKey("/ImageMask").isBool() && dict.getKey("/ImageMask").getBoolValue())
      return;
    std::vector<QPDFObjectHandle> filters, parms;
    Filters(dict, filters, parms);
    const std::string last = filters.empty() ? "" : filters.back().isName() ? filters.back().getName() : "?";
    QPDFObjectHandle space = dict.getKey("/ColorSpace");
    if (last == "/JPXDecode") {
      JpxImage(image, resources);
      return;
    }
    const Kind kind = KindOf(space, resources);
    QPDFObjectHandle resolved = Resolve(space, resources);
    if (kind == Kind::Indexed) {
      if (Palette(resolved, resources))
        ++stats_.images;
      return;
    }
    if (!Wanted(kind, resolved)) {
      if (gray_ && kind == Kind::Other)
        ++stats_.imagesKept;
      if (!gray_ && kind == Kind::Spot)
        Spot(resolved, resources);
      return;
    }
    // A colour-key mask names colours in the picture's own space, so the samples must stay: it
    // is drawn through the wrapper. So is every CMYK picture for PDF/A: no decoding, no
    // re-compressing, the same colours.
    if (dict.getKey("/Mask").isArray() || !gray_) {
      Wrap(image, kind);
      return;
    }
    // A YCbCr JPEG (behind any lossless filters): its brightness channel kept as it was
    // compressed. Not when the PDF says its colours aren't YCbCr (/ColorTransform 0).
    QPDFObjectHandle dct_parm = parms.empty() ? QPDFObjectHandle::newNull() : parms.back();
    const bool not_ycbcr = dct_parm.isDictionary() && dct_parm.getKey("/ColorTransform").isInteger() &&
                           dct_parm.getKey("/ColorTransform").getIntValueAsInt() == 0;
    std::string jpeg_bytes, converted;
    if (gray_ && kind == Kind::Rgb && last == "/DCTDecode" && !dict.hasKey("/Decode") && !not_ycbcr &&
        Filtered(image, filters.size() - 1, false, jpeg_bytes) && jpeg::GrayFromYCbCr(jpeg_bytes, converted)) {
      image.replaceStreamData(converted, QPDFObjectHandle::newName("/DCTDecode"), QPDFObjectHandle::newNull());
      dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceGray"));
      dict.removeKey("/DecodeParms");
      ++stats_.images;
      ++stats_.jpegLossless;
      return;
    }
    const int width = Integer(dict, "/Width", 0), height = Integer(dict, "/Height", 0);
    // A JPEG's samples are 8 bits whatever the dictionary says.
    const int bits = last == "/DCTDecode" ? 8 : Integer(dict, "/BitsPerComponent", 8);
    const size_t components = Components(kind, resolved);
    const unsigned long long size = (static_cast<unsigned long long>(std::max(width, 0)) * components * bits + 7) / 8 * std::max(height, 0);
    std::string data;
    if (width > 0 && height > 0 && components > 0 && size <= kMaximumPixelBytes &&
        Filtered(image, filters.size(), true, data, static_cast<size_t>(size)) &&
        ToTargetPixels(kind, resolved, Numbers(dict.getKey("/Decode")), bits, components, width, height, data, converted, resources)) {
      std::string().swap(data);  // freed before the new pixels are encoded
      WriteTarget(image, converted, width, height, last == "/DCTDecode");
      ++(kind == Kind::Spot ? stats_.spots : stats_.images);
      return;
    }
    // Not decodable here, or too large to decode: drawn grey through the wrapper instead.
    Wrap(image, kind);
  }

  // A picture whose samples stay as they are, drawn in the target through the wrapper colour
  // space; counted as kept in colour when its kind has no wrapper (Lab, spot inks).
  void Wrap(QPDFObjectHandle image, Kind kind) {
    QPDFObjectHandle wrapper = Wrapper(kind);
    if (wrapper.isNull()) {
      ++(gray_ ? stats_.imagesKept : stats_.cmykLeft);
      return;
    }
    image.getDict().replaceKey("/ColorSpace", wrapper);
    ++stats_.images;
    ++stats_.wrapped;
  }

  // |stream|'s new data, as it is or, past kRawHeld for the file, compressed (HeldData).
  void Hold(QPDFObjectHandle stream, const std::string &data) {
    if (raw_held_ + data.size() <= kRawHeld) {
      raw_held_ += data.size();
      stream.replaceStreamData(data, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
    } else {
      stream.replaceStreamData(std::make_shared<HeldData>(data), QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
    }
  }

  // A picture's new pixels in the target: a photo that was a JPEG stays one, so the file keeps
  // its size; anything else is written losslessly (qpdf compresses it when the file is written).
  void WriteTarget(QPDFObjectHandle image, const std::string &pixels, int width, int height, bool photo) {
    QPDFObjectHandle dict = image.getDict();
    std::string encoded;
    if (photo && jpeg::Encode(reinterpret_cast<const unsigned char *>(pixels.data()), width, height, static_cast<int>(Out()), quality_, encoded))
      image.replaceStreamData(encoded, QPDFObjectHandle::newName("/DCTDecode"), QPDFObjectHandle::newNull());
    else
      Hold(image, pixels);
    dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName(OutSpace()));
    dict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
    dict.removeKey("/Decode");
    dict.removeKey("/DecodeParms");
  }

  // A JPEG 2000 picture. Grayscale decodes it with OpenJPEG when the library was built with it
  // (its own alpha, /SMaskInData 1, becomes a soft mask). PDF/A leaves it, unless it is in
  // device CMYK, which is drawn through the wrapper.
  void JpxImage(QPDFObjectHandle image, QPDFObjectHandle resources) {
    QPDFObjectHandle dict = image.getDict();
    QPDFObjectHandle space = Resolve(dict.getKey("/ColorSpace"), resources);
    const Kind kind = KindOf(space, resources);
    if (!gray_) {
      if (kind == Kind::Cmyk && Device(space))
        Wrap(image, kind);
      else if (kind == Kind::Spot)
        Spot(space, resources);
      return;
    }
    if (kind == Kind::Gray)
      return;
    std::string bytes, gray, alpha;
    int width = 0, height = 0;
    if (!Filtered(image, dict.getKey("/Filter").isArray() ? dict.getKey("/Filter").getArrayNItems() - 1 : 0, false, bytes) ||
        !jpx::DecodeGray(bytes, gray, alpha, width, height) || width != Integer(dict, "/Width", width) ||
        height != Integer(dict, "/Height", height)) {
      // Without a decoder, a picture that names its colour space is drawn grey through the wrapper.
      if (kind == Kind::Rgb || kind == Kind::Cmyk)
        Wrap(image, kind);
      else
        ++stats_.imagesKept;
      return;
    }
    const bool own_alpha = Integer(dict, "/SMaskInData", 0) != 0;
    if (own_alpha && !alpha.empty()) {
      QPDFObjectHandle mask = QPDFObjectHandle::newStream(&q_);
      Hold(mask, alpha);
      QPDFObjectHandle mask_dict = mask.getDict();
      mask_dict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
      mask_dict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
      mask_dict.replaceKey("/Width", QPDFObjectHandle::newInteger(width));
      mask_dict.replaceKey("/Height", QPDFObjectHandle::newInteger(height));
      mask_dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceGray"));
      mask_dict.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
      dict.replaceKey("/SMask", mask);
    }
    dict.removeKey("/SMaskInData");
    WriteTarget(image, gray, width, height, true);
    ++stats_.images;
  }

  // ---------------------------------------------------------------------------------------------
  // Gradients

  // Grayscale: an exponential function's ends (C0, C1) as grey, or a stitched function's every
  // part; RGB and CMYK only, where grey is linear in the colour. Nothing otherwise.
  QPDFObjectHandle LinearFunction(QPDFObjectHandle function, Kind kind, int depth = 0) {
    QPDFObjectHandle dict = function.isStream() ? function.getDict() : function;
    if (!gray_ || depth > 8 || !dict.isDictionary() || (kind != Kind::Rgb && kind != Kind::Cmyk))
      return QPDFObjectHandle::newNull();
    const int type = Integer(dict, "/FunctionType", -1);
    const size_t n = kind == Kind::Rgb ? 3 : 4;
    if (type == 2) {
      std::vector<double> c0 = dict.hasKey("/C0") ? Numbers(dict.getKey("/C0")) : std::vector<double>(n, 0);
      std::vector<double> c1 = dict.hasKey("/C1") ? Numbers(dict.getKey("/C1")) : std::vector<double>(n, 1);
      if (c0.size() != n || c1.size() != n)
        return QPDFObjectHandle::newNull();
      const auto gray = [&](const std::vector<double> &v) { return n == 3 ? Luma(v[0], v[1], v[2]) : CmykGray(v[0], v[1], v[2], v[3]); };
      QPDFObjectHandle made = QPDFObjectHandle::newDictionary();
      made.replaceKey("/FunctionType", QPDFObjectHandle::newInteger(2));
      made.replaceKey("/Domain", dict.getKey("/Domain").isArray() ? dict.getKey("/Domain") : QPDFObjectHandle::parse("[0 1]"));
      made.replaceKey("/C0", NumberArray({gray(c0)}));
      made.replaceKey("/C1", NumberArray({gray(c1)}));
      made.replaceKey("/N", dict.getKey("/N").isNumber() ? dict.getKey("/N") : QPDFObjectHandle::newInteger(1));
      return q_.makeIndirectObject(made);
    }
    if (type == 3 && dict.getKey("/Functions").isArray()) {
      QPDFObjectHandle parts = dict.getKey("/Functions");
      std::vector<QPDFObjectHandle> converted;
      for (int index = 0; index < parts.getArrayNItems(); ++index) {
        QPDFObjectHandle part = LinearFunction(parts.getArrayItem(index), kind, depth + 1);
        if (part.isNull())
          return QPDFObjectHandle::newNull();
        converted.push_back(part);
      }
      QPDFObjectHandle made = QPDFObjectHandle::newDictionary();
      for (const char *key : {"/FunctionType", "/Domain", "/Bounds", "/Encode"})
        if (dict.hasKey(key))
          made.replaceKey(key, dict.getKey(key));
      made.replaceKey("/Functions", QPDFObjectHandle::newArray(converted));
      return q_.makeIndirectObject(made);
    }
    return QPDFObjectHandle::newNull();
  }

  // A gradient's function sampled in the target: 256 samples along one input, or 64 by 64 over
  // two (function-based gradients); 16 bits each.
  QPDFObjectHandle SampledFunction(QPDFObjectHandle function, Kind kind, QPDFObjectHandle space, QPDFObjectHandle resources,
                                   std::vector<double> domain) {
    const std::unique_ptr<PdfFunction> read = PdfFunction::Read(function);
    if (!read || (read->Inputs() != 1 && read->Inputs() != 2))
      return QPDFObjectHandle::newNull();
    const size_t inputs = read->Inputs();
    if (domain.size() != inputs * 2)
      domain = read->Domain();
    if (domain.size() != inputs * 2)
      return QPDFObjectHandle::newNull();
    const int size = inputs == 1 ? 256 : 64;
    const int count = inputs == 1 ? size : size * size;
    const size_t n = Out();
    std::string data(static_cast<size_t>(count) * n * 2, '\0');
    for (int at = 0; at < count; ++at) {
      std::vector<double> in;
      const int xs[2] = {at % size, at / size};
      for (size_t i = 0; i < inputs; ++i)
        in.push_back(domain[i * 2] + (domain[i * 2 + 1] - domain[i * 2]) * xs[i] / (size - 1));
      std::vector<double> values, target;
      if (!read->Evaluate(in, values) || !ToTarget(kind, space, values, target, resources))
        return QPDFObjectHandle::newNull();
      for (size_t k = 0; k < n; ++k) {
        const long value = std::lround(std::clamp(target[k], 0.0, 1.0) * 65535);
        data[(static_cast<size_t>(at) * n + k) * 2] = static_cast<char>(value >> 8);
        data[(static_cast<size_t>(at) * n + k) * 2 + 1] = static_cast<char>(value & 0xff);
      }
    }
    QPDFObjectHandle made = QPDFObjectHandle::newStream(&q_, data);
    QPDFObjectHandle made_dict = made.getDict();
    made_dict.replaceKey("/FunctionType", QPDFObjectHandle::newInteger(0));
    made_dict.replaceKey("/Domain", NumberArray(domain));
    made_dict.replaceKey("/Range", QPDFObjectHandle::parse(gray_ ? "[0 1]" : "[0 1 0 1 0 1]"));
    made_dict.replaceKey("/Size", inputs == 1 ? QPDFObjectHandle::parse("[256]") : QPDFObjectHandle::parse("[64 64]"));
    made_dict.replaceKey("/BitsPerSample", QPDFObjectHandle::newInteger(16));
    return made;
  }

  void Shading(QPDFObjectHandle shading, QPDFObjectHandle resources) {
    if (shading.isIndirect() && !done_.insert(shading.getObjGen()).second)
      return;
    QPDFObjectHandle dict = shading.isStream() ? shading.getDict() : shading;
    if (!dict.isDictionary())
      return;
    QPDFObjectHandle space = Resolve(dict.getKey("/ColorSpace"), resources);
    const Kind kind = KindOf(space, resources);
    if (!Wanted(kind, space)) {
      if (!gray_ && kind == Kind::Spot)
        Spot(space, resources);
      return;
    }
    const int type = Integer(dict, "/ShadingType", 0);
    QPDFObjectHandle function = dict.getKey("/Function");
    // A mesh gradient with its colours in its stream is drawn through the wrapper; so is every
    // device CMYK gradient for PDF/A (its function, background and all stay).
    if (function.isNull() || !gray_) {
      QPDFObjectHandle wrapper = Wrapper(kind);
      if (wrapper.isNull()) {
        ++(gray_ ? stats_.shadingsKept : stats_.cmykLeft);
        return;
      }
      dict.replaceKey("/ColorSpace", wrapper);
      ++stats_.shadings;
      ++stats_.wrapped;
      return;
    }
    // The shortcut through the function's ends holds for the grey formulas, not through a profile.
    QPDFObjectHandle made = (type == 2 || type == 3) && IccKey(space).empty() ? LinearFunction(function, kind) : QPDFObjectHandle::newNull();
    if (made.isNull()) {
      // Function-based gradients sample over their /Domain; axial and radial over theirs; a
      // mesh's function over its own domain.
      std::vector<double> domain = Numbers(dict.getKey("/Domain"));
      if (type == 1 && domain.empty())
        domain = {0, 1, 0, 1};
      if ((type == 2 || type == 3) && domain.empty())
        domain = {0, 1};
      if (type >= 4)
        domain.clear();
      made = SampledFunction(function, kind, space, resources, domain);
    }
    if (made.isNull()) {
      ++(gray_ ? stats_.shadingsKept : stats_.cmykLeft);
      return;
    }
    dict.replaceKey("/Function", made);
    dict.replaceKey("/ColorSpace", QPDFObjectHandle::newName(OutSpace()));
    std::vector<double> background = Numbers(dict.getKey("/Background")), target;
    if (!background.empty() && ToTarget(kind, space, background, target, resources))
      dict.replaceKey("/Background", NumberArray(target));
    else
      dict.removeKey("/Background");
    ++(kind == Kind::Spot ? stats_.spots : stats_.shadings);
  }

  // ---------------------------------------------------------------------------------------------
  // Comments and form fields

  // A colour array (an annotation's /C or /IC, a widget's /MK /BC or /BG) in the target: grey for
  // Grayscale; for PDF/A only CMYK (four values) to RGB.
  void ColourArray(QPDFObjectHandle dict, const char *key) {
    std::vector<double> values = Numbers(dict.getKey(key));
    if (gray_ && values.size() == 3) {
      dict.replaceKey(key, NumberArray({Luma(values[0], values[1], values[2])}));
    } else if (values.size() == 4) {
      std::vector<double> target;
      ToTarget(Kind::Cmyk, QPDFObjectHandle::newName("/DeviceCMYK"), values, target, QPDFObjectHandle::newNull());
      dict.replaceKey(key, NumberArray(target));
    }
  }

  void DefaultAppearance(QPDFObjectHandle dict) {
    QPDFObjectHandle da = dict.getKey("/DA");
    if (!da.isString())
      return;
    bool changed = false, parsed = true;
    std::string rewritten = Content(da.getStringValue(), QPDFObjectHandle::newNull(), changed, parsed);
    if (!parsed || !changed)
      return;
    std::replace(rewritten.begin(), rewritten.end(), '\n', ' ');
    while (!rewritten.empty() && rewritten.back() == ' ')
      rewritten.pop_back();
    dict.replaceKey("/DA", QPDFObjectHandle::newString(rewritten));
  }

  // Comments and form fields: their appearances, their colours, and the colours they're redrawn in.
  void Annotations(QPDFObjectHandle page, QPDFObjectHandle resources) {
    QPDFObjectHandle annotations = page.getKey("/Annots");
    for (int index = 0; annotations.isArray() && index < annotations.getArrayNItems(); ++index) {
      QPDFObjectHandle annotation = annotations.getArrayItem(index);
      if (!annotation.isDictionary())
        continue;
      QPDFObjectHandle appearance = annotation.getKey("/AP");
      for (const char *kind : {"/N", "/R", "/D"}) {
        QPDFObjectHandle state = appearance.isDictionary() ? appearance.getKey(kind) : QPDFObjectHandle::newNull();
        if (state.isStream())
          Form(state, resources);
        else if (state.isDictionary())
          for (const std::string &key : state.getKeys())
            if (state.getKey(key).isStream())
              Form(state.getKey(key), resources);
      }
      ColourArray(annotation, "/C");
      ColourArray(annotation, "/IC");
      QPDFObjectHandle look = annotation.getKey("/MK");
      if (look.isDictionary()) {
        ColourArray(look, "/BC");
        ColourArray(look, "/BG");
      }
      DefaultAppearance(annotation);
    }
  }

  QPDF &q_;
  ColourStats &stats_;
  const bool gray_, drawings_, images_;
  const int quality_;
  std::set<QPDFObjGen> done_;
  icc::Profiles profiles_;
  std::set<std::string> profiles_seen_;
  std::map<QPDFObjGen, State> inherited_;  // the colour spaces each form is first drawn in
  size_t raw_held_ = 0;  // bytes of new stream data kept uncompressed (Hold)
  QPDFObjectHandle scratch_ = QPDFObjectHandle::newNull();  // a stream reused to decode with cleaned parameters
  QPDFObjectHandle rgb_wrapper_ = QPDFObjectHandle::newNull(), cmyk_wrapper_ = QPDFObjectHandle::newNull();
  std::map<std::string, std::shared_ptr<PdfFunction>> functions_;  // spot inks' tint transforms
  std::map<std::string, QPDFObjectHandle> spots_made_;  // PDF/A: RGB tint transforms made, by the ink's old space
};

ColourConverter::ColourConverter(QPDF &q, ColourStats &stats, const ColourOptions &options)
    : impl_(std::make_unique<Converter>(q, stats, options)) {}
ColourConverter::~ColourConverter() = default;
void ColourConverter::Page(QPDFPageObjectHelper &page) { impl_->Page(page); }
void ColourConverter::FormDefaults() { impl_->FormDefaults(); }

} // namespace fynepdf::pdfa
