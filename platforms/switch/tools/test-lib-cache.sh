#!/usr/bin/env bash
# aleks2 (V1-rc19): host test for src/dusk/mods/loader/lib_cache.cpp (lib_cache_test.cpp).
# Needs FMT_SRC and PICOSHA2_SRC (the build's _deps, as the other tests take them).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-lib-cache-test-build"
mkdir -p "$out"
cd "$out"
flags=(-std=c++20 -Wall -Wextra -O1 -g -I"$root/src" -I"${FMT_SRC}/include" -DFMT_HEADER_ONLY -I"${PICOSHA2_SRC}")
if [ "${SANITIZE:-1}" = "1" ] && echo 'int main(){}' | g++ -x c++ -fsanitize=address,undefined - -o /dev/null 2>/dev/null; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
g++ "${flags[@]}" "$here/lib_cache_test.cpp" "$root/src/dusk/mods/loader/lib_cache.cpp" "$root/src/dusk/utilities.cpp" \
  -o lib_cache_test
./lib_cache_test "$out/tree"
