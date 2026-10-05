#include "dusk/perf_tour.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace dusk::perf_tour {
namespace {

// Heavy -> medium -> light. Evidence for each entry: docs/switch-h35-fifo-plan.md
// ("Performance Tour"). Ordon is entered at point 1, from Outside Link's House: point 0 is
// the Ordon Ranch pathway entrance, which on a new file plays the goat scene and leaves
// for F_SP00 (H34-c2 on hardware).
constexpr std::array kZones{
    Zone{"F_SP122", 8, 0, -1, "Outside Castle Town - West", "heavy"},
    Zone{"F_SP103", 0, 1, -1, "Ordon Village", "medium"},
    Zone{"R_SP01", 4, 0, -1, "Link's House", "light"},
};

const char* status_name(ZoneStatus status) {
    return shader_prep::zone_status_name(status);
}

void write_environment(benchmark::JsonWriter& j, const char* key, const Environment& e) {
    j.begin_object(key);
    j.str("mode", e.mode);
    j.boolean("docked", e.docked);
    j.boolean("flip_grip", e.flipGrip);
    j.boolean("mode_changed_during_window", e.modeChangedDuringWindow);
    j.begin_object("resolution");
    j.integer("width", e.renderWidth);
    j.integer("height", e.renderHeight);
    j.integer("min_width", e.minWidth);
    j.integer("min_height", e.minHeight);
    j.integer("max_width", e.maxWidth);
    j.integer("max_height", e.maxHeight);
    j.end_object();
    j.str("profile", e.profile);
    j.begin_object("companion");
    j.boolean("enabled", e.companionEnabled);
    j.str("layout", e.companionLayout);
    j.end_object();
    j.begin_object("clocks_mhz");
    j.boolean("available", e.clocksAvailable);
    j.integer("cpu", e.cpuMhz);
    j.integer("gpu", e.gpuMhz);
    j.integer("emc", e.emcMhz);
    j.end_object();
    j.end_object();
}

bool same_environment(const Environment& a, const Environment& b) {
    return a.mode == b.mode && a.docked == b.docked && a.flipGrip == b.flipGrip && a.profile == b.profile &&
           a.companionEnabled == b.companionEnabled && a.companionLayout == b.companionLayout &&
           a.renderWidth == b.renderWidth && a.renderHeight == b.renderHeight;
}

void write_performance(benchmark::JsonWriter& j, const benchmark::FrameSummary& f) {
    j.begin_object("performance");
    j.integer("frames", static_cast<std::int64_t>(f.frames));
    j.num("covered_seconds", f.coveredSeconds, 3);
    j.num("avg_fps", f.avgFps, 2);
    j.num("low_1pct_fps", f.low1PctFps, 2);
    j.begin_object("frametime_ms");
    j.num("mean", f.meanMs, 3);
    j.num("median", f.medianMs, 3);
    j.num("p95", f.p95Ms, 3);
    j.num("p99", f.p99Ms, 3);
    j.num("max", f.maxMs, 3);
    j.end_object();
    j.integer("over_33_3_ms", static_cast<std::int64_t>(f.over33));
    j.num("pct_over_33_3_ms", f.pctOver33, 2);
    j.integer("over_50_ms", static_cast<std::int64_t>(f.over50));
    j.num("pct_over_50_ms", f.pctOver50, 2);
    j.num("judder_pct", f.judderPct, 2);
    j.end_object();
}

void write_interpolation(benchmark::JsonWriter& j, const Interpolation& in) {
    const auto& w = in.window;
    j.begin_object("interpolation");
    j.str("requested_mode", in.requestedMode);
    j.str("quality", in.quality);
    j.boolean("predictive", in.predictive);
    fifo_report::write_interp_frames(j, in.frames, in.windowSeconds);
    j.integer("stall_enters", w.enters);
    j.integer("stall_exits", w.exits);
    j.integer("resume_attempts", w.attempts);
    j.integer("resume_failures", w.failures);
    j.integer("resume_blocked_by_cooldown_frames", w.cooldownBlockedFrames);
    j.num("stalled_seconds", w.stalledSeconds, 2);
    j.num("stalled_pct_of_window", in.windowSeconds > 0.0 ? 100.0 * w.stalledSeconds / in.windowSeconds : 0.0, 1);
    j.boolean("stalled_at_arrival", in.recovery.stalledAtArrival());
    j.boolean("stalled_at_measure_start", in.stalledAtMeasureStart);
    j.boolean("stalled_at_measure_end", in.stalledAtMeasureEnd);
    j.boolean("stalled_in_zone", in.recovery.stalledInZone());
    j.boolean("recovered_in_zone", in.recovery.recovered());
    if (in.recovery.recovered()) {
        j.num("recovery_seconds_from_arrival", in.recovery.recoverySeconds(), 2);
    } else {
        j.null("recovery_seconds_from_arrival");
    }
    j.str("note", "the governor is never reset between zones: stalled_at_arrival is what the previous zone left; "
                  "counts and seconds are over the measurement window, the stalled_* flags and the recovery "
                  "from the arrival in the zone");
    j.end_object();
}

void write_loading(benchmark::JsonWriter& j, const Loading& l) {
    j.begin_object("loading");
    j.num("load_seconds", l.loadSeconds, 2);
    j.integer("pipelines_queued_at_arrival", l.queuedAtArrival);
    j.integer("pipelines_queued_max", l.queuedMax);
    j.integer("pipelines_queued_max_in_window", l.queuedMaxInWindow);
    j.integer("pipelines_created_in_zone", l.createdInZone);
    j.integer("pipelines_created_in_window", l.createdInWindow);
    j.boolean("pipeline_wait_timed_out", l.pipelineWaitTimedOut);
    j.num("pipeline_wait_seconds", l.pipelineWaitSeconds, 2);
    j.integer("pipelines_queued_at_wait_end", l.queuedAtWaitEnd);
    j.str("cache_state", cache_state(l));
    j.end_object();
}

void write_zone(benchmark::JsonWriter& j, std::size_t index, const Zone& zone, const ZoneResult& z) {
    j.begin_object();
    j.integer("index", static_cast<std::int64_t>(index));
    j.str("stage", zone.stage);
    j.integer("room", zone.room);
    j.integer("point", zone.point);
    j.integer("layer", zone.layer);
    j.str("name", zone.name);
    j.str("class", zone.category);
    j.str("status", status_name(z.status));
    j.str("detail", z.detail);
    j.str("arrived_stage", z.arrivedStage);
    j.integer("arrived_room", z.arrivedRoom);
    j.boolean("measured", z.measured);
    j.boolean("measurement_complete", z.measurementComplete);
    j.num("measure_seconds_wall", z.measureSecondsWall, 3);
    j.integer("frames_with_event_running", static_cast<std::int64_t>(z.framesWithEvent));
    j.integer("frames_with_ui_visible", static_cast<std::int64_t>(z.framesWithUiVisible));
    write_performance(j, z.frames);
    j.begin_object("cpu_zones_ms");
    j.str("note", "per-frame average and worst frame; Execute holds Painter, Actors and DrawRec as siblings");
    for (const auto& c : z.cpuZones) {
        j.begin_object(c.name.c_str());
        j.num("avg_per_frame", c.avgMs, 3);
        j.num("max", c.maxMs, 3);
        j.num("calls_per_frame", c.callsPerFrame, 2);
        j.end_object();
    }
    j.end_object();
    fifo_report::write_derived(j, z.zoneTimes, z.fifo);
    fifo_report::write_fifo(j, z.fifo);
    fifo_report::write_uniforms(j, z.fifo, z.uniformMismatchSection.c_str());
    fifo_report::write_frame_waits(j, z.fifo);
    write_interpolation(j, z.interp);
    write_loading(j, z.loading);
    write_environment(j, "environment", z.env);
    j.end_object();
}

}  // namespace

