#!/usr/bin/env bash
# H38-c3: the sword gesture -> cut mapping and the one-shot gesture slot (src/dusk/wii_cut_select.hpp). No game, no hardware.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-wii-cut-select-test"
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -Wall -Wextra -O1 -g -I"$root/src" "$here/wii_cut_select_test.cpp" -o "$out/wii_cut_select_test"
"$out/wii_cut_select_test"
