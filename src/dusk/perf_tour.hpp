#pragma once
// aleks2 (H35): Run Performance Tour -- the part that knows nothing about the game.
//
// Three zones in a fixed order, heavy -> medium -> light, each measured on its own: warp,
// wait for the load, wait for the pipeline queue (with a timeout that is recorded), warm
// up, measure, keep the result, move on. The order is the point: if the interpolation
// governor stalls in the heavy zone, the report shows whether, and how fast, it recovers
// once the zones get lighter. Nothing resets the governor between zones.
//
// The run itself (warps, input lock, saves blocked, achievements suspended, restore)
// lives in benchmark.cpp with the benchmark and Prepare Shaders, whose protections it
// shares. This file is the plan, the per-zone rules, the interpolation recovery tracking
// and the report, so they build and are tested on a PC
// (platforms/switch/tools/test-perf-tour.sh).
#include "dusk/benchmark_report.hpp"
#include "dusk/fifo_report.hpp"
#include "dusk/interp_frames.hpp"
#include "dusk/shader_prep.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dusk::perf_tour {

// Bumped when a field changes meaning or is removed; adding a field does not.
inline constexpr int kReportSchemaVersion = 1;
inline constexpr const char* kSchema = "dusklight-aleks-performance-tour";

// Same shape as a Prepare Shaders zone: stage, room, point, layer, name, category.
using Zone = shader_prep::Zone;
using ZoneStatus = shader_prep::ZoneStatus;
using Outcome = shader_prep::Outcome;

/// Heavy, medium, light, in that order. Every entry is in the warp table
/// (src/dusk/map_loader_definitions.h); docs/switch-h35-fifo-plan.md gives the evidence.
std::span<const Zone> zones();

struct Timing {
    double loadTimeoutSeconds = 120.0;
    double pipelineQuietSeconds = 1.0;     ///< the queue must stay at zero this long
    double pipelineTimeoutSeconds = 45.0;  ///< then measure anyway; the report says so
    double warmupSeconds = 9.0;
    double measureSeconds = 30.0;
};

enum class PipelineWaitEnd : std::uint8_t { None, Drained, TimedOut };

struct PipelineWaitProgress {
    double waited = 0.0;         ///< seconds since the wait began
    double queueEmptyFor = 0.0;  ///< seconds the queue has been at zero (0 if it is not)
};

/// Pure: the caller measures, this decides.
PipelineWaitEnd decide_pipeline_wait(const PipelineWaitProgress& progress, const Timing& timing);

/// Planned seconds per zone once loaded, worst case (pipeline wait times out).
double max_seconds_per_zone_after_load(const Timing& timing);

/// The step a zone is in, for progress and the status line.
enum class Step : std::uint8_t { Loading, PipelineWait, Warmup, Measuring };

/// Overall progress in [0, 1]. Loading and the pipeline wait have no known length, so they
/// count as the first tenth of a zone; warm-up and measurement share the rest by time.
double overall_progress(std::size_t zoneIndex, std::size_t zoneCount, Step step, double stepSeconds,
    const Timing& timing);

/// Follows the governor's stalled flag from the moment a zone is reached: whether it was
/// stalled on arrival, whether it stalled in the zone, and how long it took to come back.
class RecoveryTracker {
public:
    void arrive(double t, bool stalled);
    void sample(double t, bool stalled);

    bool arrived() const { return mArrived; }
    bool stalledAtArrival() const { return mStalledAtArrival; }
    bool stalledNow() const { return mStalledNow; }
    bool stalledInZone() const { return mStalledInZone; }
    /// Seconds from arrival to the first frame not stalled, after being stalled on arrival
    /// or in the zone; < 0 if that did not happen.
    double recoverySeconds() const { return mRecoverySeconds; }
    bool recovered() const { return mRecoverySeconds >= 0.0; }

private:
    bool mArrived = false;
    double mArrivedAt = 0.0;
    bool mStalledAtArrival = false;
    bool mStalledNow = false;
    bool mStalledInZone = false;
    double mRecoverySeconds = -1.0;
};

/// Governor counters (game_clock::InterpTelemetry) as plain numbers, for a difference.
struct GovernorCounters {
    std::uint32_t enters = 0;
    std::uint32_t exits = 0;
    std::uint32_t attempts = 0;
    std::uint32_t failures = 0;
    std::uint32_t cooldownBlockedFrames = 0;
    double stalledSeconds = 0.0;
};

