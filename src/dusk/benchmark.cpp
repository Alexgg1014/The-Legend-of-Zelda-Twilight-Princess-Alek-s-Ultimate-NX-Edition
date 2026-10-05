#include "dusk/benchmark.hpp"

#include "dusk/achievements.h"
#include "dusk/benchmark_report.hpp"
#include "dusk/config.hpp"
#include "dusk/diagnostic_run.hpp"
#include "dusk/dualscreen.h"
#include "dusk/dusk.h"
#include "dusk/dynres.hpp"
#include "dusk/fifo_report.hpp"
#include "dusk/game_clock.h"
#include "dusk/logging.h"
#include "dusk/main.h"
#include "dusk/mod_loader.hpp"
#include "dusk/perf.hpp"
#include "dusk/perf_profile_apply.hpp"
#include "dusk/perf_tour.hpp"
#include "dusk/profiler.hpp"
#include "dusk/resolution_policy.hpp"
#include "dusk/session_log.hpp"
#include "dusk/settings.h"
#include "dusk/shader_prep.hpp"
#include "dusk/switch_display.hpp"
#include "dusk/ui/document.hpp"
#include "dusk/ui/modal.hpp"
#include "dusk/ui/ui.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_overlap_mng.h"
#include "m_Do/m_Do_audio.h"
#include "m_Do/m_Do_controller_pad.h"

#include <aurora/gfx.h>
#include <aurora/tuning.h>
#include <dolphin/gx/GXAurora.h>
#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Filled in by CMake (platforms/switch/cmake/BuildIdentity.cmake). The fallbacks
// keep a build that did not go through it honest rather than blank.
#ifndef ALEKS2_BUILD_TAG
#define ALEKS2_BUILD_TAG "untagged"
#endif
#ifndef ALEKS2_BUILD_COMMIT
#define ALEKS2_BUILD_COMMIT "unknown"
#endif
#ifndef ALEKS2_BUILD_DIRTY
#define ALEKS2_BUILD_DIRTY 0
#endif
#ifndef ALEKS2_BUILD_TIME
#define ALEKS2_BUILD_TIME "unknown"
#endif

namespace dusk::benchmark {
namespace {

inline constexpr borealis::Log Log{"benchmark"};

using clock = std::chrono::steady_clock;

// PrepLoading and PrepRender belong to Prepare Shaders (H34), the Tour* phases to the
// Performance Tour (H35); both share the rest.
enum class Phase {
    Idle,
    Loading,
    Applying,
    Warmup,
    PipelineWait,
    Measuring,
    PrepLoading,
    PrepRender,
    TourLoading,
    TourPipelineWait,
    TourWarmup,
    TourMeasuring,
    Restoring
};

enum class Outcome { Completed, Cancelled, Failed };

// How long the pipeline queue has to stay empty before it counts as drained:
// a queue that is empty for one frame between two compiles is not done.
constexpr auto kQueueQuiet = std::chrono::seconds(1);
constexpr auto kLoadTimeout = std::chrono::seconds(120);
// After the player is back: an autosave the return itself triggers must still
// find the store disabled.
constexpr auto kRestoreGrace = std::chrono::seconds(3);
// A frame this long is not rendering, it is a load, a suspend or a stall worth
// naming in the report.
constexpr float kHitchMs = 1000.0f;
// Run Profile Matrix: after a profile's settings are overridden, how long the resolution change
// and the first frames of the new profile get to settle before the warm-up starts.
constexpr double kProfileSettleSeconds = 4.0;

struct Place {
    std::string stage;
    int room = 0;
    int point = 0;
    int layer = -1;
    bool operator==(const Place&) const = default;
};

struct Pose {
    cXyz pos;
    s16 angleY = 0;
    bool valid = false;
};

struct Run {
    Phase phase = Phase::Idle;
    clock::time_point runStart;
    clock::time_point phaseStart;
    clock::time_point queueEmptySince;
    bool queueWasEmpty = false;
    bool cancelPending = false;
    std::string cancelReason;

    Place original;
    Pose originalPose;
    std::unique_ptr<dSv_info_c> savedInfo;
    bool warped = false;
    bool restoreRequested = false;
    clock::time_point restoredAt;
    bool restored = false;

    // Result carried across the Restoring phase so the summary can be shown once
    // the player is back.
    Outcome outcome = Outcome::Completed;
    std::string outcomeDetail;

    // Pipeline wait.
    bool queueTimedOut = false;
    double queueWaitSeconds = 0.0;
    unsigned queuedAtWaitEnd = 0;

    // Measurement window.
    clock::time_point measureStart;
    unsigned createdAtRunStart = 0;
    unsigned createdAtWindowStart = 0;
    unsigned queuedAtWindowStart = 0;
    AuroraPipelineMemoStats memoAtWindowStart{};
    unsigned transitionsAtWindowStart = 0;
    bool dockedAtStart = false;
    bool flipAtStart = false;
    std::uint64_t sampledFrames = 0;
    std::uint64_t drawsSum = 0;
    unsigned drawsMax = 0;
    std::uint64_t mergedSum = 0;
    std::uint64_t bindRebuildSum = 0;
    // Sums of the per-frame counters Aurora already keeps; the report divides by
    // the frames sampled.
    std::uint64_t pipelineRebuildSum = 0;
    std::uint64_t uniformRebuildSum = 0;
    std::uint64_t blockedPipelineSum = 0;
    std::uint64_t blockedTexturesSum = 0;
    std::uint64_t blockedUniformOnlySum = 0;
    std::uint64_t blockedFmtSum = 0;
    unsigned queuedMax = 0;
    std::uint64_t uiVisibleFrames = 0;
    unsigned renderW = 0, renderH = 0, minW = 0, minH = 0, maxW = 0, maxH = 0;
    bool modeChanged = false;

    // H33: where the window started, to report what happened inside it.
    std::uint64_t tickWaitNsAtStart = 0;
    std::uint64_t limiterWaitNsAtStart = 0;
    game_clock::InterpTelemetry interpAtStart{};
    // H35: FIFO, uniform-reuse and frame-wait counters, and frame kinds, at the window start.
    fifo_report::Snapshot fifoAtStart{};
    bool uniformModeChanged = false;
    interp_frames::Counters framesAtStart{};
    std::uint64_t queuedFrames = 0;
    std::uint64_t queuedSum = 0;

    // H33: Run Profile Matrix. Each profile is applied as a ConfigVar override (never written
    // to config.json), measured like a normal run, and given back. `profile` is the profile the
    // current report is about; for a normal run it is whatever the settings match.
    bool matrix = false;
    std::vector<int> matrixProfiles;
    std::size_t matrixIndex = 0;
    bool profileApplied = false;
    bool matrixDocked = false;
    bool matrixFlip = false;
    int profile = profile::kCustom;
    bool profileFromMatrix = false;
    std::string matrixSummary;
    int reportsWritten = 0;

    // H34: Prepare Shaders. A tour of shader_prep::zones(); measures nothing, changes no
    // setting. The report is written once the player is back, so it can say so.
    bool prep = false;
    std::size_t prepIndex = 0;
    shader_prep::TourReport prepReport;
    clock::time_point prepZoneStart;
    clock::time_point prepSettledAt;
    clock::time_point prepLastCreatedAt;
    clock::time_point prepEventWaitStart;
    bool prepWaitingForEvent = false;
    unsigned prepCreatedAtZoneStart = 0;
    unsigned prepLastCreated = 0;
    unsigned prepCreatedAtStart = 0;
    // H34-c2: read after the UI closed (closing Settings saves the config), see diagnostic_run.hpp.
    std::uint64_t configSavesAtStart = 0;
    // H34-c2: the achievement system is suspended for the whole run (every kind); released
    // in end_run(), after the player, the scene and the saves are back.
    std::unique_ptr<ScopedAchievementSuspension<AchievementSystem>> achievements;
    bool restoreTimedOut = false;
    bool stateRestored = false;

    // H35: Run Performance Tour (perf_tour.hpp). Heavy, medium and light zone, each warped
    // to, waited for, warmed up and measured on its own; the governor is never reset.
    bool tour = false;
    std::size_t tourIndex = 0;
    perf_tour::TourReport tourReport;
    clock::time_point tourZoneStart;
    unsigned tourCreatedAtZoneStart = 0;
    fifo_report::Snapshot tourFifoAtStart{};
    game_clock::InterpTelemetry tourInterpAtStart{};
    interp_frames::Counters tourFramesAtStart{};
    std::uint64_t tourProfiledFrames = 0;

