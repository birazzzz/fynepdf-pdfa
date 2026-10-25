#include "font_embed.h"

#include "common.h"
#include "font_fix.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace fynepdf::pdfa::font {
namespace {

int S16(const std::string &d, size_t at) {
  return at + 2 <= d.size() ? static_cast<int16_t>(static_cast<unsigned char>(d[at]) << 8 | static_cast<unsigned char>(d[at + 1])) : 0;
}

int32_t S32(const std::string &d, size_t at) {
  if (at + 4 > d.size())
    return 0;
  uint32_t value = 0;
  for (size_t k = 0; k < 4; ++k)
    value = value << 8 | static_cast<unsigned char>(d[at + k]);
  return static_cast<int32_t>(value);
}

struct Family {
  const char *prefix, *file;
  std::set<std::string> rest;  // what may follow the family's name (styles, vendor suffixes)
};

// A substitute's glyph for a character. Symbol's pieces of tall brackets and the like have private
// codes in the Adobe Glyph List (parenlefttp U+F8EB, ...): their Unicode equivalents found instead;
// wide spaces (ideographic, no-break, en, em, ...) a fallback font may lack: its space.
int Glyph(const Program &program, unsigned character) {
  const int glyph = program.ByCmap(3, 1, character);
  if (glyph > 0)
    return glyph;
  static const std::map<unsigned, unsigned> equivalents = {
      {0xf8e5, 0x203e}, {0xf8e6, 0x23d0}, {0xf8e7, 0x23af}, {0xf8e8, 0x00ae}, {0xf8e9, 0x00a9}, {0xf8ea, 0x2122}, {0xf8eb, 0x239b},
      {0xf8ec, 0x239c}, {0xf8ed, 0x239d}, {0xf8ee, 0x23a1}, {0xf8ef, 0x23a2}, {0xf8f0, 0x23a3}, {0xf8f1, 0x23a7}, {0xf8f2, 0x23a8},
      {0xf8f3, 0x23a9}, {0xf8f4, 0x23aa}, {0xf8f5, 0x23ae}, {0xf8f6, 0x239e}, {0xf8f7, 0x239f}, {0xf8f8, 0x23a0}, {0xf8f9, 0x23a4},
      {0xf8fa, 0x23a5}, {0xf8fb, 0x23a6}, {0xf8fc, 0x23ab}, {0xf8fd, 0x23ac}, {0xf8fe, 0x23ad}, {0xf6d9, 0x00ae}, {0xf6da, 0x00a9},
      {0xf6db, 0x2122}, {0x3000, 0x0020}, {0x00a0, 0x0020}, {0x2000, 0x0020}, {0x2001, 0x0020}, {0x2002, 0x0020}, {0x2003, 0x0020},
      {0x2004, 0x0020}, {0x2005, 0x0020}, {0x2006, 0x0020}, {0x2007, 0x0020}, {0x2008, 0x0020}, {0x2009, 0x0020}, {0x200a, 0x0020}};
  const auto found = equivalents.find(character);
  return found == equivalents.end() ? glyph : program.ByCmap(3, 1, found->second);
}

std::string Simplified(const std::string &base_font) {
  std::string name = base_font;
  if (!name.empty() && name[0] == '/')
    name = name.substr(1);
  if (name.size() > 7 && name[6] == '+')
    name = name.substr(7);
  std::string simple;
  for (const char c : name)
    if (std::isalnum(static_cast<unsigned char>(c)))
      simple += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return simple;
}

bool Contains(const std::string &text, std::initializer_list<const char *> words) {
  for (const char *word : words)
    if (text.find(word) != std::string::npos)
      return true;
  return false;
}

const std::vector<Family> &Families() {
  static const std::set<std::string> office = {"",       "mt",       "ps",     "psmt",   "regular", "bold",           "boldmt",
                                               "psbold", "psboldmt", "italic", "italicmt", "psitalic", "psitalicmt", "bolditalic",
                                               "bolditalicmt", "psbolditalic", "psbolditalicmt"};
  static const std::set<std::string> postscript = {"", "roman", "bold", "italic", "oblique", "bolditalic", "boldoblique"};
  // Longer names first: Times New Roman before Times, Courier New before Courier.
  static const std::vector<Family> families = {{"arial", "Arimo", office},       {"helvetica", "Arimo", postscript},
                                               {"timesnewroman", "Tinos", office}, {"times", "Tinos", postscript},
                                               {"couriernew", "Cousine", office},  {"courier", "Cousine", postscript},
                                               {"calibri", "Carlito", office},     {"cambria", "Caladea", office}};
  return families;
}

} // namespace

