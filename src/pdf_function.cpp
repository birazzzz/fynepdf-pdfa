#include "pdf_function.h"

#include "content.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace fynepdf::pdfa {
namespace {

using content::StreamData;

std::vector<double> Numbers(QPDFObjectHandle array) {
  std::vector<double> values;
  for (int index = 0; array.isArray() && index < array.getArrayNItems(); ++index) {
    QPDFObjectHandle item = array.getArrayItem(index);
    if (!item.isNumber())
      return {};
    values.push_back(item.getNumericValue());
  }
  return values;
}

double Interpolate(double x, double x0, double x1, double y0, double y1) {
  return x1 == x0 ? y0 : y0 + (x - x0) * (y1 - y0) / (x1 - x0);
}

// Type 0: samples in a table of up to eight inputs, read between the nearest ones.
class Sampled : public PdfFunction {
public:
  bool Load(QPDFObjectHandle stream) {
    QPDFObjectHandle dict = stream.getDict();
    const std::vector<double> size = Numbers(dict.getKey("/Size"));
    bits_ = dict.getKey("/BitsPerSample").isInteger() ? dict.getKey("/BitsPerSample").getIntValueAsInt() : 0;
    if (size.empty() || size.size() != Inputs() || range_.empty() || size.size() > 8 ||
        (bits_ != 1 && bits_ != 2 && bits_ != 4 && bits_ != 8 && bits_ != 12 && bits_ != 16 && bits_ != 24 && bits_ != 32))
      return false;
    double count = 1;
    for (const double each : size) {
      if (each < 1 || each > 65536)
        return false;
      size_.push_back(static_cast<int>(each));
      count *= each;
    }
    if (count * outputs_ > 16.0 * 1024 * 1024)
      return false;
    encode_ = Numbers(dict.getKey("/Encode"));
    if (encode_.size() != 2 * size_.size()) {
      encode_.clear();
      for (const int each : size_)
        encode_.insert(encode_.end(), {0, double(each - 1)});
    }
    decode_ = Numbers(dict.getKey("/Decode"));
    if (decode_.size() != range_.size())
      decode_ = range_;
    bool ok = true;
    data_ = StreamData(stream, ok);
    return ok && data_.size() * 8 >= static_cast<size_t>(count * outputs_ * bits_);
  }

protected:
  bool Run(const std::vector<double> &in, std::vector<double> &out) const override {
    const size_t m = size_.size();
    std::vector<int> low(m);
    std::vector<double> fraction(m);
    for (size_t i = 0; i < m; ++i) {
      double e = Interpolate(in[i], domain_[2 * i], domain_[2 * i + 1], encode_[2 * i], encode_[2 * i + 1]);
      e = std::clamp(e, 0.0, double(size_[i] - 1));
      low[i] = std::min(static_cast<int>(std::floor(e)), std::max(0, size_[i] - 2));
      fraction[i] = size_[i] == 1 ? 0 : e - low[i];
    }
    out.assign(outputs_, 0);
    // Every corner of the cell around the point, weighted by how near it is.
    for (unsigned corner = 0; corner < (1u << m); ++corner) {
      double weight = 1;
      size_t index = 0, stride = 1;
      for (size_t i = 0; i < m; ++i) {
        const bool high = (corner >> i) & 1;
        if (high && size_[i] == 1) {
          weight = 0;
          break;
        }
        weight *= high ? fraction[i] : 1 - fraction[i];
        index += (low[i] + (high ? 1 : 0)) * stride;
        stride *= size_[i];
      }
      if (weight == 0)
        continue;
      for (size_t k = 0; k < outputs_; ++k)
        out[k] += weight * Sample(index * outputs_ + k, k);
    }
    return true;
  }

private:
  double Sample(size_t at, size_t k) const {
    const size_t bit = at * bits_;
    unsigned long long value = 0;
    for (int read = 0; read < bits_; ++read) {
      const size_t position = bit + read;
      value = value << 1 | ((static_cast<unsigned char>(data_[position / 8]) >> (7 - position % 8)) & 1);
    }
    const double most = std::ldexp(1.0, bits_) - 1;
    return Interpolate(double(value), 0, most, decode_[2 * k], decode_[2 * k + 1]);
  }
  std::vector<int> size_;
  std::vector<double> encode_, decode_;
  std::string data_;
  int bits_ = 0;
};

// Type 2: from C0 to C1 by x to the power N.
class Exponential : public PdfFunction {
public:
  bool Load(QPDFObjectHandle dict) {
    c0_ = dict.hasKey("/C0") ? Numbers(dict.getKey("/C0")) : std::vector<double>{0};
    c1_ = dict.hasKey("/C1") ? Numbers(dict.getKey("/C1")) : std::vector<double>{1};
    n_ = dict.getKey("/N").isNumber() ? dict.getKey("/N").getNumericValue() : 1;
    if (c0_.empty() || c0_.size() != c1_.size() || Inputs() != 1)
      return false;
    outputs_ = c0_.size();
    return true;
  }

protected:
  bool Run(const std::vector<double> &in, std::vector<double> &out) const override {
    const double power = std::pow(in[0], n_);
    if (!std::isfinite(power))
      return false;
    out.resize(outputs_);
    for (size_t k = 0; k < outputs_; ++k)
      out[k] = c0_[k] + power * (c1_[k] - c0_[k]);
    return true;
  }

private:
  std::vector<double> c0_, c1_;
  double n_ = 1;
};

// Type 3: other functions, each over its part of the domain.
class Stitching : public PdfFunction {
public:
  bool Load(QPDFObjectHandle dict, int depth) {
    QPDFObjectHandle parts = dict.getKey("/Functions");
    bounds_ = Numbers(dict.getKey("/Bounds"));
    encode_ = Numbers(dict.getKey("/Encode"));
    if (!parts.isArray() || parts.getArrayNItems() == 0 || Inputs() != 1 || bounds_.size() != size_t(parts.getArrayNItems() - 1) ||
        encode_.size() != size_t(2 * parts.getArrayNItems()))
      return false;
    for (int index = 0; index < parts.getArrayNItems(); ++index) {
      std::unique_ptr<PdfFunction> part = Read(parts.getArrayItem(index), depth + 1);
      if (!part || part->Inputs() != 1 || (index > 0 && part->Outputs() != parts_.front()->Outputs()))
        return false;
      parts_.push_back(std::move(part));
    }
    outputs_ = parts_.front()->Outputs();
    return true;
  }

protected:
  bool Run(const std::vector<double> &in, std::vector<double> &out) const override {
    const double x = in[0];
    size_t part = 0;
    while (part < bounds_.size() && x >= bounds_[part])
      ++part;
    const double from = part == 0 ? domain_[0] : bounds_[part - 1];
    const double to = part == bounds_.size() ? domain_[1] : bounds_[part];
    return parts_[part]->Evaluate({Interpolate(x, from, to, encode_[2 * part], encode_[2 * part + 1])}, out);
  }

private:
  std::vector<double> bounds_, encode_;
  std::vector<std::unique_ptr<PdfFunction>> parts_;
};

// Type 4: a small PostScript program, run on a stack.
class Calculator : public PdfFunction {
public:
  bool Load(QPDFObjectHandle stream) {
    bool ok = true;
    const std::string text = StreamData(stream, ok);
    size_t at = text.find('{');
    if (!ok || at == std::string::npos || range_.empty())
      return false;
    ++at;
    outputs_ = range_.size() / 2;
    return Parse(text, at, program_, 0);
  }

protected:
  struct Value {
    double number = 0;
    bool boolean = false;
  };
  bool Run(const std::vector<double> &in, std::vector<double> &out) const override {
    std::vector<Value> stack;
    for (const double each : in)
      stack.push_back({each, false});
    int steps = 0;
    if (!Execute(program_, stack, steps) || stack.size() < outputs_)
      return false;
    out.assign(outputs_, 0);
    for (size_t k = 0; k < outputs_; ++k)
      out[k] = stack[stack.size() - outputs_ + k].number;
    return true;
  }

private:
  struct Instruction {
    std::string op;  // empty for a number
    double number = 0;
    std::vector<Instruction> then, otherwise;
  };

