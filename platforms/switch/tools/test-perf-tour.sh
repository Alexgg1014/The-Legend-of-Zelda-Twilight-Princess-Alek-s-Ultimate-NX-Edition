#!/usr/bin/env bash
# H35: builds and runs the host test for Run Performance Tour's game-free part
# (src/dusk/perf_tour.cpp with the benchmark's report helpers and fifo_report.cpp), checks the
# zones against the warp table, and validates the sample reports with a real JSON parser.
# AddressSanitizer and UBSan when the compiler has them.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
cxx="${CXX:-g++}"
out="${TMPDIR:-/tmp}/dusk-perf-tour-test"
mkdir -p "$out"
cd "$out"
aurora_inc="$root/extern/aurora/include"
if [ ! -f "$aurora_inc/aurora/tuning.h" ]; then
  echo "extern/aurora is not checked out (git submodule update --init extern/aurora)" >&2
  exit 1
fi
srcs=("$root/src/dusk/perf_tour.cpp" "$root/src/dusk/fifo_report.cpp" "$root/src/dusk/shader_prep.cpp"
      "$root/src/dusk/perf_core.cpp" "$root/src/dusk/perf_report.cpp"
      "$root/src/dusk/benchmark_report.cpp" "$here/perf_tour_test.cpp")
flags=(-std=c++20 -Wall -Wextra -Wno-missing-field-initializers -O1 -I"$root/src" -I"$aurora_inc")
"$cxx" "${flags[@]}" -fsanitize=address,undefined -fno-sanitize-recover=undefined "${srcs[@]}" \
    -o perf_tour_test 2>/dev/null || "$cxx" "${flags[@]}" "${srcs[@]}" -o perf_tour_test
./perf_tour_test "$root/src/dusk/map_loader_definitions.h"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
"$py" - <<'PYEOF'
import json

def load(name):
    with open(name) as f:
        return json.load(f)

done = load("perf_tour_test.completed.json")
assert done["schema"] == "dusklight-aleks-performance-tour" and done["schema_version"] == 1
zones = done["zones"]
# V1-rc7: Hyrule Field is a fourth zone after the three comparable ones; this sample measures only the first three.
assert len(zones) == 4 and (zones[3]["stage"], zones[3]["room"], zones[3]["point"]) == ("F_SP121", 0, 0) and zones[3]["class"] == "heavy-field", zones[3]
zones = zones[:3]
assert [(z["stage"], z["room"], z["point"], z["layer"]) for z in zones] == [
    ("F_SP122", 8, 0, -1), ("F_SP103", 0, 1, -1), ("R_SP01", 4, 0, -1)], zones
assert [z["class"] for z in zones] == ["heavy", "medium", "light"]
assert [z["index"] for z in zones] == [0, 1, 2]
# Separate metrics per zone: every zone keeps its own numbers.
busy = [z["fifo"]["worker_busy_ms_per_frame"] for z in zones]
assert busy == [6.0, 7.0, 8.0], busy
fps = [z["performance"]["avg_fps"] for z in zones]
assert len(set(fps)) == 3, fps
for z in zones:
    for block in ("performance", "cpu_zones_ms", "derived", "fifo", "uniforms", "frame_waits", "interpolation",
                  "loading", "environment"):
        assert block in z, block
    d = z["derived"]
    assert d["painter_net_ms_per_frame"] >= 0 and d["drawrec_net_ms_per_frame"] >= 0
    assert "drains_by_site" in z["fifo"] and "gx_draw_done" in z["fifo"]["drains_by_site"]
    i = z["interpolation"]
    for key in ("requested_mode", "quality", "sim_rate_hz", "presented_rate_fps", "frames_normal",
                "frames_interpolated_accepted", "frames_interpolated_rejected", "stall_enters", "stall_exits",
                "resume_attempts", "resume_failures", "stalled_seconds", "stalled_pct_of_window",
                "stalled_at_arrival", "stalled_at_measure_end", "recovery_seconds_from_arrival"):
        assert key in i, key
    l = z["loading"]
    for key in ("pipelines_queued_at_arrival", "pipelines_queued_max", "pipelines_created_in_zone",
                "pipeline_wait_timed_out", "cache_state"):
        assert key in l, key
    e = z["environment"]
    for key in ("mode", "flip_grip", "resolution", "profile", "companion", "clocks_mhz"):
        assert key in e, key
# drawrec_net = DrawRec - GXDrawDone wait: (10 + k) - (4 + k) = 6 in every zone.
assert [z["derived"]["drawrec_net_ms_per_frame"] for z in zones] == [6.0, 6.0, 6.0]
assert [z["derived"]["painter_net_ms_per_frame"] for z in zones] == [15.0, 16.0, 17.0]
light = zones[2]["interpolation"]
assert light["stalled_at_arrival"] is True and light["recovered_in_zone"] is True
assert light["recovery_seconds_from_arrival"] == 4.25
assert zones[0]["interpolation"]["recovery_seconds_from_arrival"] is None
s = done["summary"]
assert s["heavy_zone_stalled"] is True and s["light_zone_recovered"] is True and s["light_zone_recovery_seconds"] == 4.25
assert s["consistent_environment"] is True and len(s["per_zone"]) in (3, 4)
r = done["restore"]
assert r["returned_to_original_place"] and r["in_memory_save_state_restored"] and r["saves_reenabled"]
assert r["config_json_written"] is False and r["config_saves_during_run"] == 0
assert r["achievements_suspended_during_run"] is True
c = done["configuration"]
for key in ("environment", "interpolation", "uniform_reuse_mode", "pipeline_memo", "graphics_api"):
    assert key in c, key
assert done["mods"] == [{"id": "rollandsprint", "version": "1.0.0"}]
assert done["run"]["governor_reset_between_zones"] is False

cancelled = load("perf_tour_test.cancelled.json")
assert cancelled["run"]["status"] == "cancelled" and cancelled["run"]["detail"] == "cancelled with Start"
assert [z["status"] for z in cancelled["zones"]] == ["completed", "cancelled", "pending", "pending"]
assert cancelled["zones"][1]["measurement_complete"] is False
assert cancelled["zones_summary"]["not_reached"] == 2  # V1-rc7: Hyrule Field, the fourth, is not reached either

failed = load("perf_tour_test.failed_zone.json")
assert [z["status"] for z in failed["zones"]][:3] == ["failed", "completed", "completed"]
assert failed["zones"][0]["measured"] is False
assert failed["zones"][0]["detail"] == "the zone did not finish loading"
print("sample tour reports parse and check out")
PYEOF
