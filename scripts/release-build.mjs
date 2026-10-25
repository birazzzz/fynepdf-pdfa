// Builds the release command line for one platform, tests it and packs it into dist/: the
// dependencies come from vcpkg (pinned by vcpkg.json's baseline) and are linked in statically,
// PDFium (pinned below, its SHA-256 checked) goes beside the program with the fonts and every
// licence. The release workflow runs this on each platform; locally it runs the same, Linux
// targets in Docker from any machine.
//
//   node scripts/release-build.mjs <linux-x64|linux-arm64|win32-x64|darwin-arm64|darwin-x64>
//
// VCPKG_ROOT names a vcpkg checkout to use; otherwise build/vcpkg is fetched at the baseline.
// Built dependencies are cached in build/vcpkg-cache (or wherever VCPKG_BINARY_SOURCES says).

import { execFileSync } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const PDFIUM = "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F7961";
const MANYLINUX = "quay.io/pypa/manylinux_2_28";
// Each target: its vcpkg triplet (cmake/triplets), its archive's name, PDFium's pdfium-binaries
// asset and SHA-256, and for Linux the manylinux image it's built in (glibc 2.28).
const TARGETS = {
  "linux-x64": {
    triplet: "x64-linux-release",
    label: "linux-x64",
    pdfium: ["pdfium-linux-x64.tgz", "019665c8877d46fe65f625f80fd714ab07aac68554b0636acf2a2adf9288adb2"],
    docker: ["linux/amd64", `${MANYLINUX}_x86_64:2026.10.10-1@sha256:4131ddd7acb9c6c966af02fe7e71edc51e078c2d41f475966697676430bfa2ae`],
  },
  "linux-arm64": {
    triplet: "arm64-linux-release",
    label: "linux-arm64",
    pdfium: ["pdfium-linux-arm64.tgz", "974107999784a438149605024475d42d80dd306799d90e1af5f6fa63f976455f"],
    docker: ["linux/arm64", `${MANYLINUX}_aarch64:2026.10.10-1@sha256:abd1230aa9e4058a74b89d27cd14ba18c0db036d7bfe6e6f219a29ea63c24eb6`],
  },
  "win32-x64": {
    triplet: "x64-windows-static-release",
    label: "windows-x64",
    pdfium: ["pdfium-win-x64.tgz", "88276459349b291c41f10422dad0210f007c04d919c8fa56472b6b7c6406adf4"],
  },
  "darwin-arm64": {
    triplet: "arm64-osx-release",
    label: "macos-arm64",
    pdfium: ["pdfium-mac-arm64.tgz", "1193a771e0bd934530afa3df73a0d44551d8f4078442e290054e6dd38ded960f"],
  },
  "darwin-x64": {
    triplet: "x64-osx-release",
    label: "macos-x64",
    pdfium: ["pdfium-mac-x64.tgz", "17f069d7012ab83898ad5eddebd139b240f05d7411c220775d507a0e3e285536"],
  },
};

const name = process.argv[2];
const target = TARGETS[name];
if (!target) {
  console.error(`usage: node scripts/release-build.mjs <${Object.keys(TARGETS).join("|")}>`);
  process.exit(64);
}
const [os, arch] = name.split("-");

function run(command, args, options = {}) {
  console.log(`> ${command} ${args.join(" ")}`);
  execFileSync(command, args, { stdio: "inherit", cwd: root, ...options });
}
const works = (command, args) => {
  try {
    execFileSync(command, args, { stdio: "ignore" });
    return true;
  } catch {
    return false;
  }
};
// Windows' own tar: Git's GNU tar reads "C:" in a path as a remote host.
const tar = process.platform === "win32" ? path.join(process.env.SystemRoot, "System32", "tar.exe") : "tar";