std::span<const Zone> zones() {
    return kZones;
}

PipelineWaitEnd decide_pipeline_wait(const PipelineWaitProgress& p, const Timing& t) {
    if (p.queueEmptyFor >= t.pipelineQuietSeconds) {
        return PipelineWaitEnd::Drained;
    }
    if (p.waited >= t.pipelineTimeoutSeconds) {
        return PipelineWaitEnd::TimedOut;
    }
    return PipelineWaitEnd::None;
}

double max_seconds_per_zone_after_load(const Timing& t) {
    return t.pipelineTimeoutSeconds + t.warmupSeconds + t.measureSeconds;
}

double overall_progress(std::size_t zoneIndex, std::size_t zoneCount, Step step, double stepSeconds,
    const Timing& t) {
    if (zoneCount == 0) {
        return 1.0;
    }
    double zoneFraction = 0.0;
    const double timed = std::max(t.warmupSeconds + t.measureSeconds, 1e-9);
    switch (step) {
    case Step::Loading:
    case Step::PipelineWait:
        zoneFraction = 0.0;
        break;
    case Step::Warmup:
        zoneFraction = 0.1 + 0.9 * std::clamp(stepSeconds, 0.0, t.warmupSeconds) / timed;
        break;
    case Step::Measuring:
        zoneFraction = 0.1 + 0.9 * (t.warmupSeconds + std::clamp(stepSeconds, 0.0, t.measureSeconds)) / timed;
        break;
    }
    return shader_prep::overall_progress(zoneIndex, zoneCount, zoneFraction);
}