    // H35: the saves block, held for the life of the run. end_run() releases it in order; if
    // the run is destroyed without ending, the destructor does. Declared after `achievements`
    // so that, destroyed in reverse order, saving comes back before achievements resume.
    diagnostic_run::ScopedFlag savesBlock;
};

Run g_run;
std::string g_status;
std::string g_lastSummary;
// Read from the memory-card thread, which is why it is atomic.
std::atomic<bool> g_savesBlocked{false};
// A summary waiting for the player to be back in control before it is shown.
std::string g_pendingSummaryRml;
std::string g_pendingSummaryTitle;
bool g_pendingSummaryIsError = false;

double seconds_since(clock::time_point t) {
    return std::chrono::duration<double>(clock::now() - t).count();
}

Place current_place() {
    Place p;
    const char* stage = dComIfGp_getStartStageName();
    p.stage = stage != nullptr ? stage : "";
    p.room = dComIfGp_getStartStageRoomNo();
    p.point = dComIfGp_getStartStagePoint();
    p.layer = dComIfGp_getStartStageLayer();
    return p;
}

bool at_scene() {
    const char* stage = dComIfGp_getStartStageName();
    return stage != nullptr && std::strcmp(stage, kScene) == 0 && dComIfGp_getStartStageRoomNo() == kSceneRoom;
}

// The stage is the one asked for, nothing is mid-change and Link exists: the
// state in which a frame is a frame of the scene and not of a load.
bool stage_settled(const Place& place) {
    const char* stage = dComIfGp_getStartStageName();
    return stage != nullptr && place.stage == stage && dComIfGp_getStartStageRoomNo() == place.room &&
           !dComIfGp_isEnableNextStage() && !fopOvlpM_IsDoingReq() && daAlink_getAlinkActorClass() != nullptr;
}

bool ui_window_visible() {
    for (const auto scope : {ui::DocumentScope::Window, ui::DocumentScope::MenuBar}) {
        if (const auto* doc = ui::find_document(scope); doc != nullptr && doc->visible()) {
            return true;
        }
    }
    return false;
}

const char* mode_name() {
#ifdef __SWITCH__
    if (perf::is_docked()) {
        return "dock";
    }
    return dualscreen::flipActive() ? "flip_grip" : "handheld";
#else
    return "desktop";
#endif
}

void set_phase(Phase phase) {
    g_run.phase = phase;
    g_run.phaseStart = clock::now();
}

void toast(const std::string& title, const std::string& content = {}, int seconds = 3) {
    ui::push_toast({.title = title, .content = content, .duration = std::chrono::seconds(seconds)});
}

void begin_measuring() {
    auto& r = g_run;
    const auto* stats = aurora_get_stats();
    r.measureStart = clock::now();
    r.createdAtWindowStart = stats != nullptr ? stats->createdPipelines : 0;
    r.queuedAtWindowStart = stats != nullptr ? stats->queuedPipelines : 0;
    r.transitionsAtWindowStart = dynres::transitionCount();
    aurora_gx_pipeline_memo_stats(&r.memoAtWindowStart);
    r.dockedAtStart = perf::is_docked();
    r.flipAtStart = dualscreen::flipActive();
    r.sampledFrames = 0;
    r.drawsSum = r.mergedSum = r.bindRebuildSum = 0;
    r.pipelineRebuildSum = r.uniformRebuildSum = 0;
    r.blockedPipelineSum = r.blockedTexturesSum = r.blockedUniformOnlySum = r.blockedFmtSum = 0;
    r.drawsMax = r.queuedMax = 0;
    r.uiVisibleFrames = 0;
    r.minW = r.minH = ~0u;
    r.maxW = r.maxH = 0;
    r.modeChanged = false;
    r.tickWaitNsAtStart = game_clock::wait_total_ns(game_clock::WaitKind::Tick);
    r.limiterWaitNsAtStart = game_clock::wait_total_ns(game_clock::WaitKind::Limiter);
    r.interpAtStart = game_clock::interp_telemetry();
    r.framesAtStart = game_clock::interp_frame_counters();
    r.fifoAtStart = fifo_report::read_now();
    r.uniformModeChanged = false;
    r.queuedFrames = r.queuedSum = 0;
    if (!r.matrix) {
        r.profile = profile::detectCurrent();
        r.profileFromMatrix = false;
    }
    profiler::captureBegin();
    set_phase(Phase::Measuring);
    Log.info("measuring {} s ({} {}x{}, queued={} created={})", kMeasureSeconds, mode_name(), r.renderW,
        r.renderH, r.queuedAtWindowStart, r.createdAtWindowStart);
    toast("Benchmark: measuring", fmt::format("{} seconds. Press Start to cancel.", kMeasureSeconds), 4);
}

void sample_frame() {
    auto& r = g_run;
    const AuroraStats& st = lastFrameAuroraStats;
    ++r.sampledFrames;
    r.drawsSum += st.drawCallCount;
    r.drawsMax = std::max(r.drawsMax, st.drawCallCount);
    r.mergedSum += st.mergedDrawCallCount;
    r.bindRebuildSum += st.bindGroupRebuilds;
    r.pipelineRebuildSum += st.pipelineRebuilds;
    r.uniformRebuildSum += st.uniformRebuilds;
    r.blockedPipelineSum += st.mergeBlockedPipeline;
    r.blockedTexturesSum += st.mergeBlockedTextures;
    r.blockedUniformOnlySum += st.mergeBlockedUniformOnly;
    r.blockedFmtSum += st.mergeBlockedFmt;
    r.queuedMax = std::max(r.queuedMax, st.queuedPipelines);
    if (st.queuedPipelines > 0) {
        ++r.queuedFrames;
        r.queuedSum += st.queuedPipelines;
    }
    if (ui_window_visible()) {
        ++r.uiVisibleFrames;
    }
    r.minW = std::min(r.minW, r.renderW);
    r.minH = std::min(r.minH, r.renderH);
    r.maxW = std::max(r.maxW, r.renderW);
    r.maxH = std::max(r.maxH, r.renderH);
    if (perf::is_docked() != r.dockedAtStart || dualscreen::flipActive() != r.flipAtStart) {
        r.modeChanged = true;
    }
    if (aurora_gx_get_uniform_reuse_mode() != r.fifoAtStart.mode) {
        r.uniformModeChanged = true;
    }
}

const char* interp_name(FrameInterpMode mode) {
    switch (mode) {
    case FrameInterpMode::Off:
        return "off";
    case FrameInterpMode::Capped:
        return "capped";
    default:
        return "unlimited";
    }
}

void describe_settings(JsonWriter& j) {
    const auto& s = getSettings();
    j.begin_object("settings");
    // H33: which profile this is and where that came from. "detected" means the settings
    // matched a profile at the start of the window ("custom" if none); "matrix_override" means
    // Run Profile Matrix applied it as a non-persistent override for this report only.
    j.str("profile", profile::name(g_run.profile));
    j.str("profile_source", g_run.profileFromMatrix ? "matrix_override" : "detected");
    j.integer("auto_resolution_policy", s.game.autoResolutionPolicy.getValue());
    j.integer("internal_resolution_scale", s.game.internalResolutionScale.getValue());
    j.integer("shadow_resolution_multiplier", s.game.shadowResolutionMultiplier.getValue());
    j.integer("bloom_mode", static_cast<int>(s.game.bloomMode.getValue()));
    j.integer("depth_of_field_mode", static_cast<int>(s.game.depthOfFieldMode.getValue()));
    j.boolean("water_refraction", !s.game.disableWaterRefraction.getValue());
    j.integer("frame_interpolation", static_cast<int>(s.game.enableFrameInterpolation.getValue()));
    j.str("frame_interpolation_name", interp_name(s.game.enableFrameInterpolation.getValue()));
    j.boolean("interpolation_fancy", s.game.interpolationQuality.getValue() == InterpQuality::Fancy);
    j.str("interpolation_quality", s.game.interpolationQuality.getValue() == InterpQuality::Fancy ? "fancy" : "fast");
    j.boolean("predictive_interpolation", s.game.predictiveInterpolation.getValue());
    j.boolean("pipeline_memo", s.game.pipelineMemo.getValue());
    // H35: diagnostic, runtime only (never in config.json); see the "uniforms" object.
    j.str("uniform_reuse_mode", fifo_report::mode_name(aurora_gx_get_uniform_reuse_mode()));
    j.integer("max_frame_rate", s.video.maxFrameRate.getValue());
    j.boolean("vsync", s.video.enableVsync.getValue());
    j.boolean("lock_aspect_ratio_saved", s.video.lockAspectRatio.getValue());
    j.boolean("aspect_lock_effective", dualscreen::effectiveAspectLock());
    j.end_object();
}

// Zones are named at their DUSK_PROFSCOPE; the same name at two call sites (Sim, Execute and
// Audio each appear on the interpolated and the plain path) is one row. Zones nest -- Execute
// and Audio run inside Sim; Painter, Actors and DrawRec are siblings inside Execute
// (f_pc_manager.cpp) -- so the rows are not meant to add up to the frame.
struct ZoneRow {
    double sumMs = 0.0, maxMs = 0.0;
    std::uint64_t calls = 0;
};

std::map<std::string, ZoneRow> capture_zones() {
    std::map<std::string, ZoneRow> zones;
    for (int i = 0; i < profiler::numZones(); ++i) {
        const char* name = profiler::zoneName(i);
        if (name == nullptr) {
            continue;
        }
        auto& row = zones[name];
        row.sumMs += profiler::captureZoneSumMs(i);
        row.maxMs = std::max(row.maxMs, profiler::captureZoneMaxMs(i));
        row.calls += profiler::captureZoneCalls(i);
    }
    return zones;
}

void write_cpu_zones(JsonWriter& j, const std::map<std::string, ZoneRow>& zones, double profiled) {
    j.begin_object("cpu_zones_ms");
    j.str("note", "per-frame average and worst single frame; zones nest (Execute holds Painter, Actors and DrawRec "
                  "as siblings), so they do not sum to the frame time");
    for (const auto& [name, row] : zones) {
        j.begin_object(name.c_str());
        j.num("avg_per_frame", row.sumMs / profiled, 3);
        j.num("max", row.maxMs, 3);
        j.num("calls_per_frame", static_cast<double>(row.calls) / profiled, 2);
        j.end_object();
    }
    j.end_object();
}

fifo_report::ZoneTimes zone_times(const std::map<std::string, ZoneRow>& zones, double profiled) {
    fifo_report::ZoneTimes t;
    // Present = entered in the window; a slot stays registered after its zone stops running.
    const auto avg = [&](const char* name, bool* present) {
        const auto it = zones.find(name);
        const bool entered = it != zones.end() && it->second.calls > 0;
        if (present != nullptr) {
            *present = entered;
        }
        return entered ? it->second.sumMs / profiled : 0.0;
    };
    t.painterMs = avg("Painter", &t.havePainter);
    t.tickWaitMs = avg("TickWait", &t.haveTickWait);
    t.drawRecMs = avg("DrawRec", &t.haveDrawRec);
    t.drawMs = avg("Draw", &t.haveDraw);
    t.executeMs = avg("Execute", nullptr);
    t.actorsMs = avg("Actors", nullptr);
    return t;
}

// Everything the report says about the run itself, in one object.
std::string build_report(const FrameSummary& frames, const std::string& stamp, std::int64_t unixNow,
    const std::string& fileName) {
    auto& r = g_run;
    const auto* stats = aurora_get_stats();
    const unsigned createdNow = stats != nullptr ? stats->createdPipelines : 0;
    const unsigned createdInWindow =
        createdNow >= r.createdAtWindowStart ? createdNow - r.createdAtWindowStart : 0;
    const unsigned createdInRun = createdNow >= r.createdAtRunStart ? createdNow - r.createdAtRunStart : 0;

    // Cold means the run had to build pipelines it did not have; warm means it
    // had them all. "Compiling" outranks both: a queue that was busy during the
    // window means the numbers include compile stalls. The card cache cannot be
    // told from a fresh compile here -- both count as a created pipeline.
    const char* cache = r.queuedMax > 0 ? "compiling" : (createdInRun > 0 ? "cold" : "warm");

    char backend[64], device[128], driver[256];
    aurora_get_adapter_info(backend, sizeof backend, device, sizeof device, driver, sizeof driver);

    unsigned cpu = 0, gpu = 0, emc = 0;
    const bool clocks = session_log::readClocksMHz(cpu, gpu, emc);

    const double wall = std::chrono::duration<double>(clock::now() - r.measureStart).count();
    const unsigned transitionsNow = dynres::transitionCount();

    JsonWriter j;
    j.begin_object();
    j.str("schema", "dusklight-aleks-benchmark");
    j.integer("schema_version", kReportSchemaVersion);

    j.begin_object("build");
    j.str("id", build_id());
    j.str("tag", ALEKS2_BUILD_TAG);
    j.str("commit", ALEKS2_BUILD_COMMIT);
    j.boolean("tree_dirty", ALEKS2_BUILD_DIRTY != 0);
    j.str("built_at", ALEKS2_BUILD_TIME);
    j.end_object();

    j.begin_object("run");
    j.str("status", "completed");
    j.str("started_utc", stamp.c_str());
    j.integer("started_unix", unixNow);
    j.str("file", fileName);
    j.str("scene", kScene);
    j.integer("room", kSceneRoom);
    j.integer("point", kScenePoint);
    j.integer("layer", kSceneLayer);
    j.integer("warmup_seconds", kWarmupSeconds);
    j.integer("measure_seconds_target", kMeasureSeconds);
    j.num("measure_seconds_wall", wall, 3);
    j.num("measure_seconds_covered_by_frames", frames.coveredSeconds, 3);
    j.begin_object("pipeline_wait");
    j.boolean("timed_out", r.queueTimedOut);
    j.num("waited_seconds", r.queueWaitSeconds, 2);
    j.integer("timeout_seconds", kPipelineWaitTimeoutSeconds);
    j.integer("queued_when_wait_ended", r.queuedAtWaitEnd);
    j.end_object();
    j.boolean("profiler_capture_overflowed", profiler::captureOverflowed());
    j.end_object();

    j.begin_object("device");
    j.str("mode", mode_name());
    j.boolean("docked", r.dockedAtStart);
    j.boolean("flip_active", r.flipAtStart);
    j.boolean("mode_changed_during_window", r.modeChanged);
    j.str("graphics_api", backend);
    j.str("adapter", device);
    j.str("driver", driver);
    j.begin_object("clocks_mhz");
    j.boolean("available", clocks);
    j.integer("cpu", cpu);
    j.integer("gpu", gpu);
    j.integer("emc", emc);
    j.end_object();
    j.end_object();

    j.begin_object("resolution");
    j.integer("width", r.renderW);
    j.integer("height", r.renderH);
    j.integer("min_width", r.minW == ~0u ? r.renderW : r.minW);
    j.integer("min_height", r.minH == ~0u ? r.renderH : r.minH);
    j.integer("max_width", r.maxW);
    j.integer("max_height", r.maxH);
#ifdef __SWITCH__
    // H33: what the setting asked for and what the console limit let through.
    {
        const auto& g = getSettings().game;
        const auto resolved = respol::resolve(
            g.internalResolutionScale.getValue(), g.autoResolutionPolicy.getValue(), display::mode());
        j.integer("setting_saved", g.internalResolutionScale.getValue());
        j.integer("effective_rows", resolved.rows);
        j.integer("cap_rows", respol::max_rows(display::mode()));
        j.boolean("clamped", resolved.clamped);
        j.boolean("repaired", resolved.repaired);
    }
#endif
    j.end_object();

    describe_settings(j);

    j.begin_object("drs");
    j.boolean("enabled", getSettings().game.dynamicResolution.getValue());
    j.integer("target_fps", getSettings().game.dynamicResolutionTarget.getValue());
    j.integer("transitions_in_window",
        transitionsNow >= r.transitionsAtWindowStart ? transitionsNow - r.transitionsAtWindowStart : 0);
    j.integer("transitions_since_launch", transitionsNow);
    j.end_object();

    j.str("cache_state", cache);

    j.begin_object("frames");
    j.integer("count", static_cast<std::int64_t>(frames.frames));
    j.num("avg_fps", frames.avgFps, 2);
    j.num("low_1pct_fps", frames.low1PctFps, 2);
    j.begin_object("frametime_ms");
    j.num("mean", frames.meanMs, 3);
    j.num("median", frames.medianMs, 3);
    j.num("p95", frames.p95Ms, 3);
    j.num("p99", frames.p99Ms, 3);
    j.num("max", frames.maxMs, 3);
    j.end_object();
    j.integer("over_33_3_ms", static_cast<std::int64_t>(frames.over33));
    j.num("pct_over_33_3_ms", frames.pctOver33, 2);
    j.integer("over_50_ms", static_cast<std::int64_t>(frames.over50));
    j.num("pct_over_50_ms", frames.pctOver50, 2);
    // Cadence irregularity (benchmark_report.hpp): 0 for a steady 30 or 60.
    j.integer("judder_pairs", static_cast<std::int64_t>(frames.judderPairs));
    j.num("judder_pct", frames.judderPct, 2);
    j.end_object();

    auto zones = capture_zones();
    const std::uint64_t profiledFrames = profiler::captureProfiledFrames();
    const double profiled = static_cast<double>(std::max<std::uint64_t>(profiledFrames, 1));
    write_cpu_zones(j, zones, profiled);

    // H33: where the frame interval went, as far as the current instrumentation can say.
    // Only the two limiters are real waits: they report the sleep that actually elapsed.
    // `work` is the interval minus those sleeps. Begin and Present are PROXIES, not GPU or
    // present times: Begin is aurora_begin_frame, which holds the wait for a free frame slot
    // (render-worker backpressure) together with ImGui; Present is aurora_end_frame, which
    // holds the FIFO drain together with the end-of-frame recording. The vsync wait itself
    // happens on the render worker and is not visible from the main loop.
    {
        const double frameCount = static_cast<double>(std::max<std::size_t>(frames.frames, 1));
        const double tickMs = static_cast<double>(game_clock::wait_total_ns(game_clock::WaitKind::Tick) -
                                                  r.tickWaitNsAtStart) / 1e6 / frameCount;
        const double limiterMs = static_cast<double>(game_clock::wait_total_ns(game_clock::WaitKind::Limiter) -
                                                     r.limiterWaitNsAtStart) / 1e6 / frameCount;
        j.begin_object("timing");
        j.num("interval_ms_mean", frames.meanMs, 3);
        j.num("tick_wait_ms_per_frame", tickMs, 3);
        j.num("limiter_wait_ms_per_frame", limiterMs, 3);
        j.num("work_ms_per_frame", std::max(0.0, frames.meanMs - tickMs - limiterMs), 3);
        j.str("note", "tick_wait and limiter_wait are the sleeps that actually elapsed in the game's tick "
                      "limiter and the main-loop limiter; work is the interval minus both");
        j.begin_object("proxies");
        j.str("note", "NOT GPU or present times: Begin = frame-slot wait + ImGui, Present = FIFO drain + "
                      "end-of-frame recording; the vsync wait happens on the render worker");
        j.num("begin_zone_ms_per_frame", zones.count("Begin") ? zones["Begin"].sumMs / profiled : 0.0, 3);
        j.num("present_zone_ms_per_frame", zones.count("Present") ? zones["Present"].sumMs / profiled : 0.0, 3);
        j.end_object();
        j.end_object();
    }

    // H33: the predictive-interpolation governor over the window (game_clock.cpp).
    {
        const auto now = game_clock::interp_telemetry();
        const auto& at = r.interpAtStart;
        j.begin_object("interpolation");
        j.boolean("stalled_at_end", now.stalled);
        j.integer("stall_enters", now.enters - at.enters);
        j.integer("stall_exits", now.exits - at.exits);
        j.integer("resume_attempts", now.attempts - at.attempts);
        j.integer("resume_failures", now.failures - at.failures);
        j.num("stalled_seconds", now.stalledSeconds - at.stalledSeconds, 2);
        j.num("stalled_pct_of_window", wall > 0.0 ? 100.0 * (now.stalledSeconds - at.stalledSeconds) / wall : 0.0, 1);
        j.integer("resume_blocked_by_cooldown_frames", now.cooldownBlockedFrames - at.cooldownBlockedFrames);
        j.num("last_work_ms", now.workMs, 2);
        j.num("last_gap_ms", now.gapMs, 2);
        j.num("cooldown_seconds", now.cooldownSeconds, 0);
        j.str("quality", getSettings().game.interpolationQuality.getValue() == InterpQuality::Fancy ? "fancy" : "fast");
        j.num("enter_threshold_ms", 35.0, 1);
        j.num("resume_threshold_ms", 30.0, 1);
        // H35: frame kinds over the window (interp_frames.hpp).
        fifo_report::write_interp_frames(j, interp_frames::delta(r.framesAtStart, game_clock::interp_frame_counters()),
            frames.coveredSeconds);
        j.end_object();
    }

    // H35: what the FIFO thread did, where the game thread waited for it, uniform reuse,
    // the frame waits outside the FIFO, and the two net figures they make possible.
    {
        auto w = fifo_report::window(r.fifoAtStart, fifo_report::read_now(), profiledFrames);
        w.modeChangedDuringWindow = w.modeChangedDuringWindow || r.uniformModeChanged;
        fifo_report::write_fifo(j, w);
        fifo_report::write_uniforms(j, w, aurora_gx_uniform_section_name(w.lastMismatchSection));
        fifo_report::write_frame_waits(j, w);
        fifo_report::write_derived(j, zone_times(zones, profiled), w);
    }

    j.begin_object("gpu");
    j.boolean("available", false);
    j.str("reason", "no timestamp-query path is wired into the frame loop; Aurora only feeds GPU zones to Tracy");
    j.end_object();

    const double sampled = static_cast<double>(std::max<std::uint64_t>(r.sampledFrames, 1));
    j.begin_object("gfx");
    j.integer("frames_sampled", static_cast<std::int64_t>(r.sampledFrames));
    j.num("draws_per_frame_avg", static_cast<double>(r.drawsSum) / sampled, 1);
    j.integer("draws_per_frame_max", r.drawsMax);
    j.num("merged_draws_per_frame_avg", static_cast<double>(r.mergedSum) / sampled, 2);
    j.integer("merged_draws_total", static_cast<std::int64_t>(r.mergedSum));
    j.num("bind_group_rebuilds_per_frame_avg", static_cast<double>(r.bindRebuildSum) / sampled, 1);
    j.num("pipeline_rebuilds_per_frame_avg", static_cast<double>(r.pipelineRebuildSum) / sampled, 1);
    j.num("uniform_rebuilds_per_frame_avg", static_cast<double>(r.uniformRebuildSum) / sampled, 1);
    // Why a draw could not merge into the one before it, per frame: the state
    // that changed was the pipeline, the textures, only the uniforms, or the
    // vertex format.
    j.num("merge_blocked_pipeline_per_frame_avg", static_cast<double>(r.blockedPipelineSum) / sampled, 1);
    j.num("merge_blocked_textures_per_frame_avg", static_cast<double>(r.blockedTexturesSum) / sampled, 1);
    j.num("merge_blocked_uniform_only_per_frame_avg", static_cast<double>(r.blockedUniformOnlySum) / sampled, 1);
    j.num("merge_blocked_fmt_per_frame_avg", static_cast<double>(r.blockedFmtSum) / sampled, 1);
    // The GX pipeline memo over the window (hits/misses/evictions are deltas; the
    // maximum is the highest occupancy since launch).
    AuroraPipelineMemoStats memoNow{};
    aurora_gx_pipeline_memo_stats(&memoNow);
    const auto memoHits = memoNow.hits - r.memoAtWindowStart.hits;
    const auto memoMisses = memoNow.misses - r.memoAtWindowStart.misses;
    j.integer("pipeline_memo_hits", static_cast<std::int64_t>(memoHits));
    j.integer("pipeline_memo_misses", static_cast<std::int64_t>(memoMisses));
    j.integer("pipeline_memo_evictions",
        static_cast<std::int64_t>(memoNow.evictions - r.memoAtWindowStart.evictions));
    j.num("pipeline_memo_hit_rate",
        memoHits + memoMisses > 0 ? static_cast<double>(memoHits) / static_cast<double>(memoHits + memoMisses) : 0.0, 4);
    j.integer("pipeline_memo_max_occupied", memoNow.max_occupied);
    j.integer("pipeline_memo_capacity", memoNow.capacity);
    j.integer("pipelines_created_in_window", createdInWindow);
    j.integer("pipelines_created_in_run", createdInRun);
    j.integer("pipelines_queued_at_window_start", r.queuedAtWindowStart);
    j.integer("pipelines_queued_max_in_window", r.queuedMax);
    j.integer("frames_with_pipelines_queued", static_cast<std::int64_t>(r.queuedFrames));
    j.num("pipelines_queued_avg_when_queued",
        r.queuedFrames > 0 ? static_cast<double>(r.queuedSum) / static_cast<double>(r.queuedFrames) : 0.0, 1);
    j.integer("frames_with_ui_window_visible", static_cast<std::int64_t>(r.uiVisibleFrames));
    j.end_object();

    j.begin_object("companion");
    j.boolean("enabled", getSettings().game.dualScreen.getValue());
    j.str("layout", getSettings().game.dualScreenLayout.getValue() == kDualLayoutFlip ? "flip" : "normal");
    j.end_object();

    j.begin_array("mods");
    for (const auto& mod : mods::ModLoader::instance().active_mods()) {
        j.begin_object();
        j.str("id", mod.metadata.id);
        j.str("version", mod.metadata.version);
        j.end_object();
    }
    j.end_array();

    j.end_object();
    return j.take();
}

std::string html_escape(const std::string& s) {
    return ui::escape(s);
}

// What the player reads when it is over.
std::string summary_lines(const FrameSummary& f, const char* cache, const std::string& pathOrError, bool wrote) {
    auto& r = g_run;
    std::string out;
    out += fmt::format("Build {}\n", build_id());
    out += fmt::format("{} - {}x{} - {} - profile {}{}\n", mode_name(), r.renderW, r.renderH, kScene,
        profile::name(r.profile), r.profileFromMatrix ? " (matrix override)" : "");
    out += fmt::format("{:.1f} FPS average, {:.1f} FPS 1% low ({} frames)\n", f.avgFps, f.low1PctFps, f.frames);
    out += fmt::format("Frame time: median {:.1f} ms, p95 {:.1f} ms, p99 {:.1f} ms\n", f.medianMs, f.p95Ms, f.p99Ms);
    out += fmt::format("Over 33.3 ms: {:.1f}%   Over 50 ms: {:.1f}%\n", f.pctOver33, f.pctOver50);
    out += fmt::format("Pipelines: {}{}\n", cache,
        r.queueTimedOut ? " (the queue had not drained when the measurement started)" : "");
    if (r.uiVisibleFrames > 0) {
        out += fmt::format("A menu was visible for {} frames.\n", r.uiVisibleFrames);
    }
    out += wrote ? fmt::format("Report: {}\n", pathOrError) : fmt::format("REPORT NOT WRITTEN: {}\n", pathOrError);
    return out;
}

void show_summary(const std::string& title, const std::string& text, bool isError) {
    g_pendingSummaryTitle = title;
    g_pendingSummaryIsError = isError;
    std::string rml;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const auto nl = text.find('\n', pos);
        const auto line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty()) {
            rml += html_escape(line);
            rml += "<br/>";
        }
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    g_pendingSummaryRml = rml;
}

void present_pending_summary() {
    if (g_pendingSummaryRml.empty()) {
        return;
    }
    auto dismiss = [](ui::Modal& modal) {
        mDoAud_seStartMenu(ui::kSoundWindowClose);
        modal.pop();
    };
    ui::push_document(std::make_unique<ui::Modal>(ui::Modal::Props{
        .title = g_pendingSummaryTitle,
        .bodyRml = g_pendingSummaryRml,
        .actions = {ui::ModalAction{.label = "OK", .onPressed = dismiss}},
        .onDismiss = dismiss,
        .icon = g_pendingSummaryIsError ? "warning" : "",
    }));
    if (auto* doc = ui::top_document()) {
        doc->focus();
    }
    g_pendingSummaryRml.clear();
}

// Puts back what a visit to the scene changed in memory. The store is still
// disabled at this point, so none of it was ever on the card; this keeps the
// player's own later save from inheriting the visit.
void restore_game_state() {
    auto& r = g_run;
    if (r.savedInfo != nullptr) {
        dSv_info_c* info = dComIfGs_getSaveInfo();
        info->setSavedata(r.savedInfo->getSavedata());
        info->setMemory(r.savedInfo->getMemory());
        info->setDan(r.savedInfo->getDan());
        info->getRestart() = r.savedInfo->getRestart();
        info->getTmp() = r.savedInfo->getTmp();
        info->getTurnRestart() = r.savedInfo->getTurnRestart();
        r.savedInfo.reset();
        r.stateRestored = true;
        Log.info("restored the in-memory save state captured before the run");
    }
    if (r.originalPose.valid) {
        if (auto* link = daAlink_getAlinkActorClass()) {
            link->current.pos = r.originalPose.pos;
            link->old.pos = r.originalPose.pos;
            link->current.angle.y = r.originalPose.angleY;
            link->shape_angle.y = r.originalPose.angleY;
        }
    }
}

void finish_prep_report();
void finish_tour_report();

// Every outcome (completed, cancelled, failed, could not return) ends here, from the
// Restoring phase, once the player is back and the grace period is over.
void end_run() {
    // Belt and braces: every way out goes through begin_restore, which already did this.
    profile::clearOverrides();
    diagnostic_run::end({
        .reenableGameSaves =
            [] {
                g_run.savesBlock.release();
                Log.info("run over ({}); saves enabled again",
                    g_run.outcome == Outcome::Completed ? "completed" : "stopped");
            },
        .finish =
            [] {
                if (g_run.prep) {
                    finish_prep_report();
                }
                if (g_run.tour) {
                    finish_tour_report();
                }
            },
        .resumeAchievements = [] { g_run.achievements.reset(); },
    });
    g_run = Run{};
    g_status.clear();
    present_pending_summary();
}

// The start of every run, in the order diagnostic_run.hpp gives; returns the config save
// baseline. Called before the first warp.
std::uint64_t begin_diagnostic(Run& r) {
    return diagnostic_run::begin({
        .blockGameSaves = [&r] { r.savesBlock = diagnostic_run::ScopedFlag(g_savesBlocked); },
        .suspendAchievements =
            [&r] {
                r.achievements =
                    std::make_unique<ScopedAchievementSuspension<AchievementSystem>>(AchievementSystem::get());
            },
        .closeUi = [] { ui::close_all_documents(); },
        .configSaveCount = [] { return static_cast<std::uint64_t>(config::save_count()); },
    });
}

// The measurement is over or the run was stopped: stop the capture, and either
// go back to where the player was or, if nothing was moved, finish.
void begin_restore(Outcome outcome, std::string detail) {
    auto& r = g_run;
    if (profiler::captureActive()) {
        profiler::captureEnd();
    }
    r.outcome = outcome;
    r.outcomeDetail = std::move(detail);
    // Run Profile Matrix: the settings go back first, whatever ended the run.
    profile::clearOverrides();
    set_phase(Phase::Restoring);
    // Also when the warp is still pending (cancelled while loading): asking for
    // the original place replaces the request instead of letting it fire.
    if (r.warped && (!(current_place() == r.original) || dComIfGp_isEnableNextStage())) {
        r.restoreRequested = true;
        dComIfGp_setNextStage(r.original.stage.c_str(), static_cast<s16>(r.original.point),
            static_cast<s8>(r.original.room), static_cast<s8>(r.original.layer));
        Log.info("returning to {} room {} point {}", r.original.stage, r.original.room, r.original.point);
    } else {
        r.restoreRequested = false;
        // Prepare Shaders and the Performance Tour can end where they started (the last zone
        // was the original place) after visiting others: what those visits changed in memory
        // goes back here.
        if ((r.prep || r.tour) && r.warped && r.savedInfo != nullptr) {
            restore_game_state();
        }
        r.restored = true;
        r.restoredAt = clock::now();
    }
}

void finish_measurement() {
    auto& r = g_run;
    profiler::captureEnd();
    const FrameSummary frames = summarize_frames(profiler::captureFrameIntervalsMs());

    // Wall clock for the file name and the report; steady_clock has no epoch.
    const auto unixNow = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    const std::string stamp = utc_stamp_iso(unixNow);
    const std::string fileName = report_file_name(utc_stamp_for_file(unixNow), build_id());
    const std::string json = build_report(frames, stamp, unixNow, fileName);

    const WriteResult written = write_report_atomic(ConfigPath / "benchmarks", fileName, json);

    const auto* stats = aurora_get_stats();
    const unsigned createdNow = stats != nullptr ? stats->createdPipelines : 0;
    const unsigned createdInRun = createdNow >= r.createdAtRunStart ? createdNow - r.createdAtRunStart : 0;
    const char* cache = r.queuedMax > 0 ? "compiling" : (createdInRun > 0 ? "cold" : "warm");

    if (written.ok) {
        ++r.reportsWritten;
    }
    std::string text;
    if (written.ok) {
        const auto path = written.path.generic_string();
        Log.info("report written: {}", path);
        text = summary_lines(frames, cache, path, true);
    } else {
        Log.error("report not written: {}", written.error);
        text = summary_lines(frames, cache, written.error, false);
    }
    Log.info("summary:\n{}", text);

    if (r.matrix) {
        // One modal for the whole matrix, after the last profile; a report per profile.
        r.matrixSummary += text + "\n";
        if (r.matrixIndex + 1 < r.matrixProfiles.size()) {
            profile::clearOverrides();
            ++r.matrixIndex;
            r.profileApplied = false;
            r.queueTimedOut = false;
            r.queueWaitSeconds = 0.0;
            r.queuedAtWaitEnd = 0;
            r.queuedMax = 0;
            const auto* now = aurora_get_stats();
            r.createdAtRunStart = now != nullptr ? now->createdPipelines : 0;
            set_phase(Phase::Applying);
            return;
        }
        g_lastSummary = r.matrixSummary;
        show_summary(r.reportsWritten == static_cast<int>(r.matrixProfiles.size()) ? "Profile matrix finished"
                                                                                  : "Profile matrix finished, a report was not saved",
            r.matrixSummary, r.reportsWritten != static_cast<int>(r.matrixProfiles.size()));
        begin_restore(Outcome::Completed, {});
        return;
    }

    g_lastSummary = text;
    show_summary(written.ok ? "Benchmark finished" : "Benchmark finished, report not saved", text, !written.ok);
    begin_restore(Outcome::Completed, {});
}

void cancel_prep(const std::string& reason, Outcome outcome);
void cancel_tour(const std::string& reason, Outcome outcome);

void cancel_now(const std::string& reason, Outcome outcome) {
    if (g_run.prep) {
        cancel_prep(reason, outcome);
        return;
    }
    if (g_run.tour) {
        cancel_tour(reason, outcome);
        return;
    }
    Log.warn("run stopped: {}", reason);
    g_lastSummary = fmt::format("{} stopped: {}", g_run.matrix ? "Profile matrix" : "Benchmark", reason);
    if (g_run.matrix && g_run.reportsWritten > 0) {
        g_lastSummary += fmt::format("\n{} report(s) were written before it stopped.", g_run.reportsWritten);
    }
    show_summary(outcome == Outcome::Failed ? (g_run.matrix ? "Profile matrix failed" : "Benchmark failed")
                                            : (g_run.matrix ? "Profile matrix cancelled" : "Benchmark cancelled"),
        g_lastSummary,
        outcome == Outcome::Failed);
    begin_restore(outcome, reason);
}

// ---------------------------------------------------------------------------------
// H34: Prepare Shaders. Visits shader_prep::zones() one after the other, lets each draw
// until its pipeline queue has drained (or a per-zone limit), and goes back. It reuses
// everything above that protects the player: saves blocked from the start, the
// in-memory save state captured and put back, input locked, Start or a menu cancels,
// begin_restore() on every way out. It changes no setting, so there is nothing to give
// back there, and it measures no frame rate.

const shader_prep::Timing kPrepTiming{};

unsigned created_pipelines() {
    const auto* stats = aurora_get_stats();
    return stats != nullptr ? stats->createdPipelines : 0;
}

unsigned queued_pipelines() {
    const auto* stats = aurora_get_stats();
    return stats != nullptr ? stats->queuedPipelines : 0;
}

// The zone's stage is the current one, nothing is mid-change and Link exists. The room
// is not required to match: an entrance can put Link in a neighbouring room, and the
// report records where he really is.
bool prep_stage_settled(const char* stage) {
    const char* current = dComIfGp_getStartStageName();
    return current != nullptr && std::strcmp(current, stage) == 0 && !dComIfGp_isEnableNextStage() &&
           !fopOvlpM_IsDoingReq() && daAlink_getAlinkActorClass() != nullptr;
}

shader_prep::ZoneResult& prep_zone_result() {
    return g_run.prepReport.zones[g_run.prepIndex];
}

void begin_prep_zone();

void next_prep_zone() {
    auto& r = g_run;
    ++r.prepIndex;
    if (r.prepIndex >= shader_prep::zones().size()) {
        r.prepReport.outcome = shader_prep::Outcome::Completed;
        Log.info("prepare shaders: tour finished");
        begin_restore(Outcome::Completed, {});
        return;
    }
    begin_prep_zone();
}

void end_prep_zone(shader_prep::ZoneStatus status, shader_prep::ZoneEnd end, std::string detail) {
    auto& r = g_run;
    const auto& zone = shader_prep::zones()[r.prepIndex];
    auto& z = prep_zone_result();
    const unsigned created = created_pipelines();
    z.status = status;
    z.end = end;
    z.detail = std::move(detail);
    z.queuedAtEnd = queued_pipelines();
    z.created = created >= r.prepCreatedAtZoneStart ? created - r.prepCreatedAtZoneStart : 0;
    if (r.phase == Phase::PrepRender) {
        z.renderSeconds = seconds_since(r.prepSettledAt);
    }
    Log.info("prepare shaders {}/{}: {} ({} room {}) {} [{}] created={} queued_max={} queued_end={} load={:.1f}s "
             "render={:.1f}s{}{}",
        r.prepIndex + 1, shader_prep::zones().size(), zone.name, zone.stage, zone.room,
        shader_prep::zone_status_name(z.status), shader_prep::zone_end_name(z.end), z.created, z.queuedMax,
        z.queuedAtEnd, z.loadSeconds, z.renderSeconds, z.detail.empty() ? "" : " - ", z.detail);
}

void begin_prep_zone() {
    auto& r = g_run;
    const auto& zone = shader_prep::zones()[r.prepIndex];
    r.prepZoneStart = clock::now();
    r.prepWaitingForEvent = false;
    r.queueWasEmpty = false;
    r.prepCreatedAtZoneStart = created_pipelines();
    r.prepLastCreated = r.prepCreatedAtZoneStart;
    if (prep_stage_settled(zone.stage) && dComIfGp_getStartStageRoomNo() == zone.room) {
        // Already there (the first zone, when the tour starts in it): nothing to load.
        auto& z = prep_zone_result();
        z.arrivedStage = zone.stage;
        z.arrivedRoom = zone.room;
        z.queuedAtStart = queued_pipelines();
        r.prepSettledAt = r.prepLastCreatedAt = clock::now();
        set_phase(Phase::PrepRender);
        return;
    }
    r.warped = true;
    dComIfGp_setNextStage(zone.stage, static_cast<s16>(zone.point), static_cast<s8>(zone.room),
        static_cast<s8>(zone.layer));
    set_phase(Phase::PrepLoading);
}

void tick_prep(unsigned queued) {
    auto& r = g_run;
    const auto plan = shader_prep::zones();
    const auto& zone = plan[r.prepIndex];
    auto& report = r.prepReport;
    report.queuedMax = std::max(report.queuedMax, queued);
    double zoneFraction = 0.0;
    std::string detail;

    if (r.phase == Phase::PrepLoading) {
        const double waited = seconds_since(r.phaseStart);
        if (prep_stage_settled(zone.stage)) {
            auto& z = prep_zone_result();
            z.loadSeconds = waited;
            z.arrivedStage = zone.stage;
            z.arrivedRoom = dComIfGp_getStartStageRoomNo();
            z.queuedAtStart = queued;
            z.queuedMax = queued;
            // Pipelines the load itself created count for the zone.
            r.prepSettledAt = r.prepLastCreatedAt = clock::now();
            set_phase(Phase::PrepRender);
            return;
        }
        if (waited > kPrepTiming.loadTimeoutSeconds) {
            end_prep_zone(shader_prep::ZoneStatus::Failed, shader_prep::ZoneEnd::None, "the zone did not finish loading");
            next_prep_zone();
            return;
        }
        zoneFraction = 0.1 * std::min(1.0, waited / 20.0);
        detail = "loading";
    } else {
        const char* stage = dComIfGp_getStartStageName();
        if (stage == nullptr || std::strcmp(stage, zone.stage) != 0 || dComIfGp_isEnableNextStage()) {
            // Something in the zone (an exit, an event) moved Link on: count what it drew
            // and continue with the next zone from wherever this is.
            end_prep_zone(shader_prep::ZoneStatus::Skipped, shader_prep::ZoneEnd::None,
                fmt::format("left the zone for {}", stage != nullptr ? stage : "?"));
            next_prep_zone();
            return;
        }
        auto& z = prep_zone_result();
        z.queuedMax = std::max(z.queuedMax, queued);
        const unsigned created = created_pipelines();
        if (created != r.prepLastCreated) {
            r.prepLastCreated = created;
            r.prepLastCreatedAt = clock::now();
        }
        if (queued == 0) {
            if (!r.queueWasEmpty) {
                r.queueWasEmpty = true;
                r.queueEmptySince = clock::now();
            }
        } else {
            r.queueWasEmpty = false;
        }
        shader_prep::ZoneProgress progress;
        progress.sinceSettled = seconds_since(r.prepSettledAt);
        progress.sinceLastCreated = seconds_since(r.prepLastCreatedAt);
        progress.queueEmptyFor = r.queueWasEmpty ? seconds_since(r.queueEmptySince) : 0.0;
        progress.queued = queued;
        progress.createdInZone = created >= r.prepCreatedAtZoneStart ? created - r.prepCreatedAtZoneStart : 0;
        const auto end = shader_prep::decide(progress, kPrepTiming);
        if (end != shader_prep::ZoneEnd::None) {
            // A cutscene that started on arrival is let finish before warping out.
            if (dComIfGp_event_runCheck()) {
                if (!r.prepWaitingForEvent) {
                    r.prepWaitingForEvent = true;
                    r.prepEventWaitStart = clock::now();
                }
                if (seconds_since(r.prepEventWaitStart) < kPrepTiming.eventWaitSeconds) {
                    g_status = fmt::format("Preparing shaders - zone {}/{}: {} - waiting for a scene to end",
                        r.prepIndex + 1, plan.size(), zone.name);
                    return;
                }
                Log.warn("prepare shaders: an event was still running in {} after {:.0f} s; moving on", zone.stage,
                    kPrepTiming.eventWaitSeconds);
            }
            end_prep_zone(shader_prep::ZoneStatus::Completed, end, {});
            next_prep_zone();
            return;
        }
        zoneFraction = 0.1 + 0.9 * std::min(1.0, progress.sinceSettled / kPrepTiming.maxZoneSeconds);
        detail = fmt::format("{} new, {} queued", progress.createdInZone, queued);
    }

    const double overall = shader_prep::overall_progress(r.prepIndex, plan.size(), zoneFraction);
    g_status = fmt::format("Preparing shaders {:.0f}% - zone {}/{}: {} ({}) - {} - Start cancels", overall * 100.0,
        r.prepIndex + 1, plan.size(), zone.name, zone.category, detail);
}

void cancel_prep(const std::string& reason, Outcome outcome) {
    auto& r = g_run;
    Log.warn("prepare shaders stopped: {}", reason);
    if (r.prepIndex < r.prepReport.zones.size() &&
        prep_zone_result().status == shader_prep::ZoneStatus::Pending &&
        (r.phase == Phase::PrepLoading || r.phase == Phase::PrepRender))
    {
        end_prep_zone(outcome == Outcome::Failed ? shader_prep::ZoneStatus::Failed : shader_prep::ZoneStatus::Cancelled,
            shader_prep::ZoneEnd::None, reason);
    }
    r.prepReport.outcome =
        outcome == Outcome::Failed ? shader_prep::Outcome::Failed : shader_prep::Outcome::Cancelled;
    r.prepReport.detail = reason;
    begin_restore(outcome, reason);
}

// Once the player is back and saves are enabled again: the report can now confirm both.
void finish_prep_report() {
    auto& r = g_run;
    auto& report = r.prepReport;
    report.totalSeconds = seconds_since(r.runStart);
    report.queuedAtEnd = queued_pipelines();
    const unsigned created = created_pipelines();
    report.createdTotal = created >= r.prepCreatedAtStart ? created - r.prepCreatedAtStart : 0;
    report.returned = !r.restoreTimedOut;
    report.stateRestored = !r.warped || r.stateRestored;
    report.savesReenabled = !saves_blocked();
    report.configSavesDuringRun =
        diagnostic_run::config_writes_since(r.configSavesAtStart, config::save_count());

    const auto unixNow = report.startedUnix;
    report.fileName = shader_prep::report_file_name(utc_stamp_for_file(unixNow), build_id());
    const auto plan = shader_prep::zones();
    const std::string json = shader_prep::build_report(report, plan);
    const WriteResult written = write_report_atomic(ConfigPath / "benchmarks", report.fileName, json);

    std::string text = shader_prep::summary_text(report, plan);
    text += report.returned ? "You are back where you started.\n" : "Could not return to where you started.\n";
    text += report.savesReenabled ? "Saving is enabled again; nothing was saved during the tour.\n" : "";
    text += report.configSavesDuringRun == 0 ? "Your settings were not changed.\n" : "";
    if (written.ok) {
        Log.info("prepare shaders report written: {}", written.path.generic_string());
        text += fmt::format("Report: {}\n", written.path.generic_string());
    } else {
        Log.error("prepare shaders report not written: {}", written.error);
        text += fmt::format("REPORT NOT WRITTEN: {}\n", written.error);
    }
    Log.info("prepare shaders {}: zones={} created={} queued start/max/end={}/{}/{} total={:.0f}s returned={} "
             "state_restored={} saves_reenabled={} config_saves={}",
        shader_prep::outcome_name(report.outcome), plan.size(), report.createdTotal, report.queuedAtStart,
        report.queuedMax, report.queuedAtEnd, report.totalSeconds, report.returned, report.stateRestored,
        report.savesReenabled, report.configSavesDuringRun);
    g_lastSummary = text;
    show_summary(report.outcome == shader_prep::Outcome::Completed ? "Shaders prepared" :
                 report.outcome == shader_prep::Outcome::Cancelled ? "Shader preparation cancelled" :
                                                                     "Shader preparation stopped",
        text, report.outcome == shader_prep::Outcome::Failed || !written.ok);
}

// ---------------------------------------------------------------------------------
// H35: Run Performance Tour. Visits perf_tour::zones() -- heavy, medium, light -- and in
// each one waits for the load and the pipeline queue, warms up and measures a window of
// its own, with the settings the console already has. Same protections as the runs
// above: saves blocked, achievements suspended, the in-memory save state captured and put
// back, input locked, Start or a menu cancels, begin_restore() on every way out. It
// changes no setting and never resets the interpolation governor between zones: the
// point is to see whether a stall in the heavy zone recovers in the lighter ones.

const perf_tour::Timing kTourTiming{};

perf_tour::ZoneResult& tour_zone() {
    return g_run.tourReport.zones[g_run.tourIndex];
}

void capture_environment(perf_tour::Environment& e) {
    const auto& g = getSettings().game;
    e.mode = mode_name();
    e.docked = perf::is_docked();
    e.flipGrip = dualscreen::flipActive();
    e.renderWidth = g_run.renderW;
    e.renderHeight = g_run.renderH;
    e.profile = profile::name(profile::detectCurrent());
    e.companionEnabled = g.dualScreen.getValue();
    e.companionLayout = g.dualScreenLayout.getValue() == kDualLayoutFlip ? "flip" : "normal";
    e.clocksAvailable = session_log::readClocksMHz(e.cpuMhz, e.gpuMhz, e.emcMhz);
}

double tour_seconds() {
    return seconds_since(g_run.runStart);
}

void begin_tour_zone();

void next_tour_zone() {
    auto& r = g_run;
    ++r.tourIndex;
    if (r.tourIndex >= perf_tour::zones().size()) {
        r.tourReport.outcome = perf_tour::Outcome::Completed;
        Log.info("performance tour: all zones done");
        begin_restore(Outcome::Completed, {});
        return;
    }
    begin_tour_zone();
}

void begin_tour_window() {
    auto& r = g_run;
    auto& z = tour_zone();
    r.measureStart = clock::now();
    r.createdAtWindowStart = created_pipelines();
    r.tourFifoAtStart = fifo_report::read_now();
    r.tourInterpAtStart = game_clock::interp_telemetry();
    r.tourFramesAtStart = game_clock::interp_frame_counters();
    r.dockedAtStart = perf::is_docked();
    r.flipAtStart = dualscreen::flipActive();
    r.modeChanged = false;
    r.uniformModeChanged = false;
    r.minW = r.minH = ~0u;
    r.maxW = r.maxH = 0;
    z.measured = true;
    z.interp.stalledAtMeasureStart = game_clock::interpolation_stalled();
    profiler::captureBegin();
    set_phase(Phase::TourMeasuring);
}

// Closes the measurement window and keeps everything it saw in the zone's result. Also
// used when a zone ends early (cancelled, left the zone): what was measured is kept and
// marked incomplete.
void end_tour_window(bool complete) {
    auto& r = g_run;
    auto& z = tour_zone();
    profiler::captureEnd();
    z.measurementComplete = complete;
    z.measureSecondsWall = seconds_since(r.measureStart);
    z.frames = summarize_frames(profiler::captureFrameIntervalsMs());

    const auto zones = capture_zones();
    r.tourProfiledFrames = profiler::captureProfiledFrames();
    const double profiled = static_cast<double>(std::max<std::uint64_t>(r.tourProfiledFrames, 1));
    z.cpuZones.clear();
    for (const auto& [name, row] : zones) {
        z.cpuZones.push_back({name, row.sumMs / profiled, row.maxMs, static_cast<double>(row.calls) / profiled});
    }
    z.zoneTimes = zone_times(zones, profiled);

    z.fifo = fifo_report::window(r.tourFifoAtStart, fifo_report::read_now(), r.tourProfiledFrames);
    z.fifo.modeChangedDuringWindow = z.fifo.modeChangedDuringWindow || r.uniformModeChanged;
    z.uniformMismatchSection = aurora_gx_uniform_section_name(z.fifo.lastMismatchSection);

    const auto now = game_clock::interp_telemetry();
    const auto& at = r.tourInterpAtStart;
    z.interp.window = {
        .enters = now.enters - at.enters,
        .exits = now.exits - at.exits,
        .attempts = now.attempts - at.attempts,
        .failures = now.failures - at.failures,
        .cooldownBlockedFrames = now.cooldownBlockedFrames - at.cooldownBlockedFrames,
        .stalledSeconds = now.stalledSeconds - at.stalledSeconds,
    };
    z.interp.frames = interp_frames::delta(r.tourFramesAtStart, game_clock::interp_frame_counters());
    z.interp.windowSeconds = z.measureSecondsWall;
    z.interp.stalledAtMeasureEnd = now.stalled;

    const unsigned created = created_pipelines();
    z.loading.createdInWindow = created >= r.createdAtWindowStart ? created - r.createdAtWindowStart : 0;
    z.loading.createdInZone = created >= r.tourCreatedAtZoneStart ? created - r.tourCreatedAtZoneStart : 0;

    capture_environment(z.env);
    z.env.minWidth = r.minW == ~0u ? r.renderW : r.minW;
    z.env.minHeight = r.minH == ~0u ? r.renderH : r.minH;
    z.env.maxWidth = r.maxW;
    z.env.maxHeight = r.maxH;
    z.env.modeChangedDuringWindow = r.modeChanged;
}

void end_tour_zone(perf_tour::ZoneStatus status, std::string detail) {
    auto& r = g_run;
    const auto& zone = perf_tour::zones()[r.tourIndex];
    auto& z = tour_zone();
    if (r.phase == Phase::TourMeasuring && profiler::captureActive()) {
        end_tour_window(false);
    }
    z.status = status;
    z.detail = std::move(detail);
    if (z.loading.createdInZone == 0) {
        const unsigned created = created_pipelines();
        z.loading.createdInZone = created >= r.tourCreatedAtZoneStart ? created - r.tourCreatedAtZoneStart : 0;
    }
    Log.info("performance tour {}/{}: {} ({} room {} point {}) {} - {:.1f} FPS, 1% low {:.1f}, p95 {:.1f} ms, fifo busy "
             "{:.2f} ms/frame, drawdone wait {:.2f} ms/frame, uniforms {} ({} reused, {} mismatches), stalled {:.1f} s, "
             "recovered={} load={:.1f}s wait={:.1f}s{}{}{}",
        r.tourIndex + 1, perf_tour::zones().size(), zone.name, zone.stage, zone.room, zone.point,
        shader_prep::zone_status_name(z.status), z.frames.avgFps, z.frames.low1PctFps, z.frames.p95Ms,
        fifo_report::per_frame(z.fifo.busyMs, z.fifo.frames),
        fifo_report::per_frame(z.fifo.sites[AURORA_FIFO_DRAIN_DRAW_DONE].waitMs, z.fifo.frames),
        fifo_report::mode_name(z.fifo.modeAtEnd), z.fifo.reuses, z.fifo.verificationMismatches,
        z.interp.window.stalledSeconds, z.interp.recovery.recovered(), z.loading.loadSeconds,
        z.loading.pipelineWaitSeconds, z.measurementComplete ? "" : " (partial)", z.detail.empty() ? "" : " - ",
        z.detail);
}

// The zone's stage is up, nothing is changing and Link exists: from here frames are frames
// of the zone. Records the load and starts following the governor.
void tour_arrive(unsigned queued) {
    auto& r = g_run;
    auto& z = tour_zone();
    z.loading.loadSeconds = seconds_since(r.tourZoneStart);
    const char* stage = dComIfGp_getStartStageName();
    z.arrivedStage = stage != nullptr ? stage : "";
    z.arrivedRoom = dComIfGp_getStartStageRoomNo();
    z.loading.queuedAtArrival = queued;
    z.loading.queuedMax = std::max(z.loading.queuedMax, queued);
    z.interp.recovery.arrive(tour_seconds(), game_clock::interpolation_stalled());
    r.queueWasEmpty = false;
    set_phase(Phase::TourPipelineWait);
}

void begin_tour_zone() {
    auto& r = g_run;
    const auto& zone = perf_tour::zones()[r.tourIndex];
    auto& z = tour_zone();
    const auto& g = getSettings().game;
    z.interp.requestedMode = interp_name(g.enableFrameInterpolation.getValue());
    z.interp.quality = g.interpolationQuality.getValue() == InterpQuality::Fancy ? "fancy" : "fast";
    z.interp.predictive = g.predictiveInterpolation.getValue();
    r.tourZoneStart = clock::now();
    r.tourCreatedAtZoneStart = created_pipelines();
    r.queueWasEmpty = false;
    if (prep_stage_settled(zone.stage) && dComIfGp_getStartStageRoomNo() == zone.room) {
        // Already there (the first zone, when the tour starts in it): nothing to load.
        tour_arrive(queued_pipelines());
        return;
    }
    r.warped = true;
    dComIfGp_setNextStage(zone.stage, static_cast<s16>(zone.point), static_cast<s8>(zone.room),
        static_cast<s8>(zone.layer));
    set_phase(Phase::TourLoading);
}

void tick_tour(unsigned queued) {
    auto& r = g_run;
    const auto plan = perf_tour::zones();
    const auto& zone = plan[r.tourIndex];
    auto& z = tour_zone();
    z.loading.queuedMax = std::max(z.loading.queuedMax, queued);
    perf_tour::Step step = perf_tour::Step::Loading;
    std::string detail;

    if (r.phase == Phase::TourLoading) {
        if (prep_stage_settled(zone.stage)) {
            tour_arrive(queued);
            return;
        }
        if (seconds_since(r.phaseStart) > kTourTiming.loadTimeoutSeconds) {
            end_tour_zone(perf_tour::ZoneStatus::Failed, "the zone did not finish loading");
            next_tour_zone();
            return;
        }
        detail = "loading";
    } else {
        const char* stage = dComIfGp_getStartStageName();
        if (stage == nullptr || std::strcmp(stage, zone.stage) != 0 || dComIfGp_isEnableNextStage()) {
            // An exit or an event moved Link on: keep what was measured, go on from here.
            end_tour_zone(perf_tour::ZoneStatus::Skipped, fmt::format("left the zone for {}", stage != nullptr ? stage : "?"));
            next_tour_zone();
            return;
        }
        z.interp.recovery.sample(tour_seconds(), game_clock::interpolation_stalled());

        switch (r.phase) {
        case Phase::TourPipelineWait: {
            step = perf_tour::Step::PipelineWait;
            if (queued == 0) {
                if (!r.queueWasEmpty) {
                    r.queueWasEmpty = true;
                    r.queueEmptySince = clock::now();
                }
            } else {
                r.queueWasEmpty = false;
            }
            const double waited = seconds_since(r.phaseStart);
            const auto end = perf_tour::decide_pipeline_wait(
                {.waited = waited, .queueEmptyFor = r.queueWasEmpty ? seconds_since(r.queueEmptySince) : 0.0},
                kTourTiming);
            if (end != perf_tour::PipelineWaitEnd::None) {
                z.loading.pipelineWaitTimedOut = end == perf_tour::PipelineWaitEnd::TimedOut;
                z.loading.pipelineWaitSeconds = waited;
                z.loading.queuedAtWaitEnd = queued;
                if (z.loading.pipelineWaitTimedOut) {
                    Log.warn("performance tour: {} still had {} pipelines queued after {:.0f} s; measuring anyway",
                        zone.name, queued, waited);
                }
                set_phase(Phase::TourWarmup);
                return;
            }
            detail = fmt::format("waiting for shaders ({} queued)", queued);
            break;
        }
        case Phase::TourWarmup:
            step = perf_tour::Step::Warmup;
            if (seconds_since(r.phaseStart) >= kTourTiming.warmupSeconds) {
                begin_tour_window();
                return;
            }
            detail = "warming up";
            break;
        case Phase::TourMeasuring: {
            step = perf_tour::Step::Measuring;
            z.loading.queuedMaxInWindow = std::max(z.loading.queuedMaxInWindow, queued);
            r.minW = std::min(r.minW, r.renderW);
            r.minH = std::min(r.minH, r.renderH);
            r.maxW = std::max(r.maxW, r.renderW);
            r.maxH = std::max(r.maxH, r.renderH);
            if (perf::is_docked() != r.dockedAtStart || dualscreen::flipActive() != r.flipAtStart) {
                r.modeChanged = true;
            }
            if (aurora_gx_get_uniform_reuse_mode() != r.tourFifoAtStart.mode) {
                r.uniformModeChanged = true;
            }
            if (dComIfGp_event_runCheck()) {
                ++z.framesWithEvent;
            }
            if (ui_window_visible()) {
                ++z.framesWithUiVisible;
            }
            if (seconds_since(r.measureStart) >= kTourTiming.measureSeconds) {
                end_tour_window(true);
                end_tour_zone(perf_tour::ZoneStatus::Completed, {});
                next_tour_zone();
                return;
            }
            detail = fmt::format("measuring - {:.0f} FPS", aurora_get_fps());
            break;
        }
        default:
            break;
        }
    }

    const double stepSeconds = seconds_since(r.phaseStart);
    const double overall = perf_tour::overall_progress(r.tourIndex, plan.size(), step, stepSeconds, kTourTiming);
    g_status = fmt::format("Performance tour {:.0f}% - zone {}/{}: {} ({}) - {} - Start cancels", overall * 100.0,
        r.tourIndex + 1, plan.size(), zone.name, zone.category, detail);
}

void cancel_tour(const std::string& reason, Outcome outcome) {
    auto& r = g_run;
    Log.warn("performance tour stopped: {}", reason);
    if (r.tourIndex < r.tourReport.zones.size() && tour_zone().status == perf_tour::ZoneStatus::Pending &&
        (r.phase == Phase::TourLoading || r.phase == Phase::TourPipelineWait || r.phase == Phase::TourWarmup ||
            r.phase == Phase::TourMeasuring))
    {
        end_tour_zone(outcome == Outcome::Failed ? perf_tour::ZoneStatus::Failed : perf_tour::ZoneStatus::Cancelled,
            reason);
    }
    r.tourReport.outcome =
        outcome == Outcome::Failed ? perf_tour::Outcome::Failed : perf_tour::Outcome::Cancelled;
    r.tourReport.detail = reason;
    begin_restore(outcome, reason);
}

// Once the player is back and saves are enabled again (diagnostic_run::end): the report can
// now confirm both. Achievements resume right after this, as the last step.
void finish_tour_report() {
    auto& r = g_run;
    auto& report = r.tourReport;
    report.totalSeconds = seconds_since(r.runStart);
    report.returned = !r.restoreTimedOut;
    report.stateRestored = !r.warped || r.stateRestored;
    report.savesReenabled = !saves_blocked();
    report.configSavesDuringRun = diagnostic_run::config_writes_since(r.configSavesAtStart, config::save_count());
    report.achievementsSuspendedDuringRun = r.achievements != nullptr && AchievementSystem::get().suspended();
    report.fileName = perf_tour::report_file_name(utc_stamp_for_file(report.startedUnix), build_id());

    const auto plan = perf_tour::zones();
    const std::string json = perf_tour::build_report(report, plan);
    const WriteResult written = write_report_atomic(ConfigPath / "benchmarks", report.fileName, json);

    std::string text = perf_tour::summary_text(report, plan);
    text += report.returned ? "You are back where you started.\n" : "Could not return to where you started.\n";
    text += report.savesReenabled ? "Saving is enabled again; nothing was saved during the tour.\n" : "";
    text += report.configSavesDuringRun == 0 ? "Your settings were not changed.\n" : "";
    if (written.ok) {
        Log.info("performance tour report written: {}", written.path.generic_string());
        text += fmt::format("Report: {}\n", written.path.generic_string());
    } else {
        Log.error("performance tour report not written: {}", written.error);
        text += fmt::format("REPORT NOT WRITTEN: {}\n", written.error);
    }
    Log.info("performance tour {}: total={:.0f}s returned={} state_restored={} saves_reenabled={} config_saves={} "
             "achievements_suspended={}",
        shader_prep::outcome_name(report.outcome), report.totalSeconds, report.returned, report.stateRestored,
        report.savesReenabled, report.configSavesDuringRun, report.achievementsSuspendedDuringRun);
    g_lastSummary = text;
    show_summary(report.outcome == perf_tour::Outcome::Completed ? "Performance tour finished" :
                 report.outcome == perf_tour::Outcome::Cancelled ? "Performance tour cancelled" :
                                                                    "Performance tour stopped",
        text, report.outcome == perf_tour::Outcome::Failed || !written.ok);
}

}  // namespace

