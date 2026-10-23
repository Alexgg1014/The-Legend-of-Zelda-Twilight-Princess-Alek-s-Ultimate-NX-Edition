#!/usr/bin/env bash
# aleks2 (V1-rc8): host test for the console-free part of the in-app updater (src/dusk/self_update_core.cpp):
# version comparison, which asset is the NRO, and putting a download in place with a .bak. borealis' version parser
# is compiled in; its generated version.h is stood in for. ASan/UBSan when the compiler has them.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-self-update-test"
mkdir -p "$out/gen/borealis"
cd "$out"
printf '#pragma once\n#define BOREALIS_APP_DESCRIBE "v1.0.0-rc.8"\n#define BOREALIS_APP_VERSION "1.0.0"\n' \
    > gen/borealis/version.h
flags=(-std=c++20 -Wall -Wextra -O1 -g -I"$out/gen" -I"$root/src" -I"$root/extern/borealis/include")
if [ "${SANITIZE:-1}" = "1" ] && echo 'int main(){}' | g++ -x c++ -fsanitize=address,undefined - -o /dev/null 2>/dev/null; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
g++ "${flags[@]}" "$root/src/dusk/self_update_core.cpp" "$root/extern/borealis/src/update/version.cpp" \
    "$here/self_update_test.cpp" -o self_update_test
./self_update_test "$out/fs"