void RecoveryTracker::arrive(double t, bool stalled) {
    mArrived = true;
    mArrivedAt = t;
    mStalledAtArrival = stalled;
    mStalledNow = stalled;
    mStalledInZone = stalled;
    mRecoverySeconds = -1.0;
}

void RecoveryTracker::sample(double t, bool stalled) {
    if (!mArrived) {
        return;
    }
    if (stalled) {
        mStalledInZone = true;
    } else if (mStalledNow && mRecoverySeconds < 0.0) {
        // The first time it comes back in this zone; later stalls and recoveries are in the
        // governor's counts.
        mRecoverySeconds = std::max(0.0, t - mArrivedAt);
    }
    mStalledNow = stalled;
}

const char* cache_state(const Loading& l) {
    if (l.queuedMaxInWindow > 0) {
        return "compiling";
    }
    return l.createdInZone > 0 ? "cold" : "warm";
}

std::string report_file_name(std::string_view stamp, std::string_view buildId) {
    std::string name = benchmark::report_file_name(stamp, buildId);
    constexpr std::string_view benchPrefix = "bench-";
    if (name.starts_with(benchPrefix)) {
        name.replace(0, benchPrefix.size(), "tour-");
    }
    return name;
}

std::string build_report(const TourReport& r, std::span<const Zone> plan) {
    benchmark::JsonWriter j;
    j.begin_object();
    j.str("schema", kSchema);
    j.integer("schema_version", kReportSchemaVersion);
    j.str("note", "three zones measured one after the other with the settings the console already had: heavy, "
                  "medium, light. Every figure is per zone; nothing is averaged across zones");

    j.begin_object("build");
    j.str("id", r.buildId);
    j.str("tag", r.buildTag);
    j.str("commit", r.buildCommit);
    j.boolean("tree_dirty", r.treeDirty);
    j.str("built_at", r.builtAt);
    j.end_object();

    j.begin_object("run");
    j.str("status", shader_prep::outcome_name(r.outcome));
    j.str("detail", r.detail);
    j.str("started_utc", r.startedUtc);
    j.integer("started_unix", r.startedUnix);
    j.str("file", r.fileName);
    j.num("total_seconds", r.totalSeconds, 1);
    j.str("zone_order", "heavy, medium, light");
    j.boolean("governor_reset_between_zones", false);
    j.end_object();

    j.begin_object("original_state");
    j.str("stage", r.originalStage);
    j.integer("room", r.originalRoom);
    j.integer("point", r.originalPoint);
    j.integer("layer", r.originalLayer);
    j.end_object();

    j.begin_object("configuration");
    j.str("note", "what the console was set to when the tour began; the tour changes no setting");
    write_environment(j, "environment", r.envAtStart);
    j.begin_object("interpolation");
    j.str("mode", r.interpRequestedMode);
    j.str("quality", r.interpQuality);
    j.boolean("predictive", r.predictiveInterpolation);
    j.end_object();
    j.str("uniform_reuse_mode", r.uniformReuseModeAtStart);
    j.boolean("pipeline_memo", r.pipelineMemo);
    j.integer("max_frame_rate", r.maxFrameRate);
    j.boolean("vsync", r.vsync);
    j.str("graphics_api", r.graphicsApi);
    j.str("adapter", r.adapter);
    j.str("driver", r.driver);
    j.integer("pipelines_known_at_start", r.pipelinesKnownAtStart);
    j.integer("pipelines_queued_at_start", r.queuedAtStart);
    j.end_object();

    j.begin_array("mods");
    for (const auto& mod : r.mods) {
        j.begin_object();
        j.str("id", mod.id);
        j.str("version", mod.version);
        j.end_object();
    }
    j.end_array();

    std::size_t completed = 0, skipped = 0, failed = 0, cancelled = 0, measured = 0;
    bool consistent = true;
    std::uint64_t mismatches = 0;
    for (const auto& zone : r.zones) {
        completed += zone.status == ZoneStatus::Completed;
        skipped += zone.status == ZoneStatus::Skipped;
        failed += zone.status == ZoneStatus::Failed;
        cancelled += zone.status == ZoneStatus::Cancelled;
        if (zone.measured) {
            ++measured;
            consistent = consistent && same_environment(zone.env, r.envAtStart) && !zone.env.modeChangedDuringWindow &&
                         !zone.fifo.modeChangedDuringWindow &&
                         r.uniformReuseModeAtStart == fifo_report::mode_name(zone.fifo.modeAtStart);
            mismatches += zone.fifo.verificationMismatches;
        }
    }
    j.begin_object("zones_summary");
    j.integer("planned", static_cast<std::int64_t>(plan.size()));
    j.integer("completed", static_cast<std::int64_t>(completed));
    j.integer("skipped", static_cast<std::int64_t>(skipped));
    j.integer("failed", static_cast<std::int64_t>(failed));
    j.integer("cancelled", static_cast<std::int64_t>(cancelled));
    j.integer("not_reached",
        static_cast<std::int64_t>(plan.size() - std::min(plan.size(), completed + skipped + failed + cancelled)));
    j.integer("measured", static_cast<std::int64_t>(measured));
    j.end_object();

    j.begin_array("zones");
    for (std::size_t i = 0; i < plan.size(); ++i) {
        const ZoneResult empty{};
        write_zone(j, i, plan[i], i < r.zones.size() ? r.zones[i] : empty);
    }
    j.end_array();

    // The question the order exists to answer, and a compact line per zone.
    j.begin_object("summary");
    j.boolean("consistent_environment", consistent);
    j.str("consistent_environment_note", "false if the console mode, Flip Grip, companion, profile, resolution or "
                                         "uniform reuse mode differed between zones or changed inside a window: "
                                         "do not compare those zones");
    j.integer("uniform_verification_mismatches", static_cast<std::int64_t>(mismatches));
    const ZoneResult* heavy = !r.zones.empty() ? &r.zones.front() : nullptr;
    const ZoneResult* light = r.zones.size() == plan.size() && !r.zones.empty() ? &r.zones.back() : nullptr;
    j.boolean("heavy_zone_stalled", heavy != nullptr && heavy->interp.recovery.stalledInZone());
    j.boolean("light_zone_arrived_stalled", light != nullptr && light->interp.recovery.stalledAtArrival());
    j.boolean("light_zone_recovered", light != nullptr && light->interp.recovery.recovered());
    if (light != nullptr && light->interp.recovery.recovered()) {
        j.num("light_zone_recovery_seconds", light->interp.recovery.recoverySeconds(), 2);
    } else {
        j.null("light_zone_recovery_seconds");
    }
    j.begin_array("per_zone");
    for (std::size_t i = 0; i < plan.size() && i < r.zones.size(); ++i) {
        const auto& z = r.zones[i];
        const auto drawRecNet =
            fifo_report::net_ms(z.zoneTimes.drawRecMs,
                fifo_report::per_frame(z.fifo.sites[AURORA_FIFO_DRAIN_DRAW_DONE].waitMs, z.fifo.frames));
        j.begin_object();
        j.str("name", plan[i].name);
        j.str("class", plan[i].category);
        j.str("status", status_name(z.status));
        j.boolean("measured", z.measured);
        j.num("avg_fps", z.frames.avgFps, 2);
        j.num("low_1pct_fps", z.frames.low1PctFps, 2);
        j.num("p95_ms", z.frames.p95Ms, 2);
        j.num("worker_busy_ms_per_frame", fifo_report::per_frame(z.fifo.busyMs, z.fifo.frames), 3);
        j.num("drawrec_net_ms_per_frame", drawRecNet.ms, 3);
        j.num("stalled_pct_of_window",
            z.interp.windowSeconds > 0.0 ? 100.0 * z.interp.window.stalledSeconds / z.interp.windowSeconds : 0.0, 1);
        j.str("cache_state", cache_state(z.loading));
        j.end_object();
    }
    j.end_array();
    j.end_object();

    j.begin_object("restore");
    j.boolean("returned_to_original_place", r.returned);
    j.boolean("in_memory_save_state_restored", r.stateRestored);
    j.boolean("saves_reenabled", r.savesReenabled);
    j.integer("config_saves_during_run", static_cast<std::int64_t>(r.configSavesDuringRun));
    j.boolean("config_json_written", r.configSavesDuringRun != 0);
    j.boolean("achievements_suspended_during_run", r.achievementsSuspendedDuringRun);
    j.str("achievements_resumed", "after this report is written (the last step of every outcome)");
    j.end_object();

    j.end_object();
    return j.take();
}

