#include "dusk/frame_telemetry.hpp"

#include "dusk/game_clock.h"
#include "dusk/main.h"
#include "dusk/dev_mode.hpp"
#include "dusk/settings.h"

#include <aurora/gfx.h>
#include <aurora/tuning.h>
#include <aurora/shader_cache.h>  // V1-rc12: warmup keep-all
#include "dusk/sampler.hpp"        // V1-rc12: profile_main
#include <borealis/log.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <malloc.h>
#include <sstream>

#ifdef __SWITCH__
#include <switch.h>

extern bool g_duskWarmupFadeBoost;  // shader_warmup_runtime.cpp
extern bool g_duskArgShield;        // mods/svc/hook.cpp
extern bool g_duskHookFast;         // mods/svc/hook.cpp (V1-rc16)
extern bool g_duskModLibCache;      // mods/loader/natives.cpp (V1-rc19)
extern bool g_duskHookVerify;       // mods/svc/hook.cpp (V1-PGO)
extern bool g_duskHudCullAlpha0;    // libs/JSystem/src/J2DGraph/J2DPane.cpp (V1-rc10)
extern bool g_duskDbgNoRealShadow;  // src/d/d_drawlist.cpp (V1-rc12)
#ifdef __SWITCH__
extern "C" unsigned g_duskExitMode;  // platforms/switch/source/switch_startup.cpp (V1-rc10)
#endif
extern "C" {
extern u32 g_duskOsThreadCoreMask;
extern s32 g_duskOsThreadStartCore;
}
#endif

