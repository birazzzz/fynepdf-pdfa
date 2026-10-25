// Writes docs/pdfa.svg, the README's animation: a PDF's problems fixed in place and labelled PDF/A,
// then three results from docs/benchmarks.md. FynePDF's design system: the warm grey page, white
// cards, blue for ours, Hanken Grotesk (embedded, so it renders the same everywhere), Fynebot.
// Motion is CSS (transforms and opacity), a 12-second loop, still under prefers-reduced-motion.
//
//   node docs/make-animation.mjs

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const font = (file) => fs.readFileSync(path.join(here, "assets", file)).toString("base64");
const mark = fs
  .readFileSync(path.join(here, "assets", "fynepdf-mark.svg"), "utf8")
  .replace(/^<svg[^>]*>/, "")
  .replace(/<\/svg>\s*$/, "")
  .replaceAll("clip0_2233_1174", "mark-clip");

// The numbers, from docs/benchmarks.md (PDF/A-2b).
const GROUPS = [
  {
    title: "Valid PDF/A-2b",
    note: "1,000 real web PDFs · higher is better",
    max: 1000,
    rows: [
      ["fynepdf-pdfa", 993, "993"],
      ["Ghostscript 10.08", 928, "928"],
      ["LibreOffice 25.2", 923, "923"],
    ],
  },
  {
    title: "Labelled PDF/A, but isn't",
    note: "2,053 files checked by veraPDF · lower is better",
    max: 100,
    bad: true,
    rows: [
      ["fynepdf-pdfa", 0, "0"],
      ["Ghostscript 10.08", 100, "100"],
      ["LibreOffice 25.2", 62, "62"],
    ],
  },
  {
    title: "Time for 2,053 files",
    note: "every conversion, summed · lower is better",
    max: 4113,
    rows: [
      ["fynepdf-pdfa", 96, "96 s"],
      ["Ghostscript 10.08", 740, "740 s"],
      ["LibreOffice 25.2", 4113, "4,113 s"],
    ],
  },
];
const ISSUES = [
  ["Font not embedded: Arial", "Arimo embedded, same widths"],
  ["JavaScript on open", "Removed"],
  ["Device CMYK colours", "Converted for the sRGB intent"],
  ["Layers without names", "Layer settings fixed"],
];

const LOOP = 12; // seconds
const pc = (seconds) => `${((100 * seconds) / LOOP).toFixed(2)}%`;
const css = [];
let id = 0;
/** An element shown from |at| seconds (fading and rising in), to the loop's end. */
const appear = (at, { rise = 8, dur = 0.5 } = {}) => {
  const name = `a${id++}`;
  css.push(`@keyframes ${name}{0%,${pc(at)}{opacity:0;transform:translateY(${rise}px)}${pc(at + dur)},100%{opacity:1;transform:none}}`);
  return `style="animation:${name} ${LOOP}s cubic-bezier(0.16,1,0.3,1) infinite"`;
};
/** Shown from |from| to |to| seconds. */
const between = (from, to) => {
  const name = `a${id++}`;
  css.push(`@keyframes ${name}{0%,${pc(from)}{opacity:0}${pc(from + 0.3)},${pc(to)}{opacity:1}${pc(to + 0.3)},100%{opacity:0}}`);
  return `class="transient" style="animation:${name} ${LOOP}s cubic-bezier(0.4,0,0.2,1) infinite"`;
};
/** A pop with the spring curve at |at| seconds. */
const pop = (at) => {
  const name = `a${id++}`;
  css.push(`@keyframes ${name}{0%,${pc(at)}{opacity:0;transform:scale(0.6)}${pc(at + 0.45)},100%{opacity:1;transform:none}}`);
  return `class="pop" style="animation:${name} ${LOOP}s cubic-bezier(0.34,1.56,0.64,1) infinite"`;
};
/** A bar growing from the left from |at| seconds. */
const grow = (at) => {
  const name = `a${id++}`;
  css.push(`@keyframes ${name}{0%,${pc(at)}{transform:scaleX(0)}${pc(at + 0.7)},100%{transform:none}}`);
  return `class="bar" style="animation:${name} ${LOOP}s cubic-bezier(0.16,1,0.3,1) infinite"`;
};
const esc = (s) => s.replaceAll("&", "&amp;").replaceAll("<", "&lt;");

// The scene fades out at the end of the loop and comes back with its parts hidden.
css.push(`@keyframes scene{0%,${pc(LOOP - 0.8)}{opacity:1}${pc(LOOP - 0.25)},100%{opacity:0}}`);

