#!/usr/bin/env bash
# aleks2 (rc9): host test of the panned minimap's arithmetic (src/dusk/minimap_pan.hpp).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/minimap-pan-test"
mkdir -p "$out"
flags=(-std=c++20 -O1 -g -Wall -Wextra -I"$root/src")
if echo 'int main(){}' | g++ -x c++ -fsanitize=address,undefined - -o "$out/probe" 2>/dev/null; then
    flags+=(-fsanitize=address,undefined)
fi
g++ "${flags[@]}" "$here/minimap_pan_test.cpp" -o "$out/minimap_pan_test"
"$out/minimap_pan_test"
