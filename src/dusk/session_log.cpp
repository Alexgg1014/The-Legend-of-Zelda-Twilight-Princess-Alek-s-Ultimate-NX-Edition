#include "dusk/session_log.hpp"

#include "dusk/dualscreen.h"

#include "dusk/game_clock.h"
#include "dusk/logging.h"
#include "dusk/switch_net.h"
#include "dusk/switch_jit.h"
#include "dusk/perf.hpp"
#include "dusk/settings.h"
#include "dusk/minimap_stats.hpp"
#include "dusk/mods/svc/hook.hpp"
#include "dusk/frame_telemetry.hpp"
#include "dusk/texrep_report.hpp"
#ifdef __SWITCH__
#include "dusk/gpu_boost.hpp"
#include "dusk/perf_profile_apply.hpp"
#endif

// V1-rc10: 2D pane counters (libs/JSystem/src/J2DGraph/J2DPane.cpp).
extern unsigned long long g_duskPanesDrawn;
extern unsigned long long g_duskPanesAlpha0;
extern bool g_duskHudCullAlpha0;

#include <fmt/format.h>
#include <string>
#include "dusk/resolution_policy.hpp"
#include "dusk/switch_display.hpp"

#include <aurora/gfx.h>
#include <aurora/shader_cache.h>
#include <aurora/tuning.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <typeinfo>

#ifdef __SWITCH__
#include <switch.h>
#include <malloc.h>
#include <unistd.h>
#include <new>

extern "C" char* fake_heap_end;
#include <cstdio>
#include <cstring>

extern "C" {
struct aleks2_socket_counters {
    uint32_t open;
    uint32_t created;
    uint32_t closed;
    uint32_t enobufs;
    uint32_t connectFailed;
    int lastErrno;
};
void aleks2_get_socket_counters(struct aleks2_socket_counters* out);
}
#endif

