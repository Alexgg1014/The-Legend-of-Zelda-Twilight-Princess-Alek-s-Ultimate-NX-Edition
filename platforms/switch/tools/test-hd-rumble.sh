#!/usr/bin/env bash
# aleks2 (V1-rc16): host test for the HD Rumble envelopes and tracker (src/dusk/hd_rumble_core.hpp), with ASan/UBSan
# when the compiler has them.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-hd-rumble-test"
mkdir -p "$out"
flags=(-std=c++20 -Wall -Wextra -O1 -g -I"$root/src")
if [ "${SANITIZE:-1}" = "1" ] && echo 'int main(){}' | g++ -x c++ -fsanitize=address,undefined - -o /dev/null 2>/dev/null; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
g++ "${flags[@]}" -include algorithm "$here/hd_rumble_test.cpp" -o "$out/hd_rumble_test"
"$out/hd_rumble_test"
