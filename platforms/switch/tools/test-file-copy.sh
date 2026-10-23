#!/usr/bin/env bash
# aleks2 (rc18): host test for src/dusk/file_copy.hpp -- the card-safe copy behind Export Save and the mod
# updater's backup. Reproduces the console's failure (fchmod missing, as on libnx) and checks the replacement.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-file-copy-test-build"
mkdir -p "$out"
cd "$out"
flags=(-std=c++20 -Wall -Wextra -O1 -g -I"$root/src")
if [ "${SANITIZE:-1}" = "1" ] && echo 'int main(){}' | g++ -x c++ -fsanitize=address,undefined - -o /dev/null 2>/dev/null; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
g++ "${flags[@]}" "$here/file_copy_test.cpp" "$root/src/dusk/utilities.cpp" -o file_copy_test
./file_copy_test "$out/tree"
