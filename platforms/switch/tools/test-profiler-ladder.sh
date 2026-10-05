#!/usr/bin/env bash
# Builds and runs the host test for the profiler and the resolution ladder with whatever g++ is
# on PATH. Both headers include only the standard library.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-profiler-test"
mkdir -p "$out"
cd "$out"
g++ -std=c++20 -Wall -Wextra -O1 -I"$root/src" "$here/profiler_ladder_test.cpp" -o profiler_ladder_test
./profiler_ladder_test