std::string SubstituteFile(const std::string &base_font, QPDFObjectHandle descriptor) {
  const std::string simple = Simplified(base_font);
  for (const Family &family : Families()) {
    if (simple.compare(0, std::char_traits<char>::length(family.prefix), family.prefix) != 0)
      continue;
    const std::string rest = simple.substr(std::char_traits<char>::length(family.prefix));
    if (!family.rest.count(rest))
      continue;
    bool bold = rest.find("bold") != std::string::npos;
    bool italic = rest.find("italic") != std::string::npos || rest.find("oblique") != std::string::npos;
    // A name without its style ("Arial" for a bold face, with ",Bold" left out): the flags say.
    if (!bold && !italic && descriptor.isDictionary()) {
      const int flags = Integer(descriptor, "/Flags", 0);
      bold = (flags & (1 << 18)) != 0 || Integer(descriptor, "/FontWeight", 400) >= 600;
      QPDFObjectHandle angle = descriptor.getKey("/ItalicAngle");
      italic = (flags & 64) != 0 || (angle.isNumber() && angle.getNumericValue() != 0);
    }
    return std::string(family.file) + "-" + (bold && italic ? "BoldItalic" : bold ? "Bold" : italic ? "Italic" : "Regular") + ".ttf";
  }
  return "";
}

