// aleks2 (V1-rc16): see gpu_boost.hpp. Switch only (CMakeLists.txt).
#include "dusk/gpu_boost.hpp"

#include "dusk/logging.h"
#include "dusk/perf_profile.hpp"
#include "dusk/perf_profile_apply.hpp"
#include "dusk/settings.h"

#include <switch.h>

#include <chrono>

namespace dusk::gpu_boost {
namespace {

constexpr borealis::Log Log{"dusk::gpu_boost"};
using clock = std::chrono::steady_clock;

constexpr u32 kBoostConfig = 0x92220008;  // CPU 1020, GPU 460.8, EMC 1331.2 MHz (handheld)
constexpr u32 kGpuBoostMHz = 460;
constexpr u32 kEmcMHz = 1331;

bool s_apm = false;
bool s_applied = false;
bool s_steppedAside = false;  // something else owns the clocks: not asked again this session
u32 s_saved = 0;
clock::time_point s_verifyAt{};
bool s_verifyPending = false;

}  // namespace

void clocks_mhz(unsigned& cpu, unsigned& gpu, unsigned& emc) {
    cpu = gpu = emc = 0;
    static bool s_clk = R_SUCCEEDED(clkrstInitialize());
    if (!s_clk) {
        return;
    }
    const struct {
        PcvModuleId id;
        unsigned* out;
    } mods[] = {{PcvModuleId_CpuBus, &cpu}, {PcvModuleId_GPU, &gpu}, {PcvModuleId_EMC, &emc}};
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

namespace {

bool apply() {
    if (!s_apm) {
        s_apm = R_SUCCEEDED(apmInitialize());
        if (!s_apm) {
            Log.warn("apm unavailable: no GPU boost");
            s_steppedAside = true;
            return false;
        }
    }
    if (R_FAILED(apmGetPerformanceConfiguration(ApmPerformanceMode_Normal, &s_saved))) {
        s_saved = 0;
    }
    const Result rc = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, kBoostConfig);
    if (R_FAILED(rc)) {
        Log.warn("GPU boost: apmSetPerformanceConfiguration(0x{:08x}) rc 0x{:x}; stepping aside", kBoostConfig, rc);
        s_steppedAside = true;
        return false;
    }
    s_applied = true;
    s_verifyPending = true;
    s_verifyAt = clock::now() + std::chrono::milliseconds{1500};
    const int preset = profile::detectCurrent();
    Log.info("GPU boost on (setting on, handheld; preset {}): configuration 0x{:08x} (was 0x{:08x})",
        preset < 0 ? "Custom" : profile::name(preset), kBoostConfig, s_saved);
    return true;
}

void give_back(const char* why) {
    if (!s_applied) {
        return;
    }
    s_applied = false;
    s_verifyPending = false;
    if (s_saved != 0) {
        apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, s_saved);
    }
    Log.info("GPU boost off ({}): configuration 0x{:08x}", why, s_saved);
}

}  // namespace

bool active() {
    return s_applied;
}

void tick() {
    const auto now = clock::now();
    const bool setting = getSettings().game.gpuBoost.getValue();
    const bool handheld = appletGetOperationMode() == AppletOperationMode_Handheld;
    const bool front = appletGetFocusState() == AppletFocusState_InFocus;
    const bool want = setting && handheld && front && !s_steppedAside;
    if (want && !s_applied) {
        apply();
    } else if (!want && s_applied) {
        give_back(!setting ? "turned off" : !handheld ? "docked" : !front ? "HOME" : "stepped aside");
    }
    if (s_applied && s_verifyPending && now >= s_verifyAt) {
        s_verifyPending = false;
        unsigned cpu = 0, gpu = 0, emc = 0;
        clocks_mhz(cpu, gpu, emc);
        if (emc != 0 && emc != kEmcMHz) {
            Log.warn("GPU boost: the memory clock is {} MHz, not {}: undone", emc, kEmcMHz);
            s_steppedAside = true;
            give_back("memory clock moved");
        } else if (gpu != 0 && gpu != kGpuBoostMHz) {
            Log.warn("GPU boost: the GPU is at {} MHz, not {}: something else sets the clocks (sys-clk?); stepping "
                     "aside for this session",
                gpu, kGpuBoostMHz);
            s_steppedAside = true;
            give_back("clocks owned elsewhere");
        } else {
            Log.info("GPU boost verified: GPU {} MHz, memory {} MHz", gpu, emc);
        }
    }
}

void restore() {
    give_back("quit");
}

}  // namespace dusk::gpu_boost
