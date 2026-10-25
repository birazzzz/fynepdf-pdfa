#pragma once

// Embedded font programs read far enough to answer what PDF/A asks of them: which glyphs a
// font has, what each is called, how wide it is, and how readers find the glyph for a
// character code. TrueType and OpenType (glyf or CFF outlines), bare CFF (simple and
// CID-keyed) and Type 1. Nothing is drawn.

#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fynepdf::pdfa::font {

// The font program streams PDF has: FontFile (Type 1), FontFile2 (TrueType), FontFile3 with
// its subtype (Type1C or CIDFontType0C: bare CFF; OpenType: either outline kind).
enum class Container { Type1, TrueType, Cff, OpenType };

class Program {
public:
  // |length1|: a Type 1 program's cleartext length (the stream's /Length1), or 0 to find it.
  // Null when the data isn't a font of that kind or can't be read.
  static std::unique_ptr<Program> Read(const std::string &data, Container container, int length1 = 0);

  int Glyphs() const { return glyphs_; }
  // A CID-keyed CFF: glyphs are found by CID (its charset maps them).
  bool CidKeyed() const { return cid_keyed_; }
  // Whether the program has TrueType-style character maps (sfnt containers).
  bool Sfnt() const { return sfnt_; }

  // A glyph by name (Type 1 and CFF charsets; TrueType's post table); -1 when there's none.
  int ByName(const std::string &name) const;
  // Whether a character map (platform, encoding) is there, and the glyph it gives a code (0
  // when it maps it to nothing).
  bool HasCmap(unsigned platform, unsigned encoding) const;
  bool HasUnicodeCmap() const;  // platform 0, any encoding
  std::vector<std::pair<unsigned, unsigned>> Cmaps() const;  // every (platform, encoding)
  // Each glyph's character by the program's Unicode cmap (3,1, 3,10 or platform 0), the first
  // one that maps to it: what readers fall back on for text.
  std::map<int, unsigned> UnicodeByGlyph() const;
  int ByCmap(unsigned platform, unsigned encoding, unsigned code) const;
  // The glyph a Unicode character has by the program's Unicode cmaps ((3,1), (3,10), platform
  // 0); 0 when none maps it.
  int ByUnicode(unsigned character) const;
  // A glyph by CID: a CID-keyed CFF's charset; otherwise the CID is the glyph. -1 when absent.
  int ByCid(unsigned cid) const;
  // The program's own encoding (Type 1, CFF): the glyph name for a code, "" when unencoded.
  // |own| is false when the program has none of its own (it uses the standard encoding).
  std::string Encoded(unsigned code) const;
  bool OwnEncoding() const { return own_encoding_; }

  // A glyph's advance width in thousandths of the text size; negative when unknown.
  double Width(int glyph) const;
  // Whether a glyph draws nothing (no outline): true only when that's certain.
  bool Blank(int glyph) const;
  // A glyph's name, "" when the program doesn't name its glyphs.
  std::string GlyphName(int glyph) const;
  // Every CID a CID-keyed CFF has (its charset).
  std::vector<unsigned> Cids() const;

  double UnitsPerEm() const { return units_per_em_; }
  // An sfnt table's bytes, "" when there's none.
  std::string Table(const std::string &tag) const;

  // For the developer probe: the font matrix and the CFF width defaults.
  std::string Describe() const;

private:
  bool ReadSfnt(const std::string &data);
  bool ReadCff(const std::string &data, size_t at, size_t length);
  bool ReadType1(const std::string &data, int length1);
  double CffWidth(int glyph) const;