const parts = [];
// Header.
parts.push(`<g ${appear(0)}>
  <text x="60" y="84" class="t-title">fynepdf-pdfa</text>
  <text x="60" y="118" class="t-sub">Valid PDF/A by fixing the file in place, not rebuilding it.</text>
</g>
<g transform="translate(980 52)"><g ${appear(0.2)}>
  <svg width="40" height="40" viewBox="0 0 58 58">${mark}</svg>
  <text x="46" y="27" class="t-made">Made by FynePDF</text>
</g></g>`);

// Left card: the PDF and its problems.
parts.push(`<rect x="60" y="160" width="520" height="455" rx="20" class="card"/>
<g ${appear(0.4)}>
  <rect x="92" y="194" width="190" height="252" rx="6" class="paper"/>
  <rect x="112" y="220" width="110" height="10" rx="5" fill="#0f172a"/>
  ${[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10].map((i) => `<rect x="112" y="${246 + i * 16}" width="${i % 4 === 3 ? 90 : 150}" height="6" rx="3" fill="#e6e8ee"/>`).join("\n  ")}
  <rect x="112" y="422" width="60" height="6" rx="3" fill="#e6e8ee"/>
  <text x="92" y="186" class="t-file">report.pdf</text>
</g>`);
ISSUES.forEach(([problem, fix], i) => {
  const y = 194 + i * 62;
  const shownAt = 1.0 + i * 0.35, fixedAt = 2.6 + i * 0.45;
  parts.push(`<g ${appear(shownAt, { rise: 0 })}>
  <rect x="306" y="${y}" width="250" height="48" rx="12" class="chip"/>
</g>
<g ${between(shownAt, fixedAt)}>
  <circle cx="328" cy="${y + 24}" r="6" fill="#d55418"/>
  <text x="344" y="${y + 29}" class="t-chip">${esc(problem)}</text>
</g>
<g ${appear(fixedAt, { rise: 0, dur: 0.3 })}>
  <circle cx="328" cy="${y + 24}" r="9" fill="#18d54b"/>
  <path d="M323.5 ${y + 24.5} l3 3 l6 -6.5" fill="none" stroke="#fff" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"/>
  <text x="344" y="${y + 29}" class="t-chip">${esc(fix)}</text>
</g>`);
});
parts.push(`<g ${pop(4.6)}>
  <rect x="92" y="466" width="190" height="60" rx="12" fill="url(#blue)"/>
  <text x="187" y="494" class="t-badge" text-anchor="middle">PDF/A-2b</text>
  <text x="187" y="514" class="t-badge-sub" text-anchor="middle">veraPDF: pass</text>
</g>
<g ${appear(5.0)}>
  <text x="92" y="572" class="t-note">Only what PDF/A forbids changes. Text, bookmarks,</text>
  <text x="92" y="594" class="t-note">links, forms and tags stay byte for byte.</text>
</g>`);
// Fynebot: idles (bob, blink), hops when the file passes.
css.push(`@keyframes bob{0%,100%{transform:translateY(0)}50%{transform:translateY(-2px)}}`);
css.push(`@keyframes blink{0%,94%,100%{transform:scaleY(1)}96%{transform:scaleY(0.15)}}`);
css.push(`@keyframes hop{0%,${pc(4.6)}{transform:none}${pc(4.85)}{transform:translateY(-6px) scale(1.06)}${pc(5.05)}{transform:translateY(1px) scale(0.98)}${pc(5.25)},100%{transform:none}}`);
parts.push(`<g transform="translate(518 552)"><g class="bot-hop"><g class="bot-bob">
  <circle r="26" fill="url(#bot)"/>
  <g class="bot-eyes"><rect x="-13" y="-10" width="7" height="18" rx="3.5" fill="#fff"/><rect x="6" y="-10" width="7" height="18" rx="3.5" fill="#fff"/></g>
</g></g></g>`);

