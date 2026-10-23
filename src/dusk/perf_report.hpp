#pragma once
// aleks2 (V1-c1): the frame-telemetry sections every benchmark report shares (the normal run,
// each profile of the matrix and each zone of the Performance Tour): GPU time from timestamp
// queries, who explains the missed frames, the CPU sides, and the per-frame CSV written next to
// the JSON. Game-free like benchmark_report.hpp (host-tested in test-perf-core.sh).
#include "dusk/benchmark_report.hpp"
#include "dusk/perf_core.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace dusk::perf {

struct GpuInfo {
    bool supported = false;   // the device has TimestampQuery
    bool enabled = false;     // timestamps were being written
    bool calibrated = false;  // the tick period was measured against the CPU clock
    double periodNs = 1.0;
    std::uint64_t skipped = 0;
};

// Writes "gpu", "bound" and "cpu" objects into the object being written.
void write_telemetry(benchmark::JsonWriter& j, const std::vector<FrameSample>& frames, float budgetMs,
                     const GpuInfo& gpu);

// "met" (< 5 % missed), "gpu", "cpu_main", "cpu_render", "unknown" (>= 60 % of the missed
// frames) or "mixed".
const char* verdict(const WindowStats& w);

// One line per frame: zone,frame,interval_ms,main_work_ms,fifo_busy_ms,render_work_ms,gpu_ms,
// queued_pipelines,bound. gpu_ms is empty when there was no figure. `zone` -1 leaves it empty.
std::string csv_header();
void append_csv(std::string& out, const std::vector<FrameSample>& frames, float budgetMs, int zone = -1);

// bench-...json -> bench-...csv
std::string csv_name_for(const std::string& jsonName);

}  // namespace dusk::perf
