// The program that made a PDF, from its Producer and Creator, most specific first. The report
// groups real-world results by these families.

const FAMILIES = [
  ["Google Docs", /google docs|google sheets|google slides/i],
  ["Microsoft Word", /microsoft.*word|word for microsoft|winword/i],
  ["Microsoft Excel", /microsoft.*excel/i],
  ["Microsoft PowerPoint", /microsoft.*powerpoint/i],
  ["Microsoft Publisher", /publisher/i],
  ["Microsoft Print to PDF / Windows", /microsoft: print to pdf|microsoft print to pdf|windows.*pdf/i],
  ["LibreOffice / OpenOffice", /libreoffice|openoffice|staroffice|neooffice/i],
  ["Adobe InDesign", /indesign/i],
  ["Adobe Illustrator / Photoshop", /illustrator|photoshop/i],
  ["Adobe Acrobat / Distiller", /acrobat|distiller|adobe pdf library|pdfmaker/i],
  ["Apple macOS / iOS", /quartz|mac os x|macos|ios version|\bpages\b|keynote/i],
  ["Chrome / Skia", /skia|chrom/i],
  ["LaTeX", /pdftex|xetex|luatex|latex|dvipdf|tex live|miktex/i],
  ["Scanner / copier", /scan|xerox|canon|ricoh|konica|kyocera|sharp|epson|brother|toshiba|lexmark|hp digital|paperport|omnipage|abbyy|readiris|imagerunner|bizhub|workcentre|apeosport|versalink|intsig|eztwain/i],
  ["Canva", /canva/i],
  ["Other design tools", /corel|quarkxpress|affinity|scribus|inkscape/i],
  ["CAD", /autocad|pdfplot|microstation|solidworks|visio/i],
  ["PHP generators", /mpdf|tcpdf|fpdf|dompdf|cpdf/i],
  ["Report and HTML engines", /apache fop|prince|crystal|abcpdf|eo\.?pdf|dynapdf|pdflib|jasper|birt/i],
  ["Online converters", /ilovepdf|smallpdf|sejda|pdf2go|online2pdf/i],
  ["iText", /itext/i],
  ["ReportLab", /reportlab/i],
  ["wkhtmltopdf / Qt", /wkhtmltopdf|\bqt \d/i],
  ["PDFsharp / .NET", /pdfsharp|aspose|syncfusion|telerik|devexpress|spire/i],
  ["Ghostscript", /ghostscript|ps2pdf/i],
  ["Virtual printers", /primopdf|cutepdf|pdfcreator|novapdf|bullzip|dopdf|pdf24|nitro|foxit|pdf-xchange|pdfill/i],
  ["WPS Office", /wps/i],
];

export function family(producer = "", creator = "") {
  if (!producer && !creator) return "Unknown";
  const text = `${producer} | ${creator}`;
  return FAMILIES.find(([, pattern]) => pattern.test(text))?.[0] ?? "Other";
}
