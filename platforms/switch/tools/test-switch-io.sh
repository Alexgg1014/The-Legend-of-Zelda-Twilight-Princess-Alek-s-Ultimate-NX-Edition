#!/usr/bin/env bash
# H37-c4: the platform layer the file service stands on, on a POSIX host (Linux or WSL):
# borealis_io_switch.cpp (borealis::io) and borealis_file_select_switch.cpp (the picker that is not
# there), with the Horizon SDL replaced by a few functions over stdio (sdl_stdio_shim.cpp).
# switch_io_test.cpp says what is checked. AddressSanitizer and UBSan when the compiler has them.
#
#   test-switch-io.sh
#
# Needs the SDL 3 headers (SDL3_INCLUDE: the sdl-src/include directory of a Switch build tree, or any
# SDL 3 include dir) and the borealis checkout the game builds with (extern/borealis).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
sdl="${SDL3_INCLUDE:?SDL3_INCLUDE must point at the SDL 3 include directory}"
[ -f "$sdl/SDL3/SDL_iostream.h" ] || { echo "SDL3_INCLUDE has no SDL3/SDL_iostream.h"; exit 2; }
cxx="${CXX:-g++}"
out="${TMPDIR:-/tmp}/dusk-switch-io-test"
mkdir -p "$out"
cd "$out"
srcs=("$root/platforms/switch/source/borealis_io_switch.cpp" "$root/platforms/switch/source/borealis_file_select_switch.cpp"
      "$here/sdl_stdio_shim.cpp" "$here/switch_io_test.cpp")
flags=(-std=c++20 -Wall -Wextra -O1 -g -I"$root/extern/borealis/include" -I"$sdl" -I"$sdl/build_config")
"$cxx" "${flags[@]}" -fsanitize=address,undefined -fno-sanitize-recover=undefined "${srcs[@]}" \
    -o switch_io_test 2>/dev/null || "$cxx" "${flags[@]}" "${srcs[@]}" -o switch_io_test
./switch_io_test
