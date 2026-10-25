#include "appearance.h"

#include "common.h"
#include "content.h"
#include "font_program.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fynepdf::pdfa {
namespace {

using content::Num;

// A colour array as a fill (|stroke| false) or stroke colour operator; "" for no colour.
std::string Colour(QPDFObjectHandle array, bool stroke) {
  std::vector<double> values = Numbers(array);
  if (values.size() == 1)
    return Num(values[0]) + (stroke ? " G" : " g");
  if (values.size() == 4) {
    double r, g, b;
    CmykToRgb(values[0], values[1], values[2], values[3], r, g, b);
    values = {r, g, b};
  }
  if (values.size() == 3)
    return Num(values[0]) + " " + Num(values[1]) + " " + Num(values[2]) + (stroke ? " RG" : " rg");
  return "";
}

// The border width: /BS /W, the old /Border array's third number, or 1.
double BorderWidth(QPDFObjectHandle annotation) {
  QPDFObjectHandle style = annotation.getKey("/BS");
  if (style.isDictionary() && style.getKey("/W").isNumber())
    return std::max(0.0, style.getKey("/W").getNumericValue());
  std::vector<double> border = Numbers(annotation.getKey("/Border"));
  if (border.size() >= 3)
    return std::max(0.0, border[2]);
  return 1;
}

// A dashed border's dash operator, "" for a solid one.
std::string Dash(QPDFObjectHandle annotation) {
  QPDFObjectHandle style = annotation.getKey("/BS");
  if (!style.isDictionary() || !IsName(style.getKey("/S"), "/D"))
    return "";
  std::vector<double> dash = Numbers(style.getKey("/D"));
  if (dash.empty())
    dash = {3};
  std::string out = "[";
  for (const double value : dash)
    out += (out.size() > 1 ? " " : "") + Num(value);
  return out + "] 0 d\n";
}

struct Box {
  double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

Box Normalized(const std::vector<double> &r) {
  return {std::min(r[0], r[2]), std::min(r[1], r[3]), std::max(r[0], r[2]), std::max(r[1], r[3])};
}

std::string Point(double x, double y, const char *op) { return Num(x) + " " + Num(y) + " " + op + "\n"; }

// An ellipse inside |b| as four Bézier curves.
std::string Ellipse(const Box &b) {
  const double k = 0.5523, cx = (b.x1 + b.x2) / 2, cy = (b.y1 + b.y2) / 2, rx = (b.x2 - b.x1) / 2, ry = (b.y2 - b.y1) / 2;
  std::string out = Point(cx + rx, cy, "m");
  const double curves[4][6] = {{cx + rx, cy + k * ry, cx + k * rx, cy + ry, cx, cy + ry},
                               {cx - k * rx, cy + ry, cx - rx, cy + k * ry, cx - rx, cy},
                               {cx - rx, cy - k * ry, cx - k * rx, cy - ry, cx, cy - ry},
                               {cx + k * rx, cy - ry, cx + rx, cy - k * ry, cx + rx, cy}};
  for (const auto &c : curves)
    out += Num(c[0]) + " " + Num(c[1]) + " " + Num(c[2]) + " " + Num(c[3]) + " " + Num(c[4]) + " " + Num(c[5]) + " c\n";
  return out;
}

// The paint operator for a path with a fill and/or a stroke.
std::string Paint(bool fill, bool stroke) { return fill && stroke ? "B\n" : fill ? "f\n" : stroke ? "S\n" : "n\n"; }

// An arrowhead at (x, y) pointing away from (fx, fy).
std::string Arrow(double x, double y, double fx, double fy, double width, bool closed) {
  const double angle = std::atan2(y - fy, x - fx), length = std::max(6.0, width * 6), spread = 0.5;
  const double ax = x - length * std::cos(angle - spread), ay = y - length * std::sin(angle - spread);
  const double bx = x - length * std::cos(angle + spread), by = y - length * std::sin(angle + spread);
  return Point(ax, ay, "m") + Point(x, y, "l") + Point(bx, by, "l") + (closed ? "h\nB\n" : "S\n");
}

QPDFObjectHandle Form(QPDF &q, const std::string &content, const Box &box, QPDFObjectHandle resources) {
  QPDFObjectHandle stream = QPDFObjectHandle::newStream(&q, content);
  QPDFObjectHandle dict = stream.getDict();
  dict.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
  dict.replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
  dict.replaceKey("/BBox", NumberArray({box.x1, box.y1, box.x2, box.y2}));
  dict.replaceKey("/Resources", resources.isNull() ? QPDFObjectHandle::newDictionary() : resources);
  return stream;
}

// -------------------------------------------------------------------------------------------
// Text: form fields, free text and stamps

// A field's entry, from it or the fields above it.
QPDFObjectHandle Inherited(QPDFObjectHandle field, const char *key) {
  QPDFObjectHandle at = field;
  for (int depth = 0; depth < 32 && at.isDictionary(); ++depth, at = at.getKey("/Parent"))
    if (at.hasKey(key))
      return at.getKey(key);
  return QPDFObjectHandle::newNull();
}

std::string TextOf(QPDFObjectHandle value) { return value.isString() ? value.getUTF8Value() : value.isName() ? value.getName().substr(1) : ""; }

// UTF-8 as characters (a byte that doesn't start one as itself).
std::vector<unsigned> Characters(const std::string &text) {
  std::vector<unsigned> out;
  for (size_t at = 0; at < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[at]);
    const int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    unsigned value = extra == 3 ? c & 7u : extra == 2 ? c & 15u : extra == 1 ? c & 31u : c;
    bool ok = at + extra < text.size();
    for (int k = 1; ok && k <= extra; ++k) {
      const unsigned char next = static_cast<unsigned char>(text[at + k]);
      ok = (next & 0xc0) == 0x80;
      value = value << 6 | (next & 0x3fu);
    }
    out.push_back(ok ? value : c);
    at += ok ? extra + 1 : 1;
  }
  return out;
}

// A character's WinAnsi code, -1 for none.
int WinAnsi(unsigned character) {
  static const std::map<unsigned, int> codes = [] {
    std::map<unsigned, int> by_character;
    const char *const *names = font::Encoding("/WinAnsiEncoding");
    for (int code = 32; code < 256; ++code)
      if (const unsigned c = names[code] ? font::UnicodeOf(names[code]) : 0)
        by_character.emplace(c, code);  // the lower code for a character two have
    by_character.emplace(0xa0, 0x20);   // no-break space
    by_character.emplace(0xad, 0x2d);   // soft hyphen
    by_character.emplace(0x09, 0x20);   // tab
    return by_character;
  }();
  const auto found = codes.find(character);
  return found == codes.end() ? -1 : found->second;
}

// Text as WinAnsi codes, its line breaks as '\n'; false when a character has no code.
bool Encode(const std::string &utf8, std::string &out) {
  const std::vector<unsigned> characters = Characters(utf8);
  for (size_t at = 0; at < characters.size(); ++at) {
    const unsigned c = characters[at];
    if (c == '\r' || c == '\n' || c == 0x2028 || c == 0x2029) {
      if (c == '\r' && at + 1 < characters.size() && characters[at + 1] == '\n')
        ++at;
      out += '\n';
      continue;
    }
    const int code = WinAnsi(c);
    if (code < 0)
      return false;
    out += static_cast<char>(code);
  }
  return true;
}

// What a default appearance string says: the font (its resource name) and size, the text's
// colour and, for free text, the border's.
struct Look {
  std::string font;
  double size = 0;
  std::string fill = "0 g", stroke;
};

Look ReadLook(const std::string &da) {
  Look look;
  std::vector<content::Token> operands;
  size_t pos = 0;
  content::Token token;
  while (content::NextToken(da, pos, token)) {
    if (token.kind != content::Token::Operator) {
      operands.push_back(token);
      continue;
    }
    const std::string &op = token.text;
    if (op == "Tf" && operands.size() >= 2 && operands[operands.size() - 2].kind == content::Token::Name && operands.back().kind == content::Token::Number) {
      look.font = operands[operands.size() - 2].text;
      look.size = std::max(0.0, operands.back().number);
    } else if (op == "g" || op == "rg" || op == "k" || op == "G" || op == "RG" || op == "K") {
      const size_t count = op == "g" || op == "G" ? 1 : op == "k" || op == "K" ? 4 : 3;
      std::vector<double> values;
      for (size_t k = operands.size() >= count ? operands.size() - count : operands.size(); k < operands.size(); ++k)
        if (operands[k].kind == content::Token::Number)
          values.push_back(std::clamp(operands[k].number, 0.0, 1.0));
      if (values.size() == count) {
        const bool stroke = op[0] == 'G' || op[0] == 'R' || op[0] == 'K';
        (stroke ? look.stroke : look.fill) = Colour(NumberArray(values), stroke);
      }
    }
    operands.clear();
  }
  return look;
}

// A fill colour operator as the stroke one.
std::string StrokeOf(const std::string &fill) {
  const size_t space = fill.rfind(' ');
  if (space == std::string::npos)
    return "";
  std::string op = fill.substr(space + 1);
  for (char &c : op)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return fill.substr(0, space + 1) + op;
}

// Text in one font at one size.
struct Pen {
  const TextFont &font;
  double size;

