#include "font_program.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <regex>

namespace fynepdf::pdfa::font {
namespace {

#include "font_tables.inc"

unsigned U8(const std::string &d, size_t at) { return at < d.size() ? static_cast<unsigned char>(d[at]) : 0; }
unsigned U16(const std::string &d, size_t at) { return U8(d, at) << 8 | U8(d, at + 1); }
uint32_t U32(const std::string &d, size_t at) { return static_cast<uint32_t>(U16(d, at)) << 16 | U16(d, at + 2); }

using Range = std::pair<size_t, size_t>;  // offset, length

// A CFF INDEX at |at|: its entries, and where it ends.
bool Index(const std::string &d, size_t at, std::vector<Range> &out, size_t &end) {
  if (at + 2 > d.size())
    return false;
  const unsigned count = U16(d, at);
  if (count == 0) {
    end = at + 2;
    return true;
  }
  const unsigned size = U8(d, at + 2);
  if (size < 1 || size > 4)
    return false;
  const size_t offsets = at + 3;
  if (offsets + static_cast<size_t>(count + 1) * size > d.size())
    return false;
  const size_t base = offsets + static_cast<size_t>(count + 1) * size - 1;
  const auto offset = [&](unsigned i) {
    size_t value = 0;
    for (unsigned k = 0; k < size; ++k)
      value = value << 8 | U8(d, offsets + static_cast<size_t>(i) * size + k);
    return value;
  };
  size_t previous = offset(0);
  for (unsigned i = 1; i <= count; ++i) {
    const size_t next = offset(i);
    if (next < previous || base + next > d.size())
      return false;
    out.push_back({base + previous, next - previous});
    previous = next;
  }
  end = base + previous;
  return true;
}

// A CFF DICT: each operator (escaped ones as 1200 + the second byte) with its operands.
using Dict = std::map<int, std::vector<double>>;

Dict ReadDict(const std::string &d, Range range) {
  Dict out;
  std::vector<double> operands;
  size_t at = range.first;
  const size_t end = std::min(d.size(), range.first + range.second);
  while (at < end) {
    const unsigned b = U8(d, at);
    if (b <= 21) {
      int op = static_cast<int>(b);
      ++at;
      if (b == 12)
        op = 1200 + static_cast<int>(U8(d, at++));
      out[op] = operands;
      operands.clear();
    } else if (b == 28) {
      operands.push_back(static_cast<int16_t>(U16(d, at + 1)));
      at += 3;
    } else if (b == 29) {
      operands.push_back(static_cast<int32_t>(U32(d, at + 1)));
      at += 5;
    } else if (b == 30) {
      std::string text;
      bool done = false;
      for (++at; at < end && !done; ++at) {
        const unsigned byte = U8(d, at);
        for (const unsigned nibble : {byte >> 4, byte & 15u}) {
          if (nibble <= 9)
            text += static_cast<char>('0' + nibble);
          else if (nibble == 10)
            text += '.';
          else if (nibble == 11)
            text += 'E';
          else if (nibble == 12)
            text += "E-";
          else if (nibble == 14)
            text += '-';
          else if (nibble == 15) {
            done = true;
            break;
          }
        }
      }
      operands.push_back(std::strtod(text.c_str(), nullptr));
    } else if (b >= 32 && b <= 246) {
      operands.push_back(static_cast<int>(b) - 139);
      ++at;
    } else if (b >= 247 && b <= 250) {
      operands.push_back((static_cast<int>(b) - 247) * 256 + static_cast<int>(U8(d, at + 1)) + 108);
      at += 2;
    } else if (b >= 251 && b <= 254) {
      operands.push_back(-(static_cast<int>(b) - 251) * 256 - static_cast<int>(U8(d, at + 1)) - 108);
      at += 2;
    } else {
      ++at;
    }
  }
  return out;
}

double First(const Dict &dict, int op, double fallback) {
  const auto found = dict.find(op);
  return found == dict.end() || found->second.empty() ? fallback : found->second[0];
}

// Type 1 encryption (eexec with 55665, charstrings with 4330), the first |skip| bytes dropped.
std::string Decrypt(const std::string &data, unsigned short r, int skip) {
  std::string out;
  out.reserve(data.size());
  for (const char c : data) {
    const unsigned char cipher = static_cast<unsigned char>(c);
    out += static_cast<char>(cipher ^ (r >> 8));
    r = static_cast<unsigned short>((cipher + r) * 52845u + 22719u);
  }
  return skip > 0 ? (static_cast<size_t>(skip) < out.size() ? out.substr(static_cast<size_t>(skip)) : std::string()) : out;
}

// Whether a Type 1 charstring draws nothing: no path operator, no subroutine or accent call.
bool Type1Blank(const std::string &charstring) {
  for (size_t at = 0; at < charstring.size();) {
    const unsigned v = static_cast<unsigned char>(charstring[at]);
    if (v >= 32) {
      at += v <= 246 ? 1 : v <= 254 ? 2 : 5;
      continue;
    }
    if (v == 12) {
      const unsigned escaped = U8(charstring, at + 1);
      if (escaped == 6 || escaped == 16)  // seac, callothersubr
        return false;
      at += 2;
      continue;
    }
    if (v == 4 || v == 5 || v == 6 || v == 7 || v == 8 || v == 9 || v == 10 || v == 21 || v == 22 || v == 30 || v == 31)
      return false;
    ++at;
  }
  return true;
}

// A Type 1 charstring's advance width (hsbw or sbw, its first command); negative when absent.
double Type1Width(const std::string &charstring) {
  std::vector<double> stack;
  for (size_t at = 0; at < charstring.size();) {
    const unsigned v = static_cast<unsigned char>(charstring[at]);
    if (v >= 32 && v <= 246) {
      stack.push_back(static_cast<int>(v) - 139);
      ++at;
    } else if (v >= 247 && v <= 250) {
      stack.push_back((static_cast<int>(v) - 247) * 256 + static_cast<int>(U8(charstring, at + 1)) + 108);
      at += 2;
    } else if (v >= 251 && v <= 254) {
      stack.push_back(-(static_cast<int>(v) - 251) * 256 - static_cast<int>(U8(charstring, at + 1)) - 108);
      at += 2;
    } else if (v == 255) {
      stack.push_back(static_cast<int32_t>(U32(charstring, at + 1)));
      at += 5;
    } else if (v == 13) {
      return stack.size() >= 2 ? stack[stack.size() - 1] : -1;
    } else if (v == 12 && U8(charstring, at + 1) == 12 && stack.size() >= 2) {
      // div: TeX's fonts give fractional widths as a quotient.
      const double divisor = stack.back();
      stack.pop_back();
      stack.back() = divisor != 0 ? stack.back() / divisor : 0;
      at += 2;
    } else if (v == 12) {
      return U8(charstring, at + 1) == 7 && stack.size() >= 4 ? stack[stack.size() - 2] : -1;
    } else {
      return -1;
    }
  }
  return -1;
}

} // namespace

std::unique_ptr<Program> Program::Read(const std::string &data, Container container, int length1) {
  auto program = std::unique_ptr<Program>(new Program());
  bool ok = false;
  try {
    switch (container) {
    case Container::Type1:
      ok = program->ReadType1(data, length1);
      break;
    case Container::TrueType:
      ok = program->ReadSfnt(data);
      break;
    case Container::Cff:
      ok = program->ReadCff(data, 0, data.size());
      break;
    case Container::OpenType:
      // OpenType is an sfnt; some writers put bare CFF there.
      ok = data.compare(0, 4, "OTTO") == 0 || data.compare(0, 4, "true") == 0 || U32(data, 0) == 0x00010000 ? program->ReadSfnt(data)
                                                                                                             : program->ReadCff(data, 0, data.size());
      break;
    }
  } catch (const std::exception &) {
    ok = false;
  }
  return ok ? std::move(program) : nullptr;
}

bool Program::ReadSfnt(const std::string &d) {
  sfnt_ = true;
  data_ = d;
  size_t base = 0;
  if (d.compare(0, 4, "ttcf") == 0)
    base = U32(d, 12);  // the first font of a collection
  const unsigned tables = U16(d, base + 4);
  std::map<std::string, Range> directory;
  for (unsigned i = 0; i < tables && i < 512; ++i) {
    const size_t record = base + 12 + static_cast<size_t>(i) * 16;
    if (record + 16 > d.size())
      break;
    const size_t offset = U32(d, record + 8), length = U32(d, record + 12);
    if (offset < d.size())
      directory[d.substr(record, 4)] = {offset, std::min(length, d.size() - offset)};
  }
  tables_ = directory;
  const auto table = [&](const char *tag) {
    const auto found = directory.find(tag);
    return found == directory.end() ? Range{0, 0} : found->second;
  };
  const Range head = table("head"), maxp = table("maxp"), hhea = table("hhea"), hmtx = table("hmtx"), cmap = table("cmap"), post = table("post");
  if (head.second >= 20 && U16(d, head.first + 18) > 0)
    units_per_em_ = U16(d, head.first + 18);
  if (maxp.second >= 6)
    glyphs_ = static_cast<int>(U16(d, maxp.first + 4));
  const Range cff = table("CFF ");
  if (cff.second > 0) {
    const int sfnt_glyphs = glyphs_;
    if (!ReadCff(d, cff.first, cff.second))
      return false;
    if (sfnt_glyphs > 0)
      glyphs_ = std::min(glyphs_, sfnt_glyphs);
  }
  if (glyphs_ <= 0)
    return false;
  if (hhea.second >= 36 && hmtx.second > 0) {
    const unsigned metrics = std::max(1u, U16(d, hhea.first + 34));
    advances_.resize(static_cast<size_t>(glyphs_));
    unsigned last = 0;
    for (int glyph = 0; glyph < glyphs_; ++glyph) {
      if (static_cast<unsigned>(glyph) < metrics && hmtx.first + static_cast<size_t>(glyph) * 4 + 2 <= hmtx.first + hmtx.second)
        last = U16(d, hmtx.first + static_cast<size_t>(glyph) * 4);
      advances_[static_cast<size_t>(glyph)] = last;
    }
  }
  if (cmap.second >= 4) {
    const unsigned count = U16(d, cmap.first + 2);
    for (unsigned k = 0; k < count && k < 64; ++k) {
      const size_t record = cmap.first + 4 + static_cast<size_t>(k) * 8;
      const size_t offset = cmap.first + U32(d, record + 4);
      if (record + 8 <= d.size() && offset + 2 <= d.size())
        cmaps_.emplace(std::make_pair(U16(d, record), U16(d, record + 2)), offset);
    }
  }
  // Glyph names from the post table (CFF outlines name their own glyphs), when first asked for.
  if (!cff_ && post.second >= 32) {
    post_at_ = post.first;
    post_size_ = post.second;
  }
  return true;
}

void Program::PostNames() const {
  std::call_once(post_read_, [this]() {
    if (!post_size_)
      return;
    const std::string &d = data_;
    const std::pair<size_t, size_t> post{post_at_, post_size_};
    const uint32_t version = U32(d, post.first);
    names_.assign(static_cast<size_t>(glyphs_), "");
    if (version == 0x00010000) {
      for (int glyph = 0; glyph < glyphs_ && glyph < 258; ++glyph)
        names_[static_cast<size_t>(glyph)] = kMacGlyphNames[glyph];
    } else if (version == 0x00020000 && post.second >= 34) {
      const unsigned count = U16(d, post.first + 32);
      std::vector<std::string> custom;
      size_t at = post.first + 34 + static_cast<size_t>(count) * 2;
      while (at < post.first + post.second && at < d.size()) {
        const unsigned length = U8(d, at);
        custom.push_back(d.substr(at + 1, length));
        at += 1 + length;
      }
      for (unsigned glyph = 0; glyph < count && glyph < static_cast<unsigned>(glyphs_); ++glyph) {
        const unsigned index = U16(d, post.first + 34 + static_cast<size_t>(glyph) * 2);
        names_[glyph] = index < 258 ? kMacGlyphNames[index] : index - 258 < custom.size() ? custom[index - 258] : "";
      }
    }
    for (int glyph = 0; glyph < glyphs_; ++glyph)
      if (!names_[static_cast<size_t>(glyph)].empty())
        by_name_.emplace(names_[static_cast<size_t>(glyph)], glyph);
  });
}

bool Program::ReadCff(const std::string &d, size_t at, size_t length) {
  cff_ = true;
  cff_data_ = d.substr(at, length);
  const std::string &c = cff_data_;
  if (c.size() < 4)
    return false;
  std::vector<Range> names, tops, strings;
  size_t end = 0;
  if (!Index(c, U8(c, 2), names, end) || !Index(c, end, tops, end) || tops.empty() || !Index(c, end, strings, end) ||
      !Index(c, end, global_subrs_, end))
    return false;
  const Dict top = ReadDict(c, tops[0]);
  top_matrix_ = top.count(1207) > 0 && First(top, 1207, 0) != 0;
  font_matrix_ = top_matrix_ ? First(top, 1207, 0.001) : 0.001;
  cid_keyed_ = top.count(1230) > 0;
  if (!top.count(17) || !Index(c, static_cast<size_t>(First(top, 17, 0)), charstrings_, end))
    return false;
  glyphs_ = static_cast<int>(charstrings_.size());
  const auto sid_name = [&](unsigned sid) -> std::string {
    if (sid < 391)
      return kCffStandardStrings[sid];
    sid -= 391;
    return sid < strings.size() ? c.substr(strings[sid].first, strings[sid].second) : std::string();
  };
  // The charset: each glyph's name (SID), or its CID.
  std::vector<unsigned> ids(static_cast<size_t>(glyphs_), 0);
  const size_t charset = static_cast<size_t>(First(top, 15, 0));
  if (charset <= 2) {
    for (int glyph = 0; glyph < glyphs_; ++glyph)
      ids[static_cast<size_t>(glyph)] = static_cast<unsigned>(glyph);  // ISOAdobe (the expert ones too, roughly)
  } else {
    const unsigned format = U8(c, charset);
    size_t p = charset + 1;
    int glyph = 1;
    if (format == 0) {
      while (glyph < glyphs_ && p + 2 <= c.size()) {
        ids[static_cast<size_t>(glyph++)] = U16(c, p);
        p += 2;
      }
    } else if (format == 1 || format == 2) {
      while (glyph < glyphs_ && p + (format == 1 ? 3 : 4) <= c.size()) {
        const unsigned first = U16(c, p), left = format == 1 ? U8(c, p + 2) : U16(c, p + 2);
        p += format == 1 ? 3 : 4;
        for (unsigned k = 0; k <= left && glyph < glyphs_; ++k)
          ids[static_cast<size_t>(glyph++)] = first + k;
      }
    }
  }
  if (cid_keyed_) {
    cids_ = ids;
    for (int glyph = 0; glyph < glyphs_; ++glyph)
      by_cid_.emplace(ids[static_cast<size_t>(glyph)], glyph);
  } else {
    names_.resize(static_cast<size_t>(glyphs_));
    for (int glyph = 0; glyph < glyphs_; ++glyph) {
      names_[static_cast<size_t>(glyph)] = sid_name(ids[static_cast<size_t>(glyph)]);
      by_name_.emplace(names_[static_cast<size_t>(glyph)], glyph);
    }
  }
  const auto read_private = [&](const Dict &dict) {
    Private p;
    const auto found = dict.find(18);
    if (found != dict.end() && found->second.size() >= 2) {
      const size_t size = static_cast<size_t>(found->second[0]), offset = static_cast<size_t>(found->second[1]);
      const Dict own = ReadDict(c, {offset, size});
      // Whole numbers, as veraPDF reads them: a fractional nominal width (TeX fonts converted to
      // CFF have them) would otherwise give widths veraPDF disagrees with.
      p.default_width = static_cast<long>(First(own, 20, 0));
      p.nominal_width = static_cast<long>(First(own, 21, 0));
      if (own.count(19)) {
        size_t subrs_end = 0;
        Index(c, offset + static_cast<size_t>(First(own, 19, 0)), p.subrs, subrs_end);
      }
    }
    return p;
  };
  if (cid_keyed_) {
    std::vector<Range> fonts;
    if (top.count(1236) && Index(c, static_cast<size_t>(First(top, 1236, 0)), fonts, end))
      for (const Range &font : fonts) {
        const Dict dict = ReadDict(c, font);
        Private p = read_private(dict);
        p.font_matrix = First(dict, 1207, 0);
        privates_.push_back(p);
      }
    if (privates_.empty())
      privates_.push_back(Private{});
    fd_select_.assign(static_cast<size_t>(glyphs_), 0);
    if (top.count(1237)) {
      const size_t select = static_cast<size_t>(First(top, 1237, 0));
      const unsigned format = U8(c, select);
      if (format == 0) {
        for (int glyph = 0; glyph < glyphs_; ++glyph)
          fd_select_[static_cast<size_t>(glyph)] = static_cast<unsigned char>(U8(c, select + 1 + static_cast<size_t>(glyph)));
      } else if (format == 3) {
        const unsigned ranges = U16(c, select + 1);
        for (unsigned r = 0; r < ranges; ++r) {
          const size_t record = select + 3 + static_cast<size_t>(r) * 3;
          const unsigned first = U16(c, record), fd = U8(c, record + 2), next = U16(c, record + 3);
          for (unsigned glyph = first; glyph < next && glyph < static_cast<unsigned>(glyphs_); ++glyph)
            fd_select_[glyph] = static_cast<unsigned char>(fd);
        }
      }
    }
  } else {
    privates_.push_back(read_private(top));
    // Its own encoding (0: the standard one, 1: the expert one).
    const size_t encoding = static_cast<size_t>(First(top, 16, 0));
    if (encoding > 1) {
      own_encoding_ = true;
      const unsigned format = U8(c, encoding);
      size_t p = encoding + 1;
      int glyph = 1;
      if ((format & 0x7f) == 0) {
        const unsigned count = U8(c, p++);
        for (unsigned i = 0; i < count && glyph < glyphs_; ++i)
          encoding_[U8(c, p + i)] = names_[static_cast<size_t>(glyph++)];
        p += count;
      } else if ((format & 0x7f) == 1) {
        const unsigned ranges = U8(c, p++);
        for (unsigned r = 0; r < ranges; ++r, p += 2)
          for (unsigned k = 0; k <= U8(c, p + 1) && glyph < glyphs_; ++k)
            encoding_[U8(c, p) + k] = names_[static_cast<size_t>(glyph++)];
      }
      if (format & 0x80) {
        const unsigned supplements = U8(c, p++);
        for (unsigned s = 0; s < supplements; ++s, p += 3)
          encoding_[U8(c, p)] = sid_name(U16(c, p + 1));
      }
    } else if (encoding == 1) {
      own_encoding_ = true;
      for (unsigned code = 0; code < 256; ++code)
        if (*kMacExpertEncoding[code])
          encoding_[code] = kMacExpertEncoding[code];
    }
  }
  return glyphs_ > 0;
}

bool Program::ReadType1(const std::string &d, int length1) {
  std::string clear, encrypted;
  if (U8(d, 0) == 0x80 && U8(d, 1) == 1) {
    // PFB segments.
    for (size_t at = 0; at + 6 <= d.size() && U8(d, at) == 0x80;) {
      const unsigned type = U8(d, at + 1);
      if (type == 3)
        break;
      const size_t length = U8(d, at + 2) | U8(d, at + 3) << 8 | U8(d, at + 4) << 16 | static_cast<size_t>(U8(d, at + 5)) << 24;
      (type == 1 && encrypted.empty() ? clear : encrypted) += d.substr(at + 6, length);
      at += 6 + length;
    }
  } else {
    const size_t eexec = d.find("eexec");
    if (eexec == std::string::npos)
      return false;
    size_t split = eexec + 5;
    if (length1 > 0 && static_cast<size_t>(length1) >= split && static_cast<size_t>(length1) <= split + 4)
      split = static_cast<size_t>(length1);
    else
      while (split < d.size() && (d[split] == '\r' || d[split] == '\n' || d[split] == ' ' || d[split] == '\t'))
        ++split;
    clear = d.substr(0, split);
    encrypted = d.substr(split);
  }
  // The encrypted part written as hex.
  if (encrypted.size() >= 4 && std::all_of(encrypted.begin(), encrypted.begin() + 4, [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)); })) {
    std::string bytes;
    int high = -1;
    for (const char ch : encrypted) {
      if (!std::isxdigit(static_cast<unsigned char>(ch)))
        continue;
      const int value = std::isdigit(static_cast<unsigned char>(ch)) ? ch - '0' : (std::tolower(static_cast<unsigned char>(ch)) - 'a' + 10);
      if (high < 0) {
        high = value;
      } else {
        bytes += static_cast<char>(high << 4 | value);
        high = -1;
      }
    }
    encrypted.swap(bytes);
  }
  const std::string plain = Decrypt(encrypted, 55665, 4);
  std::smatch match;
  static const std::regex matrix(R"(/FontMatrix\s*\[\s*([-+0-9.eE]+))"), len_iv(R"(/lenIV\s+(-?\d+))");
  if (std::regex_search(clear, match, matrix) && std::strtod(match[1].str().c_str(), nullptr) != 0)
    font_matrix_ = std::strtod(match[1].str().c_str(), nullptr);
  // Its own encoding: "dup <code> /<name> put" lines, unless it's the standard one.
  const size_t encoding = clear.find("/Encoding");
  if (encoding != std::string::npos && clear.compare(encoding, 27, "/Encoding StandardEncoding") != 0) {
    static const std::regex entry(R"(dup\s+(\d+)\s*/([^\s/\[\]{}()<>%]+)\s+put)");
    const std::string tail = clear.substr(encoding);
    for (auto it = std::sregex_iterator(tail.begin(), tail.end(), entry); it != std::sregex_iterator(); ++it) {
      const unsigned code = static_cast<unsigned>(std::strtoul((*it)[1].str().c_str(), nullptr, 10));
      if (code < 256)
        encoding_[code] = (*it)[2].str();
    }
    own_encoding_ = !encoding_.empty();
  }
  const int skip = std::regex_search(plain, match, len_iv) ? std::atoi(match[1].str().c_str()) : 4;
  size_t at = plain.find("/CharStrings");
  if (at == std::string::npos)
    return false;
  at = plain.find("begin", at);
  if (at == std::string::npos)
    return false;
  at += 5;
  while (at < plain.size()) {
    // "/name length RD <bytes> ND"; "end" closes the dictionary.
    while (at < plain.size() && std::isspace(static_cast<unsigned char>(plain[at])))
      ++at;
    if (at >= plain.size() || plain[at] != '/')
      break;
    size_t name_end = at + 1;
    while (name_end < plain.size() && !std::isspace(static_cast<unsigned char>(plain[name_end])))
      ++name_end;
    const std::string name = plain.substr(at + 1, name_end - at - 1);
    size_t p = name_end;
    while (p < plain.size() && std::isspace(static_cast<unsigned char>(plain[p])))
      ++p;
    size_t digits = p;
    while (digits < plain.size() && std::isdigit(static_cast<unsigned char>(plain[digits])))
      ++digits;
    if (digits == p)
      break;
    const size_t length = std::strtoul(plain.substr(p, digits - p).c_str(), nullptr, 10);
    p = digits;
    while (p < plain.size() && std::isspace(static_cast<unsigned char>(plain[p])))
      ++p;
    while (p < plain.size() && !std::isspace(static_cast<unsigned char>(plain[p])))
      ++p;  // RD or -|
    ++p;    // the one space before the bytes
    if (p + length > plain.size())
      break;
    const std::string charstring = skip >= 0 ? Decrypt(plain.substr(p, length), 4330, skip) : plain.substr(p, length);
    by_name_.emplace(name, static_cast<int>(names_.size()));
    names_.push_back(name);
    type1_widths_.push_back(Type1Width(charstring));
    type1_blank_.push_back(Type1Blank(charstring));
    at = p + length;
    // ND or |- (or "noaccess def") up to the next name; "end" closes the dictionary.
    bool closed = false;
    while (at < plain.size()) {
      while (at < plain.size() && std::isspace(static_cast<unsigned char>(plain[at])))
        ++at;
      if (at >= plain.size() || plain[at] == '/')
        break;
      size_t token = at;
      while (token < plain.size() && !std::isspace(static_cast<unsigned char>(plain[token])) && plain[token] != '/')
        ++token;
      if (plain.compare(at, token - at, "end") == 0) {
        closed = true;
        break;
      }
      at = token;
    }
    if (closed)
      break;
  }
  glyphs_ = static_cast<int>(names_.size());
  return glyphs_ > 0;
}