std::string report_directory() {
    return (ConfigPath / "benchmarks").generic_string();
}

std::string build_id() {
    return fmt::format("{}-{}{}", ALEKS2_BUILD_TAG, ALEKS2_BUILD_COMMIT, ALEKS2_BUILD_DIRTY != 0 ? "-dirty" : "");
}

bool active() {
    return g_run.phase != Phase::Idle;
}

bool saves_blocked() {
    return g_savesBlocked.load(std::memory_order_acquire);
}

const std::string& last_summary() {
    return g_lastSummary;
}

std::string status_text() {
    return g_status;
}

bool can_start(std::string& whyNot) {
    if (active()) {
        whyNot = "A run is already in progress.";
        return false;
    }
    if (daAlink_getAlinkActorClass() == nullptr) {
        whyNot = "Load a save and be in the game first: the run starts from where you are.";
        return false;
    }
    const char* stage = dComIfGp_getStartStageName();
    if (stage != nullptr && (std::strcmp(stage, "F_SP102") == 0 || std::strcmp(stage, "F_SP112") == 0)) {
        whyNot = "Not available on the title or file-select screens.";
        return false;
    }
    if (dComIfGp_event_runCheck()) {
        whyNot = "Wait for the current event or cutscene to end.";
        return false;
    }
    if (dComIfGp_isEnableNextStage() || fopOvlpM_IsDoingReq()) {
        whyNot = "A scene change is in progress.";
        return false;
    }
    return true;
}

