#!/usr/bin/env bash
# H38-c6: the console findings as the game compiles them (hardware_findings_test.cpp says what is checked). Host C++20
# compiler only; the Python reading of the same file is checked by switch_pipeline_test.py.
#
#   test-hardware-findings.sh [OUT_DIR]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-hardware-findings}"
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -O1 -Wall -Wextra -Werror -I"$root/src" "$here/hardware_findings_test.cpp" \
  -o "$out/hardware_findings_test"
"$out/hardware_findings_test"
