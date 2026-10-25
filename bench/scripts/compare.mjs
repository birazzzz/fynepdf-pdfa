// How much a conversion changed a file: its pages as pictures, and its text.

import fs from "node:fs";
import os from "node:os";
import path from "node:path";

import { run } from "./lib.mjs";

const DPI = 30;
const PAGES = 10;
// A pixel counts as changed when a channel moves by more than this (of 255): enough to ignore
// anti-aliasing and colour-profile rounding, small enough to catch a lost shape or a wrong colour.
const PIXEL_TOLERANCE = 40;
// A page counts as looking different when more than 1% of it changed.
const PAGE_TOLERANCE = 0.01;

function readNetpbm(file) {
  const data = fs.readFileSync(file);
  // P5 (grey) or P6 (RGB): magic, width, height, maxval, one whitespace byte, then pixels.
  let at = 0;
  const fields = [];
  while (fields.length < 4) {
    while (/\s/.test(String.fromCharCode(data[at]))) at++;
    if (data[at] === 0x23) {
      while (data[at] !== 0x0a) at++;
      continue;
    }
    let token = "";
    while (!/\s/.test(String.fromCharCode(data[at]))) token += String.fromCharCode(data[at++]);
    fields.push(token);
  }
  at++;
  const channels = fields[0] === "P6" ? 3 : 1;
  return { width: Number(fields[1]), height: Number(fields[2]), channels, pixels: data.subarray(at) };
}

async function render(file, dir, gray) {
  fs.mkdirSync(dir, { recursive: true });
  const result = await run("pdftoppm", ["-r", String(DPI), "-l", String(PAGES), ...(gray ? ["-gray"] : []), file, path.join(dir, "p")], { timeoutMs: 120_000 });
  const pages = fs.readdirSync(dir).filter((f) => /\.(ppm|pgm)$/.test(f)).sort().map((f) => path.join(dir, f));
  return { ok: result.code === 0 && pages.length > 0, pages };
}

function pageDifference(a, b) {
  const width = Math.min(a.width, b.width);
  const height = Math.min(a.height, b.height);
  const channels = Math.min(a.channels, b.channels);
  let changed = 0;
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const ia = (y * a.width + x) * a.channels;
      const ib = (y * b.width + x) * b.channels;
      let worst = 0;
      if (a.channels === b.channels) {
        for (let c = 0; c < channels; c++) worst = Math.max(worst, Math.abs(a.pixels[ia + c] - b.pixels[ib + c]));
      } else {
        // Grey against colour: compare the colour pixel's luminance.
        const [g, rgb, i] = a.channels === 1 ? [a.pixels[ia], b.pixels, ib] : [b.pixels[ib], a.pixels, ia];
        worst = Math.abs(g - (0.299 * rgb[i] + 0.587 * rgb[i + 1] + 0.114 * rgb[i + 2]));
      }
      if (worst > PIXEL_TOLERANCE) changed++;
    }
  }
  const area = Math.max(a.width * a.height, b.width * b.height);
  changed += area - width * height;
  return { fraction: changed / area, resized: Math.abs(a.width - b.width) > 1 || Math.abs(a.height - b.height) > 1 };
}

/**
 * Draws the first pages of |input| and |output| and compares them. With |gray| the input is
 * drawn in grey (what a Grayscale result should look like).
 */
export async function compareLooks(input, output, { gray = false } = {}) {
  const work = fs.mkdtempSync(path.join(os.tmpdir(), "bench-look-"));
  try {
    const [a, b] = await Promise.all([render(input, path.join(work, "a"), gray), render(output, path.join(work, "b"), gray)]);
    if (!a.ok) return { comparable: false, reason: "input could not be drawn" };
    if (!b.ok) return { comparable: false, reason: "output could not be drawn" };
    const count = Math.min(a.pages.length, b.pages.length);
    const fractions = [];
    let resized = 0;
    for (let i = 0; i < count; i++) {
      const diff = pageDifference(readNetpbm(a.pages[i]), readNetpbm(b.pages[i]));
      fractions.push(diff.fraction);
      if (diff.resized) resized++;
    }
    return {
      comparable: true,
      pagesCompared: count,
      pagesMissing: a.pages.length - count,
      pagesDifferent: fractions.filter((f) => f > PAGE_TOLERANCE).length,
      worstPage: Math.max(0, ...fractions),
      meanChange: fractions.reduce((s, f) => s + f, 0) / (count || 1),
      resized,
    };
  } finally {
    fs.rmSync(work, { recursive: true, force: true });
  }
}

/**
 * Whether a Grayscale result still has colour: its first pages drawn in colour, and the share of
 * pixels whose channels differ by more than 24 of 255 (anti-aliasing and JPEG noise stay under).
 */
export async function colourLeft(output) {
  const work = fs.mkdtempSync(path.join(os.tmpdir(), "bench-colour-"));
  try {
    const drawn = await render(output, work, false);
    if (!drawn.ok) return { comparable: false };
    let worst = 0;
    let pagesWithColour = 0;
    for (const page of drawn.pages) {
      const image = readNetpbm(page);
      let coloured = 0;
      for (let at = 0; at < image.width * image.height * 3; at += 3) {
        const r = image.pixels[at], g = image.pixels[at + 1], b = image.pixels[at + 2];
        if (Math.max(r, g, b) - Math.min(r, g, b) > 24) coloured++;
      }
      const fraction = coloured / (image.width * image.height);
      worst = Math.max(worst, fraction);
      if (fraction > 0.001) pagesWithColour++;
    }
    return { comparable: true, pages: drawn.pages.length, pagesWithColour, worstPage: worst };
  } finally {
    fs.rmSync(work, { recursive: true, force: true });
  }
}

function bigrams(text) {
  const chars = [...text.normalize("NFKC").replace(/\s+/g, "")];
  const counts = new Map();
  for (let i = 0; i + 1 < chars.length; i++) {
    const key = chars[i] + chars[i + 1];
    counts.set(key, (counts.get(key) || 0) + 1);
  }
  return { counts, total: Math.max(0, chars.length - 1) };
}

/**
 * How much of the input's text the output still has, from 0 to 1. Pairs of characters are
 * compared, with all spacing removed, so a tool that splits text into words differently
 * isn't counted as losing it. null when the input has no text.
 */
export function compareText(inputText, outputText) {
  const a = bigrams(inputText || "");
  if (a.total === 0) return null;
  const b = bigrams(outputText || "");
  let shared = 0;
  for (const [key, count] of a.counts) shared += Math.min(count, b.counts.get(key) || 0);
  return shared / Math.max(a.total, b.total);
}
