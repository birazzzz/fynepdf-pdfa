#pragma once

// Small helpers the converters share: PDF objects, colour formulas, opening and copying.

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>

namespace fynepdf::pdfa {

inline bool IsName(QPDFObjectHandle handle, const char *name) { return handle.isName() && handle.getName() == name; }

inline int Integer(QPDFObjectHandle dict, const char *key, int fallback) {
  QPDFObjectHandle value = dict.isDictionary() ? dict.getKey(key) : QPDFObjectHandle::newNull();
  return value.isInteger() ? value.getIntValueAsInt() : fallback;
}

// An array's numbers, or nothing when it isn't an array of numbers.
inline std::vector<double> Numbers(QPDFObjectHandle array) {
  std::vector<double> values;
  for (int index = 0; array.isArray() && index < array.getArrayNItems(); ++index) {
    if (!array.getArrayItem(index).isNumber())
      return {};
    values.push_back(array.getArrayItem(index).getNumericValue());
  }
  return values;
}

inline QPDFObjectHandle NumberArray(const std::vector<double> &values) {
  std::vector<QPDFObjectHandle> items;
  for (const double value : values)
    items.push_back(QPDFObjectHandle::newReal(value, 4, true));
  return QPDFObjectHandle::newArray(items);
}

inline double Luma(double r, double g, double b) { return std::clamp(0.299 * r + 0.587 * g + 0.114 * b, 0.0, 1.0); }

inline double CmykGray(double c, double m, double y, double k) {
  return std::clamp(1 - std::min(1.0, 0.3 * c + 0.59 * m + 0.11 * y + k), 0.0, 1.0);
}

// Device CMYK as RGB the way most readers show it: between the colours the sixteen ink
// combinations print on a coated press (SWOP), each weighted by how much of its inks there is.
inline void CmykToRgb(double c, double m, double y, double k, double &r, double &g, double &b) {
  c = std::clamp(c, 0.0, 1.0), m = std::clamp(m, 0.0, 1.0), y = std::clamp(y, 0.0, 1.0), k = std::clamp(k, 0.0, 1.0);
  const double c1 = 1 - c, m1 = 1 - m, y1 = 1 - y, k1 = 1 - k;
  // {weight, red, green, blue} for paper, K, Y, YK, M, MK, MY, MYK, C, CK, CY, CYK, CM, CMK, CMY;
  // all four inks print black.
  const double corners[15][4] = {
      {c1 * m1 * y1 * k1, 1, 1, 1},          {c1 * m1 * y1 * k, 0.1373, 0.1216, 0.1255}, {c1 * m1 * y * k1, 1, 0.9490, 0},
      {c1 * m1 * y * k, 0.1098, 0.1020, 0},  {c1 * m * y1 * k1, 0.9255, 0, 0.5490},       {c1 * m * y1 * k, 0.1412, 0, 0},
      {c1 * m * y * k1, 0.9294, 0.1098, 0.1412}, {c1 * m * y * k, 0.1333, 0, 0},          {c * m1 * y1 * k1, 0, 0.6784, 0.9373},
      {c * m1 * y1 * k, 0, 0.0588, 0.1412},  {c * m1 * y * k1, 0, 0.6510, 0.3137},       {c * m1 * y * k, 0, 0.0745, 0},
      {c * m * y1 * k1, 0.1804, 0.1922, 0.5725}, {c * m * y1 * k, 0, 0, 0.0078},          {c * m * y * k1, 0.2118, 0.2119, 0.2235}};
  r = g = b = 0;
  for (const auto &corner : corners) {
    r += corner[0] * corner[1];
    g += corner[0] * corner[2];
    b += corner[0] * corner[3];
  }
  r = std::clamp(r, 0.0, 1.0), g = std::clamp(g, 0.0, 1.0), b = std::clamp(b, 0.0, 1.0);
}

// CIE Lab to sRGB, by way of XYZ under D50.
inline void LabToRgb(double l, double a, double b, double &red, double &green, double &blue) {
  const auto f = [](double t) { return t > 6.0 / 29 ? t * t * t : 3 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29); };
  const double fy = (l + 16) / 116;
  const double x = 0.9642 * f(fy + a / 500), y = f(fy), z = 0.8249 * f(fy - b / 200);
  const double linear[3] = {3.1338561 * x - 1.6168667 * y - 0.4906146 * z, -0.9787684 * x + 1.9161415 * y + 0.0334540 * z,
                            0.0719453 * x - 0.2289914 * y + 1.4052427 * z};
  double out[3];
  for (int k = 0; k < 3; ++k) {
    const double v = std::clamp(linear[k], 0.0, 1.0);
    out[k] = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
  }
  red = out[0], green = out[1], blue = out[2];
}

// Signed signature fields: rewriting the file invalidates their signatures.
inline int CountSignatures(QPDF &q) {
  QPDFObjectHandle form = q.getRoot().getKey("/AcroForm");
  if (!form.isDictionary())
    return 0;
  int signed_fields = 0;
  std::vector<QPDFObjectHandle> fields;
  QPDFObjectHandle top = form.getKey("/Fields");
  for (int index = 0; top.isArray() && index < top.getArrayNItems(); ++index)
    fields.push_back(top.getArrayItem(index));
  std::set<QPDFObjGen> seen;
  while (!fields.empty()) {
    QPDFObjectHandle field = fields.back();
    fields.pop_back();
    if (!field.isDictionary() || (field.isIndirect() && !seen.insert(field.getObjGen()).second) || seen.size() > 100000)
      continue;
    if (IsName(field.getKey("/FT"), "/Sig") && field.getKey("/V").isDictionary())
      ++signed_fields;
    QPDFObjectHandle kids = field.getKey("/Kids");
    for (int index = 0; kids.isArray() && index < kids.getArrayNItems(); ++index)
      fields.push_back(kids.getArrayItem(index));
  }
  return signed_fields;
}

// A file opened quietly, every page with its own resources and boxes (none inherited).
inline void Open(QPDF &q, const std::string &file) {
  q.setSuppressWarnings(true);
  q.processFile(file.c_str());
  QPDFPageDocumentHelper(q).pushInheritedAttributesToPage();
}

// A copy of |object| from |source| into |target|; foreign copies must start from indirect objects.
inline QPDFObjectHandle Foreign(QPDF &target, QPDF &source, QPDFObjectHandle object) {
  if (object.isNull())
    return object;
  if (!object.isIndirect())
    object = source.makeIndirectObject(object);
  return target.copyForeignObject(object);
}

} // namespace fynepdf::pdfa