// The program may load only the system's own libraries: anything else (a Homebrew libjpeg on the
// runner, say) wouldn't be on a user's machine. Windows needs no check: its triplet is all static.
function checkLinks(file) {
  let libraries = [];
  if (os === "linux") {
    const dynamic = execFileSync("readelf", ["-d", file]).toString();
    libraries = [...dynamic.matchAll(/\(NEEDED\)[^[]*\[([^\]]+)\]/g)].map((match) => match[1]);
    libraries = libraries.filter((name) => !/^(libc|libm|libpthread|libdl|librt)\.so\.\d+$|^ld-linux/.test(name));
  } else if (os === "darwin") {
    const lines = execFileSync("otool", ["-L", file]).toString().split("\n").slice(1);
    libraries = lines.map((line) => line.trim().split(" ")[0]).filter(Boolean);
    libraries = libraries.filter((name) => !name.startsWith("/usr/lib/") && !name.startsWith("/System/Library/"));
  }
  if (libraries.length) throw new Error(`${path.basename(file)} links libraries a user may not have: ${libraries.join(", ")}`);
  console.log(`${path.basename(file)}: only system libraries`);
}

// Linux: build inside manylinux_2_28, so the result runs on glibc 2.28 and newer.
if (os === "linux" && !process.env.FYNEPDF_IN_CONTAINER) {
  const [platform, base] = target.docker;
  const image = `fynepdf-pdfa-release:${name}`;
  run("docker", ["build", "-q", "--platform", platform, "--build-arg", `BASE=${base}`, "-t", image,
    "-f", "docker/release-linux.Dockerfile", "docker"]);
  const env = ["VCPKG_BINARY_SOURCES"].filter((key) => process.env[key]).flatMap((key) => ["-e", key]);
  run("docker", ["run", "--rm", "--platform", platform, "-v", `${root}:/src`, ...env, image,
    "node", "scripts/release-build.mjs", name], { env: { ...process.env, MSYS_NO_PATHCONV: "1" } });
  process.exit(0);
}
if (`${process.platform}-${process.arch}` !== name)
  throw new Error(`${name} is built on ${name} (this is ${process.platform}-${process.arch})`);

