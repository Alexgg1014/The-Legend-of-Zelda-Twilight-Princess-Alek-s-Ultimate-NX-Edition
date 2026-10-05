#!/usr/bin/env bash
# H38: the Randomizer's idempotent game-mode lifecycle. Applies the real patch stack to the pinned source and builds
# randomizer_lifecycle_test.cpp against the patched src/session_lifecycle.hpp with the SDK's service headers and fake
# services with counters: full activation, a failure at each stage with rollback, a double shutdown, deactivation
# followed by mod_shutdown and a preload thread still pending. Then checks the patched sources are wired to it: no raw
# unregister of the menu tab, no detached thread, session.cpp activates and shuts down through SessionResources.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-${TMPDIR:-/tmp}/dusk-randomizer-lifecycle-test}"
rm -rf "$out"
mkdir -p "$out/tree"
rando="${RANDOMIZER_SRC:-$root/mods/randomizer}"
[ -e "$rando/.git" ] || [ -d "$rando/generator" ] || { echo "SKIPPED: randomizer sources not found"; exit 2; }
(cd "$rando" && git archive --format=tar HEAD) | tar -x -C "$out/tree"
git -C "$out/tree" init -q
for patch in "$root"/platforms/switch/patches/randomizer/*.patch; do
    git -C "$out/tree" apply "$patch" || { echo "patch $(basename "$patch") does not apply" >&2; exit 1; }
done
t="$out/tree"
grep -q 'session_lifecycle.hpp' "$t/src/session.cpp" || { echo "FAIL: session.cpp does not use session_lifecycle.hpp" >&2; exit 1; }
grep -q 's_resources->shutdown()' "$t/src/session.cpp" || { echo "FAIL: shutdown does not go through SessionResources" >&2; exit 1; }
! grep -rn '\.detach()' "$t/src" || { echo "FAIL: a detached thread is left in src/" >&2; exit 1; }
! grep -rn --include='*.cpp' 'unregister_menu_tab' "$t/src" | grep -v 'rando_config.cpp' || { echo "FAIL: raw menu tab unregister" >&2; exit 1; }
[ "$(grep -c 'svc_mng.ui->unregister_menu_tab' "$t/src/ui/rando_config.cpp" | tr -d ' ')" -eq 1 ] || { echo "FAIL: more than one unregister_menu_tab" >&2; exit 1; }
grep -q 'release_handle(g_menu_tab' "$t/src/ui/rando_config.cpp" || { echo "FAIL: removeMenuTab does not invalidate its handle" >&2; exit 1; }
"${CXX:-g++}" -std=c++20 -Wall -Wextra -Wno-attributes -O1 -g -pthread -I"$t/src" -I"$t" -I"$root/sdk/include" \
    "$here/randomizer_lifecycle_test.cpp" -o "$out/lifecycle_test"
"$out/lifecycle_test"