namespace {

// What choosing a substitute needs of each code the pages use: the character it stands for (0
// for none), the width the document gives it (0 for none) and whether it's drawn.
struct Wanted {
  unsigned character = 0;
  double width = 0;
  bool drawn = false;
};

std::string Choose(const std::string &base, QPDFObjectHandle descriptor, const std::vector<Wanted> &codes, unsigned *lacking,
                   const std::function<const Program *(const std::string &)> &read, bool &twin) {
  std::string file = SubstituteFile(base, descriptor);
  twin = !file.empty();
  if (twin)
    return file;
  // The style, by name and descriptor.
  const std::string simple = Simplified(base);
  const int flags = Integer(descriptor, "/Flags", 0);
  QPDFObjectHandle angle = descriptor.isDictionary() ? descriptor.getKey("/ItalicAngle") : QPDFObjectHandle::newNull();
  const bool bold = Contains(simple, {"bold", "black", "heavy", "semibold", "demi"}) || (flags & (1 << 18)) || Integer(descriptor, "/FontWeight", 400) >= 600;
  const bool italic = Contains(simple, {"italic", "oblique", "slant", "kursiv"}) || (flags & 64) || (angle.isNumber() && angle.getNumericValue() != 0);
  const bool symbols = simple == "symbol" || simple.find("dingbats") != std::string::npos;
  const bool fixed = (flags & 1) || Contains(simple, {"mono", "courier", "consol", "typewriter", "code"});
  const bool serif = !Contains(simple, {"sans", "gothic", "grotesk"}) &&
                     ((flags & 2) || Contains(simple, {"times", "roman", "serif", "georgia", "garamond", "century", "book", "palatino", "minion",
                                                       "cambria", "bodoni", "baskerville", "caslon", "schoolbook", "antiqua", "didot", "goudy",
                                                       "rockwell", "calisto", "perpetua", "constantia", "cheltenham", "mincho", "song", "ming"}));
  const char *const regular_styles[4] = {"-Regular", "-Bold", "-Italic", "-BoldItalic"};
  const char *const oblique_styles[4] = {"", "-Bold", "-Oblique", "-BoldOblique"};
  const int style = (bold ? 1 : 0) + (italic ? 2 : 0);
  std::vector<std::string> candidates;
  const auto add = [&](const char *family, bool oblique) { candidates.push_back(std::string(family) + (oblique ? oblique_styles : regular_styles)[style] + ".ttf"); };
  if (symbols) {
    add("DejaVuSans", true);
  } else if (fixed) {
    add("Cousine", false);
  } else if (serif) {
    add("Tinos", false);
    add("Caladea", false);
    add("NotoSerif", false);
  } else {
    add("Arimo", false);
    add("Carlito", false);
    add("NotoSans", false);
    add("DejaVuSans", true);
  }
  // Last, for Chinese, Japanese and Korean (and anything else the others lack).
  candidates.push_back("DroidSansFallbackFull.ttf");
  // The one whose widths come closest to the document's for the codes it uses; every drawn
  // code must have a glyph.
  double best = -1;
  file.clear();
  for (const std::string &candidate : candidates) {
    // The CJK font (4 MB to read) only when none of the others fits.
    if (candidate == "DroidSansFallbackFull.ttf" && !file.empty())
      break;
    const Program *program = read(candidate);
    if (!program || !program->HasCmap(3, 1))
      continue;
    double error = 0;
    int counted = 0;
    bool fits = true;
    for (const Wanted &code : codes) {
      const int glyph = code.character ? Glyph(*program, code.character) : 0;
      if (glyph <= 0) {
        // The first drawn character the first candidate (the style's own) lacks, to say why.
        if (fits && code.drawn && lacking && !*lacking)
          *lacking = code.character;
        fits = fits && !code.drawn;
        continue;
      }
      if (code.width > 0) {
        error += std::fabs(program->Width(glyph) - code.width) / code.width;
        ++counted;
      }
    }
    if (!fits)
      continue;
    const double score = counted ? error / counted : 0;
    if (best < 0 || score < best) {
      best = score;
      file = candidate;
    }
  }
  return file;
}

// A font descriptor for an embedded substitute, from its own tables; |old| gives the serif flag
// and stem width the document said.
// A TrueType program as a font file stream.
QPDFObjectHandle FontFile(QPDF &q, const std::string &program) {
  QPDFObjectHandle file = QPDFObjectHandle::newStream(&q, program);
  file.getDict().replaceKey("/Length1", QPDFObjectHandle::newInteger(static_cast<long long>(program.size())));
  return file;
}

QPDFObjectHandle Descriptor(QPDF &q, const Program &program, QPDFObjectHandle name, QPDFObjectHandle old, bool symbolic, QPDFObjectHandle file) {
  const std::string head = program.Table("head"), hhea = program.Table("hhea"), os2 = program.Table("OS/2"), post = program.Table("post");
  const double scale = 1000.0 / program.UnitsPerEm();
  const double ascent = S16(hhea, 4) * scale, descent = S16(hhea, 6) * scale;
  const double cap = os2.size() >= 90 && S16(os2, 0) >= 2 ? S16(os2, 88) * scale : ascent * 0.7;
  const double angle = S32(post, 4) / 65536.0;
  const bool fixed = S32(post, 12) != 0;
  QPDFObjectHandle descriptor = QPDFObjectHandle::newDictionary();
  descriptor.replaceKey("/Type", QPDFObjectHandle::newName("/FontDescriptor"));
  descriptor.replaceKey("/FontName", name.isName() ? name : QPDFObjectHandle::newName("/Substitute"));
  descriptor.replaceKey("/Flags", QPDFObjectHandle::newInteger((symbolic ? 4 : 32) | (fixed ? 1 : 0) | (angle != 0 ? 64 : 0) | (Integer(old, "/Flags", 0) & 2)));
  descriptor.replaceKey("/FontBBox", NumberArray({S16(head, 36) * scale, S16(head, 38) * scale, S16(head, 40) * scale, S16(head, 42) * scale}));
  descriptor.replaceKey("/ItalicAngle", QPDFObjectHandle::newReal(angle, 2, true));
  descriptor.replaceKey("/Ascent", QPDFObjectHandle::newInteger(std::lround(ascent)));
  descriptor.replaceKey("/Descent", QPDFObjectHandle::newInteger(std::lround(descent)));
  descriptor.replaceKey("/CapHeight", QPDFObjectHandle::newInteger(std::lround(cap)));
  descriptor.replaceKey("/StemV", QPDFObjectHandle::newInteger(old.isDictionary() && old.getKey("/StemV").isNumber() ? std::lround(old.getKey("/StemV").getNumericValue()) : 80));
  if (file.isStream())
    descriptor.replaceKey("/FontFile2", file);
  return q.makeIndirectObject(descriptor);
}

QPDFObjectHandle DescendantOf(QPDFObjectHandle font) {
  QPDFObjectHandle descendants = font.getKey("/DescendantFonts");
  return descendants.isArray() && descendants.getArrayNItems() > 0 ? descendants.getArrayItem(0) : QPDFObjectHandle::newNull();
}

} // namespace

