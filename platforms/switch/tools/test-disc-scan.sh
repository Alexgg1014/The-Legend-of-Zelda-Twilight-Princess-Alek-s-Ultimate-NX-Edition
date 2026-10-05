#!/usr/bin/env bash
# H34: builds and runs the host test for the disc selector's scan
# (platforms/switch/source/disc_scan.cpp) together with the disc_header.cpp and
# disc_container.cpp the NRO compiles. Same compiler needs as test-disc-header.sh
# (zlib, zstd, bzip2, xz).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
mingw="${ALEKS2_MINGW:-/c/path/to/msys2/mingw64}"
cxx="g++"
if [ -x "$mingw/bin/g++.exe" ]; then
  export PATH="$mingw/bin:$PATH"
  cxx="$mingw/bin/g++.exe"
fi
w() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }

out="${TMPDIR:-/tmp}/dusk-disc-scan-test"
mkdir -p "$out"
cd "$out"
"$cxx" -std=c++20 -O1 -Wall -Wextra \
  -I"$(w "$root/platforms/switch/include")" \
  "$(w "$root/platforms/switch/source/disc_scan.cpp")" \
  "$(w "$root/platforms/switch/source/disc_header.cpp")" \
  "$(w "$root/platforms/switch/source/disc_container.cpp")" \
  "$(w "$here/disc_scan_test.cpp")" \
  -lz -lzstd -lbz2 -llzma -o disc_scan_test
./disc_scan_test "$out/scratch"
