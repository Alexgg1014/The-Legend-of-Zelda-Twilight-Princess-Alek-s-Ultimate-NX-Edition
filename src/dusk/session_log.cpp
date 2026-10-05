#include "dusk/session_log.hpp"

#include "dusk/dualscreen.h"

#include "dusk/game_clock.h"
#include "dusk/logging.h"
#include "dusk/switch_net.h"
#include "dusk/switch_jit.h"
#include "dusk/perf.hpp"
#include "dusk/settings.h"
#include "dusk/resolution_policy.hpp"
#include "dusk/switch_display.hpp"

#include <aurora/gfx.h>

#include <chrono>

#ifdef __SWITCH__
#include <switch.h>
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
             "water={} predictive={}{} dynres={}/{} vsync={} companion={} layout={} | clocks cpu={} gpu={} emc={} MHz",
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
        s.game.dualScreenLayout.getValue() == kDualLayoutFlip ? "Flip" : "Normal", cpu, gpu, emc);
#else
    (void)why;
#endif
}

void stampStage(const char* stageName, int procName) {
#ifdef __SWITCH__
    if (stageName != nullptr && stageName[0] != '\0') {
        std::snprintf(s_stage, sizeof s_stage, "%s", stageName);
    }
    Log.info("[t={:.1f}s] stage -> {} (proc {})", elapsed(), s_stage, procName);
#else
    (void)stageName;
    (void)procName;
#endif
}

void tick() {
#ifdef __SWITCH__
    const auto now = clock::now();
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
#endif
}

}  // namespace dusk::session_log
