"use strict";
// fynepdf-pdfa from Node: the command line for this platform comes in an optional dependency
// (fynepdf-pdfa-linux-x64, -darwin-arm64, ...), with its fonts and PDFium beside it.

const { execFile } = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");

const PLATFORMS = ["linux-x64", "linux-arm64", "win32-x64", "darwin-arm64", "darwin-x64"];
const ERRORS = {
  2: "needs a password",
  3: "can't be read as a PDF",
  5: "couldn't be written",
};

/** The command line's path: FYNEPDF_PDFA_BINARY if set, else this platform's package. */
function binaryPath() {
  if (process.env.FYNEPDF_PDFA_BINARY) return process.env.FYNEPDF_PDFA_BINARY;
  const platform = `${process.platform}-${process.arch}`;
  let folder;
  try {
    folder = path.dirname(require.resolve(`fynepdf-pdfa-${platform}/package.json`));
  } catch {
    throw new Error(
      PLATFORMS.includes(platform)
        ? `fynepdf-pdfa: its command line for ${platform} (fynepdf-pdfa-${platform}) isn't installed. ` +
            "Install again without --no-optional or --omit=optional."
        : `fynepdf-pdfa has no command line for ${platform} yet (it has ${PLATFORMS.join(", ")}). ` +
            "Build it from https://github.com/birazzzz/fynepdf-pdfa and set FYNEPDF_PDFA_BINARY to it.",
    );
  }
  const binary = path.join(folder, process.platform === "win32" ? "fynepdf-pdfa.exe" : "fynepdf-pdfa");
  if (process.platform !== "win32") {
    try {
      fs.accessSync(binary, fs.constants.X_OK);
    } catch {
      fs.chmodSync(binary, 0o755);
    }
  }
  return binary;
}

/**
 * Converts |input| to PDF/A and writes |output|. Resolves with the result even when the file
 * couldn't be made PDF/A (conforms is then false, problems say why, and output is written without
 * the PDF/A label); rejects when the file can't be read, needs a password, or can't be written.
 */
function toPdfa(input, output, options = {}) {
  const args = [String(input), String(output)];
  if (options.part !== undefined) args.push("--part", String(options.part));
  if (options.fonts !== undefined) args.push("--fonts", String(options.fonts));
  if (options.flattenDpi !== undefined) args.push("--flatten-dpi", String(options.flattenDpi));
  return new Promise((resolve, reject) => {
    let binary;
    try {
      binary = binaryPath();
    } catch (error) {
      reject(error);
      return;
    }
    execFile(binary, args, { maxBuffer: 16 * 1024 * 1024, signal: options.signal }, (error, stdout, stderr) => {
      let result;
      try {
        result = JSON.parse(stdout.trim().split("\n").pop());
      } catch {
        result = undefined;
      }
      if (!error && result) {
        resolve(result);
        return;
      }
      if (error && error.name === "AbortError") {
        reject(error);
        return;
      }
      const code = error && typeof error.code === "number" ? error.code : undefined;
      const why = code === 64 ? `wrong options: ${stderr.trim()}` : ERRORS[code] || (error ? error.message : "no result");
      const failure = new Error(`fynepdf-pdfa: ${input} ${why}`);
      failure.code = result && result.error ? result.error : code === 64 ? "options" : "failed";
      failure.result = result;
      reject(failure);
    });
  });
}

module.exports = { toPdfa, binaryPath };
