#!/usr/bin/env bash
# H38-c4: the L3 + R3 recenter chord (src/dusk/wii_recenter_combo.hpp). No game, no hardware.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-wii-recenter-combo-test"
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -Wall -Wextra -O1 -g -I"$root/src" "$here/wii_recenter_combo_test.cpp" -o "$out/wii_recenter_combo_test"
"$out/wii_recenter_combo_test"