// Right card: three results.
parts.push(`<rect x="620" y="160" width="520" height="455" rx="20" class="card"/>`);
GROUPS.forEach((group, g) => {
  const top = 196 + g * 136, at = 5.2 + g * 0.8;
  parts.push(`<g ${appear(at - 0.2)}>
  <text x="652" y="${top}" class="t-group">${esc(group.title)}</text>
  <text x="652" y="${top + 20}" class="t-groupnote">${esc(group.note)}</text>
</g>`);
  group.rows.forEach(([name, value, label], r) => {
    const y = top + 42 + r * 28, ours = r === 0;
    const width = Math.max(value > 0 ? 3 : 0, (value / group.max) * 250);
    const fill = ours ? "url(#blue)" : group.bad ? "#f0c3ad" : "#d6d6d6";
    parts.push(`<g ${appear(at, { rise: 0 })}><text x="652" y="${y + 5}" class="${ours ? "t-ours" : "t-row"}">${esc(name)}</text></g>
<rect x="800" y="${y - 8}" width="${width.toFixed(1)}" height="16" rx="8" fill="${fill}" ${grow(at)}/>
<g ${appear(at + 0.5, { rise: 0 })}><text x="${(810 + width).toFixed(1)}" y="${y + 5}" class="${ours ? "t-ours" : "t-row"}">${label}</text>${ours && group.bad ? `<circle cx="${(840 + width).toFixed(1)}" cy="${y}" r="8" fill="#18d54b"/><path d="M${(836.5 + width).toFixed(1)} ${y + 0.5} l2.3 2.3 l4.7 -5" fill="none" stroke="#fff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>` : ""}</g>`);
  });
});
parts.push(`<g ${appear(7.6)}><text x="652" y="594" class="t-foot">veraPDF 1.31 · the same files for every tool · docs/benchmarks.md</text></g>`);

const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1200 675" width="1200" height="675" role="img" aria-label="fynepdf-pdfa fixes a PDF's problems in place and labels it PDF/A-2b, which veraPDF passes. On 1,000 real web PDFs it makes 993 valid PDF/A-2b files, against 928 for Ghostscript 10.08 and 923 for LibreOffice 25.2. Of 2,053 files, none it labels PDF/A fails veraPDF, against 100 for Ghostscript and 62 for LibreOffice. Converting them all takes 96 seconds, against 740 for Ghostscript and 4,113 for LibreOffice.">
<title>fynepdf-pdfa: valid PDF/A by fixing the file in place</title>
<defs>
  <linearGradient id="blue" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#007bff"/><stop offset="1" stop-color="#0046b7"/></linearGradient>
  <linearGradient id="bot" x1="0" y1="-26" x2="0" y2="26" gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="#2d92ff"/><stop offset="1" stop-color="#0046b7"/></linearGradient>
  <filter id="card-shadow" x="-10%" y="-10%" width="120%" height="130%"><feDropShadow dx="0" dy="12" stdDeviation="14" flood-color="#1e1c1c" flood-opacity="0.08"/></filter>
  <filter id="paper-shadow" x="-20%" y="-10%" width="140%" height="130%"><feDropShadow dx="0" dy="10" stdDeviation="10" flood-color="#0f172a" flood-opacity="0.16"/></filter>
</defs>
<style>
@font-face{font-family:"Hanken Grotesk";font-weight:400;src:url(data:font/woff;base64,${font("HankenGrotesk-Regular.woff")}) format("woff")}
@font-face{font-family:"Hanken Grotesk";font-weight:600;src:url(data:font/woff;base64,${font("HankenGrotesk-SemiBold.woff")}) format("woff")}
text{font-family:"Hanken Grotesk",system-ui,-apple-system,"Segoe UI",sans-serif}
.t-title{font-size:40px;font-weight:600;fill:#1e1c1c;letter-spacing:-0.01em}
.t-sub{font-size:20px;fill:#7a7876}
.t-made{font-size:15px;font-weight:600;fill:#404040}
.t-file{font-size:13px;fill:#7a7876}
.t-chip{font-size:14px;fill:#404040}
.t-badge{font-size:22px;font-weight:600;fill:#fff}
.t-badge-sub{font-size:13px;fill:#fff;opacity:.9}
.t-note{font-size:15px;fill:#7a7876}
.t-group{font-size:17px;font-weight:600;fill:#1e1c1c}
.t-groupnote{font-size:13px;fill:#909090}
.t-row{font-size:14px;fill:#404040}
.t-ours{font-size:14px;font-weight:600;fill:#005fdb}
.t-foot{font-size:12px;fill:#909090}
.card{fill:#fff;filter:url(#card-shadow)}
.paper{fill:#fff;stroke:#e6e8ee;filter:url(#paper-shadow)}
.chip{fill:#fff;stroke:#e4e2e0}
.pop,.bar,.bot-hop,.bot-bob,.bot-eyes{transform-box:fill-box}
.pop{transform-origin:center}
.bar{transform-origin:left center}
.scene{animation:scene ${LOOP}s linear infinite}
.bot-bob{animation:bob 3.2s cubic-bezier(0.65,0,0.35,1) infinite}
.bot-eyes{transform-origin:center;animation:blink 4.6s linear infinite}
.bot-hop{transform-origin:center bottom;animation:hop ${LOOP}s cubic-bezier(0.34,1.56,0.64,1) infinite}
${css.join("\n")}
.transient{opacity:0}
@media (prefers-reduced-motion:reduce){*{animation:none!important}}
</style>
<rect width="1200" height="675" fill="#f3f1f0"/>
<g class="scene">
${parts.join("\n")}
</g>
</svg>
`;
fs.writeFileSync(path.join(here, "pdfa.svg"), svg);
console.log(`docs/pdfa.svg: ${(svg.length / 1024).toFixed(0)} KB`);
