#!/usr/bin/env bash
# aleks2 (V1-c1): host test for the game-free part of frame telemetry and dynamic resolution
# (src/dusk/perf_core.cpp), with ASan/UBSan when the compiler has them.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-perf-core-test"
mkdir -p "$out"
cd "$out"
flags=(-std=c++20 -Wall -Wextra -O1 -g -I"$root/src")
if [ "${SANITIZE:-1}" = "1" ] && echo 'int main(){}' | g++ -x c++ -fsanitize=address,undefined - -o /dev/null 2>/dev/null; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
g++ "${flags[@]}" "$root/src/dusk/perf_core.cpp" "$root/src/dusk/perf_report.cpp" "$root/src/dusk/benchmark_report.cpp" \
    "$here/perf_core_test.cpp" -o perf_core_test
./perf_core_test
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
"$py" -m json.tool perf_core_test.telemetry.json > /dev/null && echo "telemetry sample parses as JSON"
