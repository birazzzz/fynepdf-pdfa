#include "xmp.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace fynepdf::pdfa::xmp {
namespace {

std::string Escape(const std::string &text) {
  std::string out;
  for (const char c : text) {
    if (c == '&')
      out += "&amp;";
    else if (c == '<')
      out += "&lt;";
    else if (c == '>')
      out += "&gt;";
    else if (c == '"')
      out += "&quot;";
    else
      out += c;
  }
  return out;
}

void AppendUtf8(std::string &out, unsigned long code) {
  if (code < 0x80) {
    out += static_cast<char>(code);
  } else if (code < 0x800) {
    out += static_cast<char>(0xc0 | code >> 6);
    out += static_cast<char>(0x80 | (code & 0x3f));
  } else if (code < 0x10000) {
    out += static_cast<char>(0xe0 | code >> 12);
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (code & 0x3f));
  } else if (code < 0x110000) {
    out += static_cast<char>(0xf0 | code >> 18);
    out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
    out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (code & 0x3f));
  }
}

std::string Unescape(const std::string &text) {
  std::string out;
  for (size_t at = 0; at < text.size(); ++at) {
    if (text[at] != '&') {
      out += text[at];
      continue;
    }
    const size_t end = text.find(';', at);
    if (end == std::string::npos || end - at > 12) {
      out += '&';
      continue;
    }
    const std::string entity = text.substr(at + 1, end - at - 1);
    if (entity == "amp")
      out += '&';
    else if (entity == "lt")
      out += '<';
    else if (entity == "gt")
      out += '>';
    else if (entity == "quot")
      out += '"';
    else if (entity == "apos")
      out += '\'';
    else if (entity.size() > 1 && entity[0] == '#')
      AppendUtf8(out, std::strtoul(entity.c_str() + (entity[1] == 'x' ? 2 : 1), nullptr, entity[1] == 'x' ? 16 : 10));
    else
      out += "&" + entity + ";";
    at = end;
  }
  return out;
}

std::string Trim(const std::string &text) {
  size_t begin = 0, end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
    ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
    --end;
  return text.substr(begin, end - begin);
}

bool NameEnds(char c) { return c == '>' || c == '/' || std::isspace(static_cast<unsigned char>(c)); }

std::string FindExactly(const std::string &packet, const std::string &name) {
  // As an element: <name ...>value</name>, the value maybe a list or language alternative.
  for (size_t at = packet.find("<" + name); at != std::string::npos; at = packet.find("<" + name, at + 1)) {
    const size_t after = at + 1 + name.size();
    if (after >= packet.size() || !NameEnds(packet[after]))
      continue;
    const size_t open_end = packet.find('>', after);
    if (open_end == std::string::npos)
      return "";
    if (packet[open_end - 1] == '/')
      continue;
    const size_t close = packet.find("</" + name, open_end);
    if (close == std::string::npos)
      return "";
    const std::string inner = packet.substr(open_end + 1, close - open_end - 1);
    if (inner.find("<rdf:li") != std::string::npos) {
      const size_t preferred = inner.find("x-default");
      size_t item = preferred != std::string::npos ? inner.rfind("<rdf:li", preferred) : inner.find("<rdf:li");
      if (item == std::string::npos)
        item = inner.find("<rdf:li");
      const size_t item_end = inner.find('>', item);
      if (item_end == std::string::npos || inner[item_end - 1] == '/')
        return "";
      const size_t stop = inner.find("</rdf:li", item_end);
      return stop == std::string::npos ? "" : Unescape(inner.substr(item_end + 1, stop - item_end - 1));
    }
    if (inner.find('<') != std::string::npos)
      return "";
    return Unescape(Trim(inner));
  }
  // As an attribute: name="value".
  for (size_t at = packet.find(name); at != std::string::npos; at = packet.find(name, at + 1)) {
    if (at == 0 || !std::isspace(static_cast<unsigned char>(packet[at - 1])))
      continue;
    size_t pos = at + name.size();
    while (pos < packet.size() && std::isspace(static_cast<unsigned char>(packet[pos])))
      ++pos;
    if (pos >= packet.size() || packet[pos] != '=')
      continue;
    ++pos;
    while (pos < packet.size() && std::isspace(static_cast<unsigned char>(packet[pos])))
      ++pos;
    if (pos >= packet.size() || (packet[pos] != '"' && packet[pos] != '\''))
      continue;
    const size_t end = packet.find(packet[pos], pos + 1);
    if (end == std::string::npos)
      return "";
    return Unescape(packet.substr(pos + 1, end - pos - 1));
  }
  return "";
}

