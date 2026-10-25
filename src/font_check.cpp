#include "font_check.h"

#include "common.h"
#include "content.h"
#include "font_cmaps.h"

#include <cmath>
#include <map>
#include <mutex>

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

// A program read once for every checker of the same bytes (a document can embed one font many
// times over: a copy in each of its comments), while any of them holds it.
struct Read {
  std::string data;
  Container container;
  int length1;
  std::unique_ptr<Program> program;
};

std::shared_ptr<const Program> Shared(const std::string &data, Container container, int length1) {
  static std::mutex mutex;
  static std::multimap<uint64_t, std::weak_ptr<const Read>> read;  // by a hash of the bytes
  uint64_t hash = 14695981039346656037ULL;  // FNV-1a
  for (const char c : data)
    hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
  std::lock_guard<std::mutex> lock(mutex);
  for (auto [at, end] = read.equal_range(hash); at != end; ++at)
    if (std::shared_ptr<const Read> found = at->second.lock())
      if (found->container == container && found->length1 == length1 && found->data == data)
        return std::shared_ptr<const Program>(found, found->program.get());
  auto made = std::make_shared<Read>(Read{data, container, length1, Program::Read(data, container, length1)});
  if (!made->program)
    return nullptr;
  if (read.size() > 1024)
    for (auto at = read.begin(); at != read.end();)
      at = at->second.expired() ? read.erase(at) : std::next(at);
  read.emplace(hash, made);
  return std::shared_ptr<const Program>(made, made->program.get());
}

// A font descriptor's embedded program, read as the stream it's in says.
std::shared_ptr<const Program> ReadProgram(QPDFObjectHandle descriptor) {
  if (!descriptor.isDictionary())
    return nullptr;
  std::string data;
  QPDFObjectHandle stream = descriptor.getKey("/FontFile");
  if (stream.isStream() && Decode(stream, data))
    return Shared(data, Container::Type1, Integer(stream.getDict(), "/Length1", 0));
  stream = descriptor.getKey("/FontFile2");
  if (stream.isStream() && Decode(stream, data))
    return Shared(data, Container::TrueType, 0);
  stream = descriptor.getKey("/FontFile3");
  if (stream.isStream() && Decode(stream, data))
    return Shared(data, IsName(stream.getDict().getKey("/Subtype"), "/OpenType") ? Container::OpenType : Container::Cff, 0);
  return nullptr;
}

double Number(QPDFObjectHandle handle, double fallback) { return handle.isNumber() ? handle.getNumericValue() : fallback; }

// A code (bytes, big-endian) as a number.
unsigned Value(const std::string &code) {
  unsigned value = 0;
  for (const char c : code)
    value = value << 8 | static_cast<unsigned char>(c);
  return value;
}

} // namespace

Checker::Checker(QPDFObjectHandle font) {
  try {
    if (IsName(font.getKey("/Subtype"), "/Type3")) {
      ReadType3(font);
    } else if (IsName(font.getKey("/Subtype"), "/Type0")) {
      composite_ = true;
      QPDFObjectHandle descendants = font.getKey("/DescendantFonts");
      ReadComposite(font, descendants.isArray() && descendants.getArrayNItems() > 0 ? descendants.getArrayItem(0) : QPDFObjectHandle::newNull());
    } else {
      ReadSimple(font, font.getKey("/FontDescriptor"));
    }
  } catch (const std::exception &) {
    ready_ = false;
    why_ = "unreadable";
  }
}