bool can_start_matrix(std::string& whyNot) {
    if (!can_start(whyNot)) {
        return false;
    }
    // Every profile must be appliable as a pure override, and restorable, before anything is touched.
    for (int p = 0; p < profile::kCount; ++p) {
        std::string why;
        if (!profile::canApplyOverride(p, why)) {
            whyNot = std::string("Profile ") + profile::name(p) + ": " + why;
            return false;
        }
    }
    return true;
}

namespace {

bool start_impl(bool matrix, std::string& error) {
    if (matrix ? !can_start_matrix(error) : !can_start(error)) {
        return false;
    }
    g_run = Run{};
    auto& r = g_run;
    if (matrix) {
        r.matrix = true;
        r.matrixProfiles = {profile::kQuality, profile::kBalanced, profile::kPerformance};
        r.matrixDocked = perf::is_docked();
        r.matrixFlip = dualscreen::flipActive();
    }
    r.runStart = clock::now();
    r.original = current_place();
    if (const auto* link = daAlink_getAlinkActorClass()) {
        r.originalPose = {link->current.pos, link->current.angle.y, true};
    }
    const auto* stats = aurora_get_stats();
    r.createdAtRunStart = stats != nullptr ? stats->createdPipelines : 0;

    // From here nothing reaches the card and no achievement is evaluated, whatever the game
    // asks for; the config baseline is read once the UI (and its own save) is closed.
    r.configSavesAtStart = begin_diagnostic(r);

    if (at_scene()) {
        // Already there: nothing to load, nothing to put back.
        r.warped = false;
        set_phase(r.matrix ? Phase::Applying : Phase::Warmup);
        Log.info("{} started in place at {} room {}", r.matrix ? "profile matrix" : "run", kScene, kSceneRoom);
    } else {
        r.warped = true;
        r.savedInfo = std::make_unique<dSv_info_c>(*dComIfGs_getSaveInfo());
        set_phase(Phase::Loading);
        dComIfGp_setNextStage(kScene, kScenePoint, kSceneRoom, kSceneLayer);
        Log.info("run started from {} room {} point {}; loading {}", r.original.stage, r.original.room,
            r.original.point, kScene);
    }
    toast(r.matrix ? "Profile matrix started" : "Benchmark started",
        r.matrix ? "Three profiles, one report each. Controls are locked. Press Start to cancel."
                 : "Controls are locked. Press Start to cancel.",
        4);
    return true;
}

}  // namespace

