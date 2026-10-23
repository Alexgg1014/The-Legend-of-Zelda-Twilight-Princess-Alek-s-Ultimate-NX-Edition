#pragma once
// aleks2 (V1-c1): the part of frame telemetry and dynamic resolution that knows nothing about
// the game -- runtime lab options, per-frame CPU/GPU attribution, window statistics, the
// dynamic-resolution policy and the stress pattern. Kept apart so it builds and is tested on a
// PC with a plain compiler (platforms/switch/tools/test-perf-core.sh). The game-side glue is
// frame_telemetry.cpp, dynres.cpp and drs_stress.cpp.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dusk::perf {

// ---------------------------------------------------------------------------------------------
// Runtime options: <data>/perf-lab.ini (sdmc:/switch/dusklight/perf-lab.ini on the console).
// Without the file every default below applies. One build answers every value: nothing here
// needs a new NRO.
//
// V1-rc16: renderer A/B switches (deko3d), read once at start-up -- perf_core.cpp sets the environment before
// Aurora starts; a change needs a relaunch. All off by default; platforms/switch/perf-lab.example.ini lists them:
//   dk_cached_upload = 1       staging slots CPU-cached (COS_DK_CACHED_UPLOAD); pair it with fifo_fast = urep
//                              (AURORA_FIFO_FAST_UNIFORM_IN_PLACE: uniforms built in place in the staging slot)
//   dk_skip_present_clear = 1  the present's colour clear left out when the picture covers the image
//   dk_zcull = 1               the queue's zcull (COS_DK_ZCULL); SwitchWakerHD saw depth-biased decals lose tiles
//   dk_compression = 1         hardware compression of colour render targets (COS_DK_COMPRESSION)
// ---------------------------------------------------------------------------------------------

enum class GpuTiming : std::uint8_t {
    Auto,    // timestamps written only while something reads them (benchmark, DRS Auto, stress)
    Always,  // written every frame from launch (for the session log)
    Off,     // the feature is not even requested from the device
};