int Program::ByName(const std::string &name) const {
  PostNames();
  const auto found = by_name_.find(name);
  return found == by_name_.end() ? -1 : found->second;
}

bool Program::HasCmap(unsigned platform, unsigned encoding) const { return cmaps_.count({platform, encoding}) > 0; }

bool Program::HasUnicodeCmap() const {
  return std::any_of(cmaps_.begin(), cmaps_.end(), [](const auto &entry) { return entry.first.first == 0; });
}

std::string Program::Table(const std::string &tag) const {
  const auto found = tables_.find(tag);
  return found == tables_.end() ? std::string() : data_.substr(found->second.first, found->second.second);
}

int Program::ByUnicode(unsigned character) const {
  for (const auto &[key, at] : cmaps_)
    if (key.first == 0 || (key.first == 3 && (key.second == 1 || key.second == 10)))
      if (const int glyph = ByCmap(key.first, key.second, character); glyph > 0)
        return glyph;
  return 0;
}

std::map<int, unsigned> Program::UnicodeByGlyph() const {
  std::map<int, unsigned> out;
  const std::string &d = data_;
  for (const auto &[key, at] : cmaps_) {
    if (!(key.first == 0 || (key.first == 3 && (key.second == 1 || key.second == 10))))
      continue;
    const unsigned format = U16(d, at);
    if (format == 4) {
      const unsigned segments = U16(d, at + 6) / 2;
      const size_t ends = at + 14, starts = ends + segments * 2 + 2, deltas = starts + segments * 2, ranges = deltas + segments * 2;
      for (unsigned s = 0; s < segments; ++s) {
        const unsigned end = U16(d, ends + s * 2), start = U16(d, starts + s * 2), delta = U16(d, deltas + s * 2), range = U16(d, ranges + s * 2);
        for (unsigned code = start; code <= end && code != 0xffff; ++code) {
          unsigned glyph;
          if (range == 0) {
            glyph = (code + delta) & 0xffff;
          } else {
            const unsigned value = U16(d, ranges + s * 2 + range + (code - start) * 2);
            glyph = value ? (value + delta) & 0xffff : 0;
          }
          if (glyph)
            out.emplace(static_cast<int>(glyph), code);
        }
      }
    } else if (format == 12) {
      const uint32_t groups = U32(d, at + 12);
      for (uint32_t g = 0; g < groups && g < 100000; ++g) {
        const size_t record = at + 16 + static_cast<size_t>(g) * 12;
        const uint32_t start = U32(d, record), stop = std::min<uint32_t>(U32(d, record + 4), start + 0x10000);
        for (uint32_t code = start; code <= stop; ++code)
          out.emplace(static_cast<int>(U32(d, record + 8) + (code - start)), code);
      }
    }
  }
  return out;
}