struct Date {
  int year = 0, month = 1, day = 1, hour = 0, minute = 0, second = 0;
  char zone = 'Z';  // 'Z', '+' or '-'
  int zone_hour = 0, zone_minute = 0;
};

bool Digits(const std::string &text, size_t &pos, size_t count, int &value) {
  if (pos + count > text.size())
    return false;
  value = 0;
  for (size_t k = 0; k < count; ++k) {
    if (!std::isdigit(static_cast<unsigned char>(text[pos + k])))
      return false;
    value = value * 10 + (text[pos + k] - '0');
  }
  pos += count;
  return true;
}

bool Valid(const Date &d) {
  return d.year >= 1 && d.month >= 1 && d.month <= 12 && d.day >= 1 && d.day <= 31 && d.hour <= 23 && d.minute <= 59 && d.second <= 59 &&
         d.zone_hour <= 23 && d.zone_minute <= 59;
}

bool ParsePdf(const std::string &raw, Date &d) {
  const std::string text = Trim(raw);
  size_t pos = text.compare(0, 2, "D:") == 0 ? 2 : 0;
  if (!Digits(text, pos, 4, d.year))
    return false;
  int *parts[] = {&d.month, &d.day, &d.hour, &d.minute, &d.second};
  for (int *part : parts)
    if (!Digits(text, pos, 2, *part))
      break;
  if (pos < text.size() && (text[pos] == 'Z' || text[pos] == '+' || text[pos] == '-')) {
    d.zone = text[pos++];
    if (Digits(text, pos, 2, d.zone_hour)) {
      if (pos < text.size() && text[pos] == '\'')
        ++pos;
      Digits(text, pos, 2, d.zone_minute);
    }
    if (d.zone != 'Z' && d.zone_hour == 0 && d.zone_minute == 0)
      d.zone = 'Z';
  }
  return Valid(d);
}

bool ParseXmp(const std::string &raw, Date &d) {
  const std::string text = Trim(raw);
  size_t pos = 0;
  if (!Digits(text, pos, 4, d.year))
    return false;
  if (pos < text.size() && text[pos] == '-' && (++pos, Digits(text, pos, 2, d.month)) && pos < text.size() && text[pos] == '-' &&
      (++pos, Digits(text, pos, 2, d.day)) && pos < text.size() && text[pos] == 'T' && (++pos, Digits(text, pos, 2, d.hour)) &&
      pos < text.size() && text[pos] == ':' && (++pos, Digits(text, pos, 2, d.minute))) {
    if (pos < text.size() && text[pos] == ':' && (++pos, Digits(text, pos, 2, d.second))) {
      if (pos < text.size() && text[pos] == '.')
        for (++pos; pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]));)
          ++pos;
    }
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
      d.zone = text[pos++];
      if (!Digits(text, pos, 2, d.zone_hour) || pos >= text.size() || text[pos] != ':' || (++pos, !Digits(text, pos, 2, d.zone_minute)))
        return false;
      if (d.zone_hour == 0 && d.zone_minute == 0)
        d.zone = 'Z';
    }
  }
  return Valid(d);
}

std::string FormatXmp(const Date &d) {
  char buffer[40];
  std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02dT%02d:%02d:%02d", d.year, d.month, d.day, d.hour, d.minute, d.second);
  std::string out = buffer;
  if (d.zone == 'Z') {
    out += "Z";
  } else {
    std::snprintf(buffer, sizeof buffer, "%c%02d:%02d", d.zone, d.zone_hour, d.zone_minute);
    out += buffer;
  }
  return out;
}

std::string FormatPdf(const Date &d) {
  char buffer[40];
  std::snprintf(buffer, sizeof buffer, "D:%04d%02d%02d%02d%02d%02d", d.year, d.month, d.day, d.hour, d.minute, d.second);
  std::string out = buffer;
  if (d.zone == 'Z') {
    out += "Z";
  } else {
    std::snprintf(buffer, sizeof buffer, "%c%02d'%02d'", d.zone, d.zone_hour, d.zone_minute);
    out += buffer;
  }
  return out;
}

} // namespace

std::string Find(const std::string &packet, const std::string &name) {
  std::string value = FindExactly(packet, name);
  if (value.empty() && name.compare(0, 4, "xmp:") == 0)
    value = FindExactly(packet, "xap:" + name.substr(4));
  return Clean(value);
}

