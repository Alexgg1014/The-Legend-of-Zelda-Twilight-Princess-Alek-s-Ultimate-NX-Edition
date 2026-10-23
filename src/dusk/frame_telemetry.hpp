#pragma once
// aleks2 (V1-c1): what each presented frame cost and who explains a missed one -- the game
// thread, the FIFO worker (render-side CPU) or the GPU (Aurora's timestamp queries, see
// aurora/tuning.h). Read once per frame on the main thread from counters Aurora and game_clock
// already keep. The benchmark captures it, DRS Auto decides with it, the stress mode logs it.
// The rules themselves are in perf_core.hpp (host-tested).
#include "dusk/perf_core.hpp"

#include <cstdint>
#include <vector>

namespace dusk::frame_telemetry {

// Before aurora_initialize: reads <data>/perf-lab.ini. gpu_timing=off keeps the timestamp
// feature from being requested at all (AURORA_GPU_TIMING=0). Without the file: defaults.
void load_lab_options();
const perf::LabOptions& lab();
// V1-rc9 (the tour matrix): sets one perf-lab key while the game runs; false, with why, for a key that cannot
// change then (logic_core, gpu_timing...) or a value it does not take. restore_lab puts a saved set back.
bool set_lab_value(std::string_view key, std::string_view value, std::string& why);
void restore_lab(const perf::LabOptions& saved);
std::uint64_t ticks();  // V1-rc10: main-loop frames so far

// Once per frame, main thread, after this frame's Aurora stats are read.
void tick();

// The frame tick() just measured (zeros before the second frame).
const perf::FrameSample& last();
perf::Bound last_bound();

// 1000 / perf-lab drs_target_fps (30 by default): what "missed" means.
float budget_ms();

// Who needs GPU figures; timestamps are written only while at least one does (or always with
// gpu_timing=always). A no-op with gpu_timing=off.
enum Consumer : unsigned { kBenchmark = 1u, kDrs = 2u, kStress = 4u };
void want_gpu_timing(Consumer who, bool on);

struct GpuStatus {
    bool supported = false;  // the device has TimestampQuery
    bool enabled = false;    // being written now
    bool calibrated = false;
    double periodNs = 1.0;
    std::uint64_t measured = 0;  // frames read back since launch
    std::uint64_t skipped = 0;
};
GpuStatus gpu_status();

// Benchmark capture: every frame between begin and end.
void capture_begin();
void capture_end();
bool capture_active();
const std::vector<perf::FrameSample>& capture();

// Process memory (svcGetInfo; GPU allocations on this console come out of it too), in bytes.
// 0 off the console.
struct Memory {
    std::uint64_t used = 0;
    std::uint64_t total = 0;
    std::uint64_t heapInUse = 0;  // malloc arena in use (mallinfo)
};
Memory memory();

}  // namespace dusk::frame_telemetry
