// Makes the npm packages from the release archives in dist/ (scripts/release-build.mjs): one per
// platform, holding what its archive holds (the program, PDFium, the fonts, the licences), and
// fynepdf-pdfa itself from npm/fynepdf-pdfa, which installs the right one. Each is written to
// npm/out/<name>/ and packed beside it as <name>-<version>.tgz.
//
//   node scripts/npm-packages.mjs [--partial]
//
// --partial makes packages only for the archives that are there (for a local test).

import { execFileSync, execSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const main = JSON.parse(fs.readFileSync(path.join(root, "npm", "fynepdf-pdfa", "package.json"), "utf8"));
const version = main.version;
const cmake = /project\(fynepdf_pdfa\s+VERSION\s+([0-9.]+)/.exec(fs.readFileSync(path.join(root, "CMakeLists.txt"), "utf8"))[1];
if (cmake !== version) throw new Error(`npm/fynepdf-pdfa is ${version} but CMakeLists.txt is ${cmake}: run scripts/set-version.mjs`);

// npm's names for each platform, and the archive release-build.mjs makes for it.
const PLATFORMS = {
  "linux-x64": { label: "linux-x64", os: "linux", cpu: "x64", libc: "glibc", title: "Linux x64 (glibc 2.28+)" },
  "linux-arm64": { label: "linux-arm64", os: "linux", cpu: "arm64", libc: "glibc", title: "Linux arm64 (glibc 2.28+)" },
  "win32-x64": { label: "windows-x64", os: "win32", cpu: "x64", title: "Windows x64" },
  "darwin-arm64": { label: "macos-arm64", os: "darwin", cpu: "arm64", title: "macOS on Apple silicon (11+)" },
  "darwin-x64": { label: "macos-x64", os: "darwin", cpu: "x64", title: "macOS on Intel (11+)" },
};
if (Object.keys(main.optionalDependencies).join() !== Object.keys(PLATFORMS).sort().map((p) => `fynepdf-pdfa-${p}`).join())
  throw new Error("npm/fynepdf-pdfa/package.json's optionalDependencies don't list the platforms here");

const partial = process.argv.includes("--partial");
const dist = path.join(root, "dist");
const out = path.join(root, "npm", "out");
fs.rmSync(out, { recursive: true, force: true });
fs.mkdirSync(out, { recursive: true });
const tar = process.platform === "win32" ? path.join(process.env.SystemRoot, "System32", "tar.exe") : "tar";
const pack = (folder) => execSync(`npm pack --silent --pack-destination "${out}"`, { cwd: folder }).toString().trim();

for (const [platform, p] of Object.entries(PLATFORMS)) {
  const folder = `fynepdf-pdfa-${version}-${p.label}`;
  const archive = path.join(dist, p.os === "win32" ? `${folder}.zip` : `${folder}.tar.gz`);
  if (!fs.existsSync(archive)) {
    if (partial) continue;
    throw new Error(`missing ${path.relative(root, archive)}`);
  }
  const name = `fynepdf-pdfa-${platform}`;
  const target = path.join(out, name);
  fs.mkdirSync(target);
  // GNU tar doesn't read zip files; unzip does.
  if (archive.endsWith(".zip") && process.platform !== "win32") execFileSync("unzip", ["-q", archive, "-d", out]);
  else execFileSync(tar, ["-xf", archive, "-C", out]);
  for (const entry of fs.readdirSync(path.join(out, folder))) fs.renameSync(path.join(out, folder, entry), path.join(target, entry));
  fs.rmdirSync(path.join(out, folder));
  const manifest = {
    name,
    version,
    description: `The fynepdf-pdfa command line for ${p.title}. Install fynepdf-pdfa, which picks it.`,
    homepage: main.homepage,
    repository: { ...main.repository, directory: undefined },
    license: "MIT",
    author: main.author,
    os: [p.os],
    cpu: [p.cpu],
    ...(p.libc ? { libc: [p.libc] } : {}),
    preferUnplugged: true,
  };
  fs.writeFileSync(path.join(target, "package.json"), `${JSON.stringify(manifest, null, 2)}\n`);
  fs.writeFileSync(
    path.join(target, "README.md"),
    `# ${name}\n\nThe [fynepdf-pdfa](https://www.npmjs.com/package/fynepdf-pdfa) command line for ${p.title}, with its fonts and PDFium. ` +
      "Install `fynepdf-pdfa` instead: it picks this package on this platform.\n\nMade by [FynePDF](https://fynepdf.com). MIT licence; the libraries inside keep their own (`licenses/`).\n",
  );
  console.log(path.relative(root, path.join(out, pack(target))));
}

const target = path.join(out, "fynepdf-pdfa");
fs.cpSync(path.join(root, "npm", "fynepdf-pdfa"), target, { recursive: true });
fs.copyFileSync(path.join(root, "LICENSE"), path.join(target, "LICENSE"));
console.log(path.relative(root, path.join(out, pack(target))));