struct LabOptions {
    GpuTiming gpuTiming = GpuTiming::Auto;
    // Render-target sets Aurora keeps while DRS Auto or the stress mode runs (0 = recreate on
    // every change, the pre-V1-c1 behaviour; at most 6).
    unsigned rtPool = 4;
    // DRS steps as a percentage of the ceiling rows, highest first. 2-5 entries, 50-100.
    std::vector<int> drsSteps{100, 85, 75};
    // What DRS Auto aims for. The V1 goal is 30 at stock clocks.
    int drsTargetFps = 30;
    // One log line per DRS window, not only per decision.
    bool drsVerbose = false;
    // Stress mode.
    unsigned stressTransitions = 120;
    unsigned stressIntervalMs = 2000;
    bool stressLadder = false;  // false: the DRS steps; true: 900 -> 720 -> 540 rows absolute
    // FIFO-opt: Aurora's GX FIFO fast paths (aurora_gx_set_fifo_fast), AURORA_FIFO_FAST_* bits.
    // fifo_fast = off | all | a list of bind, idx, urep, pipe, pop (or 0-31). Off by default.
    unsigned fifoFast = 0;
    // hud_fast: the minimap is redrawn only when its inputs change (d_map.cpp). rc9: on by default.
    bool hudFast = true;
    // minimap_pan: the minimap's path is rendered once with a margin and panned (d_map.cpp). V1-rc10: on by
    // default (rc9 console, Hyrule Field with the full HUD: FIFO p50 25.3 -> 16.3 ms, 26.9 -> 29.7 FPS).
    bool minimapPan = true;
    // V1-rc10: hud_cull_alpha0 -- 2D panes whose effective alpha is 0 are not drawn (J2DPane::draw), counted in
    // the heartbeat either way. Off by default.
    bool hudCullAlpha0 = false;
    // V1-rc10: exit_mode -- how the process ends (switch_startup.cpp): auto (0) ends it with svcExitProcess
    // (back to HOME) unless a relaunch was requested and the memory audit at exit finds the heap clean; loader (1)
    // always returns to the loader (the rc9 behaviour, for testing); process (2) always ends it.
    unsigned exitMode = 0;
    // V1-rc12: interp_floor_fps -- with Capped interpolation at 60, interpolation gives up below this rate (a steady
    // 30 instead of an uneven 36-50) and comes back when the frame's work leaves room for 60. 0 = the rc11 rule (give
    // up only below 30).
    // V1-rc16: default 0. Capped 60 must never end up worse than Unlimited (decided with Alex, 2026-10-20): with a
    // floor, a scene that interpolates at 40-55 dropped to a locked 30 and its way back needed work under 15 ms, which
    // a heavy room never reaches (Boldiatti, rc14, D_MN06B: Unlimited 51-56 FPS, Capped 60 at 30 for minutes). A
    // locked 30 is interpolation Off. A floor stays available here for A/B.
    unsigned interpFloorFps = 0;
    // V1-rc12: black-ground bisect (rc11 console: grassy ground black, depending on the camera). Each isolates one
    // cause; all off by default. dbg_fog_off, dbg_base_mip, dbg_no_aniso: Aurora (tuning.h, AURORA_DEBUG_*);
    // dbg_no_real_shadow: the shadow-volume pass of dDlst_shadowControl_c::draw is skipped.
    unsigned debugFlags = 0;  // AURORA_DEBUG_* bits
    bool dbgNoRealShadow = false;
    // V1-rc12: wii_record -- Wii-style motion writes every raw sample (with HID's attitude) to
    // cache/logs/wii-samples-*.csv for three minutes (wii_sample_log.hpp), for host replay. Off by default.
    bool wiiRecord = false;
    // V1-rc12: warmup_keep_all -- background warmup keeps every pipeline it creates (the rc11 behaviour); off releases
    // the ones the current scene does not use once they are in the disk cache (aurora_shader_set_warmup_keep_all).
    bool warmupKeepAll = false;
    // V1-rc12: profile_main -- sampling profiler of the main thread (sampler.hpp): every tour zone's window, and a
    // free-run window of profile_seconds (5-300) starting profile_delay_s (0-3600; 0 = tours only) after boot.
    bool profileMain = false;
    unsigned profileSeconds = 30;
    unsigned profileDelayS = 90;
    // V1-rc9: warmup_fade_boost -- while the loading screen waits for the scene's shaders, the compile thread
    // also gets core 0 and the game's priority (aurora_shader_set_compile_boost). On by default.
    bool warmupFadeBoost = true;
    // V1-rc9: arg_shield -- hook callbacks read the first argument from a copy (mods/svc/arg_shield.hpp). On.
    bool argShield = true;
    // V1-rc16: hook_fast -- the hook dispatch's slot cache and early-outs (mods/svc/hook.cpp). On; 0 for A/B.
    bool hookFast = true;
    // V1-PGO: hook_fast = verify -- the cache on, every hit checked against the registry, differences logged.
    bool hookVerify = false;
    // V1-rc16: vsync_relaxed -- with Capped interpolation at <= 60 and while it interpolates, swap interval 0 and the
    // main loop paced to 1/cap (frame_telemetry.cpp): a frame that misses a refresh is shown at the next one instead
    // of costing two, so an 18 ms frame runs near 54 FPS rather than falling to a 33 ms cadence. Off; for A/B.
    // The idea is from Chansey's sa2-nx docs (THIRD_PARTY_NOTICES).
    bool vsyncRelaxed = false;
    // V1-rc19: mod_lib_cache -- the extracted native mod libraries are kept on the card, keyed by the package's
    // sha256, and not written again at every boot (mods/loader/natives.cpp). On; 0 = extract every boot, as rc18.
    bool modLibCache = true;
    // V1-rc9: tour_matrix = key:v1,v2; key:v1,v2 -- the Performance Tour measures the heavy zones once per value
    // (perf_tour.hpp, parse_matrix), switching the keys at run time. Empty: the plain tour.
    std::string tourMatrix;
    // V1-rc8: the cores the game's OS threads may run on (OSThread.cpp pins them to core 1, the FIFO worker's
    // core). logic_core = 0 | 1 | 0,1 | 0,2 ... (0 here = keep core 1). swap_interval = 1 | 2 (0 = leave it).
    unsigned logicCoreMask = 0;
    // V1-rc8: texture-replacement cache budget in MB (aurora_gx_set_texrep_budget_mb), 64-2048; 0 = default.
    unsigned texrepBudgetMb = 0;
    // swap_interval: 0 = auto (2 without interpolation; with it, 1 while interpolating and 2 while the interpolation
    // governor has stalled -- rc11), 1, 2, or 3 = adaptive (rc10's SwapGovernor, for measuring only)
    // (1 while the frame's work fits 60, 2 when it does not: frame_telemetry.cpp, swap_adaptive).
    unsigned swapInterval = 0;
    std::string unknown;        // keys or values not understood, for the log
};