std::string ChooseSubstitute(QPDFObjectHandle font, const Checker &checker, const std::function<const Program *(const std::string &)> &read, bool &twin,
                             unsigned *lacking) {
  const std::string base = font.getKey("/BaseFont").isName() ? font.getKey("/BaseFont").getName() : "";
  const std::string standard = base == "/Symbol" ? "Symbol" : base == "/ZapfDingbats" ? "ZapfDingbats" : "";
  const int first = Integer(font, "/FirstChar", -1);
  QPDFObjectHandle widths = font.getKey("/Widths");
  std::vector<Wanted> codes;
  for (const auto &[code, drawn] : checker.Codes()) {
    const std::string &name = checker.NameFor(code);
    if (name.empty() || name == ".notdef")
      continue;
    const long index = static_cast<long>(code) - first;
    const double width = first >= 0 && widths.isArray() && index >= 0 && index < widths.getArrayNItems() && widths.getArrayItem(static_cast<int>(index)).isNumber()
                             ? widths.getArrayItem(static_cast<int>(index)).getNumericValue()
                             : StandardWidth(standard, name);
    codes.push_back({UnicodeOf(name), width, drawn});
  }
  return Choose(base, font.getKey("/FontDescriptor"), codes, lacking, read, twin);
}

bool DescribeFont(QPDF &q, QPDFObjectHandle font, const std::string &data, const Checker &checker) {
  const QPDFObjectHandle subtype = font.getKey("/Subtype");
  const int first = Integer(font, "/FirstChar", -1), last = Integer(font, "/LastChar", -1);
  QPDFObjectHandle widths = font.getKey("/Widths");
  const bool has_widths = first >= 0 && last >= first && last <= 255 && widths.isArray() && widths.getArrayNItems() == last - first + 1;
  if (!(IsName(subtype, "/Type1") || IsName(subtype, "/MMType1") || IsName(subtype, "/TrueType")) || checker.Composite() || !checker.Ready() ||
      (has_widths && font.getKey("/FontDescriptor").isDictionary()))
    return false;
  const auto program = Program::Read(data, Container::TrueType);
  if (!program || !program->HasCmap(3, 1))
    return false;
  if (!has_widths) {
    unsigned low = 255, high = 0;
    for (const auto &[code, drawn] : checker.Codes()) {
      low = std::min(low, code);
      high = std::max(high, code);
    }
    if (low > high)
      low = high = 32;
    std::vector<double> list;
    for (unsigned code = low; code <= high; ++code) {
      const unsigned unicode = UnicodeOf(checker.NameFor(code));
      const int glyph = unicode ? Glyph(*program, unicode) : 0;
      list.push_back(glyph > 0 ? std::round(program->Width(glyph) * 1000) / 1000 : 0);
    }
    font.replaceKey("/FirstChar", QPDFObjectHandle::newInteger(low));
    font.replaceKey("/LastChar", QPDFObjectHandle::newInteger(high));
    font.replaceKey("/Widths", NumberArray(list));
  }
  if (!font.getKey("/FontDescriptor").isDictionary()) {
    const int flags = Integer(font.getKey("/FontDescriptor"), "/Flags", 0);
    font.replaceKey("/FontDescriptor", Descriptor(q, *program, font.getKey("/BaseFont"), QPDFObjectHandle::newNull(), (flags & 4) != 0, QPDFObjectHandle::newNull()));
  }
  return true;
}