  int glyphs_ = 0;
  bool cid_keyed_ = false, sfnt_ = false, own_encoding_ = false;
  // sfnt
  std::string data_;
  std::map<std::string, std::pair<size_t, size_t>> tables_;  // tag -> offset, length in data_
  double units_per_em_ = 1000;
  std::vector<unsigned> advances_;  // by glyph (TrueType hmtx)
  std::map<std::pair<unsigned, unsigned>, size_t> cmaps_;  // (platform, encoding) -> subtable offset
  // A TrueType program's glyph names, read from its post table when first asked for (a large
  // font has tens of thousands, rarely needed).
  void PostNames() const;
  size_t post_at_ = 0, post_size_ = 0;
  mutable std::once_flag post_read_;
  mutable std::vector<std::string> names_;  // by glyph: post table, CFF charset or Type 1 CharStrings
  mutable std::map<std::string, int> by_name_;
  // CFF
  bool cff_ = false;
  std::string cff_data_;
  double font_matrix_ = 0.001;
  bool top_matrix_ = false;  // the CFF top DICT has a FontMatrix of its own
  std::vector<std::pair<size_t, size_t>> charstrings_;  // offset, length in cff_data_
  std::vector<std::pair<size_t, size_t>> global_subrs_;
  struct Private {
    double default_width = 0, nominal_width = 0;
    std::vector<std::pair<size_t, size_t>> subrs;
    double font_matrix = 0;  // a CID font's own (0: the top one)
  };
  std::vector<Private> privates_;
  std::vector<unsigned char> fd_select_;  // by glyph, for CID-keyed
  std::vector<unsigned> cids_;            // by glyph, for CID-keyed
  std::map<unsigned, int> by_cid_;
  std::map<unsigned, std::string> encoding_;  // code -> name (own encoding)
  // Type 1
  std::vector<double> type1_widths_;
  std::vector<bool> type1_blank_;  // by glyph: its charstring draws nothing
};

// A TrueType or OpenType program with some glyphs' advance widths changed (font units), their
// outlines untouched: hmtx written out in full, hhea's count and maximum updated, the table
// directory rebuilt. False when there is no hmtx table to change (or it is a collection).
bool SetAdvances(std::string &sfnt, const std::map<int, unsigned> &advances);

// A TrueType program cut down to the glyphs in |keep| and the parts composite ones are made
// of, renumbered from 1 (glyph 0 stays): |renumbered| gives each old glyph its new number.
// Layout, device and vertical tables and glyph names are dropped; with |blank_notdef| the
// .notdef glyph draws nothing. False when it has no glyf outlines.
bool Subset(std::string &sfnt, const std::set<int> &keep, std::map<int, int> &renumbered, bool blank_notdef = false);

// A program's cmap replaced by one subtable (platform, encoding) mapping each character to its
// glyph: Unicode (3,1) for a font read by glyph names, Microsoft Symbol (3,0) with codes as
// 0xF000 + code for a symbolic one.
bool SetCmap(std::string &sfnt, unsigned platform, unsigned encoding, const std::map<unsigned, int> &glyphs);

// Glyph name to Unicode, as the Adobe Glyph List has it (and uniXXXX / uXXXX[XX] names, names
// with a suffix, ITC Zapf Dingbats' a1, a2, ...); 0 when it doesn't say.
unsigned UnicodeOf(const std::string &name);

// Whether the Adobe Glyph List has a name (exactly: veraPDF doesn't take uniXXXX ones): what
// PDF/A asks of the names a non-symbolic TrueType font's differences use.
bool InGlyphList(const std::string &name);

// The name the Adobe Glyph List gives the character a name stands for (the name itself when the
// list has it, "sfthyphen" for uni00AD); "" when it has none.
std::string ListedName(const std::string &name);

// A glyph's width in the standard 14 Symbol or ZapfDingbats font (Adobe's AFM files); negative
// for others.
double StandardWidth(const std::string &font, const std::string &glyph);

// The glyph names of a standard encoding by its PDF name (/StandardEncoding, /WinAnsiEncoding,
// /MacRomanEncoding, /MacExpertEncoding, or "Symbol"/"ZapfDingbats" for those fonts' built-in
// ones); null for others.
const char *const *Encoding(const std::string &name);

// The Mac Roman code for a glyph name, or -1.
int MacRomanCode(const std::string &name);

} // namespace fynepdf::pdfa::font