LabOptions parse_lab_options(std::string_view text);
std::string describe(const LabOptions& o);

// ---------------------------------------------------------------------------------------------
// Per-frame attribution.
// ---------------------------------------------------------------------------------------------

struct FrameSample {
    float intervalMs = 0.f;    // present to present, as the main loop saw it
    float mainWorkMs = 0.f;    // interval minus the main thread's waits (tick, limiter, frame slot)
    float fifoBusyMs = 0.f;    // FIFO worker translating GX commands (render-side CPU)
    float renderWorkMs = 0.f;  // render worker (encode, submit, present; present can block on vsync)
    float gpuMs = -1.f;        // < 0: no GPU figure for this frame
    unsigned queuedPipelines = 0;
};

enum class Bound : std::uint8_t {
    Met,        // within the budget
    Gpu,        // missed; the GPU took most of the budget and more than either CPU side
    CpuMain,    // missed; the game thread was busy for most of the budget
    CpuRender,  // missed; the FIFO worker (render-side CPU) was
    Unknown,    // missed for no measured reason (load, IO, sync, compile) or no GPU figure
};
inline constexpr int kBoundCount = 5;
const char* bound_name(Bound b);

// Budget = 1000 / target fps. A frame is missed above budget * kMissSlack.
inline constexpr float kMissSlack = 1.10f;
inline constexpr float kBusyShare = 0.85f;
Bound classify(const FrameSample& f, float budgetMs);

// V1-rc10: swap_interval adaptive (with Frame Interpolation on). At 1 a frame that misses 60 is shown after the
// next retrace or the one after (the 1v/2v/3v mix); at 2 every frame stays two retraces (an even 30). The frame's
// work -- the largest of the game thread, the FIFO worker and the GPU -- decides: over its window the 90th
// percentile above kTo2Ms goes to 2, below kTo1Ms (measured at 2, where every frame also runs a sim tick, so it
// reads high: the way back is the cautious one) returns to 1. At least kDwell frames between changes.
class SwapGovernor {
public:
    static constexpr std::size_t kWindow = 60;
    static constexpr unsigned kDwell = 120;
    static constexpr float kTo2Ms = 16.0f;
    static constexpr float kTo1Ms = 13.5f;
    /// Feeds one frame; returns the interval to use from now on (1 or 2).
    unsigned update(const FrameSample& f);
    unsigned current() const { return m_current; }
    unsigned changes() const { return m_changes; }
    float last_p90() const { return m_lastP90; }
    void reset(unsigned interval);

private:
    float m_work[kWindow] = {};
    std::size_t m_count = 0;
    std::size_t m_pos = 0;
    unsigned m_current = 1;
    unsigned m_sinceChange = 0;
    unsigned m_changes = 0;
    float m_lastP90 = 0.f;
};

// ---------------------------------------------------------------------------------------------
// Window statistics (DRS decisions, benchmark sections).
// ---------------------------------------------------------------------------------------------

struct WindowStats {
    int frames = 0;
    int missed = 0;
    int byBound[kBoundCount] = {};
    int gpuFrames = 0;           // frames that carried a GPU figure
    double seconds = 0.0;        // sum of intervals
    double gpuUtil = 0.0;        // sum of GPU ms over the sum of intervals of the frames with a figure
    float gpuMedianMs = 0.f;
    float gpuP90Ms = 0.f;
    float intervalMedianMs = 0.f;
    float mainWorkMedianMs = 0.f;
    float fifoBusyMedianMs = 0.f;
    unsigned queuedMax = 0;

    double missedPct() const { return frames > 0 ? 100.0 * missed / frames : 0.0; }
    // Share of the missed frames the GPU explains, 0-1.
    double gpuMissShare() const { return missed > 0 ? static_cast<double>(byBound[1]) / missed : 0.0; }
    double cpuMissShare() const {
        return missed > 0 ? static_cast<double>(byBound[2] + byBound[3]) / missed : 0.0;
    }
    bool hasGpu() const { return frames > 0 && gpuFrames * 2 >= frames; }
};

