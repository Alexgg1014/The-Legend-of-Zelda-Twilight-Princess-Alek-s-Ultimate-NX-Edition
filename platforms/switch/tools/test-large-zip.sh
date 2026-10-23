#!/usr/bin/env bash
# aleks2 (rc18): host test for large .dusk packages through the Switch's own IO path (large_zip_test.cpp).
#   test-large-zip.sh PACKAGE...   (MINIZ_SRC/MINIZ_BUILD: the build's _deps miniz; SDL_INCLUDE: SDL3 headers; FMT_SRC)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-large-zip-test"
mkdir -p "$out"
cd "$out"
# The build patches miniz at configure (cmake/PatchMiniz.cmake): the same patch on a copy.
cp -R "$MINIZ_SRC" miniz-patched
cmake -DSOURCE_DIR="$PWD/miniz-patched" -P "$root/cmake/PatchMiniz.cmake"
MINIZ_SRC="$PWD/miniz-patched"
cc -O1 -g -c "$MINIZ_SRC/miniz.c" -I"$MINIZ_SRC" -I"$MINIZ_BUILD" -o miniz.o
g++ -std=c++20 -O1 -g -include optional -include cstring -I"$root/src" -I"$root/extern/borealis/include" -I"$MINIZ_SRC" -I"$MINIZ_BUILD" -I"$SDL_INCLUDE" -I"$FMT_SRC/include" -DFMT_HEADER_ONLY \
  "$here/large_zip_test.cpp" "$root/src/dusk/archive.cpp" "$root/platforms/switch/source/borealis_io_switch.cpp" \
  miniz.o -o large_zip_test
./large_zip_test "$@" || status=$?
# A copy cut short (as a transfer that stopped): the error must say so.
head -c 100000000 "$1" > cut.dusk
./large_zip_test cut.dusk | tee cut.txt || true
grep -q "no ZIP end record" cut.txt || { echo "FAIL: a cut-short package is not described as such"; exit 1; }
exit ${status:-0}
