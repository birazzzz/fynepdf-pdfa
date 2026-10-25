// Installs the packed npm packages (npm/out, from scripts/npm-packages.mjs) for this platform into
// a new folder, the way a user would, and runs them: the command line through npx, then the Node
// API. The fonts and PDFium must be found beside the program.
//
//   node npm/test.mjs

import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const out = path.join(root, "npm", "out");
const { version } = JSON.parse(fs.readFileSync(path.join(root, "npm", "fynepdf-pdfa", "package.json"), "utf8"));
const fixtures = path.join(root, "tests", "fixtures");
const windows = process.platform === "win32";

const packed = (name) => {
  const file = path.join(out, `${name}-${version}.tgz`);
  assert.ok(fs.existsSync(file), `missing ${path.relative(root, file)}: run scripts/npm-packages.mjs`);
  return file;
};
// npm and npx are .cmd files on Windows, which run through the shell: arguments are quoted.
const run = (command, args, cwd) => {
  const result = windows
    ? spawnSync([command, ...args.map((arg) => `"${arg}"`)].join(" "), { cwd, encoding: "utf8", shell: true })
    : spawnSync(command, args, { cwd, encoding: "utf8" });
  return { status: result.status, stdout: result.stdout.trim(), stderr: result.stderr.trim() };
};

const project = fs.mkdtempSync(path.join(os.tmpdir(), "fynepdf-pdfa-npm-"));
console.log(`installing into ${project}`);
fs.writeFileSync(path.join(project, "package.json"), '{ "private": true }\n');
const install = run("npm", ["install", "--no-audit", "--no-fund", "--loglevel=error",
  packed("fynepdf-pdfa"), packed(`fynepdf-pdfa-${process.platform}-${process.arch}`)], project);
assert.equal(install.status, 0, install.stderr);

// The command line.
const shown = run("npx", ["--no-install", "fynepdf-pdfa", "--version"], project);
assert.equal(shown.stdout, `fynepdf-pdfa ${version}`, shown.stderr);
const cli = run("npx", ["--no-install", "fynepdf-pdfa", path.join(fixtures, "a-font-office-not-embedded.pdf"), "fonts.pdf"], project);
assert.equal(cli.status, 0, cli.stderr);
assert.equal(JSON.parse(cli.stdout).conforms, true, `a font stand-in from the package's fonts: ${cli.stdout}`);
console.log("ok  npx fynepdf-pdfa (fonts found)");

// The Node API.
const api = `
const assert = require("node:assert/strict");
const { toPdfa } = require("fynepdf-pdfa");
(async () => {
  const pdfium = await toPdfa(${JSON.stringify(path.join(fixtures, "a-transparency-opacity.pdf"))}, "pdfium.pdf", { part: 1 });
  assert.equal(pdfium.conforms, true, "PDF/A-1b transparency drawn with PDFium: " + JSON.stringify(pdfium));
  await assert.rejects(toPdfa(${JSON.stringify(path.join(fixtures, "a-encrypted-user-password.pdf"))}, "password.pdf"), { code: "password" });
  await assert.rejects(toPdfa("missing.pdf", "missing-out.pdf"), { code: "unreadable" });
})().catch((error) => { console.error(error); process.exit(1); });
`;
fs.writeFileSync(path.join(project, "api.js"), api);
const node = run("node", ["api.js"], project);
assert.equal(node.status, 0, node.stderr);
console.log("ok  toPdfa (PDFium found, errors reported)");
for (const file of ["fonts.pdf", "pdfium.pdf"])
  assert.equal(fs.readFileSync(path.join(project, file)).subarray(0, 5).toString(), "%PDF-");
fs.rmSync(project, { recursive: true, force: true });
console.log(`npm packages ${version} work on ${process.platform}-${process.arch}`);
