#!/usr/bin/env bash
# Builds and runs the host test for the H33 resolution policy with whatever g++ is on PATH.
# The header includes only the standard library and resolution_ladder.hpp.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-resolution-policy-test"
mkdir -p "$out"
cd "$out"
g++ -std=c++20 -Wall -Wextra -O1 -I"$root/src" "$here/resolution_policy_test.cpp" -o resolution_policy_test
./resolution_policy_test
