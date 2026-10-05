#!/usr/bin/env bash
# H37: safe mode for native mods (boot_guard_test.cpp says what is checked), on the host with
# ASan, UBSan and _GLIBCXX_ASSERTIONS. Crashes are real: child processes that abort().
#
#   test-boot-guard.sh [OUT_DIR]
#
# Needs nlohmann/json.hpp on the include path (NLOHMANN_JSON_INCLUDE to point elsewhere).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-boot-guard}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
cxx="${CXX:-g++}"
json=()
[ -n "${NLOHMANN_JSON_INCLUDE:-}" ] && json=(-I"$NLOHMANN_JSON_INCLUDE")
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -D_GLIBCXX_ASSERTIONS -I"$root/src" "${json[@]}" "$here/boot_guard_test.cpp" \
  "$root/src/dusk/mods/loader/boot_guard.cpp" "$root/src/dusk/mods/loader/elf_loader.cpp" \
  -o "$out/boot_guard_test"
# The children abort on purpose: ASan must let SIGABRT through rather than report it.
ASAN_OPTIONS=handle_abort=0 "$out/boot_guard_test" "$out/guard"
