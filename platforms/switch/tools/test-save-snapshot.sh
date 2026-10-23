#!/usr/bin/env bash
# aleks2 (cross-save): save_manager's snapshot pieces (save_snapshot_test.cpp says what is checked), on the host with
# ASan, UBSan and _GLIBCXX_ASSERTIONS. Compiles the real save_manager.cpp and utilities.cpp against the stand-ins in
# save_snapshot_shims/ and the test's own card/I/O stubs.
#
#   test-save-snapshot.sh [OUT_DIR]
#
# Needs: FMT_SRC (fmt sources), MINIZ_SRC (miniz 3.0.2: miniz.c/.h), PICOSHA2_SRC (picosha2.h), nlohmann/json.hpp on
# the include path (or NLOHMANN_JSON_INCLUDE), and extern/aurora + extern/borealis in the tree.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-save-snapshot}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
cxx="${CXX:-g++}"
cc="${CC:-gcc}"
: "${FMT_SRC:?FMT_SRC must point at the fmt sources}"
: "${MINIZ_SRC:?MINIZ_SRC must point at miniz 3.0.2}"
: "${PICOSHA2_SRC:?PICOSHA2_SRC must point at picosha2}"
json=()
[ -n "${NLOHMANN_JSON_INCLUDE:-}" ] && json=(-I"$NLOHMANN_JSON_INCLUDE")
san=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
"$cc" -O1 -g "${san[@]}" -w -c "$MINIZ_SRC/miniz.c" -I"$MINIZ_SRC" -o "$out/miniz.o"
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers -Wno-parentheses "${san[@]}" -D_GLIBCXX_ASSERTIONS \
  -DFMT_HEADER_ONLY \
  -I"$here/save_snapshot_shims" -I"$root/src" -I"$root/include" \
  -I"$root/extern/aurora/include" -I"$root/extern/borealis/include" \
  -I"$FMT_SRC/include" -I"$MINIZ_SRC" -I"$PICOSHA2_SRC" "${json[@]}" \
  -I"$root/mods/cross_save" \
  "$here/save_snapshot_test.cpp" "$root/src/dusk/save_manager.cpp" "$root/src/dusk/utilities.cpp" "$out/miniz.o" \
  "$root/mods/cross_save/core/dusksave.cpp" "$root/mods/cross_save/core/sha256.cpp" "$root/mods/cross_save/core/json.cpp" \
  -ldl -o "$out/save_snapshot_test"
"$out/save_snapshot_test" "$out/work"
