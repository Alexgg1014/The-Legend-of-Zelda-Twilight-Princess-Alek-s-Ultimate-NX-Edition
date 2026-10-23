#include "dusk/drs_stress.hpp"

#include "dusk/benchmark.hpp"
#include "dusk/benchmark_report.hpp"
#include "dusk/dynres.hpp"
#include "dusk/frame_telemetry.hpp"
#include "dusk/logging.h"
#include "dusk/main.h"
#include "dusk/perf_core.hpp"
#include "dusk/session_log.hpp"
#include "dusk/shader_warmup_runtime.hpp"
#include "dusk/switch_display.hpp"
#include "dusk/ui/ui.hpp"

#include <aurora/tuning.h>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <vector>

#ifdef __SWITCH__
#include <unistd.h>
#endif

namespace dusk::drs_stress {
namespace {
inline constexpr borealis::Log Log{"dusk::drs_stress"};
using clock = std::chrono::steady_clock;

struct Run {
    bool active = false;
    bool cancel = false;
    std::string cancelReason;
    std::vector<int> rows;
    std::size_t next = 0;
    int ceilingRows = 0;
    int modeAtStart = 0;
    clock::time_point started;
    clock::time_point stepAt;
    std::string stamp;
    std::int64_t startedUnix = 0;
    std::string csvName;
    std::FILE* csv = nullptr;

    // The interval since the last change.
    float worstMs = 0.f;
    double sumMs = 0.0;
    int frames = 0;
    double gpuSum = 0.0;
    int gpuFrames = 0;
    int requestedRows = 0;