void Checker::ReadSimple(QPDFObjectHandle font, QPDFObjectHandle descriptor) {
  program_ = ReadProgram(descriptor);
  symbolic_ = (Integer(descriptor, "/Flags", 0) & 4) != 0;
  QPDFObjectHandle encoding = font.getKey("/Encoding");
  const std::string base_font = NameOf(font.getKey("/BaseFont"));
  const bool symbol = base_font == "/Symbol", dingbats = base_font == "/ZapfDingbats";
  if (!program_) {
    // Not embedded: only the names its encoding gives (PDF/A-2's .notdef rule holds even for
    // text that isn't drawn). A symbolic font's own encoding is unknown.
    const bool program_streams = descriptor.isDictionary() &&
                                 (descriptor.hasKey("/FontFile") || descriptor.hasKey("/FontFile2") || descriptor.hasKey("/FontFile3"));
    if (program_streams || (symbolic_ && encoding.isNull() && !symbol && !dingbats)) {
      why_ = program_streams ? "program unreadable" : "not embedded";
      return;
    }
  }
  sfnt_lookup_ = program_ && IsName(font.getKey("/Subtype"), "/TrueType") && program_->Sfnt();
  const char *const *base = Encoding(encoding.isName() ? encoding.getName() : encoding.isDictionary() ? NameOf(encoding.getKey("/BaseEncoding")) : "");
  if (sfnt_lookup_ && symbolic_) {
    named_ = false;  // a symbolic TrueType font's codes go to its cmap as they are
  } else {
    named_ = true;
    const char *const *builtin = symbol ? Encoding("Symbol") : dingbats ? Encoding("ZapfDingbats") : Encoding("/StandardEncoding");
    for (unsigned code = 0; code < 256; ++code)
      names_[code] = base ? base[code] : program_ && !sfnt_lookup_ ? program_->Encoded(code) : builtin[code];
    QPDFObjectHandle differences = encoding.isDictionary() ? encoding.getKey("/Differences") : QPDFObjectHandle::newNull();
    unsigned code = 0;
    for (int index = 0; differences.isArray() && index < differences.getArrayNItems(); ++index) {
      QPDFObjectHandle item = differences.getArrayItem(index);
      if (item.isInteger())
        code = static_cast<unsigned>(std::max(0, item.getIntValueAsInt()));
      else if (item.isName() && code < 256)
        names_[code++] = item.getName().substr(1);
    }
  }
  first_char_ = Integer(font, "/FirstChar", 0);
  QPDFObjectHandle widths = font.getKey("/Widths");
  for (int index = 0; widths.isArray() && index < widths.getArrayNItems(); ++index)
    widths_list_.push_back(Number(widths.getArrayItem(index), 0));
  missing_width_ = Number(descriptor.getKey("/MissingWidth"), 0);
  ready_ = true;
}

// A Type 3 font: its glyphs are content streams named by its encoding; each one's width is
// the first operator's (d0 or d1), in glyph space as its Widths are.
void Checker::ReadType3(QPDFObjectHandle font) {
  type3_ = true;
  procs_ = font.getKey("/CharProcs");
  if (!procs_.isDictionary()) {
    why_ = "no glyph procedures";
    return;
  }
  QPDFObjectHandle encoding = font.getKey("/Encoding");
  const char *const *base = Encoding(encoding.isDictionary() ? NameOf(encoding.getKey("/BaseEncoding")) : "");
  for (unsigned code = 0; code < 256; ++code)
    names_[code] = base ? base[code] : "";
  QPDFObjectHandle differences = encoding.isDictionary() ? encoding.getKey("/Differences") : QPDFObjectHandle::newNull();
  unsigned code = 0;
  for (int index = 0; differences.isArray() && index < differences.getArrayNItems(); ++index) {
    QPDFObjectHandle item = differences.getArrayItem(index);
    if (item.isInteger())
      code = static_cast<unsigned>(std::max(0, item.getIntValueAsInt()));
    else if (item.isName() && code < 256)
      names_[code++] = item.getName().substr(1);
  }
  named_ = true;
  first_char_ = Integer(font, "/FirstChar", 0);
  QPDFObjectHandle widths = font.getKey("/Widths");
  for (int index = 0; widths.isArray() && index < widths.getArrayNItems(); ++index)
    widths_list_.push_back(Number(widths.getArrayItem(index), 0));
  ready_ = true;
}

double Checker::Type3Width(const std::string &name) {
  const auto found = type3_widths_.find(name);
  if (found != type3_widths_.end())
    return found->second;
  double width = -2;  // -2: no such glyph, -1: no width said
  QPDFObjectHandle proc = procs_.getKey("/" + name);
  std::string content;
  if (proc.isStream() && Decode(proc, content)) {
    width = -1;
    size_t pos = 0;
    content::Token token;
    std::vector<double> numbers;
    while (content::NextToken(content, pos, token)) {
      if (token.kind == content::Token::Number) {
        numbers.push_back(token.number);
        continue;
      }
      if (token.kind == content::Token::Operator && (token.text == "d0" || token.text == "d1") && !numbers.empty())
        width = numbers[0];
      break;
    }
  }
  type3_widths_.emplace(name, width);
  return width;
}