struct Interpolation {
    std::string requestedMode;  ///< off / capped / unlimited
    std::string quality;        ///< fast / fancy
    bool predictive = false;
    GovernorCounters window;     ///< over the measurement window (end minus start)
    interp_frames::Counters frames;  ///< over the measurement window
    double windowSeconds = 0.0;
    bool stalledAtMeasureStart = false;
    bool stalledAtMeasureEnd = false;
    RecoveryTracker recovery;   ///< from arrival in the zone to the end of the measurement
};

struct Loading {
    double loadSeconds = 0.0;
    unsigned queuedAtArrival = 0;
    unsigned queuedMax = 0;               ///< over the whole zone
    unsigned queuedMaxInWindow = 0;
    unsigned createdInZone = 0;           ///< from the warp to the end of the measurement
    unsigned createdInWindow = 0;
    bool pipelineWaitTimedOut = false;
    double pipelineWaitSeconds = 0.0;
    unsigned queuedAtWaitEnd = 0;
};

/// compiling (queue busy during the measurement), cold (pipelines created in the zone) or warm.
const char* cache_state(const Loading& loading);

struct Environment {
    std::string mode;  ///< dock / handheld / flip_grip / desktop
    bool docked = false;
    bool flipGrip = false;
    bool modeChangedDuringWindow = false;
    unsigned renderWidth = 0, renderHeight = 0;
    unsigned minWidth = 0, minHeight = 0, maxWidth = 0, maxHeight = 0;
    std::string profile;
    bool companionEnabled = false;
    std::string companionLayout;
    bool clocksAvailable = false;
    unsigned cpuMhz = 0, gpuMhz = 0, emcMhz = 0;
};

struct CpuZone {
    std::string name;
    double avgMs = 0.0;
    double maxMs = 0.0;
    double callsPerFrame = 0.0;
};

struct ZoneResult {
    ZoneStatus status = ZoneStatus::Pending;
    std::string detail;
    std::string arrivedStage;
    int arrivedRoom = -1;
    bool measured = false;  ///< the measurement window ran (fully or in part)
    bool measurementComplete = false;
    double measureSecondsWall = 0.0;
    benchmark::FrameSummary frames;
    std::vector<CpuZone> cpuZones;
    fifo_report::ZoneTimes zoneTimes;
    fifo_report::Window fifo;
    std::string uniformMismatchSection;
    std::uint64_t framesWithEvent = 0;  ///< frames of the window with an event running
    std::uint64_t framesWithUiVisible = 0;
    Interpolation interp;
    Loading loading;
    Environment env;
};

struct ModInfo {
    std::string id;
    std::string version;
};

struct TourReport {
    Outcome outcome = Outcome::Completed;
    std::string detail;  ///< why it was cancelled or failed

    std::string buildId, buildTag, buildCommit, builtAt;
    bool treeDirty = false;
    std::string startedUtc;
    std::int64_t startedUnix = 0;
    std::string fileName;
    double totalSeconds = 0.0;

    // Where the player was, and the configuration the tour ran with (it changes none).
    std::string originalStage;
    int originalRoom = -1;
    int originalPoint = 0;
    int originalLayer = -1;
    Environment envAtStart;
    std::string interpRequestedMode, interpQuality;
    bool predictiveInterpolation = false;
    std::string uniformReuseModeAtStart;
    bool pipelineMemo = false;
    int maxFrameRate = 0;
    bool vsync = false;
    std::string graphicsApi, adapter, driver;
    std::vector<ModInfo> mods;
    unsigned pipelinesKnownAtStart = 0;
    unsigned queuedAtStart = 0;

    std::vector<ZoneResult> zones;  ///< one per zones() entry, same order

    // Confirmations, filled in once the player is back.
    bool returned = false;
    bool stateRestored = false;
    bool savesReenabled = false;
    std::uint64_t configSavesDuringRun = 0;
    bool achievementsSuspendedDuringRun = false;
};

/// tour-<stamp>-<build id>.json, safe on a FAT card.
std::string report_file_name(std::string_view stamp, std::string_view buildId);

/// The JSON report (schema "dusklight-aleks-performance-tour").
std::string build_report(const TourReport& report, std::span<const Zone> plan);

/// What the player reads when it is over.
std::string summary_text(const TourReport& report, std::span<const Zone> plan);

}  // namespace dusk::perf_tour