bool start(std::string& error) {
    return start_impl(false, error);
}

bool start_matrix(std::string& error) {
    return start_impl(true, error);
}

bool matrix_active() {
    return g_run.matrix && g_run.phase != Phase::Idle;
}

bool can_start_prep(std::string& whyNot) {
    return can_start(whyNot);
}

bool start_prep(std::string& error) {
    if (!can_start_prep(error)) {
        return false;
    }
    g_run = Run{};
    auto& r = g_run;
    r.prep = true;
    r.runStart = clock::now();
    r.original = current_place();
    if (const auto* link = daAlink_getAlinkActorClass()) {
        r.originalPose = {link->current.pos, link->current.angle.y, true};
    }
    r.createdAtRunStart = created_pipelines();
    r.prepCreatedAtStart = r.createdAtRunStart;
    // Taken before the first warp, whatever happens after: every zone visit is put back.
    r.savedInfo = std::make_unique<dSv_info_c>(*dComIfGs_getSaveInfo());

    auto& report = r.prepReport;
    report.buildId = build_id();
    report.startedUnix = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    report.startedUtc = utc_stamp_iso(report.startedUnix);
    report.mode = mode_name();
    report.originalStage = r.original.stage;
    report.originalRoom = r.original.room;
    report.queuedAtStart = queued_pipelines();
    report.queuedMax = report.queuedAtStart;
    report.knownAtStart = r.createdAtRunStart;
    report.zones.assign(shader_prep::zones().size(), shader_prep::ZoneResult{});

    // From here nothing reaches the card and no achievement is evaluated, whatever the game
    // asks for; the config baseline is read once the UI (and its own save) is closed.
    r.configSavesAtStart = begin_diagnostic(r);
    Log.info("prepare shaders started from {} room {} point {}: {} zones, queued={} created since launch={}",
        r.original.stage, r.original.room, r.original.point, shader_prep::zones().size(), report.queuedAtStart,
        report.knownAtStart);
    toast("Preparing shaders", "Visiting a few places to compile their shaders. Controls are locked. "
                               "Press Start to cancel.",
        5);
    begin_prep_zone();
    return true;
}