void Checker::ReadComposite(QPDFObjectHandle font, QPDFObjectHandle descendant) {
  if (!descendant.isDictionary()) {
    why_ = "no CIDFont";
    return;
  }
  program_ = ReadProgram(descendant.getKey("/FontDescriptor"));
  QPDFObjectHandle own = descendant.getKey("/FontDescriptor");
  if (!program_ && own.isDictionary() && (own.hasKey("/FontFile") || own.hasKey("/FontFile2") || own.hasKey("/FontFile3"))) {
    why_ = "program unreadable";
    return;
  }
  QPDFObjectHandle encoding = font.getKey("/Encoding");
  if (IsName(encoding, "/Identity-H") || IsName(encoding, "/Identity-V")) {
    identity_ = true;
  } else if (encoding.isStream()) {
    identity_ = false;
    ReadCMap(encoding);
    FinishCMap();
    if (!why_.empty())
      return;
  } else if (encoding.isName() && PredefinedCMap(encoding.getName().substr(1))) {
    // A predefined CMap the CMap folder has.
    identity_ = false;
    UseCMap(encoding.getName().substr(1), 0);
    FinishCMap();
    if (!why_.empty())
      return;
  } else {
    // A standard CMap other than Identity whose mapping isn't here to check codes with.
    why_ = "CMap " + (encoding.isName() ? encoding.getName().substr(1) : std::string("missing"));
    return;
  }
  if (IsName(descendant.getKey("/Subtype"), "/CIDFontType2")) {
    QPDFObjectHandle map = descendant.getKey("/CIDToGIDMap");
    if (map.isStream()) {
      if (!Decode(map, gid_map_)) {
        why_ = "CIDToGIDMap unreadable";
        return;
      }
      gid_map_stream_ = true;
    }
  }
  default_width_ = Number(descendant.getKey("/DW"), 1000);
  QPDFObjectHandle widths = descendant.getKey("/W");
  for (int index = 0; widths.isArray() && index < widths.getArrayNItems();) {
    QPDFObjectHandle first = widths.getArrayItem(index);
    if (!first.isInteger())
      break;
    const long long start = first.getIntValue();
    QPDFObjectHandle next = index + 1 < widths.getArrayNItems() ? widths.getArrayItem(index + 1) : QPDFObjectHandle::newNull();
    if (next.isArray()) {
      for (int k = 0; k < next.getArrayNItems(); ++k)
        cid_widths_.emplace(static_cast<unsigned>(start + k), Number(next.getArrayItem(k), default_width_));
      index += 2;
    } else if (next.isInteger() && index + 2 < widths.getArrayNItems()) {
      const long long stop = next.getIntValue();
      const double width = Number(widths.getArrayItem(index + 2), default_width_);
      for (long long cid = start; cid <= stop && cid - start < 65536; ++cid)
        cid_widths_.emplace(static_cast<unsigned>(cid), width);
      index += 3;
    } else {
      break;
    }
  }
  ready_ = true;
}

void Checker::ReadCMap(QPDFObjectHandle stream, int depth) {
  if (depth > 4) {
    why_ = "CMap loop";
    return;
  }
  QPDFObjectHandle use = stream.getDict().getKey("/UseCMap");
  if (use.isStream())
    ReadCMap(use, depth + 1);
  else if (use.isName())
    UseCMap(use.getName().substr(1), depth + 1);
  if (!why_.empty())
    return;
  std::string program;
  if (!Decode(stream, program)) {
    why_ = "CMap unreadable";
    return;
  }
  ReadCMapProgram(program, depth);
}

// A CMap another uses by name: Identity, or a predefined one the CMap folder has.
void Checker::UseCMap(const std::string &name, int depth) {
  if (name == "Identity-H" || name == "Identity-V") {
    spaces_.push_back({std::string(2, '\0'), std::string(2, '\xff')});
    cid_ranges_.push_back({std::string(2, '\0'), std::string(2, '\xff'), 0});
    return;
  }
  const std::shared_ptr<const std::string> program = PredefinedCMap(name);
  if (!program) {
    why_ = "CMap uses " + name;
    return;
  }
  if (depth > 4) {
    why_ = "CMap loop";
    return;
  }
  ReadCMapProgram(*program, depth);
}

