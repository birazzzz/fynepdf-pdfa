// Writes docs/pdfa.svg, the README's animation: three short scenes, one idea each.
//   1. The same 1,000 real PDFs through Ghostscript and fynepdf-pdfa, one dot per 10 files: the
//      ones Ghostscript labels PDF/A that fail veraPDF light up; fynepdf-pdfa has none.
//   2. A race: converting 2,053 files. fynepdf-pdfa is done while Ghostscript has done 13%.
//   3. What that adds up to.
// The numbers are docs/benchmarks.md's. FynePDF's design system: the warm grey page, white cards,
// blue for ours, orange for errors, Hanken Grotesk embedded, the FynePDF logo. CSS only (transforms and
// opacity), a 15-second loop; under prefers-reduced-motion the first scene stands still.
//
//   node docs/make-animation.mjs

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const font = (file) => fs.readFileSync(path.join(here, "assets", file)).toString("base64");
const logo = fs
  .readFileSync(path.join(here, "assets", "fynepdf-logo.svg"), "utf8")
  .replace(/^<svg[^>]*>/, "")
  .replace(/<\/svg>\s*$/, "")
  .replaceAll("clip0_10990_4358", "logo-clip");

// Real-world set (1,000 web PDFs), PDF/A-2b, one dot per 10 files (rounded).
const GS = { valid: 93, wrong: 4, other: 3 };   // 928 valid; 44 labelled PDF/A that fail; the rest
const OURS = { valid: 99, wrong: 0, other: 1 }; // 993 valid; 0 labelled wrongly; 7 refused, with the reason
// 2,053 files, every conversion's time summed.
const TIME = { ours: 96, gs: 740 };

const LOOP = 15;
const SCENES = [[0, 6.2], [6.2, 12.4], [12.4, LOOP]];
const pc = (s) => `${Math.min(100, (100 * s) / LOOP).toFixed(2)}%`;
const css = [];
let n = 0;
const anim = (frames, { ease = "cubic-bezier(0.16,1,0.3,1)" } = {}) => {
  const name = `k${n++}`;
  css.push(`@keyframes ${name}{${frames}}`);
  return `animation:${name} ${LOOP}s ${ease} infinite`;
};
/** Visible from |from| to |to| (a scene), fading in and out. */
const scene = ([from, to]) =>
  `style="${anim(`0%,${pc(from)}{opacity:0}${pc(from + 0.4)},${pc(to - 0.4)}{opacity:1}${pc(to)},100%{opacity:0}`, { ease: "cubic-bezier(0.4,0,0.2,1)" })}"`;
/** Rises and fades in at |at|. */
const rise = (at, dy = 10) =>
  `style="${anim(`0%,${pc(at)}{opacity:0;transform:translateY(${dy}px)}${pc(at + 0.5)},100%{opacity:1;transform:none}`)}"`;
/** Grows from the left over |dur| seconds from |at|. */
const fill = (at, dur) =>
  `class="l" style="${anim(`0%,${pc(at)}{transform:scaleX(0)}${pc(at + dur)},100%{transform:none}`, { ease: "linear" })}"`;
const tick = (cx, cy, r = 11) =>
  `<circle cx="${cx}" cy="${cy}" r="${r}" fill="#18d54b"/><path d="M${cx - r * 0.45} ${cy + r * 0.05} l${r * 0.3} ${r * 0.3} l${r * 0.6} -${r * 0.65}" fill="none" stroke="#fff" stroke-width="${r * 0.2}" stroke-linecap="round" stroke-linejoin="round"/>`;

const parts = [];

// Scene 1: the dots.
const s1 = SCENES[0];
parts.push(`<g class="still" ${scene(s1)}>
  <g ${rise(s1[0] + 0.1)}>
    <text x="600" y="92" class="h1" text-anchor="middle">The same 1,000 real PDFs, made PDF/A</text>
    <text x="600" y="126" class="h2" text-anchor="middle">Each dot is 10 files<tspan dx="22" fill="#18d54b">●</tspan> valid PDF/A<tspan dx="18" fill="#c9c7c5">●</tspan> no PDF/A made<tspan dx="18" fill="#d55418">●</tspan> labelled PDF/A, but fails veraPDF</text>
  </g>`);