std::string summary_text(const TourReport& r, std::span<const Zone> plan) {
    std::string out;
    char line[320];
    std::snprintf(line, sizeof line, "Build %s\n", r.buildId.c_str());
    out += line;
    std::snprintf(line, sizeof line, "%s after %.0f s - %s, %s\n",
        r.outcome == Outcome::Completed ? "Finished" :
        r.outcome == Outcome::Cancelled ? "Cancelled" :
                                          "Stopped",
        r.totalSeconds, r.envAtStart.mode.c_str(), r.envAtStart.profile.c_str());
    out += line;
    if (!r.detail.empty()) {
        out += r.detail + "\n";
    }
    for (std::size_t i = 0; i < plan.size(); ++i) {
        const ZoneResult empty{};
        const ZoneResult& z = i < r.zones.size() ? r.zones[i] : empty;
        if (z.status == ZoneStatus::Pending) {
            std::snprintf(line, sizeof line, "%s (%s): not reached\n", plan[i].name, plan[i].category);
        } else if (z.measured) {
            std::snprintf(line, sizeof line, "%s (%s): %.1f FPS, 1%% low %.1f, p95 %.1f ms, FIFO busy %.1f ms/frame%s%s\n",
                plan[i].name, plan[i].category, z.frames.avgFps, z.frames.low1PctFps, z.frames.p95Ms,
                fifo_report::per_frame(z.fifo.busyMs, z.fifo.frames),
                z.interp.recovery.stalledInZone() ? (z.interp.recovery.recovered() ? ", interpolation stalled then recovered" :
                                                                                     ", interpolation stalled") :
                                                    "",
                z.status == ZoneStatus::Completed ? "" : " (partial)");
        } else {
            std::snprintf(line, sizeof line, "%s (%s): %s%s%s\n", plan[i].name, plan[i].category,
                status_name(z.status), z.detail.empty() ? "" : " - ", z.detail.c_str());
        }
        out += line;
    }
    return out;
}

}  // namespace dusk::perf_tour