WindowStats summarize(const std::vector<FrameSample>& frames, float budgetMs);

// Nearest-rank percentile of an unsorted copy; 0 for none.
float percentile(std::vector<float> values, double p);

// ---------------------------------------------------------------------------------------------
// Dynamic resolution policy: "only when the GPU is the limit".
//
// Evaluated once per window (about a second of frames). A window can be "tainted" (a load, a
// pipeline compile, the preparation pass): it says nothing about resolution and resets the
// streaks. Steps are percentages of the ceiling; index 0 is the ceiling.
//
//  - Down one step after kDownStreak pressured windows in a row: >= 10 % frames missed, mostly
//    explained by the GPU (or GPU utilisation >= 90 %), and at least kMinDwellS since the last
//    change. Never without GPU figures: no GPU timing, no decisions.
//  - A drop is checked over the next kVerifyWindows: if frames are still missed and the GPU no
//    longer explains them (the CPU or render worker is the limit), the pixels are given back and
//    drops are held off for kCpuBackoffS. "Fewer pixels that buy nothing" is never kept.
//  - Up one step after kUpStreak relaxed windows: < 2 % missed and the GPU utilisation scaled to
//    the next step's pixel count <= kUpUtil. A climb that fails (pressured within the verify
//    windows) goes back down and doubles the wait before the next climb (10 s .. 160 s).
// ---------------------------------------------------------------------------------------------

inline constexpr int kDownStreak = 3;
inline constexpr int kUpStreak = 6;
inline constexpr int kVerifyWindows = 2;
inline constexpr double kMinDwellS = 4.0;
inline constexpr double kCpuBackoffS = 30.0;
inline constexpr double kUpBackoffFirstS = 10.0;
inline constexpr double kUpBackoffMaxS = 160.0;
inline constexpr double kPressureMissPct = 10.0;
inline constexpr double kPressureGpuShare = 0.60;
inline constexpr double kPressureUtil = 0.90;
inline constexpr double kRelaxedMissPct = 2.0;
inline constexpr double kUpUtil = 0.75;

enum class Action : std::uint8_t { Hold, Down, Up };

struct Decision {
    Action action = Action::Hold;
    int level = 0;       // step index after the decision
    std::string why;     // for the log; empty for a plain hold
};

class DrsPolicy {
public:
    // stepPercents: highest first (index 0 = ceiling, normally 100).
    explicit DrsPolicy(std::vector<int> stepPercents = {100, 85, 75});

    // Back to the ceiling with no history (enabled, ceiling changed, profile applied).
    void reset();
    int level() const { return mLevel; }
    int steps() const { return static_cast<int>(mSteps.size()); }
    int percent(int level) const;
    int percent() const { return percent(mLevel); }

    // One window. `now` is seconds on any monotonic clock.
    Decision evaluate(const WindowStats& w, bool tainted, double now);

    // Status for the UI and the report: "idle", "no-gpu-timing", "verifying-drop", ...
    const std::string& state() const { return mState; }

private:
    bool pressured(const WindowStats& w) const;
    bool relaxed(const WindowStats& w) const;
    Decision move(int to, std::string why, double now);

    std::vector<int> mSteps;
    int mLevel = 0;
    int pressureStreak = 0;
    int relaxedStreak = 0;
    int verifyLeft = 0;
    bool verifyingDrop = false;
    bool verifyingClimb = false;
    double lastChange = -1e9;
    double downAllowedAt = -1e9;
    double upAllowedAt = -1e9;
    double upBackoff = kUpBackoffFirstS;
    double missBeforeDrop = 0.0;
    std::string mState = "idle";
};

// Rows for a step: the ceiling scaled, even, never under kMinRows.
inline constexpr int kMinRows = 360;
int rows_for_percent(int ceilingRows, int percent);

// ---------------------------------------------------------------------------------------------
// Stress pattern: the sequence of rows the stress mode walks. "steps" bounces through the DRS
// steps of the ceiling (100 -> 85 -> 75 -> 85 -> 100 ...); "ladder" through 900 -> 720 -> 540 ->
// 720 -> 900 rows (limited to the ceiling), as in the V1 roadmap.
// ---------------------------------------------------------------------------------------------

std::vector<int> stress_rows(int ceilingRows, const LabOptions& o, unsigned transitions);

}  // namespace dusk::perf
