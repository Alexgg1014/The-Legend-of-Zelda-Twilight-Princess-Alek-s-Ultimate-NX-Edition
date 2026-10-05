#!/usr/bin/env bash
# H38-c5: the Play / Randomizer carousel changes once per press (src/dusk/ui/game_mode_cycle_latch.hpp). No game, no UI.
# (test-game-mode-cycle-latch.ps1 runs the same test on Windows.)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-game-mode-cycle-latch-test"
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror -O1 -g -I"$root/src" "$here/game_mode_cycle_latch_test.cpp" -o "$out/game_mode_cycle_latch_test"
"$out/game_mode_cycle_latch_test"
