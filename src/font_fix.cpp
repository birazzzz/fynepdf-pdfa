#include "font_fix.h"

#include "common.h"
#include "content.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <qpdf/Buffer.hh>
#include <qpdf/Pl_Buffer.hh>

namespace fynepdf::pdfa::font {
namespace {

std::string NameOf(QPDFObjectHandle handle) { return handle.isName() ? handle.getName() : ""; }

bool Decode(QPDFObjectHandle stream, std::string &out) {
  try {
    Pl_Buffer buffer("font");
    if (!stream.isStream() || !stream.pipeStreamData(&buffer, 0, qpdf_dl_generalized))
      return false;
    std::unique_ptr<Buffer> data(buffer.getBuffer());
    out = data ? std::string(reinterpret_cast<const char *>(data->getBuffer()), data->getSize()) : std::string();
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

QPDFObjectHandle DescendantOf(QPDFObjectHandle font) {
  if (!IsName(font.getKey("/Subtype"), "/Type0"))
    return font;
  QPDFObjectHandle descendants = font.getKey("/DescendantFonts");
  return descendants.isArray() && descendants.getArrayNItems() > 0 ? descendants.getArrayItem(0) : QPDFObjectHandle::newNull();
}

QPDFObjectHandle DescriptorOf(QPDFObjectHandle font) {
  QPDFObjectHandle described = DescendantOf(font);
  return described.isDictionary() ? described.getKey("/FontDescriptor") : QPDFObjectHandle::newNull();
}

// The glyph names a simple font's encoding gives its codes (the standard encoding under it
// when it names none), as readers take text from them when the font has no ToUnicode map.
std::vector<std::string> EncodingNames(QPDFObjectHandle font) {
  QPDFObjectHandle encoding = font.getKey("/Encoding");
  const char *const *base = Encoding(encoding.isName() ? encoding.getName() : encoding.isDictionary() ? NameOf(encoding.getKey("/BaseEncoding")) : "");
  if (!base)
    base = Encoding("/StandardEncoding");
  std::vector<std::string> names(base, base + 256);
  QPDFObjectHandle differences = encoding.isDictionary() ? encoding.getKey("/Differences") : QPDFObjectHandle::newNull();
  unsigned code = 0;
  for (int index = 0; differences.isArray() && index < differences.getArrayNItems(); ++index) {
    QPDFObjectHandle item = differences.getArrayItem(index);
    if (item.isInteger())
      code = static_cast<unsigned>(std::max(0, item.getIntValueAsInt()));
    else if (item.isName() && code < 256)
      names[code++] = item.getName().substr(1);
  }
  return names;
}

} // namespace

std::map<unsigned, std::string> ReadToUnicode(QPDFObjectHandle stream) {
  std::map<unsigned, std::string> out;
  std::string program;
  if (!stream.isStream() || !Decode(stream, program))
    return out;
  std::vector<content::Token> tokens;
  size_t pos = 0;
  content::Token token;
  while (content::NextToken(program, pos, token) && tokens.size() < 200000)
    tokens.push_back(token);
  const auto code_of = [](const std::string &bytes) {
    unsigned value = 0;
    for (const char c : bytes)
      value = value << 8 | static_cast<unsigned char>(c);
    return value;
  };
  const auto string_at = [&](size_t at) { return at < tokens.size() && tokens[at].kind == content::Token::String; };
  for (size_t at = 0; at < tokens.size(); ++at) {
    if (tokens[at].kind != content::Token::Operator)
      continue;
    if (tokens[at].text == "beginbfchar") {
      for (++at; string_at(at) && string_at(at + 1); at += 2)
        out[code_of(tokens[at].text)] = tokens[at + 1].text;
    } else if (tokens[at].text == "beginbfrange") {
      for (++at; string_at(at) && string_at(at + 1) && at + 2 < tokens.size(); at += 3) {
        const unsigned low = code_of(tokens[at].text), high = std::min(code_of(tokens[at + 1].text), low + 0xffffu);
        if (tokens[at + 2].kind == content::Token::String) {
          std::string target = tokens[at + 2].text;
          for (unsigned code = low; code <= high && !target.empty(); ++code) {
            out[code] = target;
            // Counting up, carried into the bytes before (ISO 32000 says only the last byte
            // counts, but readers carry, and maps such as <0000> <FFFF> <0000> rely on it).
            for (size_t at = target.size(); at-- > 0 && ++target[at] == '\0';) {
            }
          }
        } else if (tokens[at + 2].kind == content::Token::ArrayOpen) {
          size_t k = at + 3;
          for (unsigned code = low; k < tokens.size() && tokens[k].kind == content::Token::String; ++code, ++k)
            out[code] = tokens[k].text;
          at = k - 2;  // past the array's close, less the step
        }
      }
    }
  }
  return out;
}

namespace {

// A ToUnicode map, so a font whose encoding changes keeps its text: the map it has, and for
// each code that has none there, the character by its old glyph name or, where that says
// nothing (a symbolic font's), by the Unicode the program's cmap gives its glyph, as readers
// fall back on.
QPDFObjectHandle ToUnicode(QPDF &q, QPDFObjectHandle existing, const std::vector<std::string> &names, const Checker &checker) {
  const std::map<int, unsigned> by_glyph = checker.Font() ? checker.Font()->UnicodeByGlyph() : std::map<int, unsigned>{};
  std::set<unsigned> used;
  for (const auto &[code, drawn] : checker.Codes())
    used.insert(code);
  std::map<unsigned, std::string> text_of = ReadToUnicode(existing);
  for (unsigned code = 0; code < 256; ++code) {
    if (text_of.count(code))
      continue;
    unsigned unicode = UnicodeOf(names[code]);
    if (!unicode && used.count(code)) {
      const auto found = by_glyph.find(checker.GlyphFor(code));
      unicode = found == by_glyph.end() ? 0 : found->second;
    }
    if (unicode && unicode <= 0xffff)
      text_of[code] = std::string{static_cast<char>(unicode >> 8), static_cast<char>(unicode & 0xff)};
  }
  std::vector<std::string> entries;
  char text[16];
  for (const auto &[code, bytes] : text_of) {
    if (code > 255 || bytes.empty())
      continue;
    std::snprintf(text, sizeof text, "<%02X> <", code);
    std::string entry = text;
    for (const char c : bytes) {
      std::snprintf(text, sizeof text, "%02X", static_cast<unsigned char>(c));
      entry += text;
    }
    entries.push_back(entry + ">\n");
  }
  if (entries.empty())
    return QPDFObjectHandle::newNull();
  std::string cmap = "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
                     "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                     "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
                     "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n";
  for (size_t at = 0; at < entries.size(); at += 100) {
    const size_t count = std::min<size_t>(100, entries.size() - at);
    cmap += std::to_string(count) + " beginbfchar\n";
    for (size_t k = 0; k < count; ++k)
      cmap += entries[at + k];
    cmap += "endbfchar\n";
  }
  cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
  return QPDFObjectHandle::newStream(&q, cmap);
}

// Whether |font| as it is now finds the glyph |old| found for every code the pages use.
bool SameGlyphs(QPDFObjectHandle font, const Checker &old) {
  Checker test(font);
  if (!test.Ready())
    return false;
  for (const auto &[code, drawn] : old.Codes())
    if (test.GlyphFor(code) != old.GlyphFor(code))
      return false;
  return true;
}

} // namespace

std::unique_ptr<Checker> Recheck(QPDFObjectHandle font, const Checker &old) {
  auto checker = std::make_unique<Checker>(font);
  checker->Replay(old);
  return checker;
}

bool FillWidths(QPDFObjectHandle font, const Checker &checker) {
  const std::string subtype = NameOf(font.getKey("/Subtype"));
  if ((subtype != "/Type1" && subtype != "/MMType1" && subtype != "/TrueType") || !checker.Ready() || !checker.Font())
    return false;
  const int first = Integer(font, "/FirstChar", -1), last = Integer(font, "/LastChar", -1);
  QPDFObjectHandle widths = font.getKey("/Widths");
  if (first >= 0 && last >= first && last <= 255 && widths.isArray() && widths.getArrayNItems() == last - first + 1)
    return false;
  unsigned low = 255, high = 0;
  for (const auto &[code, drawn] : checker.Codes()) {
    low = std::min(low, code);
    high = std::max(high, code);
  }
  if (first >= 0 && last >= first && last <= 255) {
    low = std::min(low, static_cast<unsigned>(first));
    high = std::max(high, static_cast<unsigned>(last));
  }
  if (low > high)
    return false;
  // The widths the dictionary had where it had them (they place the text); the program's
  // elsewhere, which readers use when the dictionary has none.
  std::vector<double> list;
  for (unsigned code = low; code <= high; ++code) {
    const long index = static_cast<long>(code) - first;
    if (first >= 0 && widths.isArray() && index >= 0 && index < widths.getArrayNItems() && widths.getArrayItem(static_cast<int>(index)).isNumber()) {
      list.push_back(widths.getArrayItem(static_cast<int>(index)).getNumericValue());
      continue;
    }
    const int glyph = checker.GlyphFor(code);
    const double width = glyph >= 0 ? checker.Font()->Width(glyph) : -1;
    list.push_back(width >= 0 ? std::round(width * 1000) / 1000 : 0);
  }
  font.replaceKey("/FirstChar", QPDFObjectHandle::newInteger(low));
  font.replaceKey("/LastChar", QPDFObjectHandle::newInteger(high));
  font.replaceKey("/Widths", NumberArray(list));
  return true;
}

bool SetProgramWidths(QPDFObjectHandle font, const Checker &checker) {
  if (!checker.Ready() || !checker.Font() || !checker.Widths())
    return false;
  QPDFObjectHandle file = DescriptorOf(font).getKey("/FontFile2");
  if (!file.isStream())
    return false;
  const Program &program = *checker.Font();
  // Each glyph drawn: the dictionary widths of the codes that draw it, which must agree.
  std::map<int, std::pair<double, double>> wanted;
  for (const auto &[code, drawn] : checker.Codes()) {
    const int glyph = checker.GlyphFor(code);
    if (!drawn || glyph < 0 || (glyph == 0 && !checker.Composite()))
      continue;
    const double width = checker.DictionaryWidth(code);
    const auto found = wanted.find(glyph);
    if (found == wanted.end())
      wanted.emplace(glyph, std::make_pair(width, width));
    else
      found->second = {std::min(found->second.first, width), std::max(found->second.second, width)};
  }
  std::map<int, unsigned> advances;
  for (const auto &[glyph, range] : wanted) {
    if (range.second - range.first > 1)
      return false;  // one glyph drawn at two widths: no single advance satisfies both
    const double target = (range.first + range.second) / 2;
    if (std::fabs(program.Width(glyph) - target) > 1 && target >= 0)
      advances.emplace(glyph, static_cast<unsigned>(std::lround(target * program.UnitsPerEm() / 1000)));
  }
  std::string data;
  QPDF *owner = file.getOwningQPDF();
  if (advances.empty() || !owner || !Decode(file, data) || !SetAdvances(data, advances))
    return false;
  // Into a copy of the program and its descriptor, as other fonts may share them.
  QPDFObjectHandle copy = QPDFObjectHandle::newStream(owner, data);
  copy.getDict().replaceKey("/Length1", QPDFObjectHandle::newInteger(static_cast<long long>(data.size())));
  QPDFObjectHandle descriptor = DescriptorOf(font).shallowCopy();
  descriptor.replaceKey("/FontFile2", copy);
  DescendantOf(font).replaceKey("/FontDescriptor", owner->makeIndirectObject(descriptor));
  return true;
}

bool Type3Widths(QPDF &q, QPDFObjectHandle font, const Checker &checker) {
  QPDFObjectHandle procs = font.getKey("/CharProcs");
  if (!IsName(font.getKey("/Subtype"), "/Type3") || !checker.Ready() || !checker.Widths() || !procs.isDictionary())
    return false;
  // Each drawn glyph: the widths of the codes that draw it, which must agree.
  std::map<std::string, std::pair<double, double>> wanted;
  for (const auto &[code, drawn] : checker.Codes()) {
    const std::string &name = checker.NameFor(code);
    if (!drawn || name.empty())
      continue;
    const double width = checker.DictionaryWidth(code);
    const auto found = wanted.find(name);
    if (found == wanted.end())
      wanted.emplace(name, std::make_pair(width, width));
    else
      found->second = {std::min(found->second.first, width), std::max(found->second.second, width)};
  }
  bool copied = false, changed = false;
  for (const auto &[name, range] : wanted) {
    QPDFObjectHandle proc = procs.getKey("/" + name);
    std::string content;
    if (range.second - range.first > 1 || !proc.isStream() || !Decode(proc, content))
      continue;
    // The procedure's first operator, d0 or d1: its first operand is the glyph's width.
    size_t pos = 0;
    content::Token token, width;
    size_t count = 0;
    while (content::NextToken(content, pos, token) && token.kind == content::Token::Number)
      if (count++ == 0)
        width = token;
    const double target = (range.first + range.second) / 2;
    if (token.kind != content::Token::Operator || (token.text != "d0" && token.text != "d1") || count < 2 || std::fabs(width.number - target) <= 1)
      continue;
    content.replace(width.begin, width.end - width.begin, content::Num(target));
    // Into a copy, in a copy of the procedures, as other fonts may share them.
    if (!copied) {
      procs = procs.shallowCopy();
      font.replaceKey("/CharProcs", procs);
      copied = true;
    }
    QPDFObjectHandle made = QPDFObjectHandle::newStream(&q, content);
    for (const std::string &key : proc.getDict().getKeys())
      if (key != "/Length" && key != "/Filter" && key != "/DecodeParms")
        made.getDict().replaceKey(key, proc.getDict().getKey(key));
    procs.replaceKey("/" + name, made);
    changed = true;
  }
  return changed;
}

bool OneCmap(QPDFObjectHandle font, const Checker &checker) {
  QPDFObjectHandle descriptor = font.getKey("/FontDescriptor");
  QPDFObjectHandle file = descriptor.isDictionary() ? descriptor.getKey("/FontFile2") : QPDFObjectHandle::newNull();
  if (!IsName(font.getKey("/Subtype"), "/TrueType") || !(Integer(descriptor, "/Flags", 0) & 4) || !checker.Ready() || !checker.Font() ||
      !checker.Font()->Sfnt() || checker.Font()->Cmaps().size() <= 1 || !file.isStream())
    return false;
  // Each code's glyph as it is found now, under the code in the Microsoft symbol range.
  std::map<unsigned, int> glyphs;
  for (unsigned code = 0; code < 256; ++code)
    if (const int glyph = checker.GlyphFor(code); glyph > 0)
      glyphs.emplace(0xf000 + code, glyph);
  std::string data;
  QPDF *owner = file.getOwningQPDF();
  if (!owner || !Decode(file, data) || !SetCmap(data, 3, 0, glyphs))
    return false;
  // Into a copy of the program and its descriptor, as other fonts may share them.
  QPDFObjectHandle old_descriptor = descriptor;
  QPDFObjectHandle copy = QPDFObjectHandle::newStream(owner, data);
  copy.getDict().replaceKey("/Length1", QPDFObjectHandle::newInteger(static_cast<long long>(data.size())));
  QPDFObjectHandle made = descriptor.shallowCopy();
  made.replaceKey("/FontFile2", copy);
  font.replaceKey("/FontDescriptor", owner->makeIndirectObject(made));
  if (!SameGlyphs(font, checker)) {
    font.replaceKey("/FontDescriptor", old_descriptor);
    return false;
  }
  return true;
}

bool AddCmap(QPDFObjectHandle font, const Checker &checker) {
  if (!IsName(font.getKey("/Subtype"), "/TrueType") || !checker.Ready() || !checker.Font() || !checker.Font()->Sfnt() || !checker.Font()->Cmaps().empty())
    return false;
  QPDFObjectHandle descriptor = font.getKey("/FontDescriptor");
  QPDFObjectHandle file = descriptor.isDictionary() ? descriptor.getKey("/FontFile2") : QPDFObjectHandle::newNull();
  std::string data, old;
  if (!file.isStream() || !Decode(file, data))
    return false;
  // The glyph each code finds now (by its name, through the post table), under its character
  // or, for a symbolic font, the code itself.
  const bool symbolic = (Integer(descriptor, "/Flags", 0) & 4) != 0;
  std::map<unsigned, int> glyphs;
  for (unsigned code = 0; code < 256; ++code) {
    const int glyph = checker.GlyphFor(code);
    if (glyph <= 0)
      continue;
    const unsigned character = symbolic ? 0xf000 + code : UnicodeOf(checker.NameFor(code));
    if (character)
      glyphs.emplace(character, glyph);
  }
  old = data;
  if (glyphs.empty() || !SetCmap(data, 3, symbolic ? 0 : 1, glyphs))
    return false;
  file.replaceStreamData(data, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
  file.getDict().replaceKey("/Length1", QPDFObjectHandle::newInteger(static_cast<long long>(data.size())));
  if (!SameGlyphs(font, checker)) {
    file.replaceStreamData(old, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
    file.getDict().replaceKey("/Length1", QPDFObjectHandle::newInteger(static_cast<long long>(old.size())));
    return false;
  }
  return true;
}

bool TrueTypeEncoding(QPDF &q, QPDFObjectHandle font, const Checker &checker) {
  if (!IsName(font.getKey("/Subtype"), "/TrueType") || !checker.Ready() || !checker.Font() || !checker.Font()->Sfnt())
    return false;
  QPDFObjectHandle descriptor = font.getKey("/FontDescriptor");
  if (!descriptor.isDictionary() || !descriptor.getKey("/FontFile2").isStream())
    return false;
  const Program &program = *checker.Font();
  const auto cmaps = program.Cmaps();
  const auto has = [&](unsigned platform, unsigned id) { return std::find(cmaps.begin(), cmaps.end(), std::make_pair(platform, id)) != cmaps.end(); };
  const bool unicode = program.HasUnicodeCmap();
  const bool symbolic_cmaps = cmaps.size() == 1 || has(3, 0), named_cmaps = has(3, 1) || has(1, 0) || unicode;
  const int flags = Integer(descriptor, "/Flags", 0);
  const bool symbolic = (flags & 4) != 0;
  QPDFObjectHandle encoding = font.getKey("/Encoding");
  const std::string base = encoding.isName() ? encoding.getName() : encoding.isDictionary() ? NameOf(encoding.getKey("/BaseEncoding")) : "";
  bool listed = true;
  QPDFObjectHandle differences = encoding.isDictionary() ? encoding.getKey("/Differences") : QPDFObjectHandle::newNull();
  // Each name the differences use: in the glyph list, its character in the program's cmap.
  const auto mapped = [&](QPDFObjectHandle list) {
    for (int index = 0; list.isArray() && index < list.getArrayNItems(); ++index) {
      QPDFObjectHandle item = list.getArrayItem(index);
      if (item.isName() && (!InGlyphList(item.getName().substr(1)) || program.ByUnicode(UnicodeOf(item.getName().substr(1))) <= 0))
        return false;
    }
    return true;
  };
  listed = mapped(differences);
  if (symbolic ? encoding.isNull() : (base == "/MacRomanEncoding" || base == "/WinAnsiEncoding") && (!differences.isArray() || (listed && (has(3, 1) || unicode))))
    return false;  // already as PDF/A wants it

  QPDFObjectHandle old_encoding = encoding;
  const auto restore = [&]() {
    if (old_encoding.isNull())
      font.removeKey("/Encoding");
    else
      font.replaceKey("/Encoding", old_encoding);
    descriptor.replaceKey("/Flags", QPDFObjectHandle::newInteger(flags));
  };
  // The text readers take from the names the font has now, kept for when they change.
  const std::vector<std::string> old_names = EncodingNames(font);
  const auto attempt = [&](QPDFObjectHandle new_encoding, int new_flags) {
    if (new_encoding.isNull())
      font.removeKey("/Encoding");
    else
      font.replaceKey("/Encoding", new_encoding);
    descriptor.replaceKey("/Flags", QPDFObjectHandle::newInteger(new_flags));
    const bool named_font = !(new_flags & 4);
    if (!SameGlyphs(font, checker) || (named_font && new_encoding.isDictionary() && !mapped(new_encoding.getKey("/Differences")))) {
      restore();
      return false;
    }
    if (EncodingNames(font) != old_names) {
      QPDFObjectHandle map = ToUnicode(q, font.getKey("/ToUnicode"), old_names, checker);
      if (!map.isNull())
        font.replaceKey("/ToUnicode", map);
    }
    return true;
  };
  // A WinAnsi or MacRoman base with the names the used codes need (from the encoding the font
  // has, or for a symbolic font the program's own glyph names), only names the Adobe Glyph
  // List knows.
  const auto named = [&](const char *base_name) -> QPDFObjectHandle {
    const char *const *table = Encoding(base_name);
    std::vector<QPDFObjectHandle> changes;
    for (const auto &[code, drawn] : checker.Codes()) {
      if (code > 255)
        return QPDFObjectHandle::newNull();
      // In the list's own spelling, which finds the same character.
      std::string name = ListedName(symbolic ? program.GlyphName(checker.GlyphFor(code)) : checker.NameFor(code));
      if (name.empty())
        return QPDFObjectHandle::newNull();
      if (name != table[code]) {
        changes.push_back(QPDFObjectHandle::newInteger(code));
        changes.push_back(QPDFObjectHandle::newName("/" + name));
      }
    }
    if (changes.empty())
      return QPDFObjectHandle::newName(base_name);
    QPDFObjectHandle dict = QPDFObjectHandle::newDictionary();
    dict.replaceKey("/Type", QPDFObjectHandle::newName("/Encoding"));
    dict.replaceKey("/BaseEncoding", QPDFObjectHandle::newName(base_name));
    dict.replaceKey("/Differences", QPDFObjectHandle::newArray(changes));
    return dict;
  };
  const int as_symbolic = (flags | 4) & ~32, as_named = (flags & ~4) | 32;
  if (symbolic) {
    if (symbolic_cmaps && attempt(QPDFObjectHandle::newNull(), flags))
      return true;
    if (named_cmaps)
      for (const char *base_name : {"/WinAnsiEncoding", "/MacRomanEncoding"}) {
        QPDFObjectHandle made = named(base_name);
        if (!made.isNull() && attempt(made, as_named))
          return true;
      }
    return false;
  }
  if (named_cmaps)
    for (const char *base_name : {"/WinAnsiEncoding", "/MacRomanEncoding"}) {
      QPDFObjectHandle made = named(base_name);
      if (!made.isNull() && attempt(made, flags))
        return true;
    }
  return symbolic_cmaps && attempt(QPDFObjectHandle::newNull(), as_symbolic);
}

bool GlyphLists(QPDF &q, QPDFObjectHandle font, const Checker &checker) {
  if (!checker.Ready() || !checker.Font())
    return false;
  const Program &program = *checker.Font();
  const std::string subtype = NameOf(font.getKey("/Subtype"));
  QPDFObjectHandle descriptor = DescriptorOf(font);
  if (!descriptor.isDictionary())
    return false;
  if ((subtype == "/Type1" || subtype == "/MMType1") && !program.Sfnt()) {
    std::string names;
    for (int glyph = 0; glyph < program.Glyphs(); ++glyph) {
      const std::string name = program.GlyphName(glyph);
      if (!name.empty() && name != ".notdef")
        names += "/" + name;
    }
    descriptor.replaceKey("/CharSet", QPDFObjectHandle::newString(names));
    return true;
  }
  if (subtype != "/Type0")
    return false;
  QPDFObjectHandle descendant = DescendantOf(font);
  std::vector<unsigned> cids;
  QPDFObjectHandle map = descendant.getKey("/CIDToGIDMap");
  std::string bytes;
  if (program.CidKeyed()) {
    cids = program.Cids();
  } else if (IsName(descendant.getKey("/Subtype"), "/CIDFontType2") && map.isStream() && Decode(map, bytes)) {
    for (size_t cid = 0; cid * 2 + 1 < bytes.size(); ++cid) {
      const unsigned glyph = static_cast<unsigned char>(bytes[cid * 2]) << 8 | static_cast<unsigned char>(bytes[cid * 2 + 1]);
      if (glyph < static_cast<unsigned>(program.Glyphs()) && (glyph != 0 || cid == 0))
        cids.push_back(static_cast<unsigned>(cid));
    }
  } else {
    for (int glyph = 0; glyph < program.Glyphs(); ++glyph)
      cids.push_back(static_cast<unsigned>(glyph));
  }
  if (cids.empty())
    return false;
  const unsigned highest = *std::max_element(cids.begin(), cids.end());
  std::string set((highest + 8) / 8, '\0');
  for (const unsigned cid : cids)
    set[cid / 8] = static_cast<char>(static_cast<unsigned char>(set[cid / 8]) | (0x80u >> (cid % 8)));
  descriptor.replaceKey("/CIDSet", QPDFObjectHandle::newStream(&q, set));
  return true;
}

} // namespace fynepdf::pdfa::font