    frame_telemetry::Memory memStart;
    frame_telemetry::Memory memLast;
    std::uint64_t memMin = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t memMax = 0;
    AuroraRenderTargetStats rtStart{};
    float worstOverall = 0.f;
    int transitionsDone = 0;
    // Process memory used at each pass through the ceiling, to see a trend rather than noise.
    std::vector<std::uint64_t> memAtCeiling;
};

Run g_run;

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int mode_value() {
#ifdef __SWITCH__
    return static_cast<int>(display::mode());
#else
    return 0;
#endif
}

void flush(std::FILE* f) {
    if (f == nullptr) {
        return;
    }
    std::fflush(f);
#ifdef __SWITCH__
    fsync(fileno(f));
#endif
}

void write_row(const char* event) {
    auto& r = g_run;
    if (r.csv == nullptr) {
        return;
    }
    const auto mem = frame_telemetry::memory();
    AuroraRenderTargetStats rt{};
    aurora_render_target_stats(&rt);
    r.memLast = mem;
    r.memMin = std::min(r.memMin, mem.used);
    r.memMax = std::max(r.memMax, mem.used);
    const double t = std::chrono::duration<double>(clock::now() - r.started).count();
    std::fprintf(r.csv, "%d,%.2f,%s,%d,%u,%u,%.2f,%.2f,%d,%.2f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u\n",
        r.transitionsDone, t, event, r.requestedRows, rt.width, rt.height, r.worstMs,
        r.frames > 0 ? r.sumMs / r.frames : 0.0, r.frames, r.gpuFrames > 0 ? r.gpuSum / r.gpuFrames : -1.0,
        static_cast<unsigned long long>(mem.used / 1024), static_cast<unsigned long long>(mem.total / 1024),
        static_cast<unsigned long long>(mem.heapInUse / 1024), static_cast<unsigned long long>(rt.allocations),
        static_cast<unsigned long long>(rt.pool_hits), static_cast<unsigned long long>(rt.evictions),
        static_cast<unsigned long long>(rt.live_bytes_estimate / 1024), rt.pool_sets);
    flush(r.csv);
}

void reset_interval() {
    auto& r = g_run;
    r.worstMs = 0.f;
    r.sumMs = 0.0;
    r.frames = 0;
    r.gpuSum = 0.0;
    r.gpuFrames = 0;
}

void finish(const char* status) {
    auto& r = g_run;
    write_row(status);
    if (r.csv != nullptr) {
        std::fclose(r.csv);
        r.csv = nullptr;
    }
    dynres::stress_end();
    frame_telemetry::want_gpu_timing(frame_telemetry::kStress, false);

    AuroraRenderTargetStats rt{};
    aurora_render_target_stats(&rt);
    const auto memEnd = frame_telemetry::memory();
    // Growth from the first to the last pass through the ceiling: same size, same scene,
    // so a steady climb is what a leak looks like.
    std::int64_t ceilingGrowthKb = 0;
    if (r.memAtCeiling.size() >= 2) {
        ceilingGrowthKb = (static_cast<std::int64_t>(r.memAtCeiling.back()) -
                           static_cast<std::int64_t>(r.memAtCeiling.front())) / 1024;
    }
    benchmark::JsonWriter j;
    j.begin_object();
    j.str("schema", "dusklight-aleks-drs-stress");
    j.integer("schema_version", 1);
    j.str("build", benchmark::build_id());
    j.str("status", status);
    if (!r.cancelReason.empty()) {
        j.str("cancel_reason", r.cancelReason);
    }
    j.str("started_utc", r.stamp);
    j.str("csv", r.csvName);
    j.integer("transitions_planned", static_cast<std::int64_t>(r.rows.size()));
    j.integer("transitions_done", r.transitionsDone);
    j.integer("interval_ms", frame_telemetry::lab().stressIntervalMs);
    j.str("pattern", frame_telemetry::lab().stressLadder ? "ladder" : "steps");
    j.integer("ceiling_rows", r.ceilingRows);
    j.num("seconds", std::chrono::duration<double>(clock::now() - r.started).count(), 1);
    j.num("worst_frame_ms", r.worstOverall, 1);
    j.begin_object("memory_kb");
    j.integer("used_start", static_cast<std::int64_t>(r.memStart.used / 1024));
    j.integer("used_end", static_cast<std::int64_t>(memEnd.used / 1024));
    j.integer("used_min", static_cast<std::int64_t>(r.memMin == std::numeric_limits<std::uint64_t>::max() ? 0 : r.memMin / 1024));
    j.integer("used_max", static_cast<std::int64_t>(r.memMax / 1024));
    j.integer("total", static_cast<std::int64_t>(memEnd.total / 1024));
    j.integer("heap_start", static_cast<std::int64_t>(r.memStart.heapInUse / 1024));
    j.integer("heap_end", static_cast<std::int64_t>(memEnd.heapInUse / 1024));
    j.integer("ceiling_passes", static_cast<std::int64_t>(r.memAtCeiling.size()));
    j.integer("growth_between_first_and_last_ceiling_pass", ceilingGrowthKb);
    j.end_object();
    j.begin_object("render_targets");
    j.integer("allocations", static_cast<std::int64_t>(rt.allocations - r.rtStart.allocations));
    j.integer("pool_hits", static_cast<std::int64_t>(rt.pool_hits - r.rtStart.pool_hits));
    j.integer("evictions", static_cast<std::int64_t>(rt.evictions - r.rtStart.evictions));
    j.integer("pool_max", frame_telemetry::lab().rtPool);
    j.integer("live_kb_estimate_end", static_cast<std::int64_t>(rt.live_bytes_estimate / 1024));
    j.end_object();
    j.str("note", "A WebGPU error or a lost device ends the program (Aurora treats both as fatal): if the run "
                  "crashed, the CSV has every change up to the crash and the crash report says why.");
    j.end_object();
    const std::string json = j.take();
    const std::string name = fmt::format("drs-stress-{}-{}.json", benchmark::utc_stamp_for_file(r.startedUnix),
        benchmark::build_id());
    const auto written = benchmark::write_report_atomic(ConfigPath / "benchmarks", name, json);
    Log.info("DRS stress {}: {} of {} changes, memory {} -> {} KB (min {} max {}), ceiling-pass growth {} KB, "
             "render-target sets created {}, pool hits {}, worst frame {:.0f} ms; {}",
        status, r.transitionsDone, r.rows.size(), r.memStart.used / 1024, memEnd.used / 1024,
        r.memMin == std::numeric_limits<std::uint64_t>::max() ? 0 : r.memMin / 1024, r.memMax / 1024,
        ceilingGrowthKb, rt.allocations - r.rtStart.allocations, rt.pool_hits - r.rtStart.pool_hits,
        r.worstOverall, written.ok ? written.path.string() : "REPORT NOT WRITTEN: " + written.error);
    ui::push_toast({
        .title = fmt::format("DRS stress {}", status),
        .content = fmt::format("{} of {} changes. Memory {} -> {} MB. Worst frame {:.0f} ms.\n{}", r.transitionsDone,
            r.rows.size(), r.memStart.used >> 20, memEnd.used >> 20, r.worstOverall,
            written.ok ? r.csvName : std::string{"Report not written"}),
        .duration = std::chrono::seconds(8),
    });
    g_run = Run{};
}
}  // namespace

bool can_start(std::string& whyNot) {
    if (g_run.active) {
        whyNot = "The DRS stress test is already running.";
        return false;
    }
    if (benchmark::active()) {
        whyNot = "A benchmark is running.";
        return false;
    }
    if (shader_warmup::busy(whyNot)) {
        return false;
    }
    // rt_pool=0 is allowed on purpose: it measures the old recreate-every-time path.
    return true;
}

bool start(std::string& error) {
    if (!can_start(error)) {
        return false;
    }
    auto& r = g_run;
    r = Run{};
    r.ceilingRows = dynres::ceilingRows();
    r.rows = perf::stress_rows(r.ceilingRows, frame_telemetry::lab(), frame_telemetry::lab().stressTransitions);
    if (r.rows.empty()) {
        error = fmt::format("No resolution steps to walk below {} rows with these perf-lab.ini options.", r.ceilingRows);
        return false;
    }
    r.startedUnix = unix_now();
    r.stamp = benchmark::utc_stamp_iso(r.startedUnix);
    r.csvName = fmt::format("drs-stress-{}-{}.csv", benchmark::utc_stamp_for_file(r.startedUnix), benchmark::build_id());
    std::error_code ec;
    const auto dir = ConfigPath / "benchmarks";
    std::filesystem::create_directories(dir, ec);
    r.csv = std::fopen((dir / r.csvName).string().c_str(), "w");
    if (r.csv == nullptr) {
        error = fmt::format("Could not create {} on the card.", (dir / r.csvName).string());
        return false;
    }
    std::fprintf(r.csv,
        "transition,t_s,event,rows_requested,fb_width,fb_height,worst_frame_ms,mean_frame_ms,frames,gpu_ms,"
        "mem_used_kb,mem_total_kb,heap_kb,rt_sets_created,rt_pool_hits,rt_evictions,rt_live_kb,rt_pool_sets\n");
    r.active = true;
    r.modeAtStart = mode_value();
    r.started = r.stepAt = clock::now();
    r.memStart = frame_telemetry::memory();
    aurora_render_target_stats(&r.rtStart);
    r.requestedRows = r.ceilingRows;
    r.memAtCeiling.push_back(r.memStart.used);
    frame_telemetry::want_gpu_timing(frame_telemetry::kStress, true);
    dynres::stress_begin();
    write_row("start");
    reset_interval();
    Log.info("DRS stress: {} changes every {} ms from {} rows ({}), render-target pool {}", r.rows.size(),
        frame_telemetry::lab().stressIntervalMs, r.ceilingRows, frame_telemetry::lab().stressLadder ? "ladder" : "steps",
        frame_telemetry::lab().rtPool);
    ui::push_toast({
        .title = "DRS stress: running",
        .content = fmt::format("{} resolution changes, about {} s. Stand still or walk around; open Diagnostics "
                               "and choose the same entry again to stop.",
            r.rows.size(), r.rows.size() * frame_telemetry::lab().stressIntervalMs / 1000),
        .duration = std::chrono::seconds(6),
    });
    return true;
}

bool active() {
    return g_run.active;
}

void request_cancel(const char* reason) {
    if (g_run.active) {
        g_run.cancel = true;
        g_run.cancelReason = reason != nullptr ? reason : "cancelled";
    }
}

void tick() {
    auto& r = g_run;
    if (!r.active) {
        return;
    }
    const auto& f = frame_telemetry::last();
    r.worstMs = std::max(r.worstMs, f.intervalMs);
    r.worstOverall = std::max(r.worstOverall, f.intervalMs);
    r.sumMs += f.intervalMs;
    ++r.frames;
    if (f.gpuMs >= 0.f) {
        r.gpuSum += f.gpuMs;
        ++r.gpuFrames;
    }
    if (r.cancel) {
        finish("cancelled");
        return;
    }
    if (mode_value() != r.modeAtStart) {
        r.cancelReason = "the console mode changed (dock, handheld or Flip Grip)";
        finish("cancelled");
        return;
    }
    const auto interval = std::chrono::milliseconds(frame_telemetry::lab().stressIntervalMs);
    if (clock::now() - r.stepAt < interval) {
        return;
    }
    // The row for the interval that just ended, then the next change.
    if (r.next > 0) {
        write_row("step");
        if (r.requestedRows == r.ceilingRows) {
            r.memAtCeiling.push_back(r.memLast.used);
        }
    }
    if (r.next >= r.rows.size()) {
        finish("completed");
        return;
    }
    r.requestedRows = r.rows[r.next++];
    dynres::stress_apply_rows(r.requestedRows == r.ceilingRows ? 0 : r.requestedRows);
    ++r.transitionsDone;
    r.stepAt = clock::now();
    reset_interval();
}

std::string status_text() {
    if (!g_run.active) {
        return {};
    }
    return fmt::format("DRS stress {}/{} - {} rows", g_run.transitionsDone, g_run.rows.size(), g_run.requestedRows);
}

}  // namespace dusk::drs_stress