bool prep_active() {
    return g_run.prep && g_run.phase != Phase::Idle;
}

bool can_start_tour(std::string& whyNot) {
    return can_start(whyNot);
}

bool start_tour(std::string& error) {
    if (!can_start_tour(error)) {
        return false;
    }
    g_run = Run{};
    auto& r = g_run;
    r.tour = true;
    r.runStart = clock::now();
    r.original = current_place();
    if (const auto* link = daAlink_getAlinkActorClass()) {
        r.originalPose = {link->current.pos, link->current.angle.y, true};
    }
    r.createdAtRunStart = created_pipelines();

    // From here nothing reaches the card and no achievement is evaluated; the baselines are
    // read once the UI (and the config save closing Settings does) is out of the way.
    r.configSavesAtStart = begin_diagnostic(r);
    // Taken before the first warp, whatever happens after: every zone visit is put back.
    r.savedInfo = std::make_unique<dSv_info_c>(*dComIfGs_getSaveInfo());
    AuroraGetRenderSize(&r.renderW, &r.renderH);

    auto& report = r.tourReport;
    report.buildId = build_id();
    report.buildTag = ALEKS2_BUILD_TAG;
    report.buildCommit = ALEKS2_BUILD_COMMIT;
    report.treeDirty = ALEKS2_BUILD_DIRTY != 0;
    report.builtAt = ALEKS2_BUILD_TIME;
    report.startedUnix = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    report.startedUtc = utc_stamp_iso(report.startedUnix);
    report.originalStage = r.original.stage;
    report.originalRoom = r.original.room;
    report.originalPoint = r.original.point;
    report.originalLayer = r.original.layer;
    capture_environment(report.envAtStart);
    report.envAtStart.minWidth = report.envAtStart.maxWidth = r.renderW;
    report.envAtStart.minHeight = report.envAtStart.maxHeight = r.renderH;
    const auto& s = getSettings();
    report.interpRequestedMode = interp_name(s.game.enableFrameInterpolation.getValue());
    report.interpQuality = s.game.interpolationQuality.getValue() == InterpQuality::Fancy ? "fancy" : "fast";
    report.predictiveInterpolation = s.game.predictiveInterpolation.getValue();
    report.uniformReuseModeAtStart = fifo_report::mode_name(aurora_gx_get_uniform_reuse_mode());
    report.pipelineMemo = s.game.pipelineMemo.getValue();
    report.maxFrameRate = s.video.maxFrameRate.getValue();
    report.vsync = s.video.enableVsync.getValue();
    char backend[64], device[128], driver[256];
    aurora_get_adapter_info(backend, sizeof backend, device, sizeof device, driver, sizeof driver);
    report.graphicsApi = backend;
    report.adapter = device;
    report.driver = driver;
    for (const auto& mod : mods::ModLoader::instance().active_mods()) {
        report.mods.push_back({mod.metadata.id, mod.metadata.version});
    }
    report.pipelinesKnownAtStart = r.createdAtRunStart;
    report.queuedAtStart = queued_pipelines();
    report.zones.assign(perf_tour::zones().size(), perf_tour::ZoneResult{});

    Log.info("performance tour started from {} room {} point {}: {} zones ({} {}, profile {}, interpolation {} {}, "
             "uniform reuse {}, companion {})",
        r.original.stage, r.original.room, r.original.point, perf_tour::zones().size(), report.envAtStart.mode,
        fmt::format("{}x{}", r.renderW, r.renderH), report.envAtStart.profile, report.interpRequestedMode,
        report.interpQuality, report.uniformReuseModeAtStart, report.envAtStart.companionEnabled ? "on" : "off");
    toast("Performance tour", "Three places, measured one after the other (about 3-5 minutes). Controls are "
                              "locked. Press Start to cancel.",
        5);
    begin_tour_zone();
    return true;
}

