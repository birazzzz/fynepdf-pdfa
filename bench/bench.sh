#!/bin/sh
# Runs a command in the bench container:
#
#   ./bench.sh node scripts/build-set-a.mjs
#
# /bench is this folder, /data the "fynepdf-pdfa-bench" Docker volume (the sets and the
# results; a volume, because Docker Desktop's Windows folders are slow with thousands of
# files). BENCH_PRIVATE lists host folders of private test files, mounted read-only under
# /private (FynePDF's own; leave it unset to run on the public sets only).
set -e
here=$(cd "$(dirname "$0")" && (pwd -W 2>/dev/null || pwd))
mounts="-v $here:/bench -v fynepdf-pdfa-bench:/data"
# Our converter's Linux build (../scripts/build-linux.sh), when there is one.
[ -d "$here/../build/linux" ] && mounts="$mounts -v $here/../build/linux:/opt/fynepdf-pdfa:ro"
# The sister project's Grayscale command line, when it's checked out next to this repository.
[ -d "$here/../../fynepdf-grayscale/build/linux" ] && mounts="$mounts -v $here/../../fynepdf-grayscale/build/linux:/opt/fynepdf-grayscale:ro"
# Its substitute fonts.
[ -d "$here/../fonts" ] && mounts="$mounts -v $here/../fonts:/opt/fynepdf-pdfa-fonts:ro"
for dir in $BENCH_PRIVATE; do
  mounts="$mounts -v $dir:/private/$(basename "$dir"):ro"
done
tty=""
[ -t 1 ] && tty="-t"
# shellcheck disable=SC2086
MSYS_NO_PATHCONV=1 exec docker run --rm -i $tty $mounts -e BENCH_WORKERS fynepdf-pdfa-bench:0 "$@"