namespace dusk::session_log {
namespace {

inline constexpr borealis::Log Log{"session"};

using clock = std::chrono::steady_clock;
const clock::time_point s_launch = clock::now();
constexpr auto kHeartbeat = std::chrono::seconds(30);
clock::time_point s_nextHeartbeat = s_launch + kHeartbeat;
char s_stage[16] = "?";

float elapsed() {
    return std::chrono::duration<float>(clock::now() - s_launch).count();
}

#ifdef __SWITCH__
// clkrst reports what the OS was asked for; horizon-OC/sys-clk push through the
// same path, so this is the clock in effect unless a sysmodule bypasses pcv.
bool s_clkInit = false;
void clocksMHz(u32& cpu, u32& gpu, u32& emc) {
    cpu = gpu = emc = 0;
    if (!s_clkInit) {
        s_clkInit = R_SUCCEEDED(clkrstInitialize());
        if (!s_clkInit) {
            return;
        }
    }
    const struct { PcvModuleId id; u32* out; } mods[] = {
        {PcvModuleId_CpuBus, &cpu}, {PcvModuleId_GPU, &gpu}, {PcvModuleId_EMC, &emc}};
    for (const auto& m : mods) {
        ClkrstSession s{};
        if (R_SUCCEEDED(clkrstOpenSession(&s, m.id, 3))) {
            u32 hz = 0;
            if (R_SUCCEEDED(clkrstGetClockRate(&s, &hz))) {
                *m.out = hz / 1000000u;
            }
            clkrstCloseSession(&s);
        }
    }
}
#else
void clocksMHz(unsigned& cpu, unsigned& gpu, unsigned& emc) { cpu = gpu = emc = 0; }
#endif

const char* interpName(FrameInterpMode m) {
    switch (m) {
    case FrameInterpMode::Off: return "Off";
    case FrameInterpMode::Capped: return "Capped";
    default: return "Unlimited";
    }
}
const char* postName(int v) {
    return v == 0 ? "Off" : v == 1 ? "Classic" : "Dusk";
}
// H33: the rows that are actually rendered (the resolution policy, limited by the mode).
int resolutionRows(int value) {
    return dusk::respol::resolve(
        value, dusk::getSettings().game.autoResolutionPolicy.getValue(), dusk::display::mode()).rows;
}

}  // namespace

bool readClocksMHz(unsigned& cpu, unsigned& gpu, unsigned& emc) {
#ifdef __SWITCH__
    u32 c, g, e;
    clocksMHz(c, g, e);
    cpu = c;
    gpu = g;
    emc = e;
    return c != 0 || g != 0 || e != 0;
#else
    cpu = gpu = emc = 0;
    return false;
#endif
}

void stampSettings(const char* why) {
#ifdef __SWITCH__
    // Once per stamp is too often; only the first one says it.
    static bool networkReported = false;
    if (!networkReported) {
        networkReported = true;
        switchnet::log_status();
        // Availability only -- one page allocated and freed. Nothing here
        // executes; see src/dusk/switch_jit.h.
        switchjit::log_status();
    }
    const auto& s = getSettings();
    u32 cpu, gpu, emc;
    clocksMHz(cpu, gpu, emc);
    Log.info("[t={:.1f}s] settings ({}): policy={} interp={}/{} cap={} res={}({} rows) shadows=x{} bloom={} dof={} "
             "water={} predictive={}{} dynres={}/{} vsync={} companion={} layout={} minimalHUD={} preset={} gpuBoost={}{} | clocks cpu={} gpu={} emc={} "
             "MHz",
        elapsed(), why, s.game.autoResolutionPolicy.getValue(),
        interpName(s.game.enableFrameInterpolation.getValue()),
        s.game.interpolationQuality.getValue() == InterpQuality::Fancy ? "Fancy" : "Fast",
        s.video.maxFrameRate.getValue(), s.game.internalResolutionScale.getValue(),
        resolutionRows(s.game.internalResolutionScale.getValue()), s.game.shadowResolutionMultiplier.getValue(),
        postName(static_cast<int>(s.game.bloomMode.getValue())),
        postName(static_cast<int>(s.game.depthOfFieldMode.getValue())),
        s.game.disableWaterRefraction.getValue() ? "off" : "on", s.game.predictiveInterpolation.getValue(),
        dusk::game_clock::interpolation_stalled() ? "(stalled)" : "",
        s.game.dynamicResolution.getValue(), s.game.dynamicResolutionTarget.getValue(),
        s.video.enableVsync.getValue(), s.game.dualScreen.getValue(),
        s.game.dualScreenLayout.getValue() == kDualLayoutFlip ? "Flip" : "Normal",
        s.game.minimalHUD.getValue(),  // V1-rc6: the field minimap adds ~800 draws a frame when this is off
        // V1-rc16: presets never touch the clocks; the opt-in GPU boost and whether it is in force right now.
        [] {
            const int p = dusk::profile::detectCurrent();
            return p < 0 ? "Custom" : dusk::profile::name(p);
        }(),
        s.game.gpuBoost.getValue() ? "on" : "off", dusk::gpu_boost::active() ? "(active)" : "",
        cpu, gpu, emc);
#else
    (void)why;
#endif
}

void stampStage(const char* stageName, int procName) {
#ifdef __SWITCH__
    if (stageName != nullptr && stageName[0] != '\0') {
        texrep_report::stage_left(stageName);  // V1-rc12: dev.txt + a texture pack only
        std::snprintf(s_stage, sizeof s_stage, "%s", stageName);
    }
    Log.info("[t={:.1f}s] stage -> {} (proc {})", elapsed(), s_stage, procName);
    // V1-rc1: the stage's shader recipes go to the front of the warmup while it loads.
    aurora_set_shader_scene(s_stage);
#else
    (void)stageName;
    (void)procName;
#endif
}

void stampBoot(const char* step) {
#ifdef __SWITCH__
    static float s_previous = 0.0f;
    const float now = elapsed();
    Log.info("[t={:.1f}s] boot: {} (+{:.1f} s)", now, step, now - s_previous);
    s_previous = now;
#else
    (void)step;
#endif
}

void cpuBoost(bool on) {
#ifdef __SWITCH__
    static bool s_on = false;
    if (on == s_on) {
        return;
    }
    const Result rc = appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
    if (R_SUCCEEDED(rc)) {
        s_on = on;
    }
    Log.info("[t={:.1f}s] CPU boost {} (rc 0x{:x})", elapsed(), on ? "on (FastLoad)" : "off", rc);
#else
    (void)on;
#endif
}

#ifdef __SWITCH__
// V1-rc5: a reserve that the first failed allocation frees, with a line in the log, so that one running out of
// memory is seen and given room to finish; the second one stops with a crash report.
void* s_memoryReserve = nullptr;
constexpr size_t kMemoryReserveBytes = 48u << 20;
#endif

void installTerminateLogger() {
#ifdef __SWITCH__
    s_memoryReserve = std::malloc(kMemoryReserveBytes);
    std::set_new_handler([] {
        static char line[256];
        if (s_memoryReserve != nullptr) {
            std::free(s_memoryReserve);
            s_memoryReserve = nullptr;
            std::snprintf(line, sizeof line,
                "[t=%.1fs] OUT OF MEMORY: an allocation failed; the 48 MiB reserve was released to carry on",
                elapsed());
            Log.error("{}", static_cast<const char*>(line));
            return;  // the allocation is tried again
        }
        std::snprintf(line, sizeof line,
            "[t=%.1fs] OUT OF MEMORY again after the reserve was used; stopping with a crash report", elapsed());
        Log.error("{}", static_cast<const char*>(line));
        svcBreak(BreakReason_Panic, reinterpret_cast<uintptr_t>(line), std::strlen(line));
    });
    std::set_terminate([] {
        std::string what = "no active exception";
        if (const std::exception_ptr current = std::current_exception()) {
            try {
                std::rethrow_exception(current);
            } catch (const std::exception& e) {
                what = std::string{typeid(e).name()} + ": " + e.what();
            } catch (...) {
                what = "an exception that is not a std::exception";
            }
        }
        u64 used = 0;
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        u64 threadId = 0;
        svcGetThreadId(&threadId, CUR_THREAD_HANDLE);
        Log.error("[t={:.1f}s] std::terminate in thread 0x{:x} (id {}): {}; memory in use {} MiB; stopping with a "
                  "crash report",
            elapsed(), static_cast<unsigned long long>(threadGetCurHandle()),
            static_cast<unsigned long long>(threadId),
            what, used >> 20);
        // V1-rc5: a panic, not std::abort(): abort exits cleanly, with no crash report (and sdmc unmounted under
        // the other threads).
        svcBreak(BreakReason_Panic, 0, 0);
        std::abort();
    });
#endif
}

#ifdef __SWITCH__
// V1-rc5: abort() anywhere in the program (platforms/switch/source/switch_abort.cpp). A fixed buffer.
extern "C" void aleks2_note_abort(const void* caller) {
    static char line[200];
    std::snprintf(line, sizeof line, "[t=%.1fs] abort() called from %p; stopping with a crash report", elapsed(),
        caller);
    Log.error("{}", static_cast<const char*>(line));
}

// V1-rc5: a mod ended the program (elf_runtime_policy.cpp's abort/exit/__stack_chk_fail/... shims). A fixed
// buffer: this may run with the heap in trouble.
extern "C" [[noreturn]] void aleks2_mod_fatal(const char* what, const void* caller) {
    static char line[320];
    std::snprintf(line, sizeof line,
        "[t=%.1fs] a mod ended the program: %s, called from %p (find the module whose range holds it in the "
        "loader lines above); stopping with a crash report",
        elapsed(), what, caller);
    Log.error("{}", static_cast<const char*>(line));
    svcBreak(BreakReason_Panic, reinterpret_cast<uintptr_t>(line), std::strlen(line));
    for (;;) {
        svcSleepThread(1'000'000'000);
    }
}
#endif

void requestHeartbeat() {
#ifdef __SWITCH__
    s_nextHeartbeat = clock::time_point{};
#endif
}

void tick() {
#ifdef __SWITCH__
    const auto now = clock::now();
    // V1-rc12: low-memory guard for the background shader warmup, every 2 s (mallinfo is not free). A tester's rc11
    // first launch ran the heap down to 65 MiB free while the whole list warmed in the background, then the capture
    // service and fatal fell over. Under 150 MiB free (arena + never used) the warmup holds; over 250 it goes on.
    {
        static clock::time_point s_nextMemCheck{};
        static bool s_warmupHeld = false;
        if (now >= s_nextMemCheck) {
            s_nextMemCheck = now + std::chrono::seconds(2);
            const struct mallinfo info = mallinfo();
            const uintptr_t top = reinterpret_cast<uintptr_t>(sbrk(0));
            const uintptr_t end = reinterpret_cast<uintptr_t>(fake_heap_end);
            const uint64_t freeMiB =
                (static_cast<uint64_t>(info.fordblks) + (end > top ? end - top : 0)) >> 20;
            if (!s_warmupHeld && freeMiB < 150) {
                s_warmupHeld = true;
                aurora_shader_set_warmup_paused(1);
                Log.warn("[t={:.1f}s] low memory ({} MiB free): background shader warmup held", elapsed(), freeMiB);
            } else if (s_warmupHeld && freeMiB > 250) {
                s_warmupHeld = false;
                aurora_shader_set_warmup_paused(0);
                Log.info("[t={:.1f}s] memory back to {} MiB free: background shader warmup resumed", elapsed(), freeMiB);
            }
        }
    }
    texrep_report::tick(s_stage);  // V1-rc12: dev.txt + a texture pack only (2 s samples, 30 s files)
    if (now < s_nextHeartbeat) {
        return;
    }
    s_nextHeartbeat = now + kHeartbeat;
    u32 cpu, gpu, emc;
    clocksMHz(cpu, gpu, emc);
    const auto* st = aurora_get_stats();
    // The last frame's draw count (after merging) and how many draws were
    // merged into another; a frame rate cannot say which of those moved.
    Log.info("[t={:.1f}s] heartbeat: fps={:.1f} stage={} res={} rows queuedPipelines={} "
             "draws/frame={} merged/frame={} | clocks cpu={} gpu={} emc={} MHz",
        elapsed(), aurora_get_fps(), s_stage, resolutionRows(getSettings().game.internalResolutionScale.getValue()),
        st != nullptr ? st->queuedPipelines : 0u, st != nullptr ? st->drawCallCount : 0u,
        st != nullptr ? st->mergedDrawCallCount : 0u, cpu, gpu, emc);
    // rc9: the minimap since the last heartbeat: frames rendered and kept (hud_fast), why it
    // rendered, and minimap_pan's composes and base renders with their reasons.
    {
        static minimap_stats::Counters s_last{};
        const minimap_stats::Counters& now = minimap_stats::counters();
        std::string why;
        for (int i = 0; i < minimap_stats::kReasonCount; ++i) {
            if (const uint32_t n = now.reasons[i] - s_last.reasons[i]) {
                why += fmt::format(" {}={}", minimap_stats::reason_name(i), n);
            }
        }
        std::string baseWhy;
        for (int i = 0; i < minimap_stats::kBaseReasonCount; ++i) {
            if (const uint32_t n = now.baseReasons[i] - s_last.baseReasons[i]) {
                baseWhy += fmt::format(" {}={}", minimap_stats::base_reason_name(i), n);
            }
        }
        if (now.renders != s_last.renders || now.kept != s_last.kept) {
            Log.info("[t={:.1f}s] minimap: rendered={} kept={} |{} | pan composes={} base={}{}", elapsed(),
                now.renders - s_last.renders, now.kept - s_last.kept, why.empty() ? " -" : why,
                now.composes - s_last.composes, now.baseRenders - s_last.baseRenders, baseWhy);
        }
        s_last = now;
    }
    // V1-rc10: 2D panes per frame since the last heartbeat, and those drawn at alpha 0 (J2DPane.cpp): what
    // hud_cull_alpha0 would save. The HUD mods add panes; this shows how many.
    {
        static unsigned long long s_panes = 0, s_alpha0 = 0;
        static std::uint64_t s_frames = 0;
        const std::uint64_t frames = frame_telemetry::ticks();
        if (frames > s_frames && g_duskPanesDrawn != s_panes) {
            const double n = static_cast<double>(frames - s_frames);
            Log.info("[t={:.1f}s] 2d panes: {:.0f}/frame, {:.0f} at alpha 0 ({})", elapsed(),
                static_cast<double>(g_duskPanesDrawn - s_panes) / n, static_cast<double>(g_duskPanesAlpha0 - s_alpha0) / n,
                g_duskHudCullAlpha0 ? "skipped" : "drawn");
        }
        s_panes = g_duskPanesDrawn;
        s_alpha0 = g_duskPanesAlpha0;
        s_frames = frames;
    }
    // V1-rc11: EFB copies per frame, and copies whose destination last held another size or format (Aurora).
    {
        static uint64_t s_copies = 0, s_reshapes = 0, s_inside = 0, s_mismatch = 0;
        static std::uint64_t s_copyFrames = 0;
        uint64_t copies = 0, reshapes = 0, inside = 0, mismatch = 0;
        aurora_gx_copy_stats(&copies, &reshapes);
        aurora_gx_copy_bind_stats(&inside, &mismatch);  // V1-rc12
        const std::uint64_t frames = frame_telemetry::ticks();
        if (frames > s_copyFrames && copies != s_copies) {
            Log.info("[t={:.1f}s] efb copies: {:.1f}/frame, {} to a destination with another size or format | "
                     "textures bound inside a copy (not at its start) {}, at a copy's start with another size {}",
                elapsed(), static_cast<double>(copies - s_copies) / static_cast<double>(frames - s_copyFrames),
                reshapes - s_reshapes, inside - s_inside, mismatch - s_mismatch);
        }
        s_copies = copies;
        s_reshapes = reshapes;
        s_inside = inside;
        s_mismatch = mismatch;
        s_copyFrames = frames;
    }
    // V1-rc9: the mods' hook dispatch on the game thread, from the last "hooks per frame" report (mods loader,
    // every 900 frames): calls and the time in them, ours and the callbacks'.
    if (const auto hooks = mods::svc::hook_stats_last(); hooks.targets != 0) {
        Log.info("[t={:.1f}s] hooks: {:.0f} calls/frame on {} targets, dispatch {:.2f} ms/frame (callbacks included)",
            elapsed(), hooks.callsPerFrame, hooks.targets, hooks.dispatchMsPerFrame);
    }
    // V1-rc5: sockets (platforms/switch/source/switch_sockets.cpp). A leak keeps "open" growing; a full
    // buffer pool shows as enobufs.
    // V1-rc5: memory, to see a session that grows (a tester's rc4 died of bad_alloc after 25 min of compiling).
    // libnx reserves the whole heap at start (the process's used memory never moves): what tells is malloc's arena,
    // what is in use in it, what is free in it, and the part of the heap never handed out yet.
    const struct mallinfo heap = mallinfo();
    const uintptr_t brk = reinterpret_cast<uintptr_t>(sbrk(0));
    const uintptr_t heapEnd = reinterpret_cast<uintptr_t>(fake_heap_end);
    Log.info("[t={:.1f}s] mem: heap arena {} MiB, in use {} MiB, free in arena {} MiB, never used {} MiB; "
             "pipelines created {}, live {}, warmup released {}",
        elapsed(), static_cast<unsigned long>(heap.arena) >> 20, static_cast<unsigned long>(heap.uordblks) >> 20,
        static_cast<unsigned long>(heap.fordblks) >> 20, heapEnd > brk ? (heapEnd - brk) >> 20 : 0,
        st != nullptr ? st->createdPipelines : 0u, aurora_pipeline_live_count(), aurora_shader_warmup_released());
    aleks2_socket_counters sockets{};
    aleks2_get_socket_counters(&sockets);
    Log.info("[t={:.1f}s] net: sockets open={} created={} closed={} connect_failed={} enobufs={} last_errno={} ({})",
        elapsed(), sockets.open, sockets.created, sockets.closed, sockets.connectFailed, sockets.enobufs,
        sockets.lastErrno, sockets.lastErrno != 0 ? std::strerror(sockets.lastErrno) : "none");
    // aleks2 (deko3d P1): the render worker's thread CPU per frame and per draw since the last heartbeat,
    // with either backend; deko3d also its GPU time
    static AuroraRenderCpuStats s_lastRender{};
    AuroraRenderCpuStats render{};
    aurora_gfx_render_cpu_stats(&render);
    const uint64_t frames = render.frames - s_lastRender.frames;
    const uint64_t draws = render.draws - s_lastRender.draws;
    const double cpuMs = static_cast<double>(render.thread_cpu_ns - s_lastRender.thread_cpu_ns) / 1e6;
    const uint64_t gpuFrames = render.gpu_frames - s_lastRender.gpu_frames;
    const double gpuMs = static_cast<double>(render.gpu_ns - s_lastRender.gpu_ns) / 1e6;
    Log.info("[t={:.1f}s] render: backend={} worker_cpu={:.2f} ms/frame draws={:.0f}/frame us_per_draw={:.2f}{}",
        elapsed(), render.deko3d ? "deko3d" : "dawn_nvk", frames > 0 ? cpuMs / static_cast<double>(frames) : 0.0,
        frames > 0 ? static_cast<double>(draws) / static_cast<double>(frames) : 0.0,
        draws > 0 ? cpuMs * 1000.0 / static_cast<double>(draws) : 0.0,
        render.deko3d && gpuFrames > 0 ? fmt::format(" gpu={:.2f} ms/frame", gpuMs / static_cast<double>(gpuFrames))
                                       : std::string{});
    s_lastRender = render;
#endif
}

}  // namespace dusk::session_log

// aleks2 (rc17): the boot split into timed phases (boot_timing.hpp). Here at the end of the file, so that no line
// of the functions above moves: the PGO profile matches functions by their line.
#include "dusk/boot_timing.hpp"

#ifdef __SWITCH__
// switch_startup.cpp, userAppInit: the first thing the process does, and the romfs mount right after it.
extern "C" unsigned long long aleks2_boot_entry_tick, aleks2_boot_romfs_tick;

namespace dusk::boot_timing {
namespace {

inline constexpr borealis::Log Log{"session"};

double seconds(std::uint64_t ticks) {
    return static_cast<double>(armTicksToNs(ticks)) / 1e9;
}

struct ModTime {
    char id[64];
    std::uint64_t loaded;
    std::uint64_t activated;
};
constexpr int kMaxMods = 192;
ModTime s_mods[kMaxMods];
int s_modCount = 0;
std::uint64_t s_loadedTotal = 0, s_activatedTotal = 0;
int s_loadedCount = 0, s_activatedCount = 0;
std::uint64_t s_symdbStart = 0, s_symdbEnd = 0;
std::uint64_t s_menu = 0;         // menu time before the game loop
std::uint64_t s_firstFrame = 0;   // the first frame done (the summary)
unsigned s_frames = 0;

ModTime* mod_entry(const char* id) {
    for (int i = 0; i < s_modCount; ++i) {
        if (std::strncmp(s_mods[i].id, id, sizeof s_mods[i].id - 1) == 0) {
            return &s_mods[i];
        }
    }
    if (s_modCount == kMaxMods) {
        return nullptr;
    }
    ModTime& m = s_mods[s_modCount++];
    std::snprintf(m.id, sizeof m.id, "%s", id);
    m.loaded = m.activated = 0;
    return &m;
}

// The interval since `since`, less the symbol manifest load when it happened inside it.
std::uint64_t since_without_symdb(std::uint64_t since) {
    const std::uint64_t end = armGetSystemTick();
    std::uint64_t d = end - since;
    if (s_symdbEnd != 0 && s_symdbStart >= since && s_symdbEnd <= end) {
        d -= s_symdbEnd - s_symdbStart;
    }
    return d;
}

}  // namespace

std::uint64_t now() noexcept {
    return armGetSystemTick();
}

void entry_report() noexcept {
    if (aleks2_boot_entry_tick == 0) {
        return;
    }
    const std::uint64_t t = armGetSystemTick();
    const double toLaunch = seconds(t - aleks2_boot_entry_tick) - session_log::elapsed();
    Log.info("[t={:.1f}s] boot phase: process entry to game_main {:.2f} s (romfs mount {:.2f} s; GPU service, "
             "static constructors and main {:.2f} s; t=0 is {:.2f} s after entry; the homebrew loader's NRO load "
             "before entry is not visible from here)",
        session_log::elapsed(), seconds(t - aleks2_boot_entry_tick),
        seconds(aleks2_boot_romfs_tick - aleks2_boot_entry_tick), seconds(t - aleks2_boot_romfs_tick), toLaunch);
}

void phase(const char* what, std::uint64_t since) noexcept {
    Log.info("[t={:.1f}s] boot phase: {} {:.2f} s", session_log::elapsed(), what,
        seconds(armGetSystemTick() - since));
}

void phase_if(bool when, const char* what, std::uint64_t since) noexcept {
    if (when) {
        phase(what, since);
    }
}

void symbol_manifest(std::uint64_t since) noexcept {
    s_symdbStart = since;
    s_symdbEnd = armGetSystemTick();
    Log.info("[t={:.1f}s] boot phase: symbol manifest (romfs:/dusklight.symdb, first by-name lookup) {:.2f} s",
        session_log::elapsed(), seconds(s_symdbEnd - s_symdbStart));
}

void mod_loaded(const char* id, std::uint64_t since) noexcept {
    const std::uint64_t d = since_without_symdb(since);
    s_loadedTotal += d;
    ++s_loadedCount;
    if (ModTime* m = mod_entry(id != nullptr ? id : "?")) {
        m->loaded += d;
    }
}

void mod_activated(const char* id, std::uint64_t since) noexcept {
    const std::uint64_t d = since_without_symdb(since);
    s_activatedTotal += d;
    ++s_activatedCount;
    if (ModTime* m = mod_entry(id != nullptr ? id : "?")) {
        m->activated += d;
    }
}

void mods_report() noexcept {
    int top[3] = {-1, -1, -1};
    for (int i = 0; i < s_modCount; ++i) {
        const std::uint64_t ti = s_mods[i].loaded + s_mods[i].activated;
        for (int k = 0; k < 3; ++k) {
            if (top[k] < 0 || ti > s_mods[top[k]].loaded + s_mods[top[k]].activated) {
                for (int j = 2; j > k; --j) {
                    top[j] = top[j - 1];
                }
                top[k] = i;
                break;
            }
        }
    }
    std::string slowest;
    for (int k = 0; k < 3 && top[k] >= 0; ++k) {
        const ModTime& m = s_mods[top[k]];
        slowest += fmt::format("{}{} {:.2f} s (load {:.2f} + activate {:.2f})", k == 0 ? "" : ", ", m.id,
            seconds(m.loaded + m.activated), seconds(m.loaded), seconds(m.activated));
    }
    Log.info("[t={:.1f}s] boot phase: mods {} loaded (unpack, map, relocate) {:.2f} s, {} activated (constructors "
             "and mod_initialize) {:.2f} s; slowest: {}",
        session_log::elapsed(), s_loadedCount, seconds(s_loadedTotal), s_activatedCount, seconds(s_activatedTotal),
        slowest.empty() ? "none" : slowest);
}

void menu_wait(std::uint64_t since) noexcept {
    const std::uint64_t d = armGetSystemTick() - since;
    s_menu += d;
    if (seconds(d) >= 0.05) {
        Log.info("[t={:.1f}s] boot phase: prelaunch menu before the game loop {:.2f} s (menu time, not boot work)",
            session_log::elapsed(), seconds(d));
    }
}

void play_pressed() noexcept {
    static bool s_logged = false;
    if (s_logged || s_firstFrame == 0) {
        return;  // pressed in the menu before the game loop: menu_wait has it
    }
    s_logged = true;
    Log.info("[t={:.1f}s] boot: Play pressed after {:.1f} s in the prelaunch menu (menu time, not boot work)",
        session_log::elapsed(), seconds(armGetSystemTick() - s_firstFrame));
}

void frame() noexcept {
    if (s_frames >= 2) {
        return;
    }
    ++s_frames;
    static std::uint64_t s_reached = 0;
    const std::uint64_t t = armGetSystemTick();
    if (s_frames == 1) {
        s_reached = t;
        Log.info("[t={:.1f}s] boot phase: first main-loop frame reached", session_log::elapsed());
        return;
    }
    s_firstFrame = t;
    const std::uint64_t start =
        aleks2_boot_entry_tick != 0 ? aleks2_boot_entry_tick
                                    : t - armNsToTicks(static_cast<std::uint64_t>(session_log::elapsed() * 1e9));
    const std::uint64_t total = t - start;
    Log.info("[t={:.1f}s] boot phase: first frame done {:.2f} s", session_log::elapsed(), seconds(t - s_reached));
    Log.info("[t={:.1f}s] boot work: {:.1f} s (excluding {:.1f} s in menus), process entry to the first frame",
        session_log::elapsed(), seconds(total > s_menu ? total - s_menu : 0), seconds(s_menu));
}

}  // namespace dusk::boot_timing
#endif