namespace dusk::frame_telemetry {
namespace {
constexpr borealis::Log Log{"dusk::perf"};
constexpr const char* kLabFile = "perf-lab.ini";
// A GPU figure is reused for this many frames when no new one came back (results arrive two
// or three frames late, sometimes two at once).
constexpr int kGpuCarryFrames = 3;
constexpr std::size_t kCaptureMax = 60 * 60 * 6;  // six minutes at 60

using clock = std::chrono::steady_clock;

perf::LabOptions g_lab;
perf::SwapGovernor g_swapGovernor;  // V1-rc10: swap_interval adaptive
std::uint64_t g_ticks = 0;          // V1-rc10: frames, for per-frame heartbeat figures
unsigned g_consumers = 0;
bool g_requested = false;

bool g_have = false;
clock::time_point g_lastTick;
std::uint64_t g_waitNs = 0;
AuroraFifoStats g_fifo{};
AuroraFrameWaitStats g_waits{};
AuroraGpuTimingStats g_gpu{};
float g_lastGpuMs = -1.f;
int g_gpuAge = 0;

perf::FrameSample g_last;
perf::Bound g_lastBound = perf::Bound::Met;
bool g_capturing = false;
std::vector<perf::FrameSample> g_capture;

std::uint64_t waits_total() {
    return game_clock::wait_total_ns(game_clock::WaitKind::Tick) +
           game_clock::wait_total_ns(game_clock::WaitKind::Limiter);
}

double ns_to_ms(std::uint64_t ns) {
    return static_cast<double>(ns) / 1e6;
}

void update_request() {
    const bool want = g_lab.gpuTiming != perf::GpuTiming::Off &&
                      (g_consumers != 0 || g_lab.gpuTiming == perf::GpuTiming::Always);
    if (want != g_requested) {
        g_requested = want;
        aurora_gpu_timing_set_enabled(want);
        Log.info("GPU timing {}", want ? "on" : "off");
    }
}
}  // namespace

void load_lab_options() {
    if (!dev_mode()) {
        return;  // V1-rc5: perf-lab.ini is a developer-mode file
    }
    const auto path = ConfigPath / kLabFile;
    std::ifstream in(path, std::ios::binary);
    if (in) {
        std::ostringstream ss;
        ss << in.rdbuf();
        g_lab = perf::parse_lab_options(ss.str());
        Log.warn("Lab options from {}: {}{}", path.string(), perf::describe(g_lab),
            g_lab.unknown.empty() ? "" : " (ignored: " + g_lab.unknown + ")");
    }
    // FIFO-opt: off unless perf-lab.ini asks (developer mode only, like the file itself).
    aurora_gx_set_fifo_fast(g_lab.fifoFast);
    // V1-rc9: the compile boost while a loading screen waits (shader_warmup_runtime.cpp).
    g_duskWarmupFadeBoost = g_lab.warmupFadeBoost;
    g_duskArgShield = g_lab.argShield;  // V1-rc9
    g_duskHookFast = g_lab.hookFast; g_duskHookVerify = g_lab.hookVerify;    // V1-rc16
    g_duskModLibCache = g_lab.modLibCache;  // V1-rc19
    g_duskHudCullAlpha0 = g_lab.hudCullAlpha0;  // V1-rc10
    aurora_debug_set_flags(g_lab.debugFlags);    // V1-rc12: black-ground bisect
    aurora_shader_set_warmup_keep_all(g_lab.warmupKeepAll ? 1u : 0u);  // V1-rc12: warmup memory
    sampler::start_if_enabled();                                        // V1-rc12: profile_main (off: returns)
    g_duskDbgNoRealShadow = g_lab.dbgNoRealShadow;
#ifdef __SWITCH__
    g_duskExitMode = g_lab.exitMode;
#endif
    // V1-rc8: the texture-replacement cache budget (0 keeps Aurora's platform default).
    aurora_gx_set_texrep_budget_mb(g_lab.texrepBudgetMb);
    Log.info("Texture replacement cache budget: {} MB{}", aurora_gx_get_texrep_budget_mb(),
        g_lab.texrepBudgetMb != 0 ? " (perf-lab.ini)" : "");
    // V1-rc8: logic_core, for the OS threads started from now on and for this one (the game thread).
    if (g_lab.logicCoreMask != 0) {
        const s32 start = __builtin_ctz(g_lab.logicCoreMask);
        g_duskOsThreadCoreMask = g_lab.logicCoreMask;
        g_duskOsThreadStartCore = start;
        const Result rc = svcSetThreadCoreMask(threadGetCurHandle(), start, g_lab.logicCoreMask);
        Log.warn("logic_core: game threads on core mask 0x{:x} (start {}); this thread: rc 0x{:x}",
            g_lab.logicCoreMask, start, static_cast<unsigned>(rc));
    }
    if (g_lab.gpuTiming == perf::GpuTiming::Off) {
        // Aurora reads it when it picks the device features (webgpu/gpu.cpp).
        setenv("AURORA_GPU_TIMING", "0", 1);
    }
}

const perf::LabOptions& lab() {
    return g_lab;
}

// V1-rc9: the keys the tour matrix may change while the game runs, and what each change does at once. swap_interval
// is picked up by tick(), hud_fast and minimap_pan by d_map.cpp on its next frame.
bool set_lab_value(std::string_view key, std::string_view value, std::string& why) {
    static constexpr std::string_view kRuntime[] = {
        "hud_fast", "minimap_pan", "swap_interval", "fifo_fast", "warmup_fade_boost", "arg_shield",
        "hud_cull_alpha0", "hook_fast", "vsync_relaxed"};
    bool runtime = false;
    for (const auto k : kRuntime) {
        runtime = runtime || k == key;
    }
    if (!runtime) {
        why = "cannot change while the game runs";
        return false;
    }
    const auto parsed = perf::parse_lab_options(fmt::format("{} = {}\n", key, value));
    if (!parsed.unknown.empty()) {
        why = "not a value it takes";
        return false;
    }
    if (key == "hud_fast") {
        g_lab.hudFast = parsed.hudFast;
    } else if (key == "minimap_pan") {
        g_lab.minimapPan = parsed.minimapPan;
    } else if (key == "swap_interval") {
        g_lab.swapInterval = parsed.swapInterval;
    } else if (key == "fifo_fast") {
        g_lab.fifoFast = parsed.fifoFast;
        aurora_gx_set_fifo_fast(g_lab.fifoFast);
    } else if (key == "warmup_fade_boost") {
        g_lab.warmupFadeBoost = parsed.warmupFadeBoost;
        g_duskWarmupFadeBoost = g_lab.warmupFadeBoost;
    } else if (key == "arg_shield") {
        g_lab.argShield = parsed.argShield;
        g_duskArgShield = g_lab.argShield;
    } else if (key == "hud_cull_alpha0") {
        g_lab.hudCullAlpha0 = parsed.hudCullAlpha0;
        g_duskHudCullAlpha0 = g_lab.hudCullAlpha0;
    } else if (key == "hook_fast") {
        g_lab.hookFast = parsed.hookFast; g_lab.hookVerify = parsed.hookVerify;
        g_duskHookFast = g_lab.hookFast; g_duskHookVerify = g_lab.hookVerify;
    } else if (key == "vsync_relaxed") {
        g_lab.vsyncRelaxed = parsed.vsyncRelaxed;  // tick() applies it on its next frame
    }
    return true;
}

void restore_lab(const perf::LabOptions& saved) {
    g_lab = saved;
    aurora_gx_set_fifo_fast(g_lab.fifoFast);
    g_duskWarmupFadeBoost = g_lab.warmupFadeBoost;
    g_duskArgShield = g_lab.argShield;
    g_duskHookFast = g_lab.hookFast; g_duskHookVerify = g_lab.hookVerify;
    g_duskHudCullAlpha0 = g_lab.hudCullAlpha0;
}

std::uint64_t ticks() {
    return g_ticks;
}

float budget_ms() {
    return 1000.f / static_cast<float>(g_lab.drsTargetFps > 0 ? g_lab.drsTargetFps : 30);
}

void want_gpu_timing(Consumer who, bool on) {
    g_consumers = on ? (g_consumers | who) : (g_consumers & ~static_cast<unsigned>(who));
    update_request();
}

void tick() {
    // V1-rc8: swap_interval, once the window exists (after the first frames).
    // rc9: without a perf-lab.ini value it follows Frame Interpolation: 2 when it is Off (the
    // game makes 30 a second; at 1 a frame finished early is shown after one retrace and the
    // next one after three, the 1v/3v pairs in the tours' cadence), 1 when it is on (60).
    // Applied again whenever the setting changes.
    // rc10 had an adaptive mode here (SwapGovernor) by default with interpolation: it moved to 2 on its own reading
    // of the frame's work while the interpolation governor (interp_governor.hpp) made its own decision, and at 2
    // the interpolated frames could not be shown ("always 30", rc10 console). rc11: with interpolation on, follow
    // that governor -- 1 while it interpolates, 2 while it has stalled to plain 30 (an even 2v, not 1v/3v pairs).
    // swap_interval=3 (adaptive, perf-lab.ini) keeps rc10's governor for measuring; 1 or 2 fix the value.
    static unsigned s_ticks = 0;
    static unsigned s_applied = 0;
    static unsigned s_mode = 0;
    // V1-rc16: vsync_relaxed (perf_core.hpp). Only where the cap is the panel's 60 or less: above it nothing would pace.
    const bool relaxed = g_lab.vsyncRelaxed && g_lab.swapInterval == 0 &&
                         getSettings().game.enableFrameInterpolation.getValue() == FrameInterpMode::Capped &&
                         getSettings().video.maxFrameRate.getValue() <= 60;
    const unsigned mode = g_lab.swapInterval != 0 ? g_lab.swapInterval
                          : getSettings().game.enableFrameInterpolation.getValue() == FrameInterpMode::Off ? 2u
                                                                                                         : 4u;
    if (mode != s_mode) {
        s_mode = mode;
        g_swapGovernor.reset(1);
    }
    if (s_ticks < 60) {
        ++s_ticks;
    } else {
        const unsigned want = mode == 3 ? g_swapGovernor.current()
                              : mode == 4 ? (game_clock::interpolation_stalled() ? 2u : relaxed ? 0u : 1u)
                                          : mode;
        if (want != s_applied) {
            s_applied = want;
            const Result rc = nwindowSetSwapInterval(nwindowGetDefault(), want);
            Log.warn("swap_interval {}{}: nwindowSetSwapInterval rc 0x{:x}{}", want,
                g_lab.swapInterval == 3 ? " (adaptive, perf-lab.ini)"
                : g_lab.swapInterval != 0 ? " (perf-lab.ini)"
                : mode == 4 ? (want == 2 ? " (interpolation stalled)"
                               : want == 0 ? " (interpolating, vsync_relaxed)" : " (interpolating)")
                            : " (auto)",
                static_cast<unsigned>(rc),
                mode == 3 ? fmt::format(", work p90 {:.1f} ms, change {}", g_swapGovernor.last_p90(),
                                g_swapGovernor.changes())
                          : std::string{});
        }
    }
    // V1-rc16: at swap interval 0 the panel no longer paces the loop: keep presents at least 1/cap apart, as vsync
    // would, and report the sleep as limiter wait (the interpolation governor measures work without it).
    if (s_applied == 0 && relaxed) {
        static clock::time_point s_paceMark{};
        const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(
            1.0 / static_cast<double>(std::max(getSettings().video.maxFrameRate.getValue(), 1))));
        const auto start = clock::now();
        const auto target = s_paceMark + period;
        if (start < target && target - start < period) {
            svcSleepThread(std::chrono::duration_cast<std::chrono::nanoseconds>(target - start).count());
            const auto end = clock::now();
            game_clock::note_wait(game_clock::WaitKind::Limiter,
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
            s_paceMark = target;  // on the period's grid while it keeps up
        } else {
            s_paceMark = start;  // late (or the first frame): the grid restarts here
        }
    }
    ++g_ticks;
    update_request();
    const auto now = clock::now();
    const std::uint64_t waitNs = waits_total();
    AuroraFifoStats fifo{};
    aurora_gx_fifo_stats(&fifo);
    AuroraFrameWaitStats waits{};
    aurora_gfx_frame_wait_stats(&waits);
    AuroraGpuTimingStats gpu{};
    aurora_gpu_timing_stats(&gpu);

    if (g_have) {
        perf::FrameSample f;
        f.intervalMs = std::chrono::duration<float, std::milli>(now - g_lastTick).count();
        // The main thread's waits: sim tick and frame limiter (game_clock), a frame slot the
        // render worker still holds, a staging buffer, and FIFO drains (waiting for the
        // render-side CPU). What is left of the interval is the game thread working.
        const double waitedMs = ns_to_ms(waitNs - g_waitNs) +
                                ns_to_ms(waits.frame_slot_wait_ns - g_waits.frame_slot_wait_ns) +
                                ns_to_ms(waits.staging_wait_ns - g_waits.staging_wait_ns) +
                                ns_to_ms(fifo.drains.wait_ns - g_fifo.drains.wait_ns);
        f.mainWorkMs = static_cast<float>(std::max(0.0, static_cast<double>(f.intervalMs) - waitedMs));
        f.fifoBusyMs = static_cast<float>(ns_to_ms(fifo.busy_ns - g_fifo.busy_ns));
        f.renderWorkMs = static_cast<float>(ns_to_ms(waits.render_work_ns - g_waits.render_work_ns));
        const std::uint64_t newFrames = gpu.frames - g_gpu.frames;
        if (gpu.enabled && newFrames > 0) {
            g_lastGpuMs = static_cast<float>(ns_to_ms(gpu.gpu_ns - g_gpu.gpu_ns) / static_cast<double>(newFrames));
            g_gpuAge = 0;
        } else if (gpu.enabled && g_lastGpuMs >= 0.f && ++g_gpuAge > kGpuCarryFrames) {
            g_lastGpuMs = -1.f;
        } else if (!gpu.enabled) {
            g_lastGpuMs = -1.f;
        }
        f.gpuMs = g_lastGpuMs;
        f.queuedPipelines = aurora_get_stats() != nullptr ? aurora_get_stats()->queuedPipelines : 0;
        g_last = f;
        g_lastBound = perf::classify(f, budget_ms());
        if (s_mode == 3) {
            g_swapGovernor.update(f);  // V1-rc10: applied at the next tick
        }
        if (g_capturing && g_capture.size() < kCaptureMax) {
            g_capture.push_back(f);
        }
    }
    g_have = true;
    g_lastTick = now;
    g_waitNs = waitNs;
    g_fifo = fifo;
    g_waits = waits;
    g_gpu = gpu;
}

const perf::FrameSample& last() {
    return g_last;
}

perf::Bound last_bound() {
    return g_lastBound;
}

GpuStatus gpu_status() {
    AuroraGpuTimingStats s{};
    aurora_gpu_timing_stats(&s);
    GpuStatus g;
    g.supported = s.supported != 0;
    g.enabled = s.enabled != 0;
    g.calibrated = s.calibrated != 0;
    g.periodNs = static_cast<double>(s.period_ppm) / 1e6;
    g.measured = s.frames;
    g.skipped = s.skipped;
    return g;
}

void capture_begin() {
    g_capture.clear();
    g_capture.reserve(4096);
    g_capturing = true;
}

void capture_end() {
    g_capturing = false;
}

bool capture_active() {
    return g_capturing;
}

const std::vector<perf::FrameSample>& capture() {
    return g_capture;
}

Memory memory() {
    Memory m;
#ifdef __SWITCH__
    u64 v = 0;
    if (R_SUCCEEDED(svcGetInfo(&v, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0))) {
        m.used = v;
    }
    if (R_SUCCEEDED(svcGetInfo(&v, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))) {
        m.total = v;
    }
    const struct mallinfo mi = mallinfo();
    m.heapInUse = static_cast<std::uint64_t>(static_cast<unsigned>(mi.uordblks));
#endif
    return m;
}

}  // namespace dusk::frame_telemetry
