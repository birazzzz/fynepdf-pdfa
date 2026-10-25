#pragma once

#include <memory>
#include <vector>

#include <qpdf/QPDFObjectHandle.hh>

namespace fynepdf::pdfa {

// A PDF function (PDF 32000 7.10), evaluated for colour conversion: a spot ink's tint transform
// to its alternate colours, and a gradient's colour along its length.
// Sampled (Type 0, multilinear between samples), exponential (2), stitched (3) and PostScript
// calculator (4) functions; inputs are clipped to the domain, outputs to the range.
class PdfFunction {
public:
  virtual ~PdfFunction() = default;
  // The function in |handle| (a dictionary or a stream), or an array of one-output functions
  // taken together (as a gradient's /Function may be); nothing when it can't be read.
  static std::unique_ptr<PdfFunction> Read(QPDFObjectHandle handle, int depth = 0);
  bool Evaluate(const std::vector<double> &in, std::vector<double> &out) const;
  size_t Inputs() const { return domain_.size() / 2; }
  size_t Outputs() const { return outputs_; }
  double DomainStart() const { return domain_.empty() ? 0 : domain_[0]; }
  double DomainEnd() const { return domain_.size() < 2 ? 1 : domain_[1]; }
  const std::vector<double> &Domain() const { return domain_; }

protected:
  virtual bool Run(const std::vector<double> &in, std::vector<double> &out) const = 0;
  std::vector<double> domain_, range_;
  size_t outputs_ = 0;
};

} // namespace fynepdf::pdfa
