#include "dusk/dynres.hpp"

#include "dusk/dusk.h"
#include "dusk/frame_telemetry.hpp"
#include "dusk/logging.h"
#include "dusk/perf_core.hpp"
#include "dusk/settings.h"
#include "dusk/shader_warmup_runtime.hpp"
#include "dusk/switch_display.hpp"

#include <aurora/tuning.h>
#include <dolphin/vi.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace dusk::dynres {
namespace {

using clock = std::chrono::steady_clock;

// One decision window. FIFO vsync quantizes the frame rate to 60/30/20/15, so a window wants
// a few dozen frames; a second at 30 is 30 of them.
constexpr auto kWindow = std::chrono::milliseconds(1000);
// A gap over this is a load, a suspend or a stall: the window says nothing about resolution.
constexpr float kLoadingGapMs = 250.f;
// After a change the resize itself hitches and the new size settles: not counted.
constexpr auto kSettleAfterChange = std::chrono::milliseconds(1000);

bool s_enabledLast = false;
bool s_stress = false;
int s_baseLast = -1;
int s_autoLast = -1;
int s_ceilingRows = 0;
int s_overrideRows = 0;
unsigned s_transitions = 0;
perf::DrsPolicy s_policy;
clock::time_point s_start = clock::now();
clock::time_point s_windowStart{};
clock::time_point s_changedAt{};
std::vector<perf::FrameSample> s_window;
bool s_windowTainted = false;
bool s_gpuSeen = false;

// What "Auto" resolves to right now, folded together with the display mode so that a change
// of either re-anchors the ceiling.
int autoValue() {
#ifdef __SWITCH__
    return ladder::exact_value(display::auto_rows()) + 100 * static_cast<int>(display::mode());
#else
    return ladder::exact_value(720);
#endif
}

int computeCeilingRows() {
    const int base = getSettings().game.internalResolutionScale.getValue();
#ifdef __SWITCH__
    return respol::resolve(base, getSettings().game.autoResolutionPolicy.getValue(), display::mode()).rows;
#else
    const int locked = ladder::exact_height(base);
    return locked > 0 ? locked : 720;
#endif
}

double seconds() {
    return std::chrono::duration<double>(clock::now() - s_start).count();
}

void imposeRows(int rows) {
    if (rows == s_overrideRows) {
        return;
    }
    s_overrideRows = rows;
    ++s_transitions;
    s_changedAt = clock::now();
    if (rows > 0) {
        VISetFrameBufferHeight(static_cast<uint32_t>(rows));
    } else {
        applyInternalResolutionScale(getSettings().game.internalResolutionScale.getValue());
    }
}

void giveBack() {
    if (s_overrideRows != 0) {
        imposeRows(0);
    }
}

void beginWindow(clock::time_point now) {
    s_windowStart = now;
    s_window.clear();
    s_windowTainted = false;
}

void setPool(bool on) {
    aurora_set_render_target_pool(on ? frame_telemetry::lab().rtPool : 0);
}

void applyLevel(const perf::Decision& d, const perf::WindowStats& w) {
    const int pct = s_policy.percent(d.level);
    const int rows = d.level == 0 ? 0 : perf::rows_for_percent(s_ceilingRows, pct);
    DuskLog.info("dynres: {} -> {} % ({} rows of {}); window {} frames, {:.0f} % missed (gpu {:.0f} % / cpu {:.0f} %), "
                 "gpu median {:.1f} ms p90 {:.1f} ms util {:.0f} %, main {:.1f} ms, fifo {:.1f} ms",
        d.why, pct, rows == 0 ? s_ceilingRows : rows, s_ceilingRows, w.frames, w.missedPct(),
        100.0 * w.gpuMissShare(), 100.0 * w.cpuMissShare(), w.gpuMedianMs, w.gpuP90Ms, 100.0 * w.gpuUtil,
        w.mainWorkMedianMs, w.fifoBusyMedianMs);
    imposeRows(rows);
}

void disable() {
    giveBack();
    s_policy.reset();
    if (!s_stress) {
        setPool(false);
    }
    frame_telemetry::want_gpu_timing(frame_telemetry::kDrs, false);
}

}  // namespace

