// Sets the version everywhere it's written: CMakeLists.txt, vcpkg.json, the npm package (and the
// platform packages it depends on), and the FetchContent tags in the README and examples.
//
//   node scripts/set-version.mjs 0.2.0
//   node scripts/set-version.mjs --check v0.2.0   (the release workflow: all of them say 0.2.0?)

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const check = process.argv[2] === "--check";
const version = (check ? process.argv[3] : process.argv[2])?.replace(/^v/, "");
if (!/^\d+\.\d+\.\d+$/.test(version ?? "")) {
  console.error("usage: node scripts/set-version.mjs <x.y.z> | --check <vx.y.z>");
  process.exit(64);
}

// Each file, and the places in it that hold the version.
const FILES = {
  "CMakeLists.txt": [/(project\(fynepdf_pdfa\s+VERSION\s+)[0-9.]+/],
  "vcpkg.json": [/("version": ")[0-9.]+/],
  "npm/fynepdf-pdfa/package.json": [/("version": ")[0-9.]+/, /("fynepdf-pdfa-[a-z0-9-]+": ")[0-9.]+/g],
  "README.md": [/(GIT_TAG v)[0-9.]+/g],
  "examples/CMakeLists.txt": [/(GIT_TAG v)[0-9.]+/g],
};

let wrong = 0;
for (const [file, patterns] of Object.entries(FILES)) {
  const full = path.join(root, file);
  const text = fs.readFileSync(full, "utf8");
  let next = text;
  for (const pattern of patterns) {
    const found = [...text.matchAll(new RegExp(pattern.source, "g"))];
    if (found.length === 0) throw new Error(`${file}: no version found by ${pattern}`);
    next = next.replace(pattern, (_, before) => `${before}${version}`);
  }
  if (check && next !== text) {
    console.error(`${file} doesn't say ${version}`);
    wrong++;
  } else if (!check && next !== text) {
    fs.writeFileSync(full, next);
    console.log(`${file}: ${version}`);
  }
}
if (check && wrong) {
  console.error(`Run node scripts/set-version.mjs ${version} in a pull request first, then release v${version}.`);
  process.exit(1);
}
if (check) console.log(`Every file says ${version}.`);
