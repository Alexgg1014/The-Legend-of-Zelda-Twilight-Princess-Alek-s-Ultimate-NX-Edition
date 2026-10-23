#include "dusk/shader_warmup_runtime.hpp"

#include "dusk/main.h"
#include "dusk/benchmark.hpp"
#include "dusk/session_log.hpp"
#include "dusk/dev_mode.hpp"
#include "dusk/settings.h"
#include "dusk/mod_loader.hpp"

#include <aurora/aurora.h>
#include <aurora/shader_cache.h>
#include <borealis/log.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

// V1-rc9: perf-lab.ini warmup_fade_boost (frame_telemetry.cpp); on unless it says 0.
bool g_duskWarmupFadeBoost = true;

namespace dusk::shader_warmup {
namespace {
constexpr borealis::Log Log{"dusk::shader_prep"};
constexpr const char* kLabFile = "shader-lab.ini";
constexpr const char* kMarkerFile = "shader_prep_done.txt";
// V1-rc1: the SQLite self-test runs once per build; its choice is kept here.
constexpr const char* kSqliteChoiceFile = "sqlite-choice.txt";
// V1-rc5: what the choice depends on is the card and the SQLite file layer, not the build: it was keyed on the
// build id, so every update ran the 12.9 s self-test again. Change this when SQLite or the libnx I/O shims change.
constexpr const char* kSqliteChoiceKey = "sqlite-layer-1";

using clock = std::chrono::steady_clock;

LabOptions g_lab;
bool g_begun = false;
bool g_screenWanted = false;
bool g_screenOn = false;
bool g_skipped = false;
bool g_markerWritten = false;
Reason g_reason = Reason::None;
// V1-rc6: a runtime change is probed, not prepared: the Dawn cache usually still holds every pipeline (rc5 on
// Alex's console: 13.5 min of Preparing for 18343 hits and 0 stores). The scoped warmup runs, and if a sample of
// its lookups misses the cache, the whole list is warmed in the background.
bool g_probing = false;
std::uint32_t g_probeHits0 = 0;
std::uint32_t g_probeMisses0 = 0;
std::uint32_t g_probeStores0 = 0;
std::uint32_t g_probeDone0 = 0;
constexpr std::uint32_t kProbeLookups = 64;
constexpr std::uint32_t kProbePipelines = 32;
// V1-rc7: the wait in a stage's loading screen, and the first 2 s after it.
bool g_sceneWaiting = false;
clock::time_point g_sceneWaitStart;
clock::time_point g_sceneEntered;
bool g_sceneMeasuring = false;
double g_sceneWaitedMs = 0.0;
std::uint32_t g_sceneLeft = 0;
clock::time_point g_lastFrame;
std::uint32_t g_sceneHitches = 0;
double g_sceneWorstMs = 0.0;
Key g_keyAtBegin;
std::uint32_t g_generation = 0;
clock::time_point g_start;
clock::time_point g_lastLog;
Eta g_eta;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

double seconds_since(clock::time_point t) {
    return std::chrono::duration<double>(clock::now() - t).count();
}

AuroraShaderCacheStatus status() {
    AuroraShaderCacheStatus st{};
    aurora_get_shader_cache_status(&st);
    return st;
}

Key current_key(const AuroraShaderCacheStatus& st) {
    Key key;
    char driver[256] = {};
    aurora_get_driver_identity(driver, sizeof driver);
    key.driver = driver;
    key.pipelineSchema = st.pipelineSchema;
    key.gxVersion = st.gxConfigVersion;
    key.clearVersion = st.clearConfigVersion;
    key.rmlVersion = st.rmlConfigVersion;
    key.dawnSchema = st.dawnSchema;
    key.listSignature = st.seedSignature;
    key.layoutKey = st.layoutKey;
    key.runtimeIdentity = st.runtimeIdentity;
    return key;
}

void log_status(const char* what, const AuroraShaderCacheStatus& st) {
    Log.info("{}: {}/{} pipelines, {:.1f} s, compile {:.0f} ms total ({:.0f} ms max); dawn {} hits {} misses {} "
             "stores ({:.1f} ms avg); known {}, seed {} rows",
        what, st.warmupDone, st.warmupQueued, seconds_since(g_start), st.warmupCompileMs, st.warmupCompileMaxMs,
        st.dawnHits, st.dawnMisses, st.dawnStores, st.dawnStores ? st.dawnStoreMsTotal / st.dawnStores : 0.0,
        st.knownPipelines, st.seedRowsMerged);
    // V1-rc1: where a GX pipeline's time goes (per pipeline, averages over this launch).
    const double n = st.gxPipelinesTimed != 0 ? static_cast<double>(st.gxPipelinesTimed) : 1.0;
    Log.info("{} (split): {} GX pipelines, per pipeline WGSL text {:.1f} ms, shader module {:.1f} ms, pipeline "
             "{:.1f} ms; {} modules reused; scene {:08x} (last session {:08x}), {} recipes with a scene, {} moved "
             "ahead at the last scene change, {} dropped by a layout change",
        what, st.gxPipelinesTimed, st.gxSourceMs / n, st.gxModuleMs / n, st.gxPipelineMs / n, st.gxModulesReused,
        st.sceneTag, st.lastSceneTag, st.recipesWithScene, st.scenePromoted, st.warmupDropped);
}

}  // namespace

// V1-c3: before Aurora opens its caches, ask the card which journal works (see
// extern/aurora/lib/sqlite_selftest.cpp). One line per probe and pair in the log and in
// <data>/sqlite-selftest.txt; the chosen pair goes to Aurora unless shader-lab.ini set one.
static void save_sqlite_choice(const char* journal, const char* sync, int shims);

static void run_sqlite_selftest() {
    const auto dir = CachePath / "sqlite-selftest";
    std::string report(64 * 1024, '\0');
    char journal[16] = {};
    char sync[16] = {};
    const int result = aurora_sqlite_selftest(dir.string().c_str(), report.data(), report.size(), journal,
        sizeof journal, sync, sizeof sync);
    report.resize(std::strlen(report.c_str()));
    size_t start = 0;
    while (start < report.size()) {
        const size_t end = report.find('\n', start);
        const std::string line = report.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (line.find("FAIL") != std::string::npos || line.find("wrong") != std::string::npos ||
            line.find("WRONG") != std::string::npos || line.find("NOT") != std::string::npos)
        {
            Log.warn("{}", line);
        } else {
            Log.info("{}", line);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    {
        std::ofstream out(ConfigPath / "sqlite-selftest.txt", std::ios::binary | std::ios::trunc);
        out << report;
    }
    if (result < 0) {
        Log.error("sqlite selftest: no journal mode works on this card; the caches will probably be off");
        return;
    }
    if (g_lab.journal.empty()) {
        setenv("AURORA_CACHE_JOURNAL", journal, 1);
        if (g_lab.sync.empty()) {
            setenv("AURORA_CACHE_SYNC", sync, 1);
        }
    }
    if (result == 0) {
        setenv("AURORA_CACHE_LIBNX_SHIMS", "0", 1);  // works without them on this card
    }
    Log.warn("sqlite selftest chose journal={} sync={} libnx_shims={}{}", journal, sync, result,
        g_lab.journal.empty() ? "" : " (overridden by shader-lab.ini)");
    save_sqlite_choice(journal, sync, result);
}

// V1-rc1: the self-test costs 6-11 s of black screen at every start. Its answer only changes with the
// build (SQLite, the shims) or the card, so it is kept in <cache>/sqlite-choice.txt with the build id and
// reused; the file is removed when a cache turns out unusable, and deleting it runs the test again.
static bool reuse_sqlite_choice() {
    const std::string text = read_file(CachePath / kSqliteChoiceFile);
    std::string build, journal, sync, shims;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "build") {
            build = v;
        } else if (k == "journal") {
            journal = v;
        } else if (k == "sync") {
            sync = v;
        } else if (k == "libnx_shims") {
            shims = v;
        }
    }
    if (build.empty() || build != kSqliteChoiceKey || journal.empty() || sync.empty()) {
        return false;
    }
    if (g_lab.journal.empty()) {
        setenv("AURORA_CACHE_JOURNAL", journal.c_str(), 1);
        if (g_lab.sync.empty()) {
            setenv("AURORA_CACHE_SYNC", sync.c_str(), 1);
        }
    }
    if (shims == "0") {
        setenv("AURORA_CACHE_LIBNX_SHIMS", "0", 1);
    }
    Log.info("sqlite selftest skipped: journal={} sync={} libnx_shims={} as chosen for build {} (delete {} to test "
             "again)",
        journal, sync, shims, build, (CachePath / kSqliteChoiceFile).string());
    return true;
}

static void save_sqlite_choice(const char* journal, const char* sync, int shims) {
    std::ofstream out(CachePath / kSqliteChoiceFile, std::ios::binary | std::ios::trunc);
    out << "build=" << kSqliteChoiceKey << "\njournal=" << journal << "\nsync=" << sync
        << "\nlibnx_shims=" << shims << "\n";
}

void load_lab_options() {
    const auto path = ConfigPath / kLabFile;
    const std::string text = dev_mode() ? read_file(path) : std::string{};  // V1-rc5: developer-mode file
    if (text.empty()) {
        if (!reuse_sqlite_choice()) {
            run_sqlite_selftest();
        }
        return;
    }
    g_lab = parse_lab_options(text);
    // Aurora reads these when it opens its caches (sqlite_cache_open.cpp).
    if (!g_lab.journal.empty()) {
        setenv("AURORA_CACHE_JOURNAL", g_lab.journal.c_str(), 1);
    }
    if (!g_lab.sync.empty()) {
        setenv("AURORA_CACHE_SYNC", g_lab.sync.c_str(), 1);
    }
    if (!g_lab.check.empty()) {
        setenv("AURORA_CACHE_CHECK", g_lab.check.c_str(), 1);
    }
    if (g_lab.seedLimit != 0) {
        setenv("AURORA_SEED_LIMIT", std::to_string(g_lab.seedLimit).c_str(), 1);
    }
    if (g_lab.sqliteSelftest) {
        run_sqlite_selftest();
    }
    Log.warn("Lab options from {}: cache_journal={} cache_sync={} cache_check={} prep={} shader_tour={} "
             "warmup_duty={}{}",
        path.string(), g_lab.journal.empty() ? "default" : g_lab.journal,
        g_lab.sync.empty() ? "default" : g_lab.sync, g_lab.check.empty() ? "default" : g_lab.check,
        g_lab.prep == PrepMode::Force ? "force" : g_lab.prep == PrepMode::Off ? "off" : "auto",
        g_lab.masterTour ? "on" : "off",
        g_lab.warmupDuty == 0 ? std::string{"default"} : std::to_string(g_lab.warmupDuty),
        g_lab.unknown.empty() ? "" : " (ignored: " + g_lab.unknown + ")");
}

const LabOptions& lab_options() {
    return g_lab;
}

void begin() {
    g_begun = true;
    g_start = clock::now();
    g_lastLog = g_start;
    auto st = status();
    g_generation = st.warmupGeneration;
    g_keyAtBegin = current_key(st);
    const auto marker = parse_marker(read_file(CachePath / kMarkerFile));
    g_reason = decide(marker, g_keyAtBegin, st.pipelineCacheRepaired || st.dawnCacheRepaired);
    // V1-rc5: a normal start (this list already compiled for this driver) no longer walks all of it
    // again (2-3 min of "Compiling" on the rc4): the last scene, its neighbours and the most shared
    // recipes; each scene then warms its own as it loads; the rest compiles on demand from the cache.
    g_probing = g_reason == Reason::RuntimeChanged && g_lab.prep == PrepMode::Auto && !g_lab.warmAll;
    if ((g_reason == Reason::None || g_probing) && g_lab.prep != PrepMode::Force && !g_lab.warmAll) {
        constexpr std::uint32_t kSharedRecipes = 512;
        aurora_scope_shader_warmup(kSharedRecipes);
        st = status();
        g_probeHits0 = st.dawnHits;
        g_probeMisses0 = st.dawnMisses;
        g_probeStores0 = st.dawnStores;
        g_probeDone0 = st.warmupDone;
        if (g_probing) {
            Log.info("Shader preparation: the runtime changed; probing the Dawn cache with the scoped warmup "
                     "before preparing anything");
        }
    } else if (g_reason == Reason::None) {
        Log.info("Warmup scope: the whole list (warmup_scope=all or prep=force in shader-lab.ini)");
    }
    const bool work = st.warmupQueued > st.warmupDone;
    switch (g_lab.prep) {
    case PrepMode::Auto:
        g_screenWanted = g_reason != Reason::None && !g_probing && work;
        break;
    case PrepMode::Force:
        g_screenWanted = work;
        if (g_reason == Reason::None) {
            g_reason = Reason::FirstLaunch;
        }
        break;
    case PrepMode::Off:
        g_screenWanted = false;
        break;
    }
    // V1-rc12: no preparation screen with deko3d. Its pipelines come from the DKSH cache in romfs (the whole
    // list in ~10 s in the background); the screen and the marker belong to Dawn/NVK's first start.
    if (aurora_get_backend() == BACKEND_DEKO3D) {
        g_screenWanted = false;
    }
    // The pass already done for this key: nothing to write again unless it changes.
    g_markerWritten = g_reason == Reason::None && g_lab.prep != PrepMode::Force;
    g_eta.start(0.0, st.warmupDone);
    // While playing (no screen) the lab pacing applies; the screen always compiles flat out.
    if (g_lab.warmupDuty != 0) {
        aurora_set_shader_warmup_duty(g_lab.warmupDuty);
    }
    Log.info("Shader preparation: reason={} screen={} queued={} known={} seed={} rows (signature {:016x}) "
             "runtime {:016x} duty {}% "
             "pipeline_cache={}{}{} dawn_cache={}{}{} driver='{}'",
        reason_name(g_reason), g_screenWanted ? "yes" : "no", st.warmupQueued, st.knownPipelines, st.seedRowsMerged,
        st.seedSignature, st.runtimeIdentity, g_lab.warmupDuty == 0 ? 100u : g_lab.warmupDuty, st.pipelineJournal, st.pipelineCacheRepaired ? " repaired" : "",
        st.pipelineCacheDirty ? " dirty" : "", st.dawnJournal[0] ? st.dawnJournal : "(opens on first use)",
        st.dawnCacheRepaired ? " repaired" : "", st.dawnCacheDirty ? " dirty" : "", g_keyAtBegin.driver);
}

bool scene_shaders_pending() {
    const auto now = clock::now();
    const std::uint32_t pending = aurora_shader_scene_pending();
    if (!g_sceneWaiting) {
        g_sceneWaiting = true;
        g_sceneWaitStart = now;
        if (pending != 0 && g_duskWarmupFadeBoost) {
            aurora_shader_set_compile_boost(1);  // V1-rc9: the compile thread gets core 0 while the screen is black
        }
    }
    // V1-rc9: up to 2.5 s (rc8: the fade into Hyrule Field ended with 33 of its pipelines left, ~70 ms each).
    const int capMs = std::clamp(getSettings().game.shaderSceneWaitMs.getValue(), 0, 2500);
    const double waited = std::chrono::duration<double, std::milli>(now - g_sceneWaitStart).count();
    if (pending != 0 && waited < capMs) {
        return true;
    }
    aurora_shader_set_compile_boost(0);
    g_sceneWaiting = false;
    g_sceneWaitedMs = waited;
    g_sceneLeft = pending;
    g_sceneEntered = now;
    g_lastFrame = now;
    g_sceneMeasuring = true;
    g_sceneHitches = 0;
    g_sceneWorstMs = 0.0;
    return false;
}

void tick() {
    if (g_sceneMeasuring) {
        const auto now = clock::now();
        const double frameMs = std::chrono::duration<double, std::milli>(now - g_lastFrame).count();
        g_lastFrame = now;
        if (frameMs > 50.0) {
            ++g_sceneHitches;
        }
        g_sceneWorstMs = std::max(g_sceneWorstMs, frameMs);
        if (std::chrono::duration<double>(now - g_sceneEntered).count() >= 2.0) {
            g_sceneMeasuring = false;
            Log.info("Scene entered: loading screen waited {:.0f} ms for its shaders ({} still queued); first 2 s: {} "
                     "frame(s) over 50 ms, worst {:.0f} ms",
                g_sceneWaitedMs, g_sceneLeft, g_sceneHitches, g_sceneWorstMs);
        }
    }
    if (!g_begun) {
        return;
    }
    const auto st = status();
    if (g_probing) {
        // V1-rc7: decided by STORES, not misses. Every pipeline also makes one Dawn lookup that is never stored
        // (hits == misses with 0 stores in every normal start since rc4), so rc6's "misses > 20 %" always fired:
        // rc7's first start on Alex's console warmed the whole list again for 863 lookups and 0 stores. A store
        // is a pipeline Dawn really had to compile.
        const std::uint32_t hits = st.dawnHits - g_probeHits0;
        const std::uint32_t misses = st.dawnMisses - g_probeMisses0;
        if (st.warmupDone < g_probeDone0 || st.dawnStores < g_probeStores0) {
            // V1-rc8 (audit M2): the pipeline cache was rebuilt (its counters start again): so does the probe.
            g_probeDone0 = st.warmupDone;
            g_probeStores0 = st.dawnStores;
        }
        const std::uint32_t stores = st.dawnStores - g_probeStores0;
        const std::uint32_t done = st.warmupDone - g_probeDone0;
        if (done >= kProbePipelines || hits + misses >= kProbeLookups * 4 || st.warmupQueued <= st.warmupDone) {
            g_probing = false;
            if (stores * 5 > std::max<std::uint32_t>(done, 1)) {
                Log.warn("Shader probe: {} of {} pipelines had to be compiled ({} lookups, {} hits) after the runtime "
                         "change; warming the whole list in the background",
                    stores, done, hits + misses, hits);
                aurora_warm_all_shaders();
                return;
            }
            Log.info("Shader probe: {} of {} pipelines compiled, {} lookups ({} hits): the Dawn cache still fits; no "
                     "preparation needed, the rest compiles on demand",
                stores, done, hits + misses, hits);
        }
        return;  // no marker before the probe has spoken
    }
    const bool finished = st.warmupQueued <= st.warmupDone;
    if (!finished && seconds_since(g_lastLog) >= 15.0) {
        g_lastLog = clock::now();
        log_status("Shader preparation progress", st);
    }
    if (!finished || g_markerWritten) {
        return;
    }
    g_markerWritten = true;
    // aleks2 (deko3d P2): with the deko3d renderer the pipelines come from its DKSH cache and Dawn's
    // device is the Null one (no dawn_cache.db). The marker stays Dawn/NVK's: written here it would
    // tell the next Dawn start its list is compiled; and the SQLite choice is kept.
    if (aurora_get_backend() == BACKEND_DEKO3D) {
        log_status("Shader preparation finished with deko3d (DKSH cache; the marker is left to Dawn/NVK)", st);
        return;
    }
    // V1-c2: the marker says "this console has the list compiled". Not true when a cache
    // could not be used or nothing was compiled (V1-c1 on the console wrote pipelines=0,
    // and the next start skipped the preparation for good).
    if (!st.pipelineCacheOk || !st.dawnCacheOk || st.warmupDone == 0) {
        Log.warn("Shader preparation marker NOT written: pipeline_cache {} dawn_cache {} compiled {} (seed {} rows)",
            st.pipelineCacheOk ? "ok" : "unusable", st.dawnCacheOk ? "ok" : "unusable", st.warmupDone,
            st.seedRowsMerged);
        if (!st.pipelineCacheOk || !st.dawnCacheOk) {
            std::remove((CachePath / kSqliteChoiceFile).string().c_str());  // test the card again next start
        }
        log_status("Shader preparation ended without a usable cache", st);
        return;
    }
    log_status(g_skipped ? "Shader preparation finished in the background" : "Shader preparation finished", st);
    // The dawn cache opens lazily: its health is only known now.
    if (st.dawnCacheRepaired) {
        Log.warn("dawn_cache.db was repaired this launch");
    }
    const Key key = current_key(st);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const auto path = CachePath / kMarkerFile;
    const auto tmp = CachePath / "shader_prep_done.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << marker_text(key, now, seconds_since(g_start), st.warmupDone);
    }
    // Written aside first, so a power-off leaves the old marker or none, never half of one.
    // (cstdio, not std::filesystem: the latter takes "sdmc:/..." for a relative path.)
    std::remove(path.string().c_str());
    if (std::rename(tmp.string().c_str(), path.string().c_str()) != 0) {
        Log.warn("Could not write {}", path.string());
    }
}

