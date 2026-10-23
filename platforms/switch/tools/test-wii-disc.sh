#!/usr/bin/env bash
# aleks2 (rc17): builds and runs the host test for Wii discs (wii_disc_test.cpp): the encrypted-.iso path, the
# RVZ/WIA stored-partition path, the key files, and the nod API over a Wii disc (iso_nod_shim.cpp). Needs the
# disc-container libraries (zlib, zstd, bzip2, xz) and OpenSSL's libcrypto for the test's own AES.
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

out="${TMPDIR:-/tmp}/dusk-wii-disc-test"
mkdir -p "$out"
cd "$out"
"$cxx" -std=c++20 -O1 -Wall -Wextra \
  -I"$(w "$root/platforms/switch/include")" \
  -I"$(w "$root/platforms/switch/third_party/nod-c")" \
  "$(w "$root/platforms/switch/source/disc_container.cpp")" \
  "$(w "$root/platforms/switch/source/wii_crypto.cpp")" \
  "$(w "$root/platforms/switch/source/iso_nod_shim.cpp")" \
  "$(w "$here/wii_aes_host.cpp")" \
  "$(w "$here/wii_disc_test.cpp")" \
  -lz -lzstd -lbz2 -llzma -lcrypto -o wii_disc_test
./wii_disc_test "$(w "$out/scratch")"