std::vector<std::pair<unsigned, unsigned>> Program::Cmaps() const {
  std::vector<std::pair<unsigned, unsigned>> out;
  for (const auto &entry : cmaps_)
    out.push_back(entry.first);
  return out;
}

int Program::ByCmap(unsigned platform, unsigned encoding, unsigned code) const {
  const auto found = cmaps_.find({platform, encoding});
  if (found == cmaps_.end())
    return -1;
  const std::string &d = data_;
  const size_t at = found->second;
  const unsigned format = U16(d, at);
  unsigned glyph = 0;
  if (format == 0) {
    glyph = code < 256 ? U8(d, at + 6 + code) : 0;
  } else if (format == 4) {
    const unsigned segments = U16(d, at + 6) / 2;
    const size_t ends = at + 14, starts = ends + segments * 2 + 2, deltas = starts + segments * 2, ranges = deltas + segments * 2;
    for (unsigned s = 0; s < segments; ++s) {
      if (U16(d, ends + s * 2) < code)
        continue;
      const unsigned start = U16(d, starts + s * 2);
      if (start > code)
        break;
      const unsigned delta = U16(d, deltas + s * 2), range = U16(d, ranges + s * 2);
      if (range == 0) {
        glyph = (code + delta) & 0xffff;
      } else {
        const unsigned value = U16(d, ranges + s * 2 + range + (code - start) * 2);
        glyph = value ? (value + delta) & 0xffff : 0;
      }
      break;
    }
  } else if (format == 6) {
    const unsigned first = U16(d, at + 6), count = U16(d, at + 8);
    glyph = code >= first && code - first < count ? U16(d, at + 10 + (code - first) * 2) : 0;
  } else if (format == 12) {
    const uint32_t groups = U32(d, at + 12);
    for (uint32_t g = 0; g < groups && g < 100000; ++g) {
      const size_t record = at + 16 + static_cast<size_t>(g) * 12;
      const uint32_t start = U32(d, record), stop = U32(d, record + 4);
      if (code >= start && code <= stop) {
        glyph = U32(d, record + 8) + (code - start);
        break;
      }
    }
  }
  return static_cast<int>(glyph);
}

