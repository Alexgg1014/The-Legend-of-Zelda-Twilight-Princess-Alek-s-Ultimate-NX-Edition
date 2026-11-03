// aleks2 (V1-rc16): see gpu_boost.hpp. Switch only (CMakeLists.txt).
#include "dusk/gpu_boost.hpp"

#include "dusk/config.hpp"
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

// v1.1: one official handheld configuration per GPU Boost level (memory always 1331.2, as stock handheld).
// The IDs are the ones SwitchWakerHD documents and applies (runtime/src/platform/settings_switch.cpp).
struct BoostConfig {
    u32 id;
    u32 gpuMHz;
};
constexpr BoostConfig kConfigs[] = {
    {0, 0},              // Off
    {0x00020004, 384},   // CPU 1020, GPU 384, EMC 1331.2 MHz
    {0x92220008, 460},   // CPU 1020, GPU 460.8, EMC 1331.2 MHz
};
constexpr u32 kEmcMHz = 1331;

GpuBoostLevel s_appliedLevel = GpuBoostLevel::Off;
int s_labLevel = -1;  // perf-lab.ini gpu_level: -1 = the setting

// The level asked for; v1.1 reads rc16's on/off setting once (on = 460.8, what it meant) and clears it.
GpuBoostLevel wanted_level() {
    auto& g = getSettings().game;
    if (s_labLevel >= 0) {
        return static_cast<GpuBoostLevel>(s_labLevel);
    }
    if (g.gpuBoost.getValue()) {
        if (g.gpuBoostLevel.getValue() == GpuBoostLevel::Off) {
            g.gpuBoostLevel.setValue(GpuBoostLevel::Mhz460);
        }
        g.gpuBoost.setValue(false);
        config::save();
        Log.info("GPU boost: the old on/off setting became the 460.8 MHz level");
    }
    return g.gpuBoostLevel.getValue();
}

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

bool apply(GpuBoostLevel level) {
    const BoostConfig& c = kConfigs[static_cast<int>(level)];
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
    const Result rc = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, c.id);
    if (R_FAILED(rc)) {
        Log.warn("GPU boost: apmSetPerformanceConfiguration(0x{:08x}) rc 0x{:x}; stepping aside", c.id, rc);
        s_steppedAside = true;
        return false;
    }
    s_applied = true;
    s_appliedLevel = level;
    s_verifyPending = true;
    s_verifyAt = clock::now() + std::chrono::milliseconds{1500};
    const int preset = profile::detectCurrent();
    Log.info("GPU boost on ({} MHz, handheld; preset {}): configuration 0x{:08x} (was 0x{:08x})", c.gpuMHz,
        preset < 0 ? "Custom" : profile::name(preset), c.id, s_saved);
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

void set_lab_level(int level) {
    s_labLevel = level;
}

void tick() {
    const auto now = clock::now();
    const GpuBoostLevel level = wanted_level();
    const bool setting = level != GpuBoostLevel::Off;
    const bool handheld = appletGetOperationMode() == AppletOperationMode_Handheld;
    const bool front = appletGetFocusState() == AppletFocusState_InFocus;
    const bool want = setting && handheld && front && !s_steppedAside;
    if (want && s_applied && level != s_appliedLevel) {
        give_back("level changed");  // the configuration in force before, then the new level below
    }
    if (want && !s_applied) {
        apply(level);
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
        } else if (gpu != 0 && gpu != kConfigs[static_cast<int>(s_appliedLevel)].gpuMHz) {
            Log.warn("GPU boost: the GPU is at {} MHz, not {}: something else sets the clocks (sys-clk?); stepping "
                     "aside for this session",
                gpu, kConfigs[static_cast<int>(s_appliedLevel)].gpuMHz);
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
