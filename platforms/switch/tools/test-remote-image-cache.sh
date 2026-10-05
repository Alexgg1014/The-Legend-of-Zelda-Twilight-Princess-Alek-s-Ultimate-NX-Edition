#!/usr/bin/env bash
# H34: builds and runs the host test for the mod browser's image cache
# (src/dusk/ui/remote_image_cache.cpp), the RmlUi-free part of the remote texture
# provider. Any C++20 g++ will do.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
cxx="${CXX:-g++}"
w() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }

out="${TMPDIR:-/tmp}/dusk-remote-image-cache-test"
mkdir -p "$out"
cd "$out"
"$cxx" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$(w "$root/src")" -I"$(w "$here")" \
  "$(w "$root/src/dusk/ui/remote_image_cache.cpp")" \
  "$(w "$here/remote_image_cache_test.cpp")" \
  -o remote_image_cache_test 2>/dev/null || \
"$cxx" -std=c++20 -O1 -Wall -Wextra \
  -I"$(w "$root/src")" -I"$(w "$here")" \
  "$(w "$root/src/dusk/ui/remote_image_cache.cpp")" \
  "$(w "$here/remote_image_cache_test.cpp")" \
  -o remote_image_cache_test
./remote_image_cache_test
