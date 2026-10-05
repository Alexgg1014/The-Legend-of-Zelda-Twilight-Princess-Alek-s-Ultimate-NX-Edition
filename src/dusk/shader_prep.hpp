#pragma once
// aleks2 (H34): Prepare Shaders -- the part that knows nothing about the game.
//
// Prepare Shaders visits a handful of zones so that the pipelines they draw are
// created once, while the player waits, instead of in the middle of play. What a
// visit leaves behind is what Aurora already keeps across launches: the recipes in
// pipeline_cache.db and Dawn's compiled blobs in dawn_cache.db, both on this console,
// both written by Aurora's own code. Nothing is shipped in the NRO and nothing is
// downloaded. It is not a benchmark: it measures no frame rate.
//
// The run itself (warps, input lock, saves blocked, restore) lives in benchmark.cpp,
// next to the benchmark and the profile matrix whose protections it reuses. This file
// is the zone list, the per-zone stopping rule and the report, so they build and are
// tested on a PC (platforms/switch/tools/test-shader-prep.sh).
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dusk::shader_prep {

// Bumped when a field changes meaning or is removed; adding a field does not.
inline constexpr int kReportSchemaVersion = 1;

struct Zone {
    const char* stage;  ///< stage file, as in map_loader_definitions.h
    int room;
    int point;
    int layer;             ///< -1: the layer the save's progress selects
    const char* name;      ///< as the warp menu names it
    const char* category;  ///< what it adds to the coverage
};

/// The tour, in order. Every stage, room and point is taken from the warp table
/// (src/dusk/map_loader_definitions.h); docs/switch-h34-content-preparation.md gives
/// the evidence for each one.
std::span<const Zone> zones();

struct Timing {
    double minRenderSeconds = 15.0;  ///< always drawn at least this long once loaded
    double quietSeconds = 3.0;       ///< the queue must stay empty this long
    double idleSeconds = 10.0;       ///< and no new pipeline for this long
    double maxZoneSeconds = 150.0;   ///< then move on regardless
    double loadTimeoutSeconds = 120.0;
    double eventWaitSeconds = 30.0;  ///< a cutscene on arrival may finish first
};

enum class ZoneEnd : std::uint8_t {
    None,      ///< keep drawing
    Warm,      ///< nothing new appeared: this zone was already prepared
    Drained,   ///< new pipelines appeared, all compiled, nothing new for a while
    TimedOut,  ///< the per-zone limit; the rest compiles in the background later
};

struct ZoneProgress {
    double sinceSettled = 0.0;      ///< seconds drawn since the zone finished loading
    double sinceLastCreated = 0.0;  ///< seconds since a pipeline was last created
    double queueEmptyFor = 0.0;     ///< seconds the queue has been empty (0 if not)
    unsigned queued = 0;
    unsigned createdInZone = 0;
};

/// The stopping rule. Pure: the caller measures, this decides.
ZoneEnd decide(const ZoneProgress& progress, const Timing& timing);

enum class ZoneStatus : std::uint8_t { Pending, Completed, Skipped, Failed, Cancelled };
enum class Outcome : std::uint8_t { Completed, Cancelled, Failed };

struct ZoneResult {
    ZoneStatus status = ZoneStatus::Pending;
    ZoneEnd end = ZoneEnd::None;
    std::string detail;            ///< why it was skipped, failed or cancelled
    double loadSeconds = 0.0;
    double renderSeconds = 0.0;
    unsigned queuedAtStart = 0;    ///< when the zone finished loading
    unsigned queuedMax = 0;
    unsigned queuedAtEnd = 0;
    unsigned created = 0;          ///< pipelines created while in the zone
    std::string arrivedStage;      ///< where the game actually was
    int arrivedRoom = -1;
};

struct TourReport {
    Outcome outcome = Outcome::Completed;
    std::string detail;
    std::string buildId;
    std::string startedUtc;
    std::int64_t startedUnix = 0;
    std::string fileName;
    std::string mode;  ///< dock / handheld / flip_grip / desktop
    std::string originalStage;
    int originalRoom = -1;
    double totalSeconds = 0.0;
    unsigned queuedAtStart = 0;
    unsigned queuedMax = 0;
    unsigned queuedAtEnd = 0;
    unsigned createdTotal = 0;
    unsigned knownAtStart = 0;  ///< pipelines created before the tour began (since launch)
    std::vector<ZoneResult> zones;  ///< one per zones() entry, same order
    // Confirmations, filled in once the player is back.
    bool returned = false;          ///< back at the original place (or never left)
    bool stateRestored = false;     ///< the in-memory save state was put back
    bool savesReenabled = false;
    std::uint64_t configSavesDuringRun = 0;  ///< config::save() calls; should be 0
};

const char* zone_status_name(ZoneStatus status);
const char* zone_end_name(ZoneEnd end);
const char* outcome_name(Outcome outcome);

/// Overall progress in [0, 1]: finished zones plus how far the current one is.
double overall_progress(std::size_t zoneIndex, std::size_t zoneCount, double zoneFraction);

/// shaders-<stamp>-<build id>.json, safe on a FAT card.
std::string report_file_name(std::string_view stamp, std::string_view buildId);

/// The JSON report (schema "dusklight-aleks-shader-prep").
std::string build_report(const TourReport& report, std::span<const Zone> plan);

/// What the player reads when it is over.
std::string summary_text(const TourReport& report, std::span<const Zone> plan);

}  // namespace dusk::shader_prep