// Rows of dots rise in one after another; the wrong ones arrive last and pulse.
const waffle = (cx, name, ours, counts, at) => {
  const out = [`<rect x="${cx - 250}" y="160" width="500" height="460" rx="20" class="card"/>`];
  out.push(`<text x="${cx}" y="210" class="${ours ? "name-ours" : "name"}" text-anchor="middle">${name}</text>`);
  const dot = (i, colour, attrs = "") =>
    `<circle cx="${cx - 135 + (i % 10) * 30}" cy="${248 + Math.floor(i / 10) * 30}" r="11" fill="${colour}"${attrs}/>`;
  for (let row = 0; row < 10; row++) {
    const dots = [];
    for (let i = row * 10; i < row * 10 + 10 && i < counts.valid + counts.other; i++)
      dots.push(dot(i, i < counts.valid ? "#18d54b" : "#c9c7c5"));
    out.push(`<g ${rise(at + 0.2 + row * 0.16, 8)}>${dots.join("")}</g>`);
  }
  for (let i = counts.valid + counts.other; i < 100; i++)
    out.push(dot(i, "#d55418", ` class="o" style="${anim(`0%,${pc(at + 2.2)}{opacity:0;transform:scale(0.3)}${pc(at + 2.6)}{opacity:1;transform:scale(1.35)}${pc(at + 2.9)}{transform:scale(1)}${pc(at + 3.3)}{transform:scale(1.25)}${pc(at + 3.6)},100%{opacity:1;transform:none}`, { ease: "cubic-bezier(0.34,1.56,0.64,1)" })}"`));
  return out.join("\n  ");
};
// Under each grid: a 16px dot, 8px, the text (its width measured in Chrome, Hanken Grotesk 18px),
// centred as one.
const legend = (cx, colour, strong, count, width) => {
  const left = cx - (24 + width) / 2;
  return `<circle cx="${left + 8}" cy="574" r="8" fill="${colour}"/><text x="${left + 24}" y="580" class="legend"><tspan class="${strong}">${count}</tspan> labelled PDF/A that fail</text>`;
};
parts.push(`  ${waffle(330, "Ghostscript 10.08", false, GS, s1[0] + 0.4)}
  ${waffle(870, "fynepdf-pdfa", true, OURS, s1[0] + 0.4)}
  <g ${rise(s1[0] + 2.8, 6)}>
    ${legend(330, "#d55418", "strong-warn", "44 files", 249)}
    ${legend(870, "#18d54b", "strong-ours", "0 files", 239)}
  </g>
</g>`);

// Scene 2: the race.
const s2 = SCENES[1];
const start = s2[0] + 0.8, oursDur = 0.5, gsDur = oursDur * (TIME.gs / TIME.ours);
parts.push(`<g ${scene(s2)}>
  <g ${rise(s2[0] + 0.1)}>
    <text x="600" y="150" class="h1" text-anchor="middle">Converting 2,053 files</text>
    <text x="600" y="184" class="h2" text-anchor="middle">every conversion's time, added up</text>
  </g>
  <text x="140" y="300" class="name-ours">fynepdf-pdfa</text>
  <rect x="140" y="320" width="880" height="34" rx="17" fill="#e7e7e7"/>
  <rect x="140" y="320" width="880" height="34" rx="17" fill="url(#blue)" ${fill(start, oursDur)}/>
  <g ${rise(start + oursDur, 0)}><text x="1040" y="345" class="time-ours">96 s</text>${tick(1124, 337, 12)}</g>
  <text x="140" y="430" class="name">Ghostscript 10.08</text>
  <rect x="140" y="450" width="880" height="34" rx="17" fill="#e7e7e7"/>
  <rect x="140" y="450" width="880" height="34" rx="17" fill="#a9a7a5" ${fill(start, gsDur)}/>
  <g ${rise(start + gsDur, 0)}><text x="1040" y="475" class="time">740 s</text></g>
  <g ${rise(start + oursDur + 0.3, 8)}>
    <text x="600" y="560" class="h3" text-anchor="middle">fynepdf-pdfa is done when Ghostscript has done 13%</text>
  </g>
</g>`);

