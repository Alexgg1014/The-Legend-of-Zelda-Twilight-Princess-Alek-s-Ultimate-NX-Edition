#!/usr/bin/env bash
# Builds and runs the host test for the H33 predictive interpolation governor with whatever g++ is on PATH.
# The header includes only the standard library.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-interp-governor-test"
mkdir -p "$out"
cd "$out"
g++ -std=c++20 -Wall -Wextra -O1 -I"$root/src" "$here/interp_governor_test.cpp" -o interp_governor_test
./interp_governor_test
