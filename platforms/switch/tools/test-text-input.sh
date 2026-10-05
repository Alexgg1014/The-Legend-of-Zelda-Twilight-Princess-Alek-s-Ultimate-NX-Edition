#!/usr/bin/env bash
# H38: the native text input seam (src/dusk/ui/platform_text_input.*) with a fake keyboard: accept, cancel, error,
# length limits, one call per activation and focus restoration. libnx and RmlUi are not needed.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-text-input-test"
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -Wall -Wextra -O1 -g -I"$root/src" "$here/text_input_test.cpp" \
    "$root/src/dusk/ui/platform_text_input.cpp" -o "$out/text_input_test"
"$out/text_input_test"
