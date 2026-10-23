#include "dusk/perf_report.hpp"

#include <algorithm>
#include <cstdio>

namespace dusk::perf {
namespace {

void write_dist(benchmark::JsonWriter& j, const char* key, const std::vector<float>& v) {
    j.begin_object(key);
    if (v.empty()) {
        j.null("p50");
        j.null("p99");
    } else {
        j.num("p50", percentile(v, 50), 2);
        j.num("p99", percentile(v, 99), 2);
    }
    j.end_object();
}

}  // namespace

const char* verdict(const WindowStats& w) {
    if (w.frames == 0 || w.missedPct() < 5.0) {
        return "met";
    }
    for (int b = 1; b < kBoundCount; ++b) {
        if (w.byBound[b] * 10 >= w.missed * 6) {
            return bound_name(static_cast<Bound>(b));
        }
    }
    return "mixed";
}

void write_telemetry(benchmark::JsonWriter& j, const std::vector<FrameSample>& frames, float budgetMs,
                     const GpuInfo& gpu) {
    const WindowStats w = summarize(frames, budgetMs);
    std::vector<float> gpuMs, mainMs, fifoMs, renderMs;
    for (const auto& f : frames) {
        if (f.gpuMs >= 0.f) {
            gpuMs.push_back(f.gpuMs);
        }
        mainMs.push_back(f.mainWorkMs);
        fifoMs.push_back(f.fifoBusyMs);
        renderMs.push_back(f.renderWorkMs);
    }

    j.begin_object("gpu");
    j.boolean("available", !gpuMs.empty());
    j.boolean("timestamp_query_supported", gpu.supported);
    j.boolean("timing_on", gpu.enabled);
    j.boolean("tick_period_calibrated", gpu.calibrated);
    j.num("tick_period_ns", gpu.periodNs, 4);
    j.integer("frames_with_gpu_time", static_cast<std::int64_t>(gpuMs.size()));
    j.integer("frames_not_measured_since_launch", static_cast<std::int64_t>(gpu.skipped));
    if (gpuMs.empty()) {
        j.str("reason", !gpu.supported ? "the device has no TimestampQuery (or perf-lab.ini gpu_timing=off)"
                                       : "no GPU figures came back during the window");
    } else {
        double sum = 0.0;
        float mx = 0.f;
        for (float g : gpuMs) {
            sum += g;
            mx = std::max(mx, g);
        }
        j.num("frame_ms_mean", sum / static_cast<double>(gpuMs.size()), 3);
        j.num("frame_ms_p50", percentile(gpuMs, 50), 3);
        j.num("frame_ms_p90", percentile(gpuMs, 90), 3);
        j.num("frame_ms_p99", percentile(gpuMs, 99), 3);
        j.num("frame_ms_max", mx, 3);
        j.num("utilisation_pct", 100.0 * w.gpuUtil, 1);
        j.str("note", "whole frame on the GPU, from the start to the end of the frame's commands");
    }
    j.end_object();

    j.begin_object("bound");
    j.num("budget_ms", budgetMs, 2);
    j.integer("frames", w.frames);
    j.integer("missed", w.missed);
    j.num("pct_missed", w.missedPct(), 2);
    for (int b = 0; b < kBoundCount; ++b) {
        j.integer(bound_name(static_cast<Bound>(b)), w.byBound[b]);
    }
    j.num("gpu_share_of_missed", w.gpuMissShare(), 3);
    j.num("cpu_share_of_missed", w.cpuMissShare(), 3);
    j.str("verdict", verdict(w));
    j.str("rule", "missed = interval > 1.1 x budget; gpu = GPU >= 85 % of the budget and above both CPU sides; "
                  "cpu_main = game thread busy >= 85 %; cpu_render = FIFO worker busy >= 85 %; else unknown");
    j.end_object();

    j.begin_object("cpu");
    write_dist(j, "main_work_ms", mainMs);
    write_dist(j, "fifo_busy_ms", fifoMs);
    write_dist(j, "render_worker_ms", renderMs);
    j.str("note", "main_work = interval minus the game thread's waits (tick, limiter, frame slot, staging, "
                  "FIFO drains); render_worker includes present, which can wait for the display");
    j.end_object();
}

std::string csv_header() {
    return "zone,frame,interval_ms,main_work_ms,fifo_busy_ms,render_work_ms,gpu_ms,queued_pipelines,bound\n";
}

void append_csv(std::string& out, const std::vector<FrameSample>& frames, float budgetMs, int zone) {
    char line[192];
    std::size_t i = 0;
    for (const auto& f : frames) {
        char zoneText[16] = "";
        if (zone >= 0) {
            std::snprintf(zoneText, sizeof zoneText, "%d", zone);
        }
        char gpuText[24] = "";
        if (f.gpuMs >= 0.f) {
            std::snprintf(gpuText, sizeof gpuText, "%.3f", f.gpuMs);
        }
        std::snprintf(line, sizeof line, "%s,%zu,%.3f,%.3f,%.3f,%.3f,%s,%u,%s\n", zoneText, i++, f.intervalMs,
            f.mainWorkMs, f.fifoBusyMs, f.renderWorkMs, gpuText, f.queuedPipelines,
            bound_name(classify(f, budgetMs)));
        out += line;
    }
}

std::string csv_name_for(const std::string& jsonName) {
    constexpr std::string_view ext = ".json";
    if (jsonName.size() > ext.size() && jsonName.compare(jsonName.size() - ext.size(), ext.size(), ext) == 0) {
        return jsonName.substr(0, jsonName.size() - ext.size()) + ".csv";
    }
    return jsonName + ".csv";
}

}  // namespace dusk::perf
