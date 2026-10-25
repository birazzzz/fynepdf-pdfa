#pragma once

// Content streams read token by token, so colours can be rewritten in place: operators with
// their operands and byte ranges, inline images skipped whole; streams decoded with qpdf; the
// matrices that place what they draw.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <qpdf/Buffer.hh>
#include <qpdf/Pl_Buffer.hh>
#include <qpdf/Pl_String.hh>
#include <qpdf/QPDFObjectHandle.hh>

namespace fynepdf::pdfa::content {

// Row-vector matrices, as PDF writes them: a point p goes to p·M, and Then(a, b) is a, then b
// (so "cm" makes Then(cm, ctm)).
struct Matrix {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

inline Matrix Then(const Matrix &m, const Matrix &p) {
  return {m.a * p.a + m.b * p.c, m.a * p.b + m.b * p.d, m.c * p.a + m.d * p.c,
          m.c * p.b + m.d * p.d, m.e * p.a + m.f * p.c + p.e, m.e * p.b + m.f * p.d + p.f};
}

inline bool Invert(const Matrix &m, Matrix &out) {
  const double det = m.a * m.d - m.b * m.c;
  if (std::abs(det) < 1e-12)
    return false;
  out = {m.d / det, -m.b / det, -m.c / det, m.a / det, (m.c * m.f - m.d * m.e) / det, (m.b * m.e - m.a * m.f) / det};
  return true;
}

inline std::string Num(double value) {
  if (std::abs(value) < 5e-5)
    return "0";
  char buffer[40];
  std::snprintf(buffer, sizeof buffer, "%.4f", value);
  std::string text(buffer);
  while (!text.empty() && text.back() == '0')
    text.pop_back();
  if (!text.empty() && text.back() == '.')
    text.pop_back();
  return text;
}

// ---------------------------------------------------------------------------
// Content tokens with their byte offsets (as redaction reads them, PdfDocument_Redact.cpp)
// ---------------------------------------------------------------------------

struct Token {
  enum Kind { Number, Name, String, ArrayOpen, ArrayClose, DictOpen, DictClose, Operator };
  Kind kind = Operator;
  size_t begin = 0, end = 0;
  double number = 0;
  std::string text;
};

inline bool IsWhite(unsigned char c) { return c == 0 || c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' '; }

inline bool IsDelimiter(unsigned char c) {
  return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}

inline int HexDigit(unsigned char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

inline bool NextToken(const std::string &s, size_t &pos, Token &t) {
  while (pos < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[pos]);
    if (IsWhite(c)) {
      ++pos;
      continue;
    }
    if (c == '%') {
      while (pos < s.size() && s[pos] != '\n' && s[pos] != '\r')
        ++pos;
      continue;
    }
    break;
  }
  if (pos >= s.size())
    return false;
  t = Token{};
  t.begin = pos;
  const unsigned char c = static_cast<unsigned char>(s[pos]);
  if (c == '(') {
    t.kind = Token::String;
    int depth = 1;
    ++pos;
    while (pos < s.size()) {
      const char ch = s[pos++];
      if (ch == '\\' && pos < s.size()) {
        const char e = s[pos++];
        if (e >= '0' && e <= '7') {
          int v = e - '0';
          for (int k = 0; k < 2 && pos < s.size() && s[pos] >= '0' && s[pos] <= '7'; k++)
            v = v * 8 + (s[pos++] - '0');
          t.text += static_cast<char>(v & 0xFF);
        } else if (e == '\r') {
          if (pos < s.size() && s[pos] == '\n')
            ++pos;
        } else if (e != '\n') {
          t.text += e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
        }
        continue;
      }
      if (ch == '(')
        depth++;
      else if (ch == ')' && --depth == 0)
        break;
      t.text += ch;
    }
  } else if (c == '<' && pos + 1 < s.size() && s[pos + 1] == '<') {
    t.kind = Token::DictOpen;
    pos += 2;
  } else if (c == '<') {
    t.kind = Token::String;
    ++pos;
    int hi = -1;
    while (pos < s.size() && s[pos] != '>') {
      const int digit = HexDigit(static_cast<unsigned char>(s[pos++]));
      if (digit < 0)
        continue;
      if (hi < 0) {
        hi = digit;
      } else {
        t.text += static_cast<char>(hi * 16 + digit);
        hi = -1;
      }
    }
    if (hi >= 0)
      t.text += static_cast<char>(hi * 16);
    if (pos < s.size())
      ++pos;
  } else if (c == '>' && pos + 1 < s.size() && s[pos + 1] == '>') {
    t.kind = Token::DictClose;
    pos += 2;
  } else if (c == '[') {
    t.kind = Token::ArrayOpen;
    ++pos;
  } else if (c == ']') {
    t.kind = Token::ArrayClose;
    ++pos;
  } else if (c == '/') {
    t.kind = Token::Name;
    ++pos;
    while (pos < s.size() && !IsWhite(static_cast<unsigned char>(s[pos])) && !IsDelimiter(static_cast<unsigned char>(s[pos]))) {
      if (s[pos] == '#' && pos + 2 < s.size() && HexDigit(s[pos + 1]) >= 0 && HexDigit(s[pos + 2]) >= 0) {
        t.text += static_cast<char>(HexDigit(s[pos + 1]) * 16 + HexDigit(s[pos + 2]));
        pos += 3;
      } else {
        t.text += s[pos++];
      }
    }
  } else if (IsDelimiter(c)) {
    t.kind = Token::Operator;
    t.text = std::string(1, static_cast<char>(c));
    ++pos;
  } else {
    while (pos < s.size() && !IsWhite(static_cast<unsigned char>(s[pos])) && !IsDelimiter(static_cast<unsigned char>(s[pos])))
      t.text += s[pos++];
    char *parsed_end = nullptr;
    const double v = std::strtod(t.text.c_str(), &parsed_end);
    if (!t.text.empty() && std::strchr("+-.0123456789", t.text[0]) && parsed_end == t.text.c_str() + t.text.size()) {
      t.kind = Token::Number;
      t.number = v;
    } else {
      t.kind = Token::Operator;
    }
  }
  t.end = pos;
  return true;
}

// Past the EI of an inline image whose BI ends at |pos|, or npos.
inline size_t SkipInlineImage(const std::string &content, size_t pos) {
  Token t;
  // The entries before ID: each key's first value token.
  std::string key;
  std::vector<std::pair<std::string, Token>> entries;
  while (NextToken(content, pos, t)) {
    if (t.kind == Token::Operator && t.text == "ID")
      break;
    if (key.empty() && t.kind == Token::Name)
      key = t.text;
    else if (!key.empty()) {
      entries.push_back({key, t});
      key.clear();
    }
  }
  if (pos >= content.size())
    return std::string::npos;
  const auto value = [&](const char *a, const char *b) -> const Token * {
    for (const auto &[name, token] : entries)
      if (name == a || name == b)
        return &token;
    return nullptr;
  };
  const size_t data = pos + 1;
  // Unfiltered data has a known length: its EI is just after it.
  const Token *width = value("W", "Width"), *height = value("H", "Height"), *bits = value("BPC", "BitsPerComponent");
  const Token *space = value("CS", "ColorSpace"), *mask = value("IM", "ImageMask");
  if (!value("F", "Filter") && width && height && width->kind == Token::Number && height->kind == Token::Number && width->number > 0 && height->number > 0) {
    const bool stencil = mask && mask->kind == Token::Operator && mask->text == "true";
    int components = stencil ? 1 : 0;
    if (!stencil && space && space->kind == Token::Name) {
      const std::string &s = space->text;
      components = s == "G" || s == "DeviceGray" || s == "I" || s == "Indexed" ? 1 : s == "RGB" || s == "DeviceRGB" ? 3 : s == "CMYK" || s == "DeviceCMYK" ? 4 : 0;
    } else if (!stencil && space && space->kind == Token::ArrayOpen) {
      components = 1;  // [/Indexed ...] (the only array an inline space can be)
    }
    const int depth = stencil ? 1 : bits && bits->kind == Token::Number ? static_cast<int>(bits->number) : 0;
    if (components > 0 && depth > 0) {
      const double length = std::ceil(width->number * components * depth / 8.0) * height->number;
      size_t at = data + static_cast<size_t>(length);
      while (at < content.size() && IsWhite(static_cast<unsigned char>(content[at])))
        ++at;
      if (length < 1e9 && content.compare(at, 2, "EI") == 0 && (at + 2 >= content.size() || IsWhite(static_cast<unsigned char>(content[at + 2])) ||
                                                                IsDelimiter(static_cast<unsigned char>(content[at + 2]))))
        return at + 2;
    }
  }
  // Otherwise the first EI between white space that content text follows (binary data can
  // hold "EI" too): the next bytes printable.
  for (size_t at = data; at < content.size();) {
    const size_t ei = content.find("EI", at);
    if (ei == std::string::npos)
      return std::string::npos;
    const bool before = ei > 0 && IsWhite(static_cast<unsigned char>(content[ei - 1]));
    const bool after = ei + 2 >= content.size() || IsWhite(static_cast<unsigned char>(content[ei + 2]));
    bool text = true;
    for (size_t k = ei + 2; text && k < std::min(content.size(), ei + 2 + 32); ++k) {
      const unsigned char c = static_cast<unsigned char>(content[k]);
      text = IsWhite(c) || (c >= 0x20 && c < 0x7f);
    }
    if (before && after && text)
      return ei + 2;
    at = ei + 1;
  }
  return std::string::npos;
}

// An operator with its operands: the bytes from just after the one before it to its end.
struct Op {
  std::string name;
  size_t begin = 0, end = 0;
  std::vector<Token> operands;
};

inline bool ReadOps(const std::string &content, std::vector<Op> &ops) {
  size_t pos = 0;
  Token t;
  Op current;
  bool first = true;
  while (NextToken(content, pos, t)) {
    if (first) {
      current.begin = t.begin;
      first = false;
    }
    if (t.kind != Token::Operator) {
      current.operands.push_back(t);
      continue;
    }
    if (t.text == "BI") {
      const size_t end = SkipInlineImage(content, pos);
      if (end == std::string::npos)
        return false;
      pos = end;
      current.name = "BI";
      current.end = end;
    } else {
      current.name = t.text;
      current.end = t.end;
    }
    ops.push_back(std::move(current));
    current = Op{};
    first = true;
  }
  return first;  // operands left without an operator: not well formed
}

// Whether ReadOps would read |content|, without keeping its operators.
inline bool WellFormed(const std::string &content) {
  size_t pos = 0;
  Token t;
  bool first = true;
  while (NextToken(content, pos, t)) {
    first = false;
    if (t.kind != Token::Operator)
      continue;
    if (t.text == "BI" && (pos = SkipInlineImage(content, pos)) == std::string::npos)
      return false;
    first = true;
  }
  return first;
}

// ReadOps' operators one at a time, for content too big to hold as a list (an Illustrator page's
// takes about 19 times its own size that way): |each| gets them in order. False, before any is
// passed, when ReadOps would be.
template <typename Each> bool ForEachOp(const std::string &content, Each &&each) {
  if (!WellFormed(content))
    return false;
  size_t pos = 0;
  Token t;
  Op current;
  bool first = true;
  while (NextToken(content, pos, t)) {
    if (first) {
      current.begin = t.begin;
      first = false;
    }
    if (t.kind != Token::Operator) {
      current.operands.push_back(t);
      continue;
    }
    if (t.text == "BI") {
      pos = SkipInlineImage(content, pos);
      current.name = "BI";
      current.end = pos;
    } else {
      current.name = t.text;
      current.end = t.end;
    }
    each(static_cast<const Op &>(current));
    current.operands.clear();
    first = true;
  }
  return true;
}

inline std::string StreamData(QPDFObjectHandle stream, bool &ok) {
  std::string out;
  Pl_String sink("outline", nullptr, out);
  if (!stream.isStream() || !stream.pipeStreamData(&sink, 0, qpdf_dl_generalized)) {
    ok = false;
    return {};
  }
  return out;
}

inline std::string PageContent(QPDFObjectHandle page, bool &ok) {
  std::string out;
  QPDFObjectHandle contents = page.getKey("/Contents");
  if (contents.isArray()) {
    for (int index = 0; index < contents.getArrayNItems(); index++) {
      out += StreamData(contents.getArrayItem(index), ok);
      out += '\n';
    }
  } else if (contents.isStream()) {
    out = StreamData(contents, ok);
  }
  return out;
}

} // namespace fynepdf::pdfa::content
