#!/usr/bin/env bash
# DM1-b: the update journal (update_journal_test.cpp says what is checked), on the host with
# ASan, UBSan and _GLIBCXX_ASSERTIONS.
#
#   test-update-journal.sh [OUT_DIR]
#
# Needs nlohmann/json.hpp on the include path (NLOHMANN_JSON_INCLUDE to point elsewhere).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-update-journal}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
cxx="${CXX:-g++}"
json=()
[ -n "${NLOHMANN_JSON_INCLUDE:-}" ] && json=(-I"$NLOHMANN_JSON_INCLUDE")
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -D_GLIBCXX_ASSERTIONS -I"$root/src" "${json[@]}" "$here/update_journal_test.cpp" \
  "$root/src/dusk/mods/loader/update_journal.cpp" -o "$out/update_journal_test"
"$out/update_journal_test" "$out/work"
