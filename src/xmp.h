#pragma once

// XMP metadata for PDF/A: the packet the catalog's /Metadata stream holds, written from the
// document's description, and the dates both it and the Info dictionary carry (PDF/A requires
// the two to agree).

#include <string>

namespace fynepdf::pdfa::xmp {

// A document's description, UTF-8; empty for what it doesn't have.
struct Description {
  std::string title, author, subject, keywords, creator, producer;
  std::string created, modified;  // XMP dates, such as 2026-10-24T13:53:50+05:45
};

// A property's value in an existing packet, "" when it isn't there: an element's text (for
// language alternatives, the default language's; for lists, the first item) or an attribute's
// value. |name| is the property with its usual prefix, such as "dc:title"; "xmp:" also finds
// the older "xap:".
std::string Find(const std::string &packet, const std::string &name);

// The packet: the description in the Dublin Core, XMP basic and Adobe PDF schemas and, when
// |part| is 1, 2 or 3, the PDF/A identification (conformance B).
std::string Packet(const Description &description, int part);

// Dates from PDF (D:YYYYMMDDHHmmSSOHH'mm') to XMP and back, missing parts filled in (a missing
// time zone taken as UTC); false when |text| isn't a date. ToPdf writes the full form.
bool PdfDateToXmp(const std::string &text, std::string &out);
bool XmpDateToXmp(const std::string &text, std::string &out);
bool XmpDateToPdf(const std::string &text, std::string &out);

// |text| without what XML 1.0 can't carry (control characters other than tab and line ends,
// broken UTF-8, U+FFFE and U+FFFF).
std::string Clean(const std::string &text);

} // namespace fynepdf::pdfa::xmp
