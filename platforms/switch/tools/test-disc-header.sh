#!/usr/bin/env bash
# Builds and runs the host test for the disc validator's header read
# (platforms/switch/source/disc_header.cpp) together with the very disc_container.cpp
# the NRO compiles. Needs a g++ that has zlib, zstd, bzip2 and xz (the container reader
# uses all four); devkitPro's host compiler has none of them, scoop's MSYS2 mingw64 does.
#
# With no arguments only the synthetic images are tested. Real images are checked too
# when they are there: the reference ISO and every container beside it that DolphinTool
# made (see aleks2/testkit/DISC.md), or any list given as `ref.iso other.rvz ...`.
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

out="${TMPDIR:-/tmp}/dusk-disc-header-test"
mkdir -p "$out"
cd "$out"
"$cxx" -std=c++20 -O1 -Wall -Wextra \
  -I"$(w "$root/platforms/switch/include")" \
  "$(w "$root/platforms/switch/source/disc_header.cpp")" \
  "$(w "$root/platforms/switch/source/disc_container.cpp")" \
  "$(w "$here/wii_aes_host.cpp")" \
  "$(w "$here/disc_header_test.cpp")" \
  -lz -lzstd -lbz2 -llzma -lcrypto -o disc_header_test

if [ "$#" -gt 0 ]; then
  ./disc_header_test --reference "$@"
  exit
fi
kit="$(cd "$root/.." && pwd)"
ref="$kit/disc/GZ2P01.iso"
if [ -f "$ref" ]; then
  images=("$ref")
  for f in "$kit"/testkit/disc/*.rvz "$kit"/testkit/disc/*.wia "$kit"/testkit/disc/*.gcz; do
    [ -f "$f" ] && images+=("$(w "$f")")
  done
  ./disc_header_test --reference "$(w "$ref")" "${images[@]:1}"
else
  ./disc_header_test
fi
