#pragma once
// aleks2 (SHADERS-c1): shader preparation at startup -- the part that knows nothing about
// the game or the UI, so it builds and is tested on a PC (platforms/switch/tools/
// test-shader-warmup.sh).
//
// At startup Aurora queues every pipeline recipe it knows -- the console's own
// pipeline_cache.db plus the list shipped in the NRO (romfs:/initial_pipeline_cache.db) --
// for compilation in the background, and Dawn stores each result in dawn_cache.db. The
// first time (or after a driver, schema, list or render-target change) that is minutes of
// work on a Tegra X1, so the game shows "Preparing shaders" with a counter and a Skip
// button. Once a pass completes, a marker (cache/shader_prep_done.txt) records the key it
// was done for; while the key still matches, later launches compile in the background
// (almost all cache hits) without showing the screen.
//
// Nothing here is a benchmark: the benchmark refuses to start while the pass is running.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dusk::shader_warmup {

/// What the preparation was done for. Any field changing makes the old work stale.
struct Key {
    std::string driver;          ///< "<device>|<driver>" as Dawn reports them
    std::uint32_t pipelineSchema = 0;
    std::uint32_t gxVersion = 0;
    std::uint32_t clearVersion = 0;
    std::uint32_t rmlVersion = 0;
    std::uint32_t dawnSchema = 0;
    std::uint64_t listSignature = 0;  ///< the shipped list (0: none shipped)
    std::uint64_t layoutKey = 0;      ///< render target layout (MSAA, formats...)
    std::uint64_t runtimeIdentity = 0;  ///< SHADERS-c2: shader-generating code + Dawn revision

    bool operator==(const Key&) const = default;
};

enum class Reason : std::uint8_t {
    None,           ///< the marker matches: nothing to show
    FirstLaunch,    ///< no marker yet (a clean card, or never completed)
    DriverChanged,
    SchemaChanged,  ///< pipeline or cache format version
    RuntimeChanged, ///< SHADERS-c2: the code that makes shaders from recipes changed
    ListChanged,    ///< a new NRO ships a different pipeline list
    RenderChanged,  ///< an option that changes the render target layout
    CacheRepaired,  ///< a damaged cache file was set aside and recreated this launch
};

/// Why the preparation must run, comparing the marker (if any) with now.
Reason decide(const std::optional<Key>& marker, const Key& now, bool cacheRepaired);
const char* reason_name(Reason reason);   ///< machine name, for logs and the marker
const char* reason_text(Reason reason);   ///< what the player reads

/// The marker: "key=value" lines. Unknown lines are ignored; a missing or malformed
/// field makes it unreadable (std::nullopt), which counts as "first launch".
std::string marker_text(const Key& key, std::int64_t completedUnix, double seconds, std::uint32_t pipelines);
std::optional<Key> parse_marker(std::string_view text);

/// A time estimate, only once there is enough to go on: at least `minDone` pipelines and
/// `minSeconds` of samples since the first one. Rate = done / elapsed since the start.
struct Eta {
    double minSeconds = 8.0;
    std::uint32_t minDone = 20;

    void start(double now, std::uint32_t done);
    std::optional<double> seconds_left(double now, std::uint32_t done, std::uint32_t total) const;

    double startTime = -1.0;
    std::uint32_t startDone = 0;
};

/// "312 / 584"
std::string counter_text(std::uint32_t done, std::uint32_t total);
/// "About 2 min left" / "About 40 s left" / "" when there is no estimate yet.
std::string eta_text(std::optional<double> secondsLeft);

/// Lab options, read at startup from sdmc:/switch/dusklight/shader-lab.ini ("key=value",
/// '#' comments). None of this is needed in normal play; without the file nothing changes.
enum class PrepMode : std::uint8_t {
    Auto,   ///< show the screen when decide() says so (default)
    Force,  ///< show it every launch (testing the screen, timing a cold pass)
    Off,    ///< never show it (the background compile still happens)
};

struct LabOptions {
    std::string journal;  ///< AURORA_CACHE_JOURNAL (wal, delete, truncate, persist, memory)
    std::string sync;     ///< AURORA_CACHE_SYNC (off, normal, full)
    std::string check;    ///< AURORA_CACHE_CHECK (dirty, always, never)
    PrepMode prep = PrepMode::Auto;
    bool sqliteSelftest = true;  ///< V1-c3: probe the card's file layer at startup and pick the journal
    bool masterTour = false;
    bool harvestTour = false;    ///< V1-c4: shader_tour=all, every room of the warp table  ///< Prepare Shaders visits the full Shader Tour and exports the list
    unsigned warmupDuty = 0;
    bool warmAll = false;     ///< V1-rc5: warmup_scope=all, a normal start warms the whole list as before rc5
    unsigned seedLimit = 0;   ///< V1-c5: AURORA_SEED_LIMIT (0: the whole bundled list)  ///< SHADERS-c2: background pacing 10-100 % while playing (0: Aurora's default, 100)
    std::string unknown;      ///< keys or values not understood, for the log
};

LabOptions parse_lab_options(std::string_view text);

}  // namespace dusk::shader_warmup
