#!/usr/bin/env bash
# H37-c5: the Randomizer's data-directory lookup. Applies platforms/switch/patches/randomizer to a copy of the
# pinned sources, then runs the patched src/paths.cpp (real, with a stub session.hpp and host) under ASan+UBSan:
# a good path resolves once; data_dir failing, or "OK" with a null or empty path, gives no path from any of the five
# functions, host->fail exactly once, and nothing is built from the null pointer (what crashed H37-c4 after
# "Failed to get data directory (1)"). Needs git and a host C++20 compiler; RANDOMIZER_SRC if the submodule is absent.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-randomizer-paths}"; rm -rf "$out"; mkdir -p "$out/src"; out="$(cd "$out" && pwd)"
rando="${RANDOMIZER_SRC:-$root/mods/randomizer}"
[ -e "$rando/.git" ] || [ -d "$rando/src" ] || { echo "SKIPPED: randomizer sources not found"; exit 2; }
mkdir -p "$out/tree"
(cd "$rando" && git archive --format=tar HEAD) | tar -x -C "$out/tree"
# Keep `git apply` anchored here even when OUT lives under another checkout.
git -C "$out/tree" init -q
for patch in "$root"/platforms/switch/patches/randomizer/*.patch; do
  git -C "$out/tree" apply "$patch" || { echo "patch $(basename "$patch") does not apply" >&2; exit 1; }
done
# H37-c5: no caller anywhere in the patched tree -- the generator included, which the first c5 package build
# found still dividing the optional by a string -- may use one of the five functions as if it returned a path.
if grep -rnE 'GetRandomizer(Settings|Preferences|Presets|Seeds)?Path\(\)[[:space:]]*(/|\.)' "$out/tree"      --include='*.cpp' --include='*.hpp' --include='*.h' --include='*.inc'; then
  echo "FAIL: a caller uses a GetRandomizer*Path() result as a path without checking it" >&2; exit 1
fi
grep -q 'const auto seedsPath = ::randomizer::paths::GetRandomizerSeedsPath();' "$out/tree/generator/randomizer.cpp" ||
  { echo "FAIL: generator/randomizer.cpp does not check the seeds path" >&2; exit 1; }
cp "$out/tree/src/paths.cpp" "$out/tree/src/paths.hpp" "$out/tree/src/data_dir.hpp" "$out/src/"
cat > "$out/src/session.hpp" <<'EOF'
#pragma once
#include <mods/svc/host.h>
namespace randomizer::session { struct ServiceManager { ModContext* mod_ctx; const HostService* host; }; extern ServiceManager svc_mng; }
EOF
for t in randomizer_paths_test randomizer_paths_failure_test; do
  "${CXX:-g++}" -std=c++20 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -w \
    -I"$out/src" -I"$root/sdk/include" "$here/$t.cpp" "$out/src/paths.cpp" -o "$out/$t"
  "$out/$t"
done
