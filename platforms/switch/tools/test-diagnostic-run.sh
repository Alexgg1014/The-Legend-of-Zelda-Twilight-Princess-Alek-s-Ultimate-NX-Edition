#!/usr/bin/env bash
# H34-c2: builds and runs the host test for the start/end order of diagnostic runs, the
# Prepare Shaders config_json_written baseline and the achievement suspension, with
# AddressSanitizer and UBSan when the compiler has them.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
cxx="${CXX:-g++}"
out="${TMPDIR:-/tmp}/dusk-diagnostic-run-test"
mkdir -p "$out"
cd "$out"
srcs=("$root/src/dusk/shader_prep.cpp" "$root/src/dusk/benchmark_report.cpp" "$root/src/dusk/perf_tour.cpp"
      "$root/src/dusk/fifo_report.cpp" "$here/diagnostic_run_test.cpp")
# H35: the tour report includes Aurora's C structs (aurora/tuning.h, header only).
flags=(-std=c++20 -Wall -Wextra -Wno-missing-field-initializers -O1 -I"$root/src" -I"$root/extern/aurora/include")
"$cxx" "${flags[@]}" -fsanitize=address,undefined -fno-sanitize-recover=undefined "${srcs[@]}" \
    -o diagnostic_run_test 2>/dev/null || "$cxx" "${flags[@]}" "${srcs[@]}" -o diagnostic_run_test
./diagnostic_run_test
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
for f in diagnostic_run_test.tour.*.json; do
  "$py" -m json.tool "$f" > /dev/null
done
echo "tour reports of every exit parse as JSON"
