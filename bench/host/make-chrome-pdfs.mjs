// Set E, Chrome's part: prints set-e/html/*.html to PDF with the Chrome (or Edge) installed on
// this computer, so the files carry a real browser's output and the system's own fonts. Runs
// on the host, not in the container:
//
//   node host/make-chrome-pdfs.mjs
//
// Writes set-e/out/chrome-<name>.pdf. A throwaway profile is used, never the person's own.

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const bench = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const html = path.join(bench, "set-e", "html");
const out = path.join(bench, "set-e", "out");

const candidates = [
  process.env.CHROME,
  "C:/Program Files/Google/Chrome/Application/chrome.exe",
  "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
  "/usr/bin/google-chrome",
  "/usr/bin/chromium",
].filter(Boolean);
const chrome = candidates.find((file) => fs.existsSync(file));
if (!chrome) throw new Error("No Chrome or Edge found; set CHROME to its path");

fs.mkdirSync(out, { recursive: true });
const profile = fs.mkdtempSync(path.join(os.tmpdir(), "pdfa-bench-chrome-"));
try {
  for (const name of fs.readdirSync(html).filter((file) => file.endsWith(".html"))) {
    const target = path.join(out, `chrome-${name.replace(/\.html$/, "")}.pdf`);
    execFileSync(
      chrome,
      [
        "--headless=new",
        "--disable-gpu",
        "--no-first-run",
        "--no-default-browser-check",
        `--user-data-dir=${profile}`,
        "--no-pdf-header-footer",
        "--generate-pdf-document-outline",
        `--print-to-pdf=${target}`,
        pathToFileURL(path.join(html, name)).href,
      ],
      { stdio: "inherit", timeout: 120_000 },
    );
    console.log(`${path.basename(target)}: ${fs.statSync(target).size} bytes`);
  }
  fs.writeFileSync(path.join(out, "chrome-version.txt"), `${chrome}\n${execFileSync("powershell", ["-NoProfile", "-Command", `(Get-Item '${chrome}').VersionInfo.ProductVersion`]).toString().trim()}\n`);
} finally {
  fs.rmSync(profile, { recursive: true, force: true });
}