std::map<unsigned, unsigned> Characters(QPDFObjectHandle to_unicode) {
  // The ligatures a ToUnicode map spells out, as the one character the font has a glyph for.
  static const std::map<std::vector<unsigned>, unsigned> ligatures = {
      {{'f', 'f'}, 0xfb00}, {{'f', 'i'}, 0xfb01}, {{'f', 'l'}, 0xfb02}, {{'f', 'f', 'i'}, 0xfb03}, {{'f', 'f', 'l'}, 0xfb04}};
  std::map<unsigned, unsigned> out;
  for (const auto &[code, text] : ReadToUnicode(to_unicode)) {
    std::vector<unsigned> letters;
    for (size_t at = 0; at + 1 < text.size(); at += 2) {
      unsigned unit = static_cast<unsigned char>(text[at]) << 8 | static_cast<unsigned char>(text[at + 1]);
      if (unit >= 0xd800 && unit < 0xdc00 && at + 3 < text.size()) {
        const unsigned low = static_cast<unsigned char>(text[at + 2]) << 8 | static_cast<unsigned char>(text[at + 3]);
        if (low >= 0xdc00 && low < 0xe000) {
          unit = 0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00);
          at += 2;
        }
      }
      letters.push_back(unit);
    }
    const auto ligature = ligatures.find(letters);
    if (letters.size() == 1 && letters[0])
      out.emplace(code, letters[0]);
    else if (ligature != ligatures.end())
      out.emplace(code, ligature->second);
  }
  return out;
}

std::string ChooseCompositeSubstitute(QPDFObjectHandle font, const Checker &checker, const std::map<unsigned, unsigned> &characters,
                                      const std::function<const Program *(const std::string &)> &read, bool &twin, unsigned *lacking) {
  QPDFObjectHandle descendant = DescendantOf(font);
  std::vector<Wanted> codes;
  for (const auto &[cid, drawn] : checker.Codes()) {
    if (cid == 0)
      continue;  // .notdef, drawn as nothing
    const auto found = characters.find(cid);
    codes.push_back({found == characters.end() ? 0u : found->second, checker.DictionaryWidth(cid), drawn});
  }
  // Its name is the CIDFont's, or the Type 0 font's with the CMap after it.
  std::string base = descendant.isDictionary() && descendant.getKey("/BaseFont").isName() ? descendant.getKey("/BaseFont").getName() : "";
  if (base.empty() && font.getKey("/BaseFont").isName())
    base = font.getKey("/BaseFont").getName();
  for (const char *suffix : {"-Identity-H", "-Identity-V"})
    if (base.size() > std::strlen(suffix) && base.compare(base.size() - std::strlen(suffix), std::string::npos, suffix) == 0)
      base.resize(base.size() - std::strlen(suffix));
  return Choose(base, descendant.isDictionary() ? descendant.getKey("/FontDescriptor") : QPDFObjectHandle::newNull(), codes, lacking, read, twin);
}

