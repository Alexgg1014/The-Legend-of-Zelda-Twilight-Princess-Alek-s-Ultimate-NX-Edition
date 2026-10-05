#!/usr/bin/env bash
# H37-c4: builds and runs the host test of the services a native mod imports (mod_services_test.cpp
# says what is checked), with AddressSanitizer and UBSan when the compiler has them.
#
#   test-mod-services.sh
#
# With H37_RANDOMIZER_SO (a Randomizer mod.so, e.g. from build-h37-test-mods.sh) its real service
# imports are read too and must all be satisfied by this Switch build's services.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
cxx="${CXX:-g++}"
out="${TMPDIR:-/tmp}/dusk-mod-services-test"
mkdir -p "$out"
cd "$out"
srcs=("$root/src/dusk/mods/loader/mod_services.cpp" "$here/mod_services_test.cpp")
flags=(-std=c++20 -Wall -Wextra -Wno-missing-field-initializers -O1 -g -I"$root/src" -I"$root/sdk/include")
"$cxx" "${flags[@]}" -fsanitize=address,undefined -fno-sanitize-recover=undefined "${srcs[@]}" \
    -o mod_services_test 2>/dev/null || "$cxx" "${flags[@]}" "${srcs[@]}" -o mod_services_test
./mod_services_test "$root/sdk/include"