  bool Parse(const std::string &text, size_t &at, std::vector<Instruction> &into, int depth) {
    if (depth > 16)
      return false;
    std::vector<std::vector<Instruction>> blocks;  // procedures waiting for their if or ifelse
    while (at < text.size()) {
      const char c = text[at];
      if (std::isspace(static_cast<unsigned char>(c))) {
        ++at;
      } else if (c == '{') {
        ++at;
        blocks.emplace_back();
        if (!Parse(text, at, blocks.back(), depth + 1))
          return false;
      } else if (c == '}') {
        ++at;
        return blocks.empty();
      } else {
        size_t end = at;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) && text[end] != '{' && text[end] != '}')
          ++end;
        const std::string word = text.substr(at, end - at);
        at = end;
        char *stop = nullptr;
        const double number = std::strtod(word.c_str(), &stop);
        Instruction instruction;
        if (stop == word.c_str() + word.size() && !word.empty()) {
          instruction.number = number;
        } else if (word == "if") {
          if (blocks.size() != 1)
            return false;
          instruction.op = word;
          instruction.then = std::move(blocks[0]);
          blocks.clear();
        } else if (word == "ifelse") {
          if (blocks.size() != 2)
            return false;
          instruction.op = word;
          instruction.then = std::move(blocks[0]);
          instruction.otherwise = std::move(blocks[1]);
          blocks.clear();
        } else {
          instruction.op = word;
        }
        if (!blocks.empty())
          return false;
        into.push_back(std::move(instruction));
      }
    }
    return false;
  }

  static bool Execute(const std::vector<Instruction> &program, std::vector<Value> &stack, int &steps) {
    const auto pop = [&stack](Value &value) {
      if (stack.empty())
        return false;
      value = stack.back();
      stack.pop_back();
      return true;
    };
    for (const Instruction &instruction : program) {
      if (++steps > 100000 || stack.size() > 1000)
        return false;
      const std::string &op = instruction.op;
      if (op.empty()) {
        stack.push_back({instruction.number, false});
        continue;
      }
      Value a, b;
      if (op == "if" || op == "ifelse") {
        if (!pop(a))
          return false;
        const std::vector<Instruction> &branch = a.number != 0 ? instruction.then : instruction.otherwise;
        if (!Execute(branch, stack, steps))
          return false;
      } else if (op == "true" || op == "false") {
        stack.push_back({op == "true" ? 1.0 : 0.0, true});
      } else if (op == "dup") {
        if (stack.empty())
          return false;
        stack.push_back(stack.back());
      } else if (op == "pop") {
        if (!pop(a))
          return false;
      } else if (op == "exch") {
        if (!pop(b) || !pop(a))
          return false;
        stack.push_back(b);
        stack.push_back(a);
      } else if (op == "copy" || op == "index") {
        if (!pop(a))
          return false;
        const long n = std::lround(a.number);
        if (n < 0 || size_t(n) > stack.size() || (op == "index" && size_t(n) >= stack.size()))
          return false;
        if (op == "copy")
          stack.insert(stack.end(), stack.end() - n, stack.end());
        else
          stack.push_back(stack[stack.size() - 1 - n]);
      } else if (op == "roll") {
        if (!pop(b) || !pop(a))
          return false;
        const long n = std::lround(a.number), j = std::lround(b.number);
        if (n < 0 || size_t(n) > stack.size())
          return false;
        if (n > 0) {
          const long shift = ((j % n) + n) % n;
          std::rotate(stack.end() - n, stack.end() - shift, stack.end());
        }
      } else if (op == "abs" || op == "neg" || op == "ceiling" || op == "floor" || op == "round" || op == "truncate" || op == "sqrt" ||
                 op == "sin" || op == "cos" || op == "ln" || op == "log" || op == "cvi" || op == "cvr" || op == "not") {
        if (!pop(a))
          return false;
        const double x = a.number, radians = x * 3.14159265358979323846 / 180;
        double r = x;
        if (op == "abs") r = std::abs(x);
        else if (op == "neg") r = -x;
        else if (op == "ceiling") r = std::ceil(x);
        else if (op == "floor") r = std::floor(x);
        else if (op == "round") r = std::floor(x + 0.5);
        else if (op == "truncate" || op == "cvi") r = std::trunc(x);
        else if (op == "sqrt") r = std::sqrt(x);
        else if (op == "sin") r = std::sin(radians);
        else if (op == "cos") r = std::cos(radians);
        else if (op == "ln") r = std::log(x);
        else if (op == "log") r = std::log10(x);
        else if (op == "not") r = a.boolean ? (x != 0 ? 0 : 1) : double(~static_cast<long long>(x));
        stack.push_back({r, op == "not" && a.boolean});
      } else {
        if (!pop(b) || !pop(a))
          return false;
        const double x = a.number, y = b.number;
        const long long i = static_cast<long long>(x), j = static_cast<long long>(y);
        double r = 0;
        bool boolean = false;
        if (op == "add") r = x + y;
        else if (op == "sub") r = x - y;
        else if (op == "mul") r = x * y;
        else if (op == "div") {
          if (y == 0) return false;
          r = x / y;
        } else if (op == "idiv" || op == "mod") {
          if (j == 0) return false;
          r = double(op == "idiv" ? i / j : i % j);
        } else if (op == "exp") r = std::pow(x, y);
        else if (op == "atan") {
          r = std::atan2(x, y) * 180 / 3.14159265358979323846;
          if (r < 0) r += 360;
        } else if (op == "bitshift") r = double(j >= 0 ? i << j : i >> -j);
        else if (op == "and" || op == "or" || op == "xor") {
          boolean = a.boolean && b.boolean;
          const long long v = op == "and" ? (i & j) : op == "or" ? (i | j) : (i ^ j);
          r = double(v);
        } else if (op == "eq" || op == "ne" || op == "gt" || op == "ge" || op == "lt" || op == "le") {
          boolean = true;
          r = (op == "eq" ? x == y : op == "ne" ? x != y : op == "gt" ? x > y : op == "ge" ? x >= y : op == "lt" ? x < y : x <= y) ? 1 : 0;
        } else {
          return false;
        }
        if (!std::isfinite(r))
          return false;
        stack.push_back({r, boolean});
      }
    }
    return true;
  }

  std::vector<Instruction> program_;
};