bool EmbedComposite(QPDF &q, QPDFObjectHandle font, const std::string &data, const Checker &checker, const std::map<unsigned, unsigned> &characters,
                    std::string &why) {
  QPDFObjectHandle descendant = DescendantOf(font);
  // Horizontal: Identity-H, or a predefined CMap ending in -H.
  QPDFObjectHandle encoding = font.getKey("/Encoding");
  const std::string cmap = encoding.isName() ? encoding.getName() : "";
  if (!descendant.isDictionary() || !checker.Composite() || !checker.Ready() || cmap.size() < 3 || cmap.compare(cmap.size() - 2, 2, "-H") != 0) {
    why = "not a horizontal font with a CMap it names";
    return false;
  }
  const auto program = Program::Read(data, Container::TrueType);
  if (!program || !program->HasCmap(3, 1)) {
    why = "substitute unreadable";
    return false;
  }
  // Each CID's glyph in the substitute, by the character the ToUnicode map gives it.
  std::set<int> keep;
  std::map<unsigned, int> glyphs;
  unsigned highest = 0;
  for (const auto &[cid, drawn] : checker.Codes()) {
    const auto found = characters.find(cid);
    const int glyph = cid == 0 || found == characters.end() ? 0 : Glyph(*program, found->second);
    if (glyph <= 0) {
      if (drawn && cid != 0) {
        why = "no glyph for CID " + std::to_string(cid);
        return false;
      }
      continue;
    }
    keep.insert(glyph);
    glyphs.emplace(cid, glyph);
    highest = std::max(highest, cid);
  }
  if (highest > 0xffff) {
    why = "CIDs past 65,535";
    return false;
  }
  std::string subset = data;
  std::map<int, int> renumbered;
  if (!Subset(subset, keep, renumbered, true)) {
    why = "substitute has no TrueType outlines";
    return false;
  }
  // Its glyphs found by CID (2 bytes each, glyph 0 for the CIDs it doesn't draw), and a Unicode
  // cmap of just them, so the substitute's own maps don't name glyphs it no longer has.
  std::string map(static_cast<size_t>(highest + 1) * 2, '\0');
  std::map<unsigned, int> unicode;
  for (const auto &[cid, glyph] : glyphs) {
    const int renamed = renumbered.at(glyph);
    map[cid * 2] = static_cast<char>(renamed >> 8);
    map[cid * 2 + 1] = static_cast<char>(renamed & 0xff);
    const unsigned character = characters.at(cid);
    if (character <= 0xffff)
      unicode.emplace(character, renamed);
  }
  if (!SetCmap(subset, 3, 1, unicode)) {
    why = "substitute's cmap can't be replaced";
    return false;
  }
  descendant.replaceKey("/Type", QPDFObjectHandle::newName("/Font"));
  descendant.replaceKey("/Subtype", QPDFObjectHandle::newName("/CIDFontType2"));
  descendant.replaceKey("/CIDToGIDMap", QPDFObjectHandle::newStream(&q, map));
  if (!descendant.getKey("/CIDSystemInfo").isDictionary())
    descendant.replaceKey("/CIDSystemInfo", QPDFObjectHandle::parse("<< /Registry (Adobe) /Ordering (Identity) /Supplement 0 >>"));
  descendant.replaceKey("/FontDescriptor", Descriptor(q, *program, descendant.getKey("/BaseFont"), descendant.getKey("/FontDescriptor"), true, FontFile(q, subset)));
  return true;
}

