#!/usr/bin/env bash
# H37: hook composition and conflicts through the game's own HookService (hook_conflicts_test.cpp
# says what is checked), on the host with ASan, UBSan and _GLIBCXX_ASSERTIONS.
#
#   test-hook-conflicts.sh [OUT_DIR]
#
# Needs BOREALIS_SRC (the borealis checkout the game builds with) and FMT_SRC (fmt 12.1.0), as
# test-luau-probe.sh does.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-hook-conflicts}"
mkdir -p "$out/gen/borealis"
out="$(cd "$out" && pwd)"
borealis="${BOREALIS_SRC:-$root/extern/borealis}"
fmt="${FMT_SRC:?FMT_SRC must point at fmt 12.1.0}"
[ -f "$borealis/include/borealis/log.hpp" ] || { echo "BOREALIS_SRC has no include/borealis/log.hpp"; exit 2; }
cxx="${CXX:-g++}"
sed -e 's/@BOREALIS_APP_DESCRIBE@/h37-test/' -e 's/@BOREALIS_APP_VERSION_CODE@/0/' \
    -e 's/@[A-Z_]*@/0/g' "$borealis/cmake/version.h.in" > "$out/gen/borealis/version.h"
cat > "$out/gen/prelude.hpp" <<'PRELUDE'
// The game's own sources get these through its precompiled header.
#include <dolphin/types.h>
#include <optional>
#include "dusk/config_var.hpp"
namespace dusk::mods { using dusk::config::ConfigVar; }
PRELUDE
# No patching backend: registration and dispatch are under test, not funchook.
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -Wno-attributes \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined -D_GLIBCXX_ASSERTIONS \
  -DTARGET_PC=1 -DDUSK_CODE_MODS=1 -DDUSK_HAS_FUNCHOOK=0 -DDUSK_HAS_PREPATCH=0 \
  -I"$root/src" -I"$root/include" -I"$root/sdk/include" -I"$root/extern/aurora/include" \
  -I"$root/extern/aurora/include/dolphin" -I"$root/libs/JSystem/include" -I"$borealis/include" \
  -I"$out/gen" -I"$fmt/include" -DFMT_HEADER_ONLY=1 -include "$out/gen/prelude.hpp" \
  "$here/hook_conflicts_test.cpp" "$root/src/dusk/mods/svc/hook.cpp" \
  "$root/src/dusk/mods/svc/hook_conflicts.cpp" -o "$out/hook_conflicts_test"
"$out/hook_conflicts_test"
