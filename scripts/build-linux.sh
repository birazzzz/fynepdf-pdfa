#!/bin/sh
# Builds the library, its command line and its tests for Linux in a Debian bookworm container
# (see docker/build-linux.Dockerfile), then runs the tests: build/linux for x86-64, or with
# ARCH=arm64 build/linux-arm64 (under QEMU on an x86 machine: slow, but the same compiler and
# libraries as on an arm64 one).
set -e
root=$(cd "$(dirname "$0")/.." && (pwd -W 2>/dev/null || pwd))
arch=${ARCH:-amd64}
case "$arch" in
  amd64) out=build/linux; image=fynepdf-pdfa-build:0; pdfium_asset=pdfium-linux-x64.tgz
         pdfium_sha=019665c8877d46fe65f625f80fd714ab07aac68554b0636acf2a2adf9288adb2 ;;
  arm64) out=build/linux-arm64; image=fynepdf-pdfa-build:0-arm64; pdfium_asset=pdfium-linux-arm64.tgz
         pdfium_sha=974107999784a438149605024475d42d80dd306799d90e1af5f6fa63f976455f ;;
  *) echo "ARCH must be amd64 or arm64" >&2; exit 64 ;;
esac
docker build -q --platform "linux/$arch" -t "$image" -f "$root/docker/build-linux.Dockerfile" "$root/docker" >/dev/null
# PDFium, for PDF/A-1 pages that use transparency: PDFIUM_DIR names a pdfium-binaries folder
# (include/, lib/libpdfium.so); otherwise the pinned release is downloaded once into build/ and
# checked. Only its headers are built against; the library is loaded when a page is drawn.
pdfium=${PDFIUM_DIR:-}
if [ -z "$pdfium" ]; then
  pdfium="$root/build/pdfium-$arch"
  if [ ! -f "$pdfium/lib/libpdfium.so" ]; then
    mkdir -p "$pdfium"
    curl -fsSL "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F7961/$pdfium_asset" -o "$pdfium.tgz"
    echo "$pdfium_sha  $pdfium.tgz" | sha256sum -c - >/dev/null
    tar -xzf "$pdfium.tgz" -C "$pdfium"
    rm "$pdfium.tgz"
  fi
fi
# shellcheck disable=SC2086
MSYS_NO_PATHCONV=1 docker run --rm --platform "linux/$arch" -v "$root:/src" -v "$pdfium:/pdfium:ro" "$image" sh -c \
  "cmake -S /src -B /src/$out -G Ninja -DCMAKE_BUILD_TYPE=Release -DFYNEPDF_PDFA_TESTS=ON -DFYNEPDF_PDFA_PDFIUM_DIR=/pdfium | grep fynepdf-pdfa \
   && cmake --build /src/$out && ctest --test-dir /src/$out --output-on-failure"