// Scene 3: what it adds up to.
const s3 = SCENES[2];
parts.push(`<g ${scene(s3)}>
  <g ${rise(s3[0] + 0.2)}><svg x="460" y="178.6" width="280" height="62.8" viewBox="0 0 107 24">${logo}</svg></g>
  <g ${rise(s3[0] + 0.4)}>
    <text x="600" y="345" class="title" text-anchor="middle">fynepdf-pdfa</text>
    <text x="600" y="395" class="h2b" text-anchor="middle">Valid PDF/A. No false labels. 7.7× faster than Ghostscript.</text>
  </g>
  <g ${rise(s3[0] + 0.8)}>
    <text x="600" y="465" class="made" text-anchor="middle">Made by FynePDF · MIT</text>
  </g>
</g>`);

const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1200 675" width="1200" height="675" role="img" aria-label="The same 1,000 real web PDFs made PDF/A: Ghostscript 10.08 labels 44 files PDF/A that fail veraPDF; fynepdf-pdfa labels none wrongly and makes 993 valid against Ghostscript's 928. Converting 2,053 files takes fynepdf-pdfa 96 seconds and Ghostscript 740: fynepdf-pdfa is done when Ghostscript has done 13%. Valid PDF/A, no false labels, 7.7 times faster than Ghostscript. Made by FynePDF.">
<title>fynepdf-pdfa against Ghostscript</title>
<defs>
  <linearGradient id="blue" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#007bff"/><stop offset="1" stop-color="#0046b7"/></linearGradient>
  <filter id="shadow" x="-10%" y="-10%" width="120%" height="130%"><feDropShadow dx="0" dy="12" stdDeviation="14" flood-color="#1e1c1c" flood-opacity="0.08"/></filter>
</defs>
<style>
@font-face{font-family:"Hanken Grotesk";font-weight:400;src:url(data:font/woff;base64,${font("HankenGrotesk-Regular.woff")}) format("woff")}
@font-face{font-family:"Hanken Grotesk";font-weight:600;src:url(data:font/woff;base64,${font("HankenGrotesk-SemiBold.woff")}) format("woff")}
text{font-family:"Hanken Grotesk",system-ui,-apple-system,"Segoe UI",sans-serif}
.h1{font-size:34px;font-weight:600;fill:#1e1c1c;letter-spacing:-0.01em}
.h2{font-size:19px;fill:#7a7876}
.h2b{font-size:24px;fill:#404040}
.h3{font-size:22px;font-weight:600;fill:#1e1c1c}
.title{font-size:56px;font-weight:600;fill:#1e1c1c;letter-spacing:-0.01em}
.name{font-size:22px;font-weight:600;fill:#404040}
.name-ours{font-size:22px;font-weight:600;fill:#005fdb}
.legend{font-size:18px;fill:#404040}
.strong-warn{font-weight:600;fill:#d55418}
.strong-ours{font-weight:600;fill:#0d7a2b}
.time{font-size:22px;font-weight:600;fill:#404040}
.time-ours{font-size:22px;font-weight:600;fill:#005fdb}
.made{font-size:18px;font-weight:600;fill:#404040}
.card{fill:#fff;filter:url(#shadow)}
.o,.l{transform-box:fill-box}
.o{transform-origin:center}
.l{transform-origin:left center}
${css.join("\n")}
@media (prefers-reduced-motion:reduce){*{animation:none!important}g[style]{opacity:0}.still,.still [style]{opacity:1!important}}
</style>
<rect width="1200" height="675" fill="#f3f1f0"/>
${parts.join("\n")}
</svg>
`;
fs.writeFileSync(path.join(here, "pdfa.svg"), svg);
console.log(`docs/pdfa.svg: ${(svg.length / 1024).toFixed(0)} KB`);