bool Embed(QPDF &q, QPDFObjectHandle font, const std::string &data, const Checker &checker, bool plain, std::string &why, SharedProgram *shared) {
  const QPDFObjectHandle subtype = font.getKey("/Subtype");
  if (!(IsName(subtype, "/Type1") || IsName(subtype, "/MMType1") || IsName(subtype, "/TrueType")) || checker.Composite() || !checker.Ready()) {
    why = "not a simple font";
    return false;
  }
  const auto program = Program::Read(data, Container::TrueType);
  if (!program || !program->HasCmap(3, 1)) {
    why = "substitute unreadable";
    return false;
  }
  // Each code's glyph in the substitute, by the glyph name it has.
  std::set<int> keep;
  std::map<unsigned, int> by_code;
  for (unsigned code = 0; code < 256; ++code) {
    const unsigned unicode = UnicodeOf(checker.NameFor(code));
    const int glyph = unicode ? Glyph(*program, unicode) : 0;
    if (glyph > 0)
      by_code.emplace(code, glyph);
  }
  for (const auto &[code, drawn] : checker.Codes()) {
    const std::string &name = checker.NameFor(code);
    if (name.empty() || name == ".notdef")
      continue;
    const unsigned unicode = UnicodeOf(name);
    const int glyph = unicode ? Glyph(*program, unicode) : 0;
    if (glyph <= 0) {
      if (drawn) {
        why = "no " + name + " in the substitute";
        return false;
      }
      continue;
    }
    keep.insert(glyph);
  }
  // The codes the pages use keep their names: WinAnsi underneath, the others as differences
  // (only those: veraPDF wants each difference's character in the font's cmap, which has just
  // the characters used).
  const char *const *win = Encoding("/WinAnsiEncoding");
  std::vector<QPDFObjectHandle> differences;
  bool listed = true;     // every name the differences use is in the Adobe Glyph List
  bool unnamed = false;   // a used code the encoding leaves unnamed, which WinAnsi would name
  std::set<unsigned> used;
  for (const auto &[code, drawn] : checker.Codes())
    used.insert(code);
  // Fonts sharing a program get the same encoding, from all their codes: veraPDF reads a program
  // once, with the first font's.
  const std::set<unsigned> &named = shared && !plain ? shared->codes : used;
  int previous = -2;
  for (unsigned code = 0; code < 256; ++code) {
    if (!named.count(code))
      continue;
    const std::string &name = checker.NameFor(code);
    if ((name.empty() || name == ".notdef") && *win[code])
      unnamed = true;
    if (name.empty() || name == ".notdef" || name == win[code] || !by_code.count(code))
      continue;
    // In the list's own spelling where it has one (the same character, so the same glyph).
    const std::string spelled = ListedName(name);
    listed = listed && !spelled.empty();
    if (static_cast<int>(code) != previous + 1)
      differences.push_back(QPDFObjectHandle::newInteger(code));
    differences.push_back(QPDFObjectHandle::newName("/" + (spelled.empty() ? name : spelled)));
    previous = static_cast<int>(code);
  }
  // Its own cmap, with just the characters the codes need: Unicode, or for a symbolic font the
  // codes themselves. Symbolic when the names can't be differences of a non-symbolic TrueType
  // font (not in the glyph list, as dingbats' aren't; any at all in PDF/A-1).
  // Also when a used code has no name: a symbolic font's own cmap leaves it unmapped, as it was.
  const bool symbolic = ((plain || !listed) && !differences.empty()) || unnamed;
  // A non-symbolic font finds its glyphs by character, so one subset of the substitute, made
  // once with every character the fonts it stands in for need, serves them all.
  QPDFObjectHandle file = QPDFObjectHandle::newNull();
  if (!symbolic && shared) {
    if (shared->file.isNull()) {
      std::set<int> all = keep;
      for (const unsigned character : shared->characters)
        if (const int glyph = Glyph(*program, character); glyph > 0)
          all.insert(glyph);
      std::string made = data;
      std::map<int, int> numbers;
      std::map<unsigned, int> map;
      if (Subset(made, all, numbers, true)) {
        for (const unsigned character : shared->characters)
          if (const int glyph = Glyph(*program, character); glyph > 0 && numbers.count(glyph))
            map.emplace(character, numbers.at(glyph));
        if (!map.empty() && SetCmap(made, 3, 1, map)) {
          shared->file = FontFile(q, made);
          for (const auto &entry : map)
            shared->covered.insert(entry.first);
        }
      }
    }
    bool covered = shared->file.isStream();
    for (const auto &[code, glyph] : by_code)
      covered = covered && (!used.count(code) || shared->covered.count(UnicodeOf(checker.NameFor(code))));
    if (covered)
      file = shared->file;
  }
  if (file.isNull()) {
    // The glyphs of every code its encoding names (a shared group's), so its cmap has them.
    for (const unsigned code : named)
      if (const auto glyph = by_code.find(code); glyph != by_code.end())
        keep.insert(glyph->second);
    std::string subset = data;
    std::map<int, int> renumbered;
    // Its .notdef draws nothing, as readers show a code a font they don't have doesn't map.
    if (!Subset(subset, keep, renumbered, true)) {
      why = "substitute has no TrueType outlines";
      return false;
    }
    std::map<unsigned, int> characters;
    for (const auto &[code, glyph] : by_code) {
      const auto renamed = renumbered.find(glyph);
      if (renamed == renumbered.end())
        continue;
      if (symbolic)
        characters.emplace(0xf000 + code, renamed->second);
      else
        characters.emplace(UnicodeOf(checker.NameFor(code)), renamed->second);
    }
    // (A symbolic one always gets its own, even with nothing in it: the substitute's Unicode
    // cmaps would otherwise be read for its codes.)
    if ((symbolic || !characters.empty()) && !SetCmap(subset, 3, symbolic ? 0 : 1, characters)) {
      why = "substitute's cmap can't be replaced";
      return false;
    }
    file = FontFile(q, subset);
  }
  QPDFObjectHandle encoding = QPDFObjectHandle::newName("/WinAnsiEncoding");
  if (!differences.empty() && !symbolic) {
    encoding = QPDFObjectHandle::newDictionary();
    encoding.replaceKey("/Type", QPDFObjectHandle::newName("/Encoding"));
    encoding.replaceKey("/BaseEncoding", QPDFObjectHandle::newName("/WinAnsiEncoding"));
    encoding.replaceKey("/Differences", QPDFObjectHandle::newArray(differences));
  }
  // The dictionary's widths place the text and stay; where it has none (the standard 14
  // fonts), the substitute's, which are the same as the font it stands in for.
  const int first = Integer(font, "/FirstChar", -1), last = Integer(font, "/LastChar", -1);
  QPDFObjectHandle widths = font.getKey("/Widths");
  const bool has_widths = first >= 0 && last >= first && widths.isArray() && widths.getArrayNItems() == last - first + 1;
  QPDFObjectHandle descriptor = Descriptor(q, *program, font.getKey("/BaseFont"), font.getKey("/FontDescriptor"), symbolic, file);

  font.replaceKey("/Type", QPDFObjectHandle::newName("/Font"));
  font.replaceKey("/Subtype", QPDFObjectHandle::newName("/TrueType"));
  if (symbolic) {
    // No encoding; the text from the names the codes had.
    font.removeKey("/Encoding");
    std::string map = "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
                      "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                      "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n1 begincodespacerange\n<00> <FF>\nendcodespacerange\n";
    std::vector<std::string> entries;
    char text[32];
    for (unsigned code = 0; code < 256; ++code) {
      const unsigned unicode = UnicodeOf(checker.NameFor(code));
      if (!unicode || unicode > 0xffff)
        continue;
      std::snprintf(text, sizeof text, "<%02X> <%04X>\n", code, unicode);
      entries.push_back(text);
    }
    for (size_t at = 0; at < entries.size(); at += 100) {
      const size_t count = std::min<size_t>(100, entries.size() - at);
      map += std::to_string(count) + " beginbfchar\n";
      for (size_t k = 0; k < count; ++k)
        map += entries[at + k];
      map += "endbfchar\n";
    }
    map += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    if (!font.getKey("/ToUnicode").isStream())
      font.replaceKey("/ToUnicode", QPDFObjectHandle::newStream(&q, map));
  } else {
    font.replaceKey("/Encoding", encoding);
  }
  font.replaceKey("/FontDescriptor", descriptor);
  if (!has_widths) {
    // The standard 14 Symbol and ZapfDingbats fonts are laid out by Adobe's widths for them.
    const std::string base = font.getKey("/BaseFont").isName() ? font.getKey("/BaseFont").getName() : "";
    const std::string standard = base == "/Symbol" ? "Symbol" : base == "/ZapfDingbats" ? "ZapfDingbats" : "";
    unsigned low = 255, high = 0;
    for (const auto &[code, drawn] : checker.Codes()) {
      low = std::min(low, code);
      high = std::max(high, code);
    }
    if (low > high)
      low = high = 32;  // nothing shown: still the three keys PDF requires, for the space
    {
      std::vector<double> list;
      for (unsigned code = low; code <= high; ++code) {
        const unsigned unicode = UnicodeOf(checker.NameFor(code));
        const int glyph = unicode ? Glyph(*program, unicode) : -1;
        const double adobe = standard.empty() ? -1 : StandardWidth(standard, checker.NameFor(code));
        list.push_back(adobe >= 0 ? adobe : glyph > 0 ? std::round(program->Width(glyph) * 1000) / 1000 : 0);
      }
      font.replaceKey("/FirstChar", QPDFObjectHandle::newInteger(low));
      font.replaceKey("/LastChar", QPDFObjectHandle::newInteger(high));
      font.replaceKey("/Widths", NumberArray(list));
    }
  }
  return true;
}

} // namespace fynepdf::pdfa::font