int Program::ByCid(unsigned cid) const {
  if (cid_keyed_) {
    const auto found = by_cid_.find(cid);
    return found == by_cid_.end() ? -1 : found->second;
  }
  return cid < static_cast<unsigned>(glyphs_) ? static_cast<int>(cid) : -1;
}

std::string Program::Encoded(unsigned code) const {
  if (code > 255)
    return "";
  if (own_encoding_) {
    const auto found = encoding_.find(code);
    return found == encoding_.end() ? "" : found->second;
  }
  return kStandardEncoding[code];
}

double Program::Width(int glyph) const {
  if (glyph < 0 || glyph >= glyphs_)
    return -1;
  if (!type1_widths_.empty()) {
    const double width = type1_widths_[static_cast<size_t>(glyph)];
    return width < 0 ? -1 : width * font_matrix_ * 1000;
  }
  if (!advances_.empty())
    return advances_[static_cast<size_t>(glyph)] * 1000.0 / units_per_em_;
  if (cff_)
    return CffWidth(glyph);
  return -1;
}

double Program::CffWidth(int glyph) const {
  const std::string &c = cff_data_;
  const size_t fd = cid_keyed_ && static_cast<size_t>(glyph) < fd_select_.size() ? fd_select_[static_cast<size_t>(glyph)] : 0;
  const Private &p = privates_[fd < privates_.size() ? fd : 0];
  double width = p.default_width;
  std::vector<double> stack;
  const auto bias = [](size_t count) { return count < 1240 ? 107 : count < 33900 ? 1131 : 32768; };
  // Runs until the first operator that clears the stack: a width before its arguments is
  // the glyph's (nominal width plus it). True when that point was reached.
  std::function<bool(Range, int)> run = [&](Range range, int depth) {
    size_t at = range.first;
    const size_t end = std::min(c.size(), range.first + range.second);
    while (at < end) {
      const unsigned b = U8(c, at);
      if (b == 28) {
        stack.push_back(static_cast<int16_t>(U16(c, at + 1)));
        at += 3;
        continue;
      }
      if (b >= 32) {
        if (b <= 246) {
          stack.push_back(static_cast<int>(b) - 139);
          ++at;
        } else if (b <= 250) {
          stack.push_back((static_cast<int>(b) - 247) * 256 + static_cast<int>(U8(c, at + 1)) + 108);
          at += 2;
        } else if (b <= 254) {
          stack.push_back(-(static_cast<int>(b) - 251) * 256 - static_cast<int>(U8(c, at + 1)) - 108);
          at += 2;
        } else {
          stack.push_back(static_cast<int32_t>(U32(c, at + 1)) / 65536.0);
          at += 5;
        }
        continue;
      }
      ++at;
      if (b == 10 || b == 29) {
        if (stack.empty() || depth > 10)
          return true;
        const std::vector<Range> &subrs = b == 10 ? p.subrs : global_subrs_;
        const long index = static_cast<long>(stack.back()) + bias(subrs.size());
        stack.pop_back();
        if (index < 0 || static_cast<size_t>(index) >= subrs.size())
          return true;
        if (run(subrs[static_cast<size_t>(index)], depth + 1))
          return true;
        continue;
      }
      if (b == 11)
        return false;  // back from a subroutine
      const size_t n = stack.size();
      bool has_width = false;
      if (b == 1 || b == 3 || b == 18 || b == 23 || b == 19 || b == 20)
        has_width = n % 2 == 1;
      else if (b == 21)
        has_width = n > 2;
      else if (b == 4 || b == 22)
        has_width = n > 1;
      else if (b == 14)
        has_width = n == 1 || n == 5;
      if (has_width)
        width = p.nominal_width + stack[0];
      return true;
    }
    return false;
  };
  run(charstrings_[static_cast<size_t>(glyph)], 0);
  // A CID font's own matrix (in its FD) applies with the top one, which is identity when absent.
  double matrix = font_matrix_;
  if (cid_keyed_ && p.font_matrix != 0)
    matrix = top_matrix_ ? font_matrix_ * p.font_matrix : p.font_matrix;
  return width * matrix * 1000;
}

