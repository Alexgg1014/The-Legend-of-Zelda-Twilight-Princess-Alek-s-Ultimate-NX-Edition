#!/usr/bin/env bash
# aleks2 (V1): host test for the mod loader's same-file check (src/dusk/mods/loader/same_path.hpp), with ASan/UBSan
# when the compiler has them. The Switch branch (names, not inodes) is the one under test: same_path_by_name.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-same-path-test"
mkdir -p "$out"
cd "$out"
flags=(-std=c++20 -Wall -Wextra -O1 -g -I"$root/src")
if [ "${SANITIZE:-1}" = "1" ] && echo 'int main(){}' | g++ -x c++ -fsanitize=address,undefined - -o /dev/null 2>/dev/null; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
g++ "${flags[@]}" "$here/same_path_test.cpp" -o same_path_test
./same_path_test "$out/tree"
