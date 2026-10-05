#!/usr/bin/env bash
# Builds and runs the host test for the GX pipeline memo (H31) with whatever g++ is
# on PATH. The header under test includes only the standard library.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-memo-test"
mkdir -p "$out"
cd "$out"
g++ -std=c++20 -Wall -Wextra -O1 -I"$root/extern/aurora/lib" \
    "$here/pipeline_memo_test.cpp" -o pipeline_memo_test
./pipeline_memo_test