bool Program::Blank(int glyph) const {
  if (glyph < 0 || glyph >= glyphs_)
    return false;
  if (!type1_blank_.empty())
    return type1_blank_[static_cast<size_t>(glyph)];
  if (cff_) {
    // A Type 2 charstring with no path operator, subroutine call or flex.
    const Range range = charstrings_[static_cast<size_t>(glyph)];
    const std::string &c = cff_data_;
    for (size_t at = range.first; at < range.first + range.second && at < c.size();) {
      const unsigned b = U8(c, at);
      if (b == 28) {
        at += 3;
        continue;
      }
      if (b >= 32) {
        at += b <= 246 ? 1 : b <= 254 ? 2 : 5;
        continue;
      }
      if (b == 12)
        return false;  // flex and the other escaped operators draw
      if (b == 4 || b == 5 || b == 6 || b == 7 || b == 8 || b == 10 || b == 21 || b == 22 || b == 24 || b == 25 || b == 26 || b == 27 || b == 29 ||
          b == 30 || b == 31)
        return false;
      if (b == 14)
        return true;  // endchar
      if (b == 19 || b == 20) {
        return false;  // a hint mask's length depends on the hints: not worth reading here
      }
      ++at;
    }
    return true;
  }
  // TrueType: an empty glyf entry.
  const auto head = tables_.find("head"), loca = tables_.find("loca");
  if (head == tables_.end() || loca == tables_.end() || head->second.second < 54)
    return false;
  const bool long_offsets = U16(data_, head->second.first + 50) != 0;
  const size_t at = loca->second.first + static_cast<size_t>(glyph) * (long_offsets ? 4 : 2);
  if (at + (long_offsets ? 8 : 4) > loca->second.first + loca->second.second)
    return false;
  return long_offsets ? U32(data_, at) == U32(data_, at + 4) : U16(data_, at) == U16(data_, at + 2);
}