std::string Packet(const Description &d, int part) {
  std::string out = "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
                    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
                    " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n";
  if (part >= 1 && part <= 3)
    out += "  <rdf:Description rdf:about=\"\" xmlns:pdfaid=\"http://www.aiim.org/pdfa/ns/id/\">\n"
           "   <pdfaid:part>" +
           std::to_string(part) +
           "</pdfaid:part>\n"
           "   <pdfaid:conformance>B</pdfaid:conformance>\n"
           "  </rdf:Description>\n";
  out += "  <rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
         "   <dc:format>application/pdf</dc:format>\n";
  if (!d.title.empty())
    out += "   <dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">" + Escape(d.title) + "</rdf:li></rdf:Alt></dc:title>\n";
  if (!d.author.empty())
    out += "   <dc:creator><rdf:Seq><rdf:li>" + Escape(d.author) + "</rdf:li></rdf:Seq></dc:creator>\n";
  if (!d.subject.empty())
    out += "   <dc:description><rdf:Alt><rdf:li xml:lang=\"x-default\">" + Escape(d.subject) + "</rdf:li></rdf:Alt></dc:description>\n";
  out += "  </rdf:Description>\n";
  if (!d.creator.empty() || !d.created.empty() || !d.modified.empty()) {
    out += "  <rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\">\n";
    if (!d.creator.empty())
      out += "   <xmp:CreatorTool>" + Escape(d.creator) + "</xmp:CreatorTool>\n";
    if (!d.created.empty())
      out += "   <xmp:CreateDate>" + d.created + "</xmp:CreateDate>\n";
    if (!d.modified.empty())
      out += "   <xmp:ModifyDate>" + d.modified + "</xmp:ModifyDate>\n";
    out += "  </rdf:Description>\n";
  }
  if (!d.producer.empty() || !d.keywords.empty()) {
    out += "  <rdf:Description rdf:about=\"\" xmlns:pdf=\"http://ns.adobe.com/pdf/1.3/\">\n";
    if (!d.producer.empty())
      out += "   <pdf:Producer>" + Escape(d.producer) + "</pdf:Producer>\n";
    if (!d.keywords.empty())
      out += "   <pdf:Keywords>" + Escape(d.keywords) + "</pdf:Keywords>\n";
    out += "  </rdf:Description>\n";
  }
  out += " </rdf:RDF>\n</x:xmpmeta>\n";
  // Room for editors to change the packet in place, as XMP recommends.
  for (int line = 0; line < 20; ++line)
    out += std::string(99, ' ') + "\n";
  out += "<?xpacket end=\"w\"?>";
  return out;
}

bool PdfDateToXmp(const std::string &text, std::string &out) {
  Date date;
  if (!ParsePdf(text, date))
    return false;
  out = FormatXmp(date);
  return true;
}

bool XmpDateToXmp(const std::string &text, std::string &out) {
  Date date;
  if (!ParseXmp(text, date))
    return false;
  out = FormatXmp(date);
  return true;
}

bool XmpDateToPdf(const std::string &text, std::string &out) {
  Date date;
  if (!ParseXmp(text, date))
    return false;
  out = FormatPdf(date);
  return true;
}

std::string Clean(const std::string &text) {
  std::string out;
  for (size_t at = 0; at < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[at]);
    size_t length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (length == 0 || at + length > text.size()) {
      ++at;
      continue;
    }
    unsigned long code = length == 1 ? c : length == 2 ? c & 0x1f : length == 3 ? c & 0x0f : c & 0x07;
    bool ok = true;
    for (size_t k = 1; k < length; ++k) {
      const unsigned char next = static_cast<unsigned char>(text[at + k]);
      ok = ok && (next >> 6) == 2;
      code = code << 6 | (next & 0x3f);
    }
    const bool overlong = (length == 2 && code < 0x80) || (length == 3 && code < 0x800) || (length == 4 && code < 0x10000);
    const bool allowed = code == 9 || code == 10 || code == 13 || (code >= 0x20 && code < 0xd800) || (code >= 0xe000 && code < 0xfffe) ||
                         (code >= 0x10000 && code < 0x110000);
    if (ok && !overlong && allowed && code != 0x7f)
      out.append(text, at, length);
    at += ok ? length : 1;
  }
  return out;
}

} // namespace fynepdf::pdfa::xmp