void Checker::ReadCMapProgram(const std::string &program, int depth) {
  using content::Token;
  std::vector<Token> tokens;
  size_t pos = 0;
  Token token;
  while (content::NextToken(program, pos, token) && tokens.size() < 2000000)
    tokens.push_back(token);
  const auto is = [&](size_t at, Token::Kind kind) { return at < tokens.size() && tokens[at].kind == kind; };
  for (size_t at = 0; at < tokens.size(); ++at) {
    const std::string &word = tokens[at].kind == Token::Operator ? tokens[at].text : std::string();
    if (word == "begincodespacerange") {
      for (++at; is(at, Token::String) && is(at + 1, Token::String); at += 2)
        spaces_.push_back({tokens[at].text, tokens[at + 1].text});
    } else if (word == "begincidrange") {
      for (++at; is(at, Token::String) && is(at + 1, Token::String) && is(at + 2, Token::Number); at += 3)
        cid_ranges_.push_back({tokens[at].text, tokens[at + 1].text, static_cast<unsigned>(tokens[at + 2].number)});
    } else if (word == "begincidchar") {
      for (++at; is(at, Token::String) && is(at + 1, Token::Number); at += 2)
        cid_ranges_.push_back({tokens[at].text, tokens[at].text, static_cast<unsigned>(tokens[at + 1].number)});
    } else if (word == "usecmap" && at > 0 && tokens[at - 1].kind == Token::Name) {
      UseCMap(tokens[at - 1].text, depth + 1);
      if (!why_.empty())
        return;
    }
  }
}

// After a CMap is read: its code spaces there, its CIDs past 65,535 counted, and the one- and
// two-byte codes' CIDs put in tables (in the order defined, so a later definition wins).
void Checker::FinishCMap() {
  if (!why_.empty())
    return;
  if (spaces_.empty()) {
    why_ = "CMap without code spaces";
    return;
  }
  // CIDs past 65,535, used or not (PDF/A-2 6.1.13).
  for (const CidRange &range : cid_ranges_)
    if (static_cast<unsigned long long>(range.cid) + (Value(range.high) - std::min(Value(range.high), Value(range.low))) > 65535)
      ++big_cids_;
  for (const CidRange &range : cid_ranges_) {
    if (range.low.size() != range.high.size() || range.low.size() > 2)
      continue;
    std::vector<int> &table = range.low.size() == 1 ? cid1_ : cid2_;
    if (table.empty())
      table.assign(range.low.size() == 1 ? 256 : 65536, -1);
    const unsigned low = Value(range.low), high = Value(range.high);
    for (unsigned value = low; value <= high && value < table.size(); ++value)
      table[value] = static_cast<int>(std::min<unsigned long long>(static_cast<unsigned long long>(range.cid) + (value - low), 0x7fffffff));
  }
}



double Checker::DictionaryWidth(unsigned code) const {
  if (composite_) {
    const auto found = cid_widths_.find(code);
    return found == cid_widths_.end() ? default_width_ : found->second;
  }
  const long index = static_cast<long>(code) - first_char_;
  return index >= 0 && static_cast<size_t>(index) < widths_list_.size() ? widths_list_[static_cast<size_t>(index)] : missing_width_;
}