std::string Program::GlyphName(int glyph) const {
  PostNames();
  return glyph >= 0 && static_cast<size_t>(glyph) < names_.size() ? names_[static_cast<size_t>(glyph)] : "";
}

std::vector<unsigned> Program::Cids() const { return cids_; }

std::string Program::Describe() const {
  char text[160];
  std::snprintf(text, sizeof text, "matrix %g (top %d) units %g", font_matrix_, top_matrix_, units_per_em_);
  std::string out = text;
  for (const Private &p : privates_) {
    std::snprintf(text, sizeof text, "; default %g nominal %g fd-matrix %g subrs %zu", p.default_width, p.nominal_width, p.font_matrix, p.subrs.size());
    out += text;
  }
  return out;
}

namespace {

template <typename Entry, size_t N>
const Entry *Search(const Entry (&table)[N], const std::string &key) {
  const Entry *found = std::lower_bound(table, table + N, key, [](const Entry &entry, const std::string &k) { return std::strcmp(entry.name, k.c_str()) < 0; });
  return found != table + N && key == found->name ? found : nullptr;
}

// The Adobe Glyph List's character for a name, or a uniXXXX / uXXXX[XX] name's; 0 for others.
unsigned Listed(const std::string &name) {
  if (const GlyphUnicode *entry = Search(kGlyphList, name))
    return entry->code;
  const auto hex = [](const std::string &text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) && !std::islower(static_cast<unsigned char>(ch)); });
  };
  if (name.size() == 7 && name.compare(0, 3, "uni") == 0 && hex(name.substr(3)))
    return static_cast<unsigned>(std::strtoul(name.substr(3).c_str(), nullptr, 16));
  if (name.size() >= 5 && name.size() <= 7 && name[0] == 'u' && hex(name.substr(1)))
    return static_cast<unsigned>(std::strtoul(name.substr(1).c_str(), nullptr, 16));
  return 0;
}

} // namespace

unsigned UnicodeOf(const std::string &name) {
  if (const unsigned code = Listed(name))
    return code;
  const size_t dot = name.find('.');
  if (dot != std::string::npos && dot > 0)
    return UnicodeOf(name.substr(0, dot));
  // ITC Zapf Dingbats' names (a1, a2, ...).
  if (const GlyphUnicode *entry = Search(kDingbatsList, name))
    return entry->code;
  return 0;
}

bool InGlyphList(const std::string &name) { return Search(kGlyphList, name) != nullptr; }

std::string ListedName(const std::string &name) {
  if (InGlyphList(name))
    return name;
  const unsigned character = UnicodeOf(name);
  if (!character)
    return "";
  // The list's first name for that character.
  static const std::map<unsigned, std::string> names = [] {
    std::map<unsigned, std::string> by_character;
    for (const GlyphUnicode &entry : kGlyphList)
      by_character.emplace(entry.code, entry.name);
    return by_character;
  }();
  const auto found = names.find(character);
  return found == names.end() ? "" : found->second;
}

double StandardWidth(const std::string &font, const std::string &glyph) {
  const GlyphWidth *entry = font == "Symbol" ? Search(kSymbolWidths, glyph) : font == "ZapfDingbats" ? Search(kDingbatsWidths, glyph) : nullptr;
  return entry ? entry->width : -1;
}

const char *const *Encoding(const std::string &name) {
  if (name == "/StandardEncoding")
    return kStandardEncoding;
  if (name == "/WinAnsiEncoding")
    return kWinAnsiEncoding;
  if (name == "/MacRomanEncoding")
    return kMacRomanEncoding;
  if (name == "/MacExpertEncoding")
    return kMacExpertEncoding;
  if (name == "Symbol")
    return kSymbolEncoding;
  if (name == "ZapfDingbats")
    return kZapfDingbatsEncoding;
  return nullptr;
}

int MacRomanCode(const std::string &name) {
  for (int code = 0; code < 256; ++code)
    if (name == kMacRomanEncoding[code])
      return code;
  return -1;
}

} // namespace fynepdf::pdfa::font

