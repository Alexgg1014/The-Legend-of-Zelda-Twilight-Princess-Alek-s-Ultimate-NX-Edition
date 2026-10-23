#!/usr/bin/env bash
# DM2-a: the loader's check that a mod's hooks fit the code arenas before it starts (hook_room_test.cpp
# says what is checked), through the game's own src/dusk/mods/svc/hook.cpp, on the host with ASan,
# UBSan and _GLIBCXX_ASSERTIONS.
#
#   test-hook-room.sh [OUT_DIR]
#
# Needs BOREALIS_SRC and FMT_SRC as test-hook-conflicts.sh does.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-hook-room}"
mkdir -p "$out/gen/borealis"
out="$(cd "$out" && pwd)"
borealis="${BOREALIS_SRC:-$root/extern/borealis}"
fmt="${FMT_SRC:?FMT_SRC must point at fmt 12.1.0}"
[ -f "$borealis/include/borealis/log.hpp" ] || { echo "BOREALIS_SRC has no include/borealis/log.hpp"; exit 2; }
cxx="${CXX:-g++}"
sed -e 's/@BOREALIS_APP_DESCRIBE@/dm2a-test/' -e 's/@BOREALIS_APP_VERSION_CODE@/0/' \
    -e 's/@[A-Z_]*@/0/g' "$borealis/cmake/version.h.in" > "$out/gen/borealis/version.h"
printf '%s\n' '#include <dolphin/types.h>' '#include <optional>' '#include "dusk/config_var.hpp"' \
  'namespace dusk::mods { using dusk::config::ConfigVar; }' > "$out/gen/prelude.hpp"
# No funchook: hooks are held through a fake prepatch backend; the arenas' count is the test's.
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -Wno-attributes \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined -D_GLIBCXX_ASSERTIONS \
  -DTARGET_PC=1 -DDUSK_CODE_MODS=1 -DDUSK_HAS_FUNCHOOK=0 -DDUSK_HAS_PREPATCH=1 -DALEKS2_HOOK_ROOM=1 \
  -I"$root/src" -I"$root/include" -I"$root/sdk/include" -I"$root/extern/aurora/include" \
  -I"$root/extern/aurora/include/dolphin" -I"$root/libs/JSystem/include" -I"$borealis/include" \
  -I"$out/gen" -I"$fmt/include" -DFMT_HEADER_ONLY=1 -include "$out/gen/prelude.hpp" \
  "$here/hook_room_test.cpp" "$root/src/dusk/mods/svc/hook.cpp" \
  "$root/src/dusk/mods/svc/hook_conflicts.cpp" -o "$out/hook_room_test"
"$out/hook_room_test"