  double Code(char code) const {
    const double width = font.width(static_cast<unsigned char>(code));
    return std::max(0.0, width) * size / 1000;
  }
  double Width(const std::string &codes) const {
    double total = 0;
    for (const char c : codes)
      total += Code(c);
    return total;
  }
  // Each code drawn by the font (it has a glyph for it).
  bool Covers(const std::string &codes) const {
    for (const char c : codes)
      if (c != '\n' && font.width(static_cast<unsigned char>(c)) < 0)
        return false;
    return true;
  }
  // Lines no wider than |width|: broken at spaces, or inside a word wider than a line; at most
  // |most| of them.
  std::vector<std::string> Wrap(const std::string &text, double width, size_t most) const {
    std::vector<std::string> lines;
    size_t start = 0;
    while (lines.size() < most) {
      const size_t end = std::min(text.find('\n', start), text.size());
      std::string line;
      bool started = false;
      size_t from = start;
      while (from <= end && lines.size() < most) {
        const size_t space = std::min(text.find(' ', from), end);
        std::string word = text.substr(from, space - from);
        from = space + 1;
        if (started && Width(line) + Code(' ') + Width(word) <= width) {
          line += " " + word;
          continue;
        }
        if (started)
          lines.push_back(line);
        while (word.size() > 1 && Width(word) > width && lines.size() < most) {
          size_t fit = 1;
          for (double used = Code(word[0]); fit < word.size() && used + Code(word[fit]) <= width; ++fit)
            used += Code(word[fit]);
          lines.push_back(word.substr(0, fit));
          word.erase(0, fit);
        }
        line = word;
        started = true;
      }
      if (lines.size() < most)
        lines.push_back(line);
      if (end >= text.size())
        break;
      start = end + 1;
    }
    return lines;
  }
};

std::string Hex(const std::string &codes) {
  static const char digits[] = "0123456789ABCDEF";
  std::string out = "<";
  for (const char c : codes) {
    out += digits[static_cast<unsigned char>(c) >> 4];
    out += digits[static_cast<unsigned char>(c) & 15];
  }
  return out + ">";
}

// A text appearance's form: |w| by |h|, turned |rotation| degrees (a widget's MK /R) to fit
// its rectangle.
QPDFObjectHandle TextForm(QPDF &q, const std::string &content, double w, double h, int rotation, QPDFObjectHandle font) {
  QPDFObjectHandle resources = QPDFObjectHandle::newNull();
  if (font.isDictionary()) {
    QPDFObjectHandle fonts = QPDFObjectHandle::newDictionary();
    fonts.replaceKey("/F0", font);
    resources = QPDFObjectHandle::newDictionary();
    resources.replaceKey("/Font", fonts);
  }
  QPDFObjectHandle form = Form(q, content, Box{0, 0, w, h}, resources);
  if (rotation == 90)
    form.getDict().replaceKey("/Matrix", NumberArray({0, 1, -1, 0, 0, 0}));
  else if (rotation == 180)
    form.getDict().replaceKey("/Matrix", NumberArray({-1, 0, 0, -1, 0, 0}));
  else if (rotation == 270)
    form.getDict().replaceKey("/Matrix", NumberArray({0, -1, 1, 0, 0, 0}));
  return form;
}

// Lines of text in a box, from its top: |x| the box's left, |width| its width, |top| its top
// edge; each line placed by |align| (0 left, 1 centre, 2 right).
std::string Lines(const Pen &pen, const std::vector<std::string> &lines, double x, double width, double top, int align, double line_height) {
  std::string out;
  double y = top - 0.9 * pen.size;  // the first baseline, an ascender below the top
  for (const std::string &line : lines) {
    const double free = width - pen.Width(line);
    const double at = x + (align == 1 ? free / 2 : align == 2 ? free : 0);
    out += "1 0 0 1 " + Num(at) + " " + Num(y) + " Tm\n" + Hex(line) + " Tj\n";
    y -= line_height;
  }
  return out;
}

// A widget's background and border (MK /BG and /BC, the border's width and style) in a box
// |w| by |h|; |round| for a radio button's. |inset|: how far in its content starts.
std::string Frame(QPDFObjectHandle widget, double w, double h, bool round, double &inset) {
  QPDFObjectHandle mk = widget.getKey("/MK");
  const std::string background = mk.isDictionary() ? Colour(mk.getKey("/BG"), false) : "";
  const std::string border = mk.isDictionary() ? Colour(mk.getKey("/BC"), true) : "";
  const double width = border.empty() ? 0 : BorderWidth(widget);
  QPDFObjectHandle style_dict = widget.getKey("/BS");
  const std::string style = style_dict.isDictionary() && style_dict.getKey("/S").isName() ? style_dict.getKey("/S").getName() : "/S";
  std::string out;
  if (!background.empty())
    out += background + "\n" + (round ? Ellipse({0, 0, w, h}) : "0 0 " + Num(w) + " " + Num(h) + " re\n") + "f\n";
  if (width > 0) {
    out += border + "\n" + Num(width) + " w\n";
    if (style == "/U") {
      out += Point(0, width / 2, "m") + Point(w, width / 2, "l") + "S\n";
    } else {
      out += Dash(widget);
      const Box inner{width / 2, width / 2, w - width / 2, h - width / 2};
      out += round ? Ellipse(inner) : Num(inner.x1) + " " + Num(inner.y1) + " " + Num(inner.x2 - inner.x1) + " " + Num(inner.y2 - inner.y1) + " re\n";
      out += "S\n";
    }
  }
  inset = style == "/B" || style == "/I" ? 2 * width : width;
  return out;
}

// A check box's or radio button's mark (its ZapfDingbats character: check, circle, cross,
// diamond, square or star), drawn as a shape in a box |w| by |h|.
std::string Mark(char character, double w, double h, double inset, const std::string &colour) {
  const double s = std::max(0.0, std::min(w, h) - 2 * inset), cx = w / 2, cy = h / 2;
  std::string out = colour + "\n" + StrokeOf(colour) + "\n";
  switch (character) {
  case 'l':
    return out + Ellipse({cx - 0.25 * s, cy - 0.25 * s, cx + 0.25 * s, cy + 0.25 * s}) + "f\n";
  case '8':
    return out + Num(0.1 * s) + " w\n1 J\n" + Point(cx - 0.28 * s, cy - 0.28 * s, "m") + Point(cx + 0.28 * s, cy + 0.28 * s, "l") +
           Point(cx - 0.28 * s, cy + 0.28 * s, "m") + Point(cx + 0.28 * s, cy - 0.28 * s, "l") + "S\n";
  case 'u':
    return out + Point(cx, cy + 0.3 * s, "m") + Point(cx + 0.3 * s, cy, "l") + Point(cx, cy - 0.3 * s, "l") + Point(cx - 0.3 * s, cy, "l") + "h\nf\n";
  case 'n':
    return out + Num(cx - 0.25 * s) + " " + Num(cy - 0.25 * s) + " " + Num(0.5 * s) + " " + Num(0.5 * s) + " re\nf\n";
  case 'H': {
    for (int k = 0; k < 10; ++k) {
      const double angle = 3.14159265358979 / 2 + k * 3.14159265358979 / 5, r = (k % 2 ? 0.13 : 0.32) * s;
      out += Point(cx + r * std::cos(angle), cy + r * std::sin(angle), k == 0 ? "m" : "l");
    }
    return out + "h\nf\n";
  }
  default:
    return out + Num(0.12 * s) + " w\n1 J\n1 j\n" + Point(cx - 0.3 * s, cy, "m") + Point(cx - 0.08 * s, cy - 0.25 * s, "l") +
           Point(cx + 0.32 * s, cy + 0.28 * s, "l") + "S\n";
  }
}

// The base font a default appearance's font resource is (the form's resources say), or the
// resource's own name.
std::string BaseFontOf(const std::string &resource, QPDFObjectHandle form) {
  QPDFObjectHandle resources = form.isDictionary() ? form.getKey("/DR") : QPDFObjectHandle::newNull();
  QPDFObjectHandle fonts = resources.isDictionary() ? resources.getKey("/Font") : QPDFObjectHandle::newNull();
  QPDFObjectHandle font = fonts.isDictionary() && !resource.empty() ? fonts.getKey("/" + resource) : QPDFObjectHandle::newNull();
  if (font.isDictionary() && font.getKey("/BaseFont").isName())
    return font.getKey("/BaseFont").getName().substr(1);
  return resource == "Helv" || resource.empty() ? "Helvetica" : resource == "HeBo" ? "Helvetica-Bold" : resource == "Cour" ? "Courier" : resource == "TiRo" ? "Times-Roman" : resource;
}

// A form field's appearance: its frame, then its value. A check box's or radio button's is a
// dictionary of its states' (its /AS made one of them). Null when its text can't be drawn.
QPDFObjectHandle FieldAppearance(QPDF &q, QPDFObjectHandle widget, QPDFObjectHandle form, const FontFor &font_for) {
  const Box box = Normalized(Numbers(widget.getKey("/Rect")));
  QPDFObjectHandle mk = widget.getKey("/MK");
  const int rotation = ((Integer(mk, "/R", 0) % 360) + 360) % 360;
  const bool turned = rotation == 90 || rotation == 270;
  const double w = turned ? box.y2 - box.y1 : box.x2 - box.x1, h = turned ? box.x2 - box.x1 : box.y2 - box.y1;
  QPDFObjectHandle type_object = Inherited(widget, "/FT");
  const std::string type = type_object.isName() ? type_object.getName() : "";
  QPDFObjectHandle flags_object = Inherited(widget, "/Ff");
  const long long flags = flags_object.isInteger() ? flags_object.getIntValue() : 0;
  QPDFObjectHandle da = Inherited(widget, "/DA");
  if (!da.isString() && form.isDictionary())
    da = form.getKey("/DA");
  const Look look = ReadLook(TextOf(da));
  const bool push = type == "/Btn" && (flags & (1 << 16)), radio = type == "/Btn" && !push && (flags & (1 << 15));
  double inset = 0;
  const std::string frame = Frame(widget, w, h, radio, inset);

  if (type == "/Btn" && !push) {
    // Its states: off, and on under the name its state or value has.
    std::string state = widget.getKey("/AS").isName() ? widget.getKey("/AS").getName() : "";
    QPDFObjectHandle value = Inherited(widget, "/V");
    std::string on = state != "/Off" ? state : "";
    if (on.empty() && !radio && value.isName() && value.getName() != "/Off")
      on = value.getName();
    QPDFObjectHandle parent = widget.getKey("/Parent");
    if (on.empty() && radio && parent.isDictionary() && Inherited(widget, "/Opt").isArray()) {
      // Radio buttons with options are named by their place among the field's widgets.
      QPDFObjectHandle kids = parent.getKey("/Kids");
      for (int index = 0; kids.isArray() && index < kids.getArrayNItems(); ++index)
        if (kids.getArrayItem(index).isIndirect() && widget.isIndirect() && kids.getArrayItem(index).getObjGen() == widget.getObjGen())
          on = "/" + std::to_string(index);
    }
    if (on.empty() && !radio)
      on = "/Yes";
    if (state.empty()) {
      state = !radio && value.isName() && value.getName() == on ? on : "/Off";
      widget.replaceKey("/AS", QPDFObjectHandle::newName(state));
    }
    const std::string caption = mk.isDictionary() ? TextOf(mk.getKey("/CA")) : "";
    const char character = caption.size() == 1 ? caption[0] : radio ? 'l' : '4';
    QPDFObjectHandle states = QPDFObjectHandle::newDictionary();
    states.replaceKey("/Off", TextForm(q, frame, w, h, rotation, QPDFObjectHandle::newNull()));
    if (!on.empty())
      states.replaceKey(on, TextForm(q, frame + "q\n" + Mark(character, w, h, inset, look.fill) + "Q\n", w, h, rotation, QPDFObjectHandle::newNull()));
    return states;
  }

  // What it shows.
  QPDFObjectHandle quadding = Inherited(widget, "/Q");
  const int align = quadding.isInteger() ? quadding.getIntValueAsInt() : Integer(form, "/Q", 0);
  QPDFObjectHandle value = Inherited(widget, "/V");
  QPDFObjectHandle options = Inherited(widget, "/Opt");
  const bool list = type == "/Ch" && !(flags & (1 << 17));
  std::string text;
  if (type == "/Tx") {
    text = TextOf(value);
    if (flags & (1 << 13))
      text = std::string(Characters(text).size(), '*');
  } else if (type == "/Ch" && !list) {
    text = value.isArray() && value.getArrayNItems() > 0 ? TextOf(value.getArrayItem(0)) : TextOf(value);
    for (int index = 0; options.isArray() && index < options.getArrayNItems(); ++index) {
      QPDFObjectHandle option = options.getArrayItem(index);
      if (option.isArray() && option.getArrayNItems() >= 2 && TextOf(option.getArrayItem(0)) == text)
        text = TextOf(option.getArrayItem(1));
    }
  } else if (push) {
    text = mk.isDictionary() ? TextOf(mk.getKey("/CA")) : "";
  }
  if (text.empty() && !(list && options.isArray() && options.getArrayNItems() > 0))
    return TextForm(q, frame, w, h, rotation, QPDFObjectHandle::newNull());

  TextFont font = font_for(BaseFontOf(look.font, form));
  if (!font.font.isDictionary() || !font.width)
    return QPDFObjectHandle::newNull();
  const double pad = inset + 2;
  const bool multiline = type == "/Tx" && (flags & (1 << 12));
  std::string body = "/Tx BMC\nq\n" + Num(inset) + " " + Num(inset) + " " + Num(std::max(0.0, w - 2 * inset)) + " " + Num(std::max(0.0, h - 2 * inset)) + " re\nW\nn\n";
  std::string lines_content;
  double size = look.size;

  if (list) {
    // Its options from the top one shown, the chosen ones on a highlight.
    if (size <= 0)
      size = 12;
    const Pen pen{font, size};
    const double line_height = size * 1.15;
    std::set<std::string> chosen;
    if (value.isArray())
      for (int index = 0; index < value.getArrayNItems(); ++index)
        chosen.insert(TextOf(value.getArrayItem(index)));
    else if (!value.isNull())
      chosen.insert(TextOf(value));
    std::set<int> indices;
    QPDFObjectHandle selected = widget.getKey("/I");
    for (int index = 0; selected.isArray() && index < selected.getArrayNItems(); ++index)
      if (selected.getArrayItem(index).isInteger())
        indices.insert(selected.getArrayItem(index).getIntValueAsInt());
    const int top = std::max(0, Integer(widget, "/TI", 0));
    double y = h - inset;
    for (int index = top; index < options.getArrayNItems() && y > inset; ++index) {
      QPDFObjectHandle option = options.getArrayItem(index);
      const std::string exported = option.isArray() && option.getArrayNItems() >= 2 ? TextOf(option.getArrayItem(0)) : TextOf(option);
      const std::string shown = option.isArray() && option.getArrayNItems() >= 2 ? TextOf(option.getArrayItem(1)) : exported;
      std::string codes;
      if (!Encode(shown, codes) || !pen.Covers(codes))
        return QPDFObjectHandle::newNull();
      if (chosen.count(exported) || indices.count(index))
        lines_content += "0.6 0.757 0.855 rg\n" + Num(inset) + " " + Num(y - line_height) + " " + Num(w - 2 * inset) + " " + Num(line_height) + " re\nf\n";
      lines_content += "BT\n/F0 " + Num(size) + " Tf\n" + look.fill + "\n" + Lines(pen, {codes}, pad, w - 2 * pad, y - line_height + (line_height - 0.72 * size) / 2 + 0.9 * size, 0, line_height) + "ET\n";
      y -= line_height;
    }
    return TextForm(q, frame + body + lines_content + "Q\nEMC\n", w, h, rotation, font.font);
  }

  std::string codes;
  if (!Encode(text, codes))
    return QPDFObjectHandle::newNull();
  if (!multiline)
    std::replace(codes.begin(), codes.end(), '\n', ' ');
  if (!Pen{font, 1000}.Covers(codes))
    return QPDFObjectHandle::newNull();
  if (multiline) {
    if (size <= 0)
      size = 12;
    const Pen pen{font, size};
    const double line_height = size * 1.15;
    const auto lines = pen.Wrap(codes, w - 2 * pad, static_cast<size_t>(std::max(1.0, h / line_height)) + 1);
    lines_content = Lines(pen, lines, pad, w - 2 * pad, h - pad, align, line_height);
  } else {
    // One line, centred on its height; at a size that fits when it says none.
    const double room = std::max(1.0, w - 2 * pad), natural = Pen{font, 1}.Width(codes);
    if (size <= 0)
      size = std::clamp(std::min((h - 2 * inset) / 1.35, natural > 0 ? room / natural : 1e9), 4.0, 72.0);
    const Pen pen{font, size};
    const double baseline = std::max(inset + 1, (h - 0.72 * size) / 2);
    QPDFObjectHandle max_length = Inherited(widget, "/MaxLen");
    const int cells = max_length.isInteger() ? max_length.getIntValueAsInt() : 0;
    if (type == "/Tx" && (flags & (1 << 24)) && cells > 0 && !(flags & ((1 << 12) | (1 << 13) | (1 << 20)))) {
      // Comb: each character centred in its cell.
      const double cell = (w - 2 * inset) / cells;
      for (size_t at = 0; at < codes.size() && at < static_cast<size_t>(cells); ++at)
        lines_content += "1 0 0 1 " + Num(inset + cell * at + (cell - pen.Code(codes[at])) / 2) + " " + Num(baseline) + " Tm\n" + Hex(codes.substr(at, 1)) + " Tj\n";
    } else {
      const double free = room - pen.Width(codes);
      const double x = pad + (push || align == 1 ? free / 2 : align == 2 ? free : 0);
      lines_content = "1 0 0 1 " + Num(x) + " " + Num(baseline) + " Tm\n" + Hex(codes) + " Tj\n";
    }
  }
  body += "BT\n/F0 " + Num(size) + " Tf\n" + look.fill + "\n" + lines_content + "ET\nQ\nEMC\n";
  return TextForm(q, frame + body, w, h, rotation, font.font);
}

// A free text comment's appearance: its text in its rectangle (less /RD), from the top, on its
// background (/C) inside its border.
QPDFObjectHandle FreeTextAppearance(QPDF &q, QPDFObjectHandle annotation, QPDFObjectHandle form, const FontFor &font_for) {
  const Box box = Normalized(Numbers(annotation.getKey("/Rect")));
  const double w = box.x2 - box.x1, h = box.y2 - box.y1;
  Box inner{0, 0, w, h};
  const std::vector<double> rd = Numbers(annotation.getKey("/RD"));
  if (rd.size() == 4)
    inner = {rd[0], rd[3], w - rd[2], h - rd[1]};
  QPDFObjectHandle da = annotation.getKey("/DA");
  if (!da.isString() && form.isDictionary())
    da = form.getKey("/DA");
  Look look = ReadLook(da.isString() ? TextOf(da) : "0 g /Helv 12 Tf");
  const double size = look.size > 0 ? look.size : 12;
  // A border only when it says one: free text is mostly typed on the page.
  const bool bordered = annotation.getKey("/BS").isDictionary() || annotation.getKey("/Border").isArray();
  const double width = bordered ? BorderWidth(annotation) : 0;
  std::string out;
  const std::string background = Colour(annotation.getKey("/C"), false);
  if (!background.empty())
    out += background + "\n" + Num(inner.x1) + " " + Num(inner.y1) + " " + Num(inner.x2 - inner.x1) + " " + Num(inner.y2 - inner.y1) + " re\nf\n";
  if (width > 0)
    out += (look.stroke.empty() ? StrokeOf(look.fill) : look.stroke) + "\n" + Num(width) + " w\n" + Dash(annotation) + Num(inner.x1 + width / 2) + " " +
           Num(inner.y1 + width / 2) + " " + Num(inner.x2 - inner.x1 - width) + " " + Num(inner.y2 - inner.y1 - width) + " re\nS\n";
  std::string codes;
  const std::string text = TextOf(annotation.getKey("/Contents"));
  if (text.empty())
    return Form(q, out, Box{0, 0, w, h}, QPDFObjectHandle::newNull());
  TextFont font = font_for(BaseFontOf(look.font, form));
  if (!font.font.isDictionary() || !font.width || !Encode(text, codes) || !Pen{font, 1000}.Covers(codes))
    return QPDFObjectHandle::newNull();
  const Pen pen{font, size};
  const double pad = width + 2, line_height = size * 1.15;
  const auto lines = pen.Wrap(codes, inner.x2 - inner.x1 - 2 * pad, static_cast<size_t>(std::max(1.0, (inner.y2 - inner.y1) / line_height)) + 1);
  out += "q\n" + Num(inner.x1) + " " + Num(inner.y1) + " " + Num(inner.x2 - inner.x1) + " " + Num(inner.y2 - inner.y1) + " re\nW\nn\nBT\n/F0 " + Num(size) + " Tf\n" +
         look.fill + "\n" + Lines(pen, lines, inner.x1 + pad, inner.x2 - inner.x1 - 2 * pad, inner.y2 - pad, Integer(annotation, "/Q", 0), line_height) + "ET\nQ\n";
  return TextForm(q, out, w, h, 0, font.font);
}

// A rubber stamp's appearance, as readers draw the standard ones: its name in capitals in a
// rounded box, in its colour.
QPDFObjectHandle StampAppearance(QPDF &q, QPDFObjectHandle annotation, const FontFor &font_for) {
  const Box box = Normalized(Numbers(annotation.getKey("/Rect")));
  const double w = box.x2 - box.x1, h = box.y2 - box.y1;
  std::string name = annotation.getKey("/Name").isName() ? annotation.getKey("/Name").getName().substr(1) : "Draft";
  std::string label;
  for (size_t at = 0; at < name.size(); ++at) {
    const unsigned char c = static_cast<unsigned char>(name[at]);
    if (at > 0 && std::isupper(c) && std::islower(static_cast<unsigned char>(name[at - 1])))
      label += ' ';
    label += static_cast<char>(std::toupper(c));
  }
  TextFont font = font_for("Helvetica-Bold");
  std::string codes;
  if (!font.font.isDictionary() || !font.width || !Encode(label, codes) || !Pen{font, 1000}.Covers(codes))
    return QPDFObjectHandle::newNull();
  std::string colour = Colour(annotation.getKey("/C"), false);
  if (colour.empty())
    colour = "0.75 0.12 0.12 rg";
  const double line = std::max(1.0, std::min(w, h) / 20), natural = Pen{font, 1}.Width(codes);
  const double size = std::max(1.0, std::min(h * 0.6, natural > 0 ? (w - 6 * line) / natural : h));
  const Pen pen{font, size};
  std::string out = colour + "\n" + StrokeOf(colour) + "\n" + Num(line) + " w\n" + Num(line / 2) + " " + Num(line / 2) + " " + Num(w - line) + " " + Num(h - line) +
                    " re\nS\nBT\n/F0 " + Num(size) + " Tf\n1 0 0 1 " + Num((w - pen.Width(codes)) / 2) + " " + Num((h - 0.72 * size) / 2) + " Tm\n" + Hex(codes) + " Tj\nET\n";
  return TextForm(q, out, w, h, 0, font.font);
}

} // namespace

