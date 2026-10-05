#!/usr/bin/env bash
# Builds and runs the host test for the H33 performance profiles with whatever g++ is on PATH.
# The header includes only the standard library and resolution_ladder.hpp.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-perf-profile-test"
mkdir -p "$out"
cd "$out"
g++ -std=c++20 -Wall -Wextra -O1 -I"$root/src" "$here/perf_profile_test.cpp" -o perf_profile_test
./perf_profile_test
