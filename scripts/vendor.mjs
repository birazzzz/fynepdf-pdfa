// Copies the library (CMakeLists.txt, include/, src/) into another project, and records each
// file's SHA-256 in VENDORED.json there, so the copy can be checked against its source:
//
//   node scripts/vendor.mjs <target folder>
//

import { createHash } from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const target = process.argv[2];
if (!target) throw new Error("usage: node scripts/vendor.mjs <target folder>");

const files = ["CMakeLists.txt"];
for (const dir of ["include", "src"]) {
  const walk = (at) => {
    for (const entry of fs.readdirSync(path.join(root, at), { withFileTypes: true })) {
      const relative = path.posix.join(at, entry.name);
      if (entry.isDirectory()) walk(relative);
      else files.push(relative);
    }
  };
  walk(dir);
}

fs.rmSync(target, { recursive: true, force: true });
const hashes = {};
for (const file of files.sort()) {
  const bytes = fs.readFileSync(path.join(root, file));
  fs.mkdirSync(path.dirname(path.join(target, file)), { recursive: true });
  fs.writeFileSync(path.join(target, file), bytes);
  hashes[file] = createHash("sha256").update(bytes).digest("hex");
}
const version = fs.readFileSync(path.join(root, "CMakeLists.txt"), "utf8").match(/project\(fynepdf_pdfa VERSION ([\d.]+)/)?.[1];
fs.writeFileSync(
  path.join(target, "VENDORED.json"),
  `${JSON.stringify({ library: "fynepdf-pdfa", version, source: "https://github.com/fynepdf/fynepdf-pdfa", copied: new Date().toISOString(), files: hashes }, null, 2)}\n`,
);
console.log(`copied ${files.length} files to ${target}`);