void reset() {
    if (!s_stress) {
        giveBack();
    }
    s_baseLast = -1;
    s_autoLast = -1;
    s_policy = perf::DrsPolicy(frame_telemetry::lab().drsSteps);
    s_window.clear();
}

unsigned transitionCount() {
    return s_transitions;
}

int currentOverride() {
    return s_overrideRows;
}

int ceilingRows() {
    return computeCeilingRows();
}

Status status() {
    Status s;
    s.enabled = getSettings().game.dynamicResolution.getValue();
    s.gpuTiming = s_gpuSeen;
    s.ceilingRows = s_ceilingRows > 0 ? s_ceilingRows : computeCeilingRows();
    s.rows = s_overrideRows > 0 ? s_overrideRows : s.ceilingRows;
    s.percent = s_overrideRows > 0 && !s_stress ? s_policy.percent() : 100;
    if (s_stress) {
        s.state = "stress test running";
    } else if (!s.enabled) {
        s.state = "off";
    } else if (frame_telemetry::lab().gpuTiming == perf::GpuTiming::Off) {
        s.state = "no GPU timing (perf-lab.ini gpu_timing=off): resolution left alone";
    } else {
        s.state = s_policy.state();
    }
    return s;
}

void stress_begin() {
    s_stress = true;
    s_policy.reset();
    giveBack();
    setPool(true);
}

void stress_apply_rows(int rows) {
    imposeRows(rows);
}

void stress_end() {
    giveBack();
    s_stress = false;
    reset();
    setPool(getSettings().game.dynamicResolution.getValue());
}

void update() {
    if (s_stress) {
        return;
    }
    const bool enabled = getSettings().game.dynamicResolution.getValue();
    if (!enabled) {
        if (s_enabledLast) {
            disable();
            DuskLog.info("dynres: off");
        }
        s_enabledLast = false;
        return;
    }
    const auto now = clock::now();
    if (!s_enabledLast) {
        s_enabledLast = true;
        s_policy = perf::DrsPolicy(frame_telemetry::lab().drsSteps);
        setPool(true);
        frame_telemetry::want_gpu_timing(frame_telemetry::kDrs, true);
        s_baseLast = -1;
        DuskLog.info("dynres: Auto (only when the GPU is the limit); {}", perf::describe(frame_telemetry::lab()));
    }

    // The ceiling follows the user's resolution and what Auto means right now (dock, Flip).
    // A new ceiling is a new situation: back to the top, history forgotten.
    const int base = getSettings().game.internalResolutionScale.getValue();
    const int autoNow = autoValue();
    if (base != s_baseLast || autoNow != s_autoLast) {
        s_baseLast = base;
        s_autoLast = autoNow;
        // Whatever changed already applied the user's value; ours no longer stands.
        if (s_overrideRows != 0) {
            s_overrideRows = 0;
            applyInternalResolutionScale(base);
        }
        s_ceilingRows = computeCeilingRows();
        s_policy.reset();
        beginWindow(now);
        s_changedAt = now;
        return;
    }

    const auto& f = frame_telemetry::last();
    if (f.gpuMs >= 0.f) {
        s_gpuSeen = true;
    }
    if (now - s_changedAt < kSettleAfterChange) {
        beginWindow(now);
        return;
    }
    std::string why;
    if (f.intervalMs > kLoadingGapMs || f.queuedPipelines > 0 || shader_warmup::busy(why)) {
        s_windowTainted = true;
    }
    s_window.push_back(f);
    if (now - s_windowStart < kWindow) {
        return;
    }
    const auto w = perf::summarize(s_window, frame_telemetry::budget_ms());
    const bool tainted = s_windowTainted || w.frames < 10;
    beginWindow(now);
    const auto d = s_policy.evaluate(w, tainted, seconds());
    if (frame_telemetry::lab().drsVerbose) {
        DuskLog.info("dynres window: {} frames, {:.0f} % missed, gpu util {:.0f} %, median {:.1f} ms, tainted {} -> {}",
            w.frames, w.missedPct(), 100.0 * w.gpuUtil, w.gpuMedianMs, tainted, s_policy.state());
    }
    if (d.action != perf::Action::Hold) {
        applyLevel(d, w);
    }
}

}  // namespace dusk::dynres