const version = /project\(fynepdf_pdfa\s+VERSION\s+([0-9.]+)/.exec(fs.readFileSync(path.join(root, "CMakeLists.txt"), "utf8"))[1];
const build = path.join(root, "build", `release-${name}`);
fs.mkdirSync(path.join(root, "build"), { recursive: true });

// vcpkg at vcpkg.json's baseline.
let vcpkg = process.env.VCPKG_ROOT;
if (!vcpkg) {
  vcpkg = path.join(root, "build", "vcpkg");
  const baseline = JSON.parse(fs.readFileSync(path.join(root, "vcpkg.json"), "utf8"))["builtin-baseline"];
  if (!fs.existsSync(path.join(vcpkg, ".git"))) {
    run("git", ["init", "-q", vcpkg]);
    run("git", ["-C", vcpkg, "fetch", "-q", "--depth", "1", "https://github.com/microsoft/vcpkg", baseline]);
    run("git", ["-C", vcpkg, "checkout", "-q", "FETCH_HEAD"]);
  }
  if (!fs.existsSync(path.join(vcpkg, process.platform === "win32" ? "vcpkg.exe" : "vcpkg"))) {
    if (process.platform === "win32") run("cmd", ["/c", path.join(vcpkg, "bootstrap-vcpkg.bat"), "-disableMetrics"]);
    else run("sh", [path.join(vcpkg, "bootstrap-vcpkg.sh"), "-disableMetrics"]);
  }
}
const cache = path.join(root, "build", "vcpkg-cache");
fs.mkdirSync(cache, { recursive: true });
const env = { ...process.env, VCPKG_BINARY_SOURCES: process.env.VCPKG_BINARY_SOURCES || `clear;files,${cache},readwrite` };
// vcpkg has no tools of its own for arm64 Linux: it uses the system's cmake and ninja.
if (os === "linux" && arch === "arm64") env.VCPKG_FORCE_SYSTEM_BINARIES = "1";

// PDFium, checked against its SHA-256.
const [asset, sha256] = target.pdfium;
const pdfium = path.join(root, "build", `pdfium-${name}`);
if (!fs.existsSync(path.join(pdfium, "include"))) {
  console.log(`> download ${asset}`);
  const response = await fetch(`${PDFIUM}/${asset}`);
  if (!response.ok) throw new Error(`${asset}: HTTP ${response.status}`);
  const data = Buffer.from(await response.arrayBuffer());
  const actual = crypto.createHash("sha256").update(data).digest("hex");
  if (actual !== sha256) throw new Error(`${asset}: SHA-256 ${actual}, expected ${sha256}`);
  const archive = path.join(root, "build", asset);
  fs.writeFileSync(archive, data);
  fs.mkdirSync(pdfium, { recursive: true });
  run(tar, ["-xzf", archive, "-C", pdfium]);
  fs.rmSync(archive);
}

// Configure, build, test.
const configure = ["-S", root, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
  `-DCMAKE_TOOLCHAIN_FILE=${path.join(vcpkg, "scripts", "buildsystems", "vcpkg.cmake")}`,
  `-DVCPKG_TARGET_TRIPLET=${target.triplet}`, `-DVCPKG_OVERLAY_TRIPLETS=${path.join(root, "cmake", "triplets")}`,
  "-DFYNEPDF_PDFA_TESTS=ON", `-DFYNEPDF_PDFA_PDFIUM_DIR=${pdfium}`];
if (os === "win32") configure.push("-A", "x64", "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded");
else if (works("ninja", ["--version"])) configure.push("-G", "Ninja");
if (os === "linux") configure.push("-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc -s");
if (os === "darwin")
  configure.push("-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0", `-DCMAKE_OSX_ARCHITECTURES=${arch === "x64" ? "x86_64" : "arm64"}`);
run("cmake", configure, { env });
run("cmake", ["--build", build, "--config", "Release", "--parallel"], { env });
run("ctest", ["--test-dir", build, "-C", "Release", "--output-on-failure"], { env });

// Pack: the program, PDFium beside it, the fonts, and the licences of everything inside.
const exe = os === "win32" ? "fynepdf-pdfa.exe" : "fynepdf-pdfa";
const built = [path.join(build, "Release", exe), path.join(build, exe)].find((file) => fs.existsSync(file));
checkLinks(built);
const library =["lib/libpdfium.so", "lib/libpdfium.dylib", "bin/pdfium.dll"]
  .map((file) => path.join(pdfium, file))
  .find((file) => fs.existsSync(file));
const folder = `fynepdf-pdfa-${version}-${target.label}`;
const dist = path.join(root, "dist");
const stage = path.join(dist, folder);
fs.rmSync(stage, { recursive: true, force: true });
fs.mkdirSync(path.join(stage, "licenses"), { recursive: true });
fs.copyFileSync(built, path.join(stage, exe));
fs.chmodSync(path.join(stage, exe), 0o755);
fs.copyFileSync(library, path.join(stage, path.basename(library)));
fs.cpSync(path.join(root, "fonts"), path.join(stage, "fonts"), { recursive: true });
for (const file of ["LICENSE", "THIRD-PARTY-NOTICES.md"]) fs.copyFileSync(path.join(root, file), path.join(stage, file));
const share = path.join(build, "vcpkg_installed", target.triplet, "share");
for (const port of fs.readdirSync(share).sort()) {
  const copyright = path.join(share, port, "copyright");
  if (!port.startsWith("vcpkg-") && fs.existsSync(copyright)) fs.copyFileSync(copyright, path.join(stage, "licenses", `${port}.txt`));
}
fs.copyFileSync(path.join(pdfium, "LICENSE"), path.join(stage, "licenses", "pdfium.txt"));
fs.writeFileSync(path.join(stage, "README.txt"), `fynepdf-pdfa ${version} for ${target.label}

  ${os === "win32" ? "fynepdf-pdfa.exe" : "./fynepdf-pdfa"} input.pdf output.pdf             PDF/A-2b
  ${os === "win32" ? "fynepdf-pdfa.exe" : "./fynepdf-pdfa"} input.pdf output.pdf --part 1    PDF/A-1b (or --part 3)

Keep the files together: the program finds its fonts and ${path.basename(library)} beside it.
Documentation: https://github.com/birazzzz/fynepdf-pdfa

Made by FynePDF (https://fynepdf.com), MIT licence (LICENSE). The libraries linked into the
program and PDFium keep their own licences, in licenses/; the fonts' are in fonts/.
`);
const archive = os === "win32" ? `${folder}.zip` : `${folder}.tar.gz`;
fs.rmSync(path.join(dist, archive), { force: true });
run(tar, [os === "win32" ? "-a" : "-z", "-cf", path.join(dist, archive), "-C", dist, folder]);
console.log(`dist/${archive}`);