// One-output functions side by side.
class Joined : public PdfFunction {
public:
  bool Load(QPDFObjectHandle array, int depth) {
    for (int index = 0; index < array.getArrayNItems(); ++index) {
      std::unique_ptr<PdfFunction> part = Read(array.getArrayItem(index), depth + 1);
      if (!part || part->Outputs() != 1 || (index > 0 && part->Inputs() != parts_.front()->Inputs()))
        return false;
      parts_.push_back(std::move(part));
    }
    if (parts_.empty())
      return false;
    domain_ = parts_.front()->Domain();
    outputs_ = parts_.size();
    return true;
  }

protected:
  bool Run(const std::vector<double> &in, std::vector<double> &out) const override {
    out.clear();
    std::vector<double> one;
    for (const auto &part : parts_) {
      if (!part->Evaluate(in, one))
        return false;
      out.push_back(one[0]);
    }
    return true;
  }

private:
  std::vector<std::unique_ptr<PdfFunction>> parts_;
};

} // namespace

std::unique_ptr<PdfFunction> PdfFunction::Read(QPDFObjectHandle handle, int depth) {
  if (depth > 8)
    return nullptr;
  if (handle.isArray()) {
    auto joined = std::make_unique<Joined>();
    return joined->Load(handle, depth) ? std::move(joined) : nullptr;
  }
  QPDFObjectHandle dict = handle.isStream() ? handle.getDict() : handle;
  if (!dict.isDictionary() || !dict.getKey("/FunctionType").isInteger())
    return nullptr;
  const std::vector<double> domain = Numbers(dict.getKey("/Domain"));
  const std::vector<double> range = Numbers(dict.getKey("/Range"));
  if (domain.empty() || domain.size() % 2 != 0 || range.size() % 2 != 0)
    return nullptr;
  const auto prepared = [&](PdfFunction &function) {
    function.domain_ = domain;
    function.range_ = range;
    function.outputs_ = range.size() / 2;
  };
  switch (dict.getKey("/FunctionType").getIntValueAsInt()) {
  case 0: {
    auto sampled = std::make_unique<Sampled>();
    prepared(*sampled);
    return handle.isStream() && sampled->Load(handle) ? std::move(sampled) : nullptr;
  }
  case 2: {
    auto exponential = std::make_unique<Exponential>();
    prepared(*exponential);
    return exponential->Load(dict) ? std::move(exponential) : nullptr;
  }
  case 3: {
    auto stitching = std::make_unique<Stitching>();
    prepared(*stitching);
    return stitching->Load(dict, depth) ? std::move(stitching) : nullptr;
  }
  case 4: {
    auto calculator = std::make_unique<Calculator>();
    prepared(*calculator);
    return handle.isStream() && calculator->Load(handle) ? std::move(calculator) : nullptr;
  }
  default:
    return nullptr;
  }
}

bool PdfFunction::Evaluate(const std::vector<double> &in, std::vector<double> &out) const {
  if (in.size() < Inputs())
    return false;
  std::vector<double> clipped(in.begin(), in.begin() + Inputs());
  for (size_t i = 0; i < clipped.size(); ++i)
    clipped[i] = std::clamp(clipped[i], std::min(domain_[2 * i], domain_[2 * i + 1]), std::max(domain_[2 * i], domain_[2 * i + 1]));
  if (!Run(clipped, out) || out.size() < outputs_)
    return false;
  for (size_t k = 0; k < range_.size() / 2 && k < out.size(); ++k)
    out[k] = std::clamp(out[k], std::min(range_[2 * k], range_[2 * k + 1]), std::max(range_[2 * k], range_[2 * k + 1]));
  return true;
}

} // namespace fynepdf::pdfa
