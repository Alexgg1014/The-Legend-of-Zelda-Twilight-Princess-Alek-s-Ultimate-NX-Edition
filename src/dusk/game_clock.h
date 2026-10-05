#pragma once

#include "dusk/interp_frames.hpp"

#include <cstdint>

namespace dusk::game_clock {

// Amount of time that a simulation tick advances
constexpr float kSimPeriod = 1.0f / 30.0f;

// aleks2: true while interpolation has given up because the console cannot beat
// the simulation rate. Reported in the session log so a run says which way it
// went without anyone having to guess.
bool interpolation_stalled();

// H33: time the loop spent asleep in a limiter, reported by whoever slept (the game's tick
// limiter in JFWDisplay::waitForTick, the main loop's Capped limiter). Used to tell the work
// of a frame from its padding, and by the benchmark. Report the sleep that actually elapsed,
// not the one that was asked for.
enum class WaitKind { Tick = 0, Limiter = 1 };
void note_wait(WaitKind kind, uint64_t ns);
// Monotonic total of a kind since launch; the benchmark samples it once per frame.
uint64_t wait_total_ns(WaitKind kind);

// H33: what the interpolation governor (interp_governor.hpp) has done since launch.
struct InterpTelemetry {
    bool stalled;
    uint32_t enters;
    uint32_t exits;
    uint32_t attempts;
    uint32_t failures;
    uint32_t cooldownBlockedFrames;
    double stalledSeconds;
    float workMs;  // EMAs at the last sample or transition
    float gapMs;
    float cooldownSeconds;
};
InterpTelemetry interp_telemetry();

// H35: frames by kind since launch (interp_frames.hpp), counted once per advance().
interp_frames::Counters interp_frame_counters();

constexpr float kUiMaximumDt = 0.05f;
constexpr float kUiInitialDt = 1.0f / 60.0f;

float original_frames();

struct FrameTiming {
    // Amount of time elapsed in seconds since the last advance
    float dt;
    // Whether interpolation is active
    bool interpolating;
    // Run simulation and presentation separately (for interpolation or time scaling)
    bool separatePresentation;
    // Number of simulation ticks to run
    int numSimTicks;
    // Changes whenever presentation history must be discarded and re-anchored.
    uint64_t presentationEpoch;
};
extern FrameTiming g_frameTiming;

void initialize();
void reset();
const FrameTiming& advance();
void begin_sim_tick();
void commit_sim_tick();
float sample_interpolation_step();

bool is_sim_frame();
bool is_presentation_frame();

double sample_time();
float consume_interval(double& lastSample);

// Sets the effective simulation rate through the game clock time scale.
void set_sim_rate(float hz);
float get_sim_rate();

}  // namespace dusk::game_clock