int Checker::GlyphFor(unsigned code) const {
  if (!program_)
    return -1;
  const Program &p = *program_;
  if (composite_) {
    if (p.CidKeyed() || !gid_map_stream_) {
      if (p.CidKeyed())
        return p.ByCid(code);
      return code < static_cast<unsigned>(p.Glyphs()) ? static_cast<int>(code) : -1;
    }
    if (static_cast<size_t>(code) * 2 + 1 >= gid_map_.size())
      return 0;
    const unsigned glyph = static_cast<unsigned char>(gid_map_[code * 2]) << 8 | static_cast<unsigned char>(gid_map_[code * 2 + 1]);
    return glyph < static_cast<unsigned>(p.Glyphs()) ? static_cast<int>(glyph) : -1;
  }
  if (!sfnt_lookup_) {
    const std::string &name = names_[code & 0xff];
    return name.empty() ? -1 : p.ByName(name);
  }
  // TrueType (ISO 32000-1 9.6.6.4).
  const auto symbol_cmap = [&]() -> int {
    if (p.HasCmap(3, 0)) {
      for (const unsigned base : {0u, 0xf000u, 0xf100u, 0xf200u}) {
        const int glyph = p.ByCmap(3, 0, base + code);
        if (glyph > 0)
          return glyph;
      }
      return 0;
    }
    if (p.HasCmap(1, 0))
      return p.ByCmap(1, 0, code);
    if (p.HasCmap(3, 1))
      return p.ByCmap(3, 1, code);
    return -1;
  };
  if (!named_)
    return symbol_cmap();
  const std::string &name = names_[code & 0xff];
  int glyph = -1;
  if (!name.empty()) {
    if (const unsigned unicode = UnicodeOf(name)) {
      if (p.HasCmap(3, 1))
        glyph = p.ByCmap(3, 1, unicode);
      for (const unsigned id : {3u, 4u, 1u, 0u, 2u, 6u})
        if (glyph <= 0 && p.HasCmap(0, id))
          glyph = p.ByCmap(0, id, unicode);
    }
    if (glyph <= 0 && p.HasCmap(1, 0)) {
      const int mac = MacRomanCode(name);
      if (mac >= 0)
        glyph = p.ByCmap(1, 0, static_cast<unsigned>(mac));
    }
    if (glyph <= 0) {
      const int named = p.ByName(name);
      if (named > 0)
        glyph = named;
    }
  }
  if (glyph <= 0 && p.HasCmap(3, 0))
    glyph = symbol_cmap();
  return glyph < 0 ? 0 : glyph;
}

std::vector<std::pair<unsigned, bool>> Checker::Codes() const {
  std::vector<std::pair<unsigned, bool>> out;
  for (const auto &[code, state] : seen_)
    out.push_back({code, (state & 16) != 0});
  return out;
}

void Checker::Replay(const Checker &other) {
  for (const auto &[code, drawn] : other.Codes())
    Check(code, drawn);
  big_cids_ = std::max(big_cids_, other.big_cids_);
}

void Checker::Check(unsigned code, bool drawn) {
  unsigned char &state = seen_[code];
  if (drawn)
    state |= 16;
  // .notdef: CID 0 in a composite font; a code its encoding leaves unnamed (or names
  // .notdef) in a simple one found by name; glyph 0 in a TrueType one found by its cmap.
  int glyph = -1;
  bool notdef = false;
  if (composite_) {
    notdef = code == 0;
    if (!notdef)
      glyph = GlyphFor(code);
  } else if (type3_) {
    const std::string &name = names_[code & 0xff];
    notdef = name.empty() || name == ".notdef";
    if (!notdef && drawn && !(state & 8)) {
      state |= 8;
      const double width = Type3Width(name);
      if (width == -2) {
        if (!missing_++ && example_.empty())
          example_ = "code " + std::to_string(code) + " has no glyph";
      } else if (width >= 0 && std::fabs(width - DictionaryWidth(code)) > 1) {
        if (!widths_++ && example_.empty())
          example_ = "code " + std::to_string(code) + " is " + std::to_string(static_cast<long>(DictionaryWidth(code))) + " wide, its glyph " +
                     std::to_string(static_cast<long>(width));
      }
    }
    if (!notdef)
      return;
  } else if (!sfnt_lookup_) {
    const std::string &name = names_[code & 0xff];
    notdef = name.empty() || name == ".notdef";
    if (!notdef)
      glyph = GlyphFor(code);
  } else {
    glyph = GlyphFor(code);
    notdef = glyph == 0;
  }
  if (notdef) {
    if (!(state & 4)) {
      state |= 4;
      if (!notdef_++ && example_.empty())
        example_ = "code " + std::to_string(code) + " draws .notdef";
    }
    return;
  }
  if (!drawn || !program_)
    return;
  if (glyph < 0) {
    if (!(state & 2)) {
      state |= 2;
      if (!missing_++ && example_.empty())
        example_ = "code " + std::to_string(code) + " has no glyph";
    }
    return;
  }
  if (state & 8)
    return;
  state |= 8;
  const double program = program_->Width(glyph), dictionary = DictionaryWidth(code);
  if (program >= 0 && std::fabs(program - dictionary) > 1) {
    if (!widths_++ && example_.empty()) {
      char text[96];
      std::snprintf(text, sizeof text, "code %u is %.0f wide, its glyph %.0f", code, dictionary, program);
      example_ = text;
    }
  }
}