namespace fynepdf::pdfa::font {
namespace {

struct Table {
  std::string tag, data;
};

bool ReadTables(const std::string &d, std::vector<Table> &tables) {
  if (d.size() < 12 || d.compare(0, 4, "ttcf") == 0)
    return false;
  const unsigned count = U16(d, 4);
  for (unsigned i = 0; i < count && i < 512; ++i) {
    const size_t record = 12 + static_cast<size_t>(i) * 16;
    const size_t offset = U32(d, record + 8), length = U32(d, record + 12);
    if (record + 16 > d.size() || offset > d.size())
      return false;
    tables.push_back({d.substr(record, 4), d.substr(offset, std::min(length, d.size() - offset))});
  }
  return !tables.empty();
}

Table *Find(std::vector<Table> &tables, const char *tag) {
  for (Table &table : tables)
    if (table.tag == tag)
      return &table;
  return nullptr;
}

void Put16(std::string &out, size_t at, unsigned value) {
  out[at] = static_cast<char>(value >> 8);
  out[at + 1] = static_cast<char>(value & 0xff);
}

void Put32(std::string &out, size_t at, uint32_t value) {
  for (int k = 0; k < 4; ++k)
    out[at + static_cast<size_t>(k)] = static_cast<char>(value >> (24 - 8 * k));
}

// The tables written as an sfnt (the original's version), in their order, each on a 4-byte
// boundary with its checksum.
std::string WriteTables(const std::string &original, const std::vector<Table> &tables) {
  std::string out = original.substr(0, 4);
  out.append(8, '\0');
  const unsigned count = static_cast<unsigned>(tables.size());
  unsigned power = 1, log = 0;
  while (power * 2 <= count) {
    power *= 2;
    ++log;
  }
  Put16(out, 4, count);
  Put16(out, 6, power * 16);
  Put16(out, 8, log);
  Put16(out, 10, count * 16 - power * 16);
  out.append(tables.size() * 16, '\0');
  for (size_t i = 0; i < tables.size(); ++i) {
    const std::string &data = tables[i].data;
    const size_t offset = out.size();
    uint32_t sum = 0;
    for (size_t at = 0; at < data.size(); at += 4) {
      uint32_t word = 0;
      for (size_t k = 0; k < 4; ++k)
        word = word << 8 | (at + k < data.size() ? static_cast<unsigned char>(data[at + k]) : 0u);
      sum += word;
    }
    out += data;
    while (out.size() % 4)
      out += '\0';
    const size_t record = 12 + i * 16;
    out.replace(record, 4, tables[i].tag);
    Put32(out, record + 4, sum);
    Put32(out, record + 8, static_cast<uint32_t>(offset));
    Put32(out, record + 12, static_cast<uint32_t>(data.size()));
  }
  return out;
}

} // namespace

bool SetAdvances(std::string &d, const std::map<int, unsigned> &advances) {
  std::vector<Table> tables;
  if (!ReadTables(d, tables))
    return false;
  Table *hhea = Find(tables, "hhea"), *hmtx = Find(tables, "hmtx"), *maxp = Find(tables, "maxp");
  if (!hhea || !hmtx || !maxp || hhea->data.size() < 36 || maxp->data.size() < 6)
    return false;
  const unsigned glyphs = U16(maxp->data, 4), metrics = std::max(1u, U16(hhea->data, 34));
  if (glyphs == 0 || hmtx->data.size() < static_cast<size_t>(std::min(metrics, glyphs)) * 4)
    return false;
  std::vector<unsigned> widths(glyphs), sides(glyphs);
  for (unsigned g = 0; g < glyphs; ++g) {
    if (g < metrics) {
      widths[g] = U16(hmtx->data, static_cast<size_t>(g) * 4);
      sides[g] = U16(hmtx->data, static_cast<size_t>(g) * 4 + 2);
    } else {
      widths[g] = widths[metrics - 1];
      sides[g] = U16(hmtx->data, static_cast<size_t>(metrics) * 4 + static_cast<size_t>(g - metrics) * 2);
    }
  }
  for (const auto &[glyph, width] : advances)
    if (glyph >= 0 && static_cast<unsigned>(glyph) < glyphs)
      widths[static_cast<size_t>(glyph)] = std::min(width, 0xffffu);
  std::string table(static_cast<size_t>(glyphs) * 4, '\0');
  unsigned widest = 0;
  for (unsigned g = 0; g < glyphs; ++g) {
    Put16(table, static_cast<size_t>(g) * 4, widths[g]);
    Put16(table, static_cast<size_t>(g) * 4 + 2, sides[g]);
    widest = std::max(widest, widths[g]);
  }
  hmtx->data = table;
  Put16(hhea->data, 10, widest);  // advanceWidthMax
  Put16(hhea->data, 34, glyphs);  // numberOfHMetrics
  d = WriteTables(d, tables);
  return true;
}

bool SetCmap(std::string &d, unsigned platform, unsigned encoding, const std::map<unsigned, int> &glyphs) {
  std::vector<Table> tables;
  if (!ReadTables(d, tables))
    return false;
  Table *cmap = Find(tables, "cmap");
  if (!cmap) {
    // A program without one gets one (the directory stays in tag order).
    tables.push_back({"cmap", ""});
    std::sort(tables.begin(), tables.end(), [](const Table &a, const Table &b) { return a.tag < b.tag; });
    cmap = Find(tables, "cmap");
  }
  // Format 4: a segment for each run of characters whose glyphs follow on, and the closing one.
  std::vector<std::pair<unsigned, unsigned>> runs;  // first character, last
  for (const auto &[character, glyph] : glyphs) {
    if (character >= 0xffff)
      continue;
    if (!runs.empty() && character == runs.back().second + 1 &&
        static_cast<unsigned>(glyph) == static_cast<unsigned>(glyphs.at(runs.back().first)) + (character - runs.back().first)) {
      runs.back().second = character;
      continue;
    }
    runs.push_back({character, character});
  }
  const unsigned segments = static_cast<unsigned>(runs.size()) + 1;
  std::string sub(16 + static_cast<size_t>(segments) * 8, '\0');
  Put16(sub, 0, 4);
  Put16(sub, 2, static_cast<unsigned>(sub.size()));
  unsigned power = 1, log = 0;
  while (power * 2 <= segments) {
    power *= 2;
    ++log;
  }
  Put16(sub, 6, segments * 2);
  Put16(sub, 8, power * 2);
  Put16(sub, 10, log);
  Put16(sub, 12, segments * 2 - power * 2);
  const size_t ends = 14, starts = ends + segments * 2 + 2, deltas = starts + segments * 2, ranges = deltas + segments * 2;
  unsigned s = 0;
  for (const auto &[first, last] : runs) {
    Put16(sub, ends + s * 2, last);
    Put16(sub, starts + s * 2, first);
    Put16(sub, deltas + s * 2, (static_cast<unsigned>(glyphs.at(first)) - first) & 0xffff);
    Put16(sub, ranges + s * 2, 0);
    ++s;
  }
  Put16(sub, ends + s * 2, 0xffff);
  Put16(sub, starts + s * 2, 0xffff);
  Put16(sub, deltas + s * 2, 1);
  Put16(sub, ranges + s * 2, 0);
  std::string table(12, '\0');
  Put16(table, 2, 1);
  Put16(table, 4, platform);
  Put16(table, 6, encoding);
  Put32(table, 8, 12);
  cmap->data = table + sub;
  d = WriteTables(d, tables);
  return true;
}

bool Subset(std::string &d, const std::set<int> &keep, std::map<int, int> &renumbered, bool blank_notdef) {
  std::vector<Table> tables;
  if (!ReadTables(d, tables))
    return false;
  Table *head = Find(tables, "head"), *loca = Find(tables, "loca"), *glyf = Find(tables, "glyf"), *maxp = Find(tables, "maxp"), *hhea = Find(tables, "hhea"),
        *hmtx = Find(tables, "hmtx");
  if (!head || !loca || !glyf || !maxp || !hhea || !hmtx || head->data.size() < 54 || maxp->data.size() < 6 || hhea->data.size() < 36)
    return false;
  const unsigned glyphs = U16(maxp->data, 4), metrics = std::max(1u, U16(hhea->data, 34));
  const bool long_offsets = U16(head->data, 50) != 0;
  if (glyphs == 0 || loca->data.size() < (static_cast<size_t>(glyphs) + 1) * (long_offsets ? 4 : 2) ||
      hmtx->data.size() < static_cast<size_t>(std::min(metrics, glyphs)) * 4)
    return false;
  const auto offset = [&](unsigned g) -> size_t {
    return long_offsets ? U32(loca->data, static_cast<size_t>(g) * 4) : static_cast<size_t>(U16(loca->data, static_cast<size_t>(g) * 2)) * 2;
  };
  const auto glyph_data = [&](unsigned g) -> std::string {
    const size_t begin = offset(g), end = offset(g + 1);
    return end > begin && end <= glyf->data.size() ? glyf->data.substr(begin, end - begin) : std::string();
  };
  // A composite glyph's parts, with where each part's number is (to renumber it).
  const auto components = [&](const std::string &data, std::vector<size_t> *places) {
    std::vector<unsigned> parts;
    if (data.size() < 10 || static_cast<int16_t>(U16(data, 0)) >= 0)
      return parts;
    size_t at = 10;
    for (int count = 0; count < 64 && at + 4 <= data.size(); ++count) {
      const unsigned flags = U16(data, at);
      parts.push_back(U16(data, at + 2));
      if (places)
        places->push_back(at + 2);
      at += 4 + ((flags & 1) ? 4 : 2) + ((flags & 8) ? 2 : (flags & 0x40) ? 4 : (flags & 0x80) ? 8 : 0);
      if (!(flags & 0x20))
        break;
    }
    return parts;
  };
  std::set<unsigned> wanted = {0};
  std::vector<unsigned> pending;
  for (const int g : keep)
    if (g > 0 && static_cast<unsigned>(g) < glyphs && wanted.insert(static_cast<unsigned>(g)).second)
      pending.push_back(static_cast<unsigned>(g));
  while (!pending.empty()) {
    const unsigned g = pending.back();
    pending.pop_back();
    for (const unsigned part : components(glyph_data(g), nullptr))
      if (part < glyphs && wanted.insert(part).second)
        pending.push_back(part);
  }
  renumbered.clear();
  for (const unsigned g : wanted)
    renumbered.emplace(static_cast<int>(g), static_cast<int>(renumbered.size()));
  const unsigned count = static_cast<unsigned>(wanted.size());
  std::string new_glyf, new_loca(static_cast<size_t>(count + 1) * 4, '\0'), new_hmtx(static_cast<size_t>(count) * 4, '\0');
  unsigned widest = 0, index = 0;
  for (const unsigned g : wanted) {
    Put32(new_loca, static_cast<size_t>(index) * 4, static_cast<uint32_t>(new_glyf.size()));
    std::string data = g == 0 && blank_notdef ? std::string() : glyph_data(g);
    std::vector<size_t> places;
    components(data, &places);
    for (const size_t at : places) {
      const auto found = renumbered.find(static_cast<int>(U16(data, at)));
      Put16(data, at, found == renumbered.end() ? 0u : static_cast<unsigned>(found->second));
    }
    new_glyf += data;
    while (new_glyf.size() % 4)
      new_glyf += '\0';
    const unsigned advance = U16(hmtx->data, static_cast<size_t>(std::min(g, metrics - 1)) * 4);
    const unsigned side = g < metrics ? U16(hmtx->data, static_cast<size_t>(g) * 4 + 2)
                                      : U16(hmtx->data, static_cast<size_t>(metrics) * 4 + static_cast<size_t>(g - metrics) * 2);
    Put16(new_hmtx, static_cast<size_t>(index) * 4, advance);
    Put16(new_hmtx, static_cast<size_t>(index) * 4 + 2, side);
    widest = std::max(widest, advance);
    ++index;
  }
  Put32(new_loca, static_cast<size_t>(count) * 4, static_cast<uint32_t>(new_glyf.size()));
  glyf->data = new_glyf;
  loca->data = new_loca;
  hmtx->data = new_hmtx;
  Put16(head->data, 50, 1);  // long offsets
  Put16(maxp->data, 4, count);
  Put16(hhea->data, 10, widest);
  Put16(hhea->data, 34, count);
  // A PDF reader needs none of the layout, device or vertical tables, nor glyph names; the
  // hinting stays (glyphs' instructions call it).
  static const std::set<std::string> drop = {"GSUB", "GPOS", "GDEF", "BASE", "JSTF", "DSIG", "kern", "hdmx", "VDMX",
                                             "LTSH", "PCLT", "morx", "feat", "vhea", "vmtx", "VORG", "MATH", "STAT"};
  tables.erase(std::remove_if(tables.begin(), tables.end(), [&](const Table &t) { return drop.count(t.tag) > 0; }), tables.end());
  if (Table *post = Find(tables, "post"); post && post->data.size() >= 32) {
    post->data = post->data.substr(0, 32);
    Put32(post->data, 0, 0x00030000);
  }
  d = WriteTables(d, tables);
  return true;
}

} // namespace fynepdf::pdfa::font