bool tour_active() {
    return g_run.tour && g_run.phase != Phase::Idle;
}

void request_cancel(const char* reason) {
    if (!active() || g_run.phase == Phase::Restoring) {
        return;
    }
    g_run.cancelPending = true;
    g_run.cancelReason = reason != nullptr ? reason : "cancelled";
}

void filter_pad(interface_of_controller_pad* pads, int count) {
    if (!active()) {
        return;
    }
    if (pads != nullptr && count > 0 && (pads[0].mPressedButtonFlags & PAD_BUTTON_START) != 0) {
        request_cancel("cancelled with Start");
    }
    // The same state a disconnected pad produces, which every consumer already
    // handles: no buttons, no sticks, nothing latched.
    for (int i = 0; i < count; ++i) {
        std::memset(&pads[i], 0, sizeof(interface_of_controller_pad));
    }
}

void tick() {
    auto& r = g_run;
    if (r.phase == Phase::Idle) {
        return;
    }

    AuroraGetRenderSize(&r.renderW, &r.renderH);
    const auto* stats = aurora_get_stats();
    const unsigned queued = stats != nullptr ? stats->queuedPipelines : 0;

    if (r.cancelPending && r.phase != Phase::Restoring) {
        r.cancelPending = false;
        cancel_now(r.cancelReason, Outcome::Cancelled);
        return;
    }

    // A matrix is about one console mode: docking or undocking (or Flip Grip changing) in the
    // middle makes the three reports incomparable, so it stops and gives the settings back.
    if (r.matrix && r.phase != Phase::Restoring && r.phase != Phase::Loading &&
        (perf::is_docked() != r.matrixDocked || dualscreen::flipActive() != r.matrixFlip))
    {
        cancel_now("the console mode changed (dock or Flip Grip)", Outcome::Cancelled);
        return;
    }

    // The player opening the menu mid-run would put UI in the measurement, and
    // the menu is how they would ask to stop. Treat it as a stop.
    if ((r.phase == Phase::Applying || r.phase == Phase::Warmup || r.phase == Phase::PipelineWait ||
            r.phase == Phase::Measuring || r.phase == Phase::PrepRender || r.phase == Phase::TourPipelineWait ||
            r.phase == Phase::TourWarmup || r.phase == Phase::TourMeasuring) &&
        seconds_since(r.phaseStart) > 1.5 && ui_window_visible())
    {
        cancel_now("a menu was opened", Outcome::Cancelled);
        return;
    }

    switch (r.phase) {
    case Phase::Loading:
        g_status = fmt::format("Benchmark: loading {} ...", kScene);
        if (at_scene() && stage_settled(Place{kScene, kSceneRoom, kScenePoint, kSceneLayer})) {
            Log.info("scene loaded after {:.1f} s", seconds_since(r.phaseStart));
            set_phase(r.matrix ? Phase::Applying : Phase::Warmup);
        } else if (seconds_since(r.phaseStart) > std::chrono::duration<double>(kLoadTimeout).count()) {
            cancel_now("the scene did not finish loading", Outcome::Failed);
        }
        break;

    case Phase::Applying: {
        if (!at_scene()) {
            cancel_now("left the benchmark scene", Outcome::Failed);
            break;
        }
        const int which = r.matrixProfiles[r.matrixIndex];
        if (!r.profileApplied) {
            std::string why;
            if (!profile::applyOverride(which, why)) {
                cancel_now(std::string("could not apply the ") + profile::name(which) + " profile: " + why,
                    Outcome::Failed);
                break;
            }
            r.profileApplied = true;
            r.profile = which;
            r.profileFromMatrix = true;
            Log.info("matrix {}/{}: {} applied as an override (not saved)", r.matrixIndex + 1,
                r.matrixProfiles.size(), profile::name(which));
        }
        const double left = kProfileSettleSeconds - seconds_since(r.phaseStart);
        g_status = fmt::format("Profile matrix {}/{}: {} - settling ({:.0f} s)", r.matrixIndex + 1,
            r.matrixProfiles.size(), profile::name(which), std::max(left, 0.0));
        if (left <= 0.0) {
            set_phase(Phase::Warmup);
        }
        break;
    }

    case Phase::Warmup: {
        if (!at_scene()) {
            cancel_now("left the benchmark scene", Outcome::Failed);
            break;
        }
        const double left = kWarmupSeconds - seconds_since(r.phaseStart);
        g_status = r.matrix ? fmt::format("Profile matrix {}/{}: warming up ({:.0f} s)", r.matrixIndex + 1,
                                 r.matrixProfiles.size(), std::max(left, 0.0))
                            : fmt::format("Benchmark: warming up ({:.0f} s)", std::max(left, 0.0));
        if (left <= 0.0) {
            r.queueWasEmpty = false;
            set_phase(Phase::PipelineWait);
        }
        break;
    }

    case Phase::PipelineWait: {
        if (!at_scene()) {
            cancel_now("left the benchmark scene", Outcome::Failed);
            break;
        }
        const double waited = seconds_since(r.phaseStart);
        if (queued == 0) {
            if (!r.queueWasEmpty) {
                r.queueWasEmpty = true;
                r.queueEmptySince = clock::now();
            }
        } else {
            r.queueWasEmpty = false;
        }
        const bool drained = r.queueWasEmpty && clock::now() - r.queueEmptySince >= kQueueQuiet;
        const bool timedOut = waited >= kPipelineWaitTimeoutSeconds;
        g_status = r.matrix ? fmt::format("Profile matrix {}/{}: waiting for shaders ({} queued, {:.0f} s)",
                                 r.matrixIndex + 1, r.matrixProfiles.size(), queued, waited)
                            : fmt::format("Benchmark: waiting for shaders ({} queued, {:.0f} s)", queued, waited);
        if (drained || timedOut) {
            r.queueTimedOut = !drained;
            r.queueWaitSeconds = waited;
            r.queuedAtWaitEnd = queued;
            if (r.queueTimedOut) {
                Log.warn("pipeline queue still at {} after {} s; measuring anyway", queued,
                    kPipelineWaitTimeoutSeconds);
                toast("Benchmark: shaders still compiling",
                    fmt::format("{} still queued after {} s. Measuring anyway; the report says so.", queued,
                        kPipelineWaitTimeoutSeconds),
                    4);
            }
            begin_measuring();
        }
        break;
    }

    case Phase::Measuring: {
        if (!at_scene()) {
            cancel_now("left the benchmark scene", Outcome::Failed);
            break;
        }
        sample_frame();
        const double elapsed = seconds_since(r.measureStart);
        g_status = fmt::format("{}: measuring {:.0f}/{} s - {:.0f} FPS",
            r.matrix ? fmt::format("Profile matrix {}/{} ({})", r.matrixIndex + 1, r.matrixProfiles.size(),
                           profile::name(r.profile))
                     : std::string("Benchmark"),
            std::min<double>(elapsed, kMeasureSeconds), kMeasureSeconds, aurora_get_fps());
        if (elapsed >= kMeasureSeconds) {
            finish_measurement();
        }
        break;
    }

    case Phase::PrepLoading:
    case Phase::PrepRender:
        tick_prep(queued);
        break;

    case Phase::TourLoading:
    case Phase::TourPipelineWait:
    case Phase::TourWarmup:
    case Phase::TourMeasuring:
        tick_tour(queued);
        break;

    case Phase::Restoring: {
        g_status = r.prep ? "Preparing shaders: going back ..." :
                   r.tour ? "Performance tour: going back ..." :
                            "Benchmark: putting things back ...";
        if (r.restoreRequested && !r.restored) {
            if (stage_settled(r.original)) {
                restore_game_state();
                r.restored = true;
                r.restoredAt = clock::now();
            } else if (seconds_since(r.phaseStart) > std::chrono::duration<double>(kLoadTimeout).count()) {
                // Could not get back; do not leave the card locked forever.
                Log.error("could not return to {} within {} s", r.original.stage,
                    std::chrono::duration_cast<std::chrono::seconds>(kLoadTimeout).count());
                r.restoreTimedOut = true;
                r.restored = true;
                r.restoredAt = clock::now();
            }
        }
        if (r.restored && clock::now() - r.restoredAt >= kRestoreGrace) {
            end_run();
        }
        break;
    }

    case Phase::Idle:
        break;
    }
}

}  // namespace dusk::benchmark