QPDFObjectHandle EmptyAppearance(QPDF &q) { return Form(q, "", Box{0, 0, 1, 1}, QPDFObjectHandle::newNull()); }

QPDFObjectHandle MakeAppearance(QPDF &q, QPDFObjectHandle annotation, QPDFObjectHandle form, bool transparency, const FontFor &font_for) {
  QPDFObjectHandle subtype_object = annotation.getKey("/Subtype");
  std::vector<double> rect = Numbers(annotation.getKey("/Rect"));
  if (!subtype_object.isName() || rect.size() != 4)
    return QPDFObjectHandle::newNull();
  const std::string subtype = subtype_object.getName();
  if (subtype == "/Widget")
    return FieldAppearance(q, annotation, form, font_for);
  if (subtype == "/FreeText")
    return FreeTextAppearance(q, annotation, form, font_for);
  if (subtype == "/Stamp")
    return StampAppearance(q, annotation, font_for);
  // Readers draw nothing for these without an appearance.
  if (subtype == "/Watermark" || subtype == "/PrinterMark" || subtype == "/TrapNet")
    return EmptyAppearance(q);
  // Marks without a colour: in the one readers use (yellow highlights, black otherwise); an
  // empty colour array is transparent.
  QPDFObjectHandle colour = annotation.getKey("/C");
  const bool marking = subtype == "/Highlight" || subtype == "/Underline" || subtype == "/StrikeOut" || subtype == "/Squiggly" || subtype == "/Ink" || subtype == "/Caret";
  if (marking && colour.isArray() && colour.getArrayNItems() == 0)
    return EmptyAppearance(q);
  if (marking && Numbers(colour).empty())
    colour = NumberArray(subtype == "/Highlight" ? std::vector<double>{1, 1, 0} : std::vector<double>{0});
  const Box box = Normalized(rect);
  const std::string stroke = Colour(colour, true), fill = Colour(annotation.getKey("/IC"), false);
  const std::string stroke_as_fill = Colour(colour, false);
  const double width = BorderWidth(annotation);
  std::string body;
  bool multiply = false;
  Box bbox = box;

  if (subtype == "/Square" || subtype == "/Circle") {
    Box inner = box;
    std::vector<double> rd = Numbers(annotation.getKey("/RD"));
    if (rd.size() == 4)
      inner = {box.x1 + rd[0], box.y1 + rd[3], box.x2 - rd[2], box.y2 - rd[1]};
    if (!stroke.empty())
      inner = {inner.x1 + width / 2, inner.y1 + width / 2, inner.x2 - width / 2, inner.y2 - width / 2};
    const bool stroked = !stroke.empty() && width > 0;
    body = fill + "\n" + (stroked ? stroke + "\n" + Num(width) + " w\n" + Dash(annotation) : "");
    if (subtype == "/Square")
      body += Num(inner.x1) + " " + Num(inner.y1) + " " + Num(inner.x2 - inner.x1) + " " + Num(inner.y2 - inner.y1) + " re\n";
    else
      body += Ellipse(inner);
    body += Paint(!fill.empty(), stroked);
  } else if (subtype == "/Line") {
    std::vector<double> l = Numbers(annotation.getKey("/L"));
    if (l.size() != 4)
      return QPDFObjectHandle::newNull();
    body = stroke + "\n" + fill + "\n" + Num(width) + " w\n" + Dash(annotation) + Point(l[0], l[1], "m") + Point(l[2], l[3], "l") + "S\n";
    QPDFObjectHandle endings = annotation.getKey("/LE");
    for (int end = 0; endings.isArray() && end < 2 && end < endings.getArrayNItems(); ++end) {
      QPDFObjectHandle style = endings.getArrayItem(end);
      const bool open = IsName(style, "/OpenArrow"), closed = IsName(style, "/ClosedArrow");
      if (open || closed)
        body += end == 0 ? Arrow(l[0], l[1], l[2], l[3], width, closed) : Arrow(l[2], l[3], l[0], l[1], width, closed);
    }
  } else if (subtype == "/Polygon" || subtype == "/PolyLine") {
    std::vector<double> v = Numbers(annotation.getKey("/Vertices"));
    if (v.size() < 4)
      return QPDFObjectHandle::newNull();
    const bool polygon = subtype == "/Polygon";
    body = (polygon ? fill + "\n" : "") + stroke + "\n" + Num(width) + " w\n" + Dash(annotation);
    for (size_t at = 0; at + 1 < v.size(); at += 2)
      body += Point(v[at], v[at + 1], at == 0 ? "m" : "l");
    body += polygon ? "h\n" + Paint(!fill.empty(), !stroke.empty()) : "S\n";
  } else if (subtype == "/Ink") {
    QPDFObjectHandle paths = annotation.getKey("/InkList");
    if (!paths.isArray() || stroke.empty())
      return QPDFObjectHandle::newNull();
    body = stroke + "\n" + Num(width) + " w\n1 J\n1 j\n";
    for (int index = 0; index < paths.getArrayNItems(); ++index) {
      std::vector<double> p = Numbers(paths.getArrayItem(index));
      for (size_t at = 0; at + 1 < p.size(); at += 2)
        body += Point(p[at], p[at + 1], at == 0 ? "m" : "l");
      if (p.size() == 2)
        body += Point(p[0], p[1], "l");
    }
    body += "S\n";
  } else if (subtype == "/Highlight" || subtype == "/Underline" || subtype == "/StrikeOut" || subtype == "/Squiggly") {
    std::vector<double> quads = Numbers(annotation.getKey("/QuadPoints"));
    if (quads.size() < 8 || stroke_as_fill.empty() || (subtype == "/Highlight" && !transparency))
      return QPDFObjectHandle::newNull();
    body = stroke_as_fill + "\n" + stroke + "\n";
    for (size_t at = 0; at + 7 < quads.size(); at += 8) {
      // Points: upper left, upper right, lower left, lower right (as Acrobat writes them); the
      // text's height is between the top and bottom edges.
      const double *p = &quads[at];
      const double height = std::hypot(p[0] - p[4], p[1] - p[5]);
      const auto along = [&](double t, const char *op) {
        // A line parallel to the bottom edge, |t| of the height above it.
        const double dx = (p[0] - p[4]) * t, dy = (p[1] - p[5]) * t;
        return Point(p[4] + dx, p[5] + dy, "m") + Point(p[6] + dx, p[7] + dy, op);
      };
      if (subtype == "/Highlight") {
        multiply = true;
        body += Point(p[0], p[1], "m") + Point(p[2], p[3], "l") + Point(p[6], p[7], "l") + Point(p[4], p[5], "l") + "h\nf\n";
      } else if (subtype == "/Underline") {
        body += Num(std::max(0.5, height / 14)) + " w\n" + along(0.08, "l") + "S\n";
      } else if (subtype == "/StrikeOut") {
        body += Num(std::max(0.5, height / 14)) + " w\n" + along(0.45, "l") + "S\n";
      } else {
        // A zigzag along the bottom.
        const double length = std::hypot(p[6] - p[4], p[7] - p[5]);
        const double step = std::max(1.5, height / 8);
        const int teeth = std::max(1, static_cast<int>(length / step));
        const double ux = (p[6] - p[4]) / teeth, uy = (p[7] - p[5]) / teeth;
        const double vx = (p[0] - p[4]) * 0.08, vy = (p[1] - p[5]) * 0.08;
        body += Num(std::max(0.5, height / 18)) + " w\n";
        for (int tooth = 0; tooth <= teeth; ++tooth) {
          const double up = tooth % 2 ? 1 : 0;
          body += Point(p[4] + ux * tooth + vx * up, p[5] + uy * tooth + vy * up, tooth == 0 ? "m" : "l");
        }
        body += "S\n";
      }
    }
  } else if (subtype == "/Text" || subtype == "/FileAttachment") {
    // An icon in a 20 by 20 box, scaled to the rectangle: a note, or a page with a folded corner.
    bbox = {0, 0, 20, 20};
    const std::string colour = stroke_as_fill.empty() ? (subtype == "/Text" ? "1 0.82 0.2 rg" : "0.85 g") : stroke_as_fill;
    body = colour + "\n0 G\n0.8 w\n";
    if (subtype == "/Text")
      body += "1 4 m\n1 19 l\n19 19 l\n19 4 l\n8 4 l\n4 0.8 l\n5 4 l\nh\nB\n0.6 w\n4 15 m\n16 15 l\n4 11.5 m\n16 11.5 l\n4 8 m\n12 8 l\nS\n";
    else
      body += "4 1 m\n4 19 l\n12 19 l\n16 15 l\n16 1 l\nh\nB\n12 19 m\n12 15 l\n16 15 l\nS\n";
  } else if (subtype == "/Caret") {
    if (stroke_as_fill.empty())
      return QPDFObjectHandle::newNull();
    body = stroke_as_fill + "\n" + Point(box.x1, box.y1, "m") + Point((box.x1 + box.x2) / 2, box.y2, "l") + Point(box.x2, box.y1, "l") + "h\nf\n";
  } else if (subtype == "/Redact") {
    // Before it's applied, a redaction is shown as its outline.
    const std::string outline = stroke.empty() ? "1 0 0 RG" : stroke;
    body = outline + "\n1 w\n" + Num(box.x1 + 0.5) + " " + Num(box.y1 + 0.5) + " " + Num(box.x2 - box.x1 - 1) + " " + Num(box.y2 - box.y1 - 1) + " re\nS\n";
  } else {
    return QPDFObjectHandle::newNull();
  }

  QPDFObjectHandle resources = QPDFObjectHandle::newNull();
  QPDFObjectHandle opacity = annotation.getKey("/CA");
  const double alpha = opacity.isNumber() ? std::clamp(opacity.getNumericValue(), 0.0, 1.0) : 1;
  if (transparency && (alpha < 1 || multiply)) {
    QPDFObjectHandle state = QPDFObjectHandle::newDictionary();
    if (alpha < 1) {
      state.replaceKey("/CA", QPDFObjectHandle::newReal(alpha, 4, true));
      state.replaceKey("/ca", QPDFObjectHandle::newReal(alpha, 4, true));
    }
    if (multiply)
      state.replaceKey("/BM", QPDFObjectHandle::newName("/Multiply"));
    QPDFObjectHandle states = QPDFObjectHandle::newDictionary();
    states.replaceKey("/GS0", state);
    resources = QPDFObjectHandle::newDictionary();
    resources.replaceKey("/ExtGState", states);
    body = "/GS0 gs\n" + body;
  }
  return Form(q, "q\n" + body + "Q\n", bbox, resources);
}

} // namespace fynepdf::pdfa
