#!/usr/bin/env bash
# Builds and runs the host test for the benchmark's report code with whatever
# g++ is on PATH (the MSYS2 mingw64 one in this workspace). No game, no
# devkitPro: the module under test deliberately includes nothing else.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-bench-test"
mkdir -p "$out"
cd "$out"
# H35: fifo_report.cpp reads Aurora's C structs from aurora/tuning.h (header only).
aurora_inc="$root/extern/aurora/include"
if [ ! -f "$aurora_inc/aurora/tuning.h" ]; then
  echo "extern/aurora is not checked out (git submodule update --init extern/aurora)" >&2
  exit 1
fi
flags=(-std=c++20 -Wall -Wextra -O1 -I"$root/src" -I"$aurora_inc")
if [ "${SANITIZE:-1}" = "1" ] && echo 'int main(){}' | g++ -x c++ -fsanitize=address,undefined - -o /dev/null 2>/dev/null; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
g++ "${flags[@]}" \
    "$root/src/dusk/benchmark_report.cpp" "$root/src/dusk/fifo_report.cpp" "$here/benchmark_report_test.cpp" \
    -o benchmark_report_test
./benchmark_report_test
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
"$py" -m json.tool benchmark_report_test.sample.json > /dev/null && echo "sample report parses as JSON"
"$py" - benchmark_report_test.fifo.json <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
assert d["schema_version"] == 4
f = d["fifo"]
for key in ("worker_busy_ms_total", "worker_busy_ms_per_frame", "processed_bytes_total", "processed_bytes_per_frame",
            "drain_calls_total", "drain_wait_ms_total", "drain_wait_ms_per_frame", "publishes_total",
            "publishes_per_frame", "worker_wakes_total", "worker_wakes_per_frame"):
    assert key in f, key
sites = f["drains_by_site"]
assert set(sites) == {"gx_draw_done", "end_frame", "companion_capture", "explicit", "aux_pass", "other"}, sites
assert sum(s["drain_calls"] for s in sites.values()) == f["drain_calls_total"]
assert abs(sum(s["drain_wait_ms"] for s in sites.values()) - f["drain_wait_ms_total"]) < 1e-6
assert abs(f["worker_busy_ms_per_frame"] - 1.0) < 1e-9
u = d["uniforms"]
assert u["mode"] == "verify" and u["verification_mismatches"] == 0 and u["disabled_after_mismatch"] is False
for key in ("full_builds", "repeat_hits", "reuse_candidates", "reuses", "verification_checks"):
    assert key in u, key
der = d["derived"]
assert abs(der["painter_net_ms_per_frame"] - 16.0) < 1e-9
assert abs(der["drawrec_net_ms_per_frame"] - 9.5) < 1e-9
assert der["painter_net_ms_per_frame"] >= 0 and der["drawrec_net_ms_per_frame"] >= 0
i = d["interpolation"]
assert i["frames_total"] == 300 and i["frames_interpolated_rejected"] == 1
assert i["frames_normal"] + i["frames_interpolated_accepted"] + i["frames_interpolated_with_sim_tick"] == 300
assert "frame_waits" in d
print("H35 fifo/uniform sample parses and checks out")
EOF