Progress progress() {
    Progress p;
    const auto st = status();
    p.done = st.warmupDone;
    p.total = st.warmupQueued;
    p.finished = st.warmupQueued <= st.warmupDone;
    p.elapsedSeconds = g_begun ? seconds_since(g_start) : 0.0;
    p.counter = counter_text(p.done, p.total);
    p.eta = eta_text(g_eta.seconds_left(p.elapsedSeconds, p.done, p.total));
    return p;
}

bool screen_wanted() {
    return g_screenWanted;
}

void screen_shown() {
    g_screenWanted = false;
    g_screenOn = true;
    aurora_set_shader_warmup_duty(100);
    // V1-rc6: boosted again with native mods. rc5 left it off with them, suspecting it in three tester cuts on
    // the title; their crash reports showed an old (beta.2) Twilit package instead (now refused: packages.cpp),
    // and without the boost each pipeline took ~5x as long on Alex's console.
    session_log::cpuBoost(true);
    Log.info("Preparation screen shown ({})", reason_name(g_reason));
}

void screen_closed(bool skipped) {
    if (!g_screenOn) {
        return;
    }
    g_screenOn = false;
    g_skipped = skipped;
    session_log::cpuBoost(false);
    if (g_lab.warmupDuty != 0) {
        aurora_set_shader_warmup_duty(g_lab.warmupDuty);
    }
    const auto st = status();
    log_status(skipped ? "Preparation screen skipped" : "Preparation screen closed", st);
}

Reason reason() {
    return g_reason;
}

bool busy(std::string& why) {
    if (!g_begun) {
        return false;
    }
    const auto st = status();
    if (st.warmupQueued <= st.warmupDone) {
        return false;
    }
    why = fmt::format("Shaders are still being prepared ({} of {}); measuring now would count that work. "
                      "Wait for it to finish.",
        st.warmupDone, st.warmupQueued);
    return true;
}

}  // namespace dusk::shader_warmup