void Checker::Show(const std::string &bytes, bool drawn) {
  if (!ready_)
    return;
  for (const Piece &piece : Pieces(bytes)) {
    if (piece.code > 65535)
      ++big_cids_;
    if (composite_ && !identity_) {
      shown_codes_.emplace(piece.code, Value(bytes.substr(piece.begin, piece.length)));
      shown_cids_.emplace(bytes.substr(piece.begin, piece.length), piece.code);
    }
    Check(piece.code, drawn);
  }
  // Half a code left over: no glyph a reader can be sure of (veraPDF reads one anyway).
  if (composite_ && identity_ && bytes.size() % 2 && drawn && !missing_++ && example_.empty())
    example_ = "a string ends in half a code";
}

bool Checker::DrawsNotdef(unsigned code) const {
  if (composite_)
    return code == 0 || (program_ && GlyphFor(code) < 0);
  if (type3_) {
    const std::string &name = names_[code & 0xff];
    QPDFObjectHandle procs = procs_;
    return name.empty() || name == ".notdef" || !procs.getKey("/" + name).isStream();
  }
  if (!sfnt_lookup_) {
    const std::string &name = names_[code & 0xff];
    return name.empty() || name == ".notdef" || (program_ && program_->ByName(name) < 0);
  }
  return GlyphFor(code) <= 0;
}

bool Checker::NotdefBlank() const {
  if (!program_)
    return false;
  if (sfnt_lookup_ || (composite_ && !program_->CidKeyed()))
    return program_->Blank(0);
  if (composite_)
    return program_->Blank(std::max(0, program_->ByCid(0)));
  const int glyph = program_->ByName(".notdef");
  return glyph < 0 || program_->Blank(glyph);
}

std::vector<Checker::Piece> Checker::Pieces(const std::string &bytes) const {
  std::vector<Piece> pieces;
  if (!composite_) {
    for (size_t at = 0; at < bytes.size(); ++at)
      pieces.push_back({at, 1, static_cast<unsigned char>(bytes[at])});
    return pieces;
  }
  if (identity_) {
    for (size_t at = 0; at + 1 < bytes.size(); at += 2)
      pieces.push_back({at, 2, static_cast<unsigned>(static_cast<unsigned char>(bytes[at]) << 8 | static_cast<unsigned char>(bytes[at + 1]))});
    return pieces;
  }
  // An embedded CMap: the code space each code falls in gives its length (one byte when it
  // falls in none), then its CID (0, .notdef, when none maps it).
  const auto within = [](const std::string &code, const std::string &low, const std::string &high) {
    if (code.size() != low.size() || code.size() != high.size())
      return false;
    for (size_t k = 0; k < code.size(); ++k)
      if (static_cast<unsigned char>(code[k]) < static_cast<unsigned char>(low[k]) || static_cast<unsigned char>(code[k]) > static_cast<unsigned char>(high[k]))
        return false;
    return true;
  };
  for (size_t at = 0; at < bytes.size();) {
    size_t length = 0;
    for (size_t n = 1; n <= 4 && at + n <= bytes.size() && !length; ++n)
      for (const Space &space : spaces_)
        if (within(bytes.substr(at, n), space.low, space.high)) {
          length = n;
          break;
        }
    if (!length)
      length = 1;
    const std::string code = bytes.substr(at, length);
    unsigned cid = 0;
    const unsigned value = Value(code);
    if (length <= 2) {
      const std::vector<int> &table = length == 1 ? cid1_ : cid2_;
      if (value < table.size() && table[value] >= 0)
        cid = static_cast<unsigned>(table[value]);
    } else {
      for (auto range = cid_ranges_.rbegin(); range != cid_ranges_.rend(); ++range)  // the later definition wins
        if (range->low.size() == code.size() && value >= Value(range->low) && value <= Value(range->high)) {
          cid = range->cid + (value - Value(range->low));
          break;
        }
    }
    pieces.push_back({at, length, cid});
    at += length;
  }
  return pieces;
}

} // namespace fynepdf::pdfa::font
