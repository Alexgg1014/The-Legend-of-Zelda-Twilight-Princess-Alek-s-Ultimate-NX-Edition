#pragma once

// aleks2 (performance): dynamic resolution.
//
// V1-c1: "Auto -- only when the GPU is the limit". Off by default. The decisions are made in
// perf_core.hpp (DrsPolicy) from per-frame telemetry (frame_telemetry.hpp): a few fixed steps
// below the ceiling (100/85/75 % of its rows by default, perf-lab.ini drs_steps), down only
// when frames are missed and the GPU explains them, never on a CPU/render-worker limit, with
// hysteresis both ways and no change during loads or pipeline compiles. Without GPU timing it
// never moves.
//
// The ceiling is what the user's internalResolutionScale renders after the resolution policy
// (900 docked, 720 handheld, 540 Flip Grip with Auto). Steps below it are exact row counts;
// at the ceiling the user's own value is handed back. While Auto (or the stress mode) is on,
// Aurora keeps a render-target set per size (aurora_set_render_target_pool) so going back to a
// step already visited allocates nothing.

#include <string>

namespace dusk::dynres {

// Once per presented frame, main thread, after frame_telemetry::tick().
void update();

// Forget history and return to the ceiling (preset applied, feature toggled).
void reset();

// Rows the controller (or the stress mode) imposes right now, 0 when the user's value is in
// effect.
int currentOverride();

// H28: how many times the resolution was changed by the controller or the stress mode since
// launch. Only ever grows, so a benchmark reads it before and after its window.
unsigned transitionCount();

// V1-c1: for the settings screen, the benchmark report and the log.
struct Status {
    bool enabled = false;
    bool gpuTiming = false;  // GPU figures have arrived since Auto was turned on
    int percent = 100;
    int rows = 0;            // in effect (the ceiling when not overridden)
    int ceilingRows = 0;
    std::string state;       // DrsPolicy::state(), or why it is idle
};
Status status();

// The ceiling rows for the current mode and settings (what 100 % means).
int ceilingRows();

// Stress mode (drs_stress.cpp) owns the resolution while it runs: rows > 0 imposes that many
// rows, 0 hands the user's value back. The controller stays out of the way meanwhile.
void stress_begin();
void stress_apply_rows(int rows);
void stress_end();

}  // namespace dusk::dynres
