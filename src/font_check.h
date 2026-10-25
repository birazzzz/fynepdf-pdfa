#pragma once

// A PDF font's glyphs checked as PDF/A asks: every character code a page shows is looked up
// in the embedded font program the way readers find it (ISO 32000-1 9.6.6 and 9.7.4), and its
// width compared with the font dictionary's.

#include "font_program.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <qpdf/QPDFObjectHandle.hh>

namespace fynepdf::pdfa::font {

class Checker {
public:
  // |font|: a simple or Type 0 font dictionary with an embedded program.
  explicit Checker(QPDFObjectHandle font);

  // Whether the codes can be checked; Why() says why not (the program can't be read, a CMap
  // that isn't embedded and isn't Identity).
  bool Ready() const { return ready_; }
  const std::string &Why() const { return why_; }

  // A string a text operator shows; |drawn|: in a rendering mode other than 3 (invisible).
  void Show(const std::string &bytes, bool drawn);

  // A shown string split into its codes (a byte each for simple fonts; by the CMap for Type 0,
  // where |code| is the CID): where each starts and how long it is. A half code left at the end
  // isn't one.
  struct Piece {
    size_t begin = 0, length = 0;
    unsigned code = 0;
  };
  std::vector<Piece> Pieces(const std::string &bytes) const;
  // Whether a code draws .notdef (or a glyph the program doesn't have, which readers draw as
  // .notdef), and whether that glyph draws nothing.
  bool DrawsNotdef(unsigned code) const;
  bool NotdefBlank() const;
  // A Type 0 font's widths by CID, and its default.
  const std::map<unsigned, double> &CidWidths() const { return cid_widths_; }
  double DefaultWidth() const { return default_width_; }

  // What the shown codes found (each code counted once).
  int BigCids() const { return big_cids_; }   // CIDs past 65,535 (PDF/A-2 6.1.13)
  int Missing() const { return missing_; }    // no glyph in the program
  int Notdef() const { return notdef_; }      // drawn with the .notdef glyph
  int Widths() const { return widths_; }      // the dictionary's width differs from the program's by more than 1
  // The first code each finding was seen for, to say in a problem.
  const std::string &Example() const { return example_; }

  const Program *Font() const { return program_.get(); }
  // The glyph a code (simple fonts) or CID (Type 0) draws, -1 for none: the lookup Show uses.
  int GlyphFor(unsigned code) const;
  // The glyph name a simple font's encoding gives a code, "" for none.
  const std::string &NameFor(unsigned code) const { return names_[code & 0xff]; }
  double DictionaryWidth(unsigned code) const;
  bool Composite() const { return composite_; }
  // Every code (CID for Type 0) the pages showed, and whether any showing was drawn (not
  // rendering mode 3).
  std::vector<std::pair<unsigned, bool>> Codes() const;
  // A Type 0 font's CIDs the pages showed and the code each was first shown by (for CMaps whose
  // codes are characters, the predefined Unicode ones: several codes can give one CID).
  const std::map<unsigned, unsigned> &ShownCodes() const { return shown_codes_; }
  // Every code (bytes) shown in such a font, with its CID.
  const std::map<std::string, unsigned> &ShownCids() const { return shown_cids_; }
  // The same codes checked again, as |other| saw them (after the font was changed).
  void Replay(const Checker &other);

private:
  void ReadSimple(QPDFObjectHandle font, QPDFObjectHandle descriptor);
  void ReadComposite(QPDFObjectHandle font, QPDFObjectHandle descendant);
  void ReadType3(QPDFObjectHandle font);
  double Type3Width(const std::string &name);
  void ReadCMap(QPDFObjectHandle stream, int depth = 0);
  void UseCMap(const std::string &name, int depth);
  void ReadCMapProgram(const std::string &program, int depth);
  void FinishCMap();
  void Check(unsigned code, bool drawn);

  bool ready_ = false, composite_ = false, sfnt_lookup_ = false, symbolic_ = false, type3_ = false;
  QPDFObjectHandle procs_ = QPDFObjectHandle::newNull();  // a Type 3 font's glyphs
  std::map<std::string, double> type3_widths_;           // by glyph name, read from d0 or d1
  std::string why_, example_;
  std::shared_ptr<const Program> program_;  // shared by checkers of the same bytes
  // Simple fonts: the glyph name each code has (from the encoding), and the widths.
  std::vector<std::string> names_ = std::vector<std::string>(256);
  bool named_ = false;  // an encoding gives names (else codes go straight to the cmap)
  int first_char_ = 0;
  std::vector<double> widths_list_;
  double missing_width_ = 0;
  // Type 0: codes to CIDs, CIDs to glyphs, widths by CID.
  bool identity_ = true;
  struct Space {
    std::string low, high;
  };
  std::vector<Space> spaces_;
  struct CidRange {
    std::string low, high;
    unsigned cid;
  };
  std::vector<CidRange> cid_ranges_;          // in the order defined: a later one wins
  std::vector<int> cid1_, cid2_;               // one- and two-byte codes' CIDs (-1: none)
  std::map<unsigned, unsigned> shown_codes_;   // CID: the code first shown for it
  std::map<std::string, unsigned> shown_cids_; // code: its CID
  std::string gid_map_;  // CIDToGIDMap stream (2 bytes a CID); empty: identity
  bool gid_map_stream_ = false;
  double default_width_ = 1000;
  std::map<unsigned, double> cid_widths_;
  // Results by code: 2 missing, 4 notdef, 8 width checked, 16 drawn.
  std::map<unsigned, unsigned char> seen_;
  int missing_ = 0, notdef_ = 0, widths_ = 0, big_cids_ = 0;
};

} // namespace fynepdf::pdfa::font
