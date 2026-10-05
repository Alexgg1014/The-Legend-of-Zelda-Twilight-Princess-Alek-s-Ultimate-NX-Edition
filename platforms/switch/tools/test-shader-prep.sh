#!/usr/bin/env bash
# H34: builds and runs the host test for Prepare Shaders' game-free part
# (src/dusk/shader_prep.cpp, with the benchmark's JSON writer), checks every zone
# against the warp table, and parses the sample reports as JSON.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-shader-prep-test"
mkdir -p "$out"
cd "$out"
g++ -std=c++20 -Wall -Wextra -Wno-missing-field-initializers -O1 -I"$root/src" \
    "$root/src/dusk/shader_prep.cpp" "$root/src/dusk/benchmark_report.cpp" \
    "$here/shader_prep_test.cpp" -o shader_prep_test
./shader_prep_test "$root/src/dusk/map_loader_definitions.h"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
for f in shader_prep_test.completed.json shader_prep_test.cancelled.json shader_prep_test.failed.json; do
  "$py" -m json.tool "$f" > /dev/null
done
echo "sample shader-prep reports parse as JSON"
