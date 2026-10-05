#pragma once
// aleks2 (H35): what kind of frame the game clock produced, counted since launch. The
// governor (interp_governor.hpp) already counts its transitions; this counts frames, so a
// report can say how many frames were really interpolated rather than infer it from a frame
// rate. Pure (no game headers) so the host tests can use it; game_clock.cpp owns the one
// instance and counts every advance().
#include <cstdint>

namespace dusk::interp_frames {

struct Counters {
    // Every frame the clock produced (one per advance()).
    std::uint64_t frames = 0;
    // Drawn from the simulation state as is, without interpolation: interpolation off, not
    // wanted, or given up by the governor.
    std::uint64_t normal = 0;
    // Of the normal frames, those where interpolation was wanted but the governor had it
    // stalled.
    std::uint64_t stalled = 0;
    // Interpolating, and no simulation tick was due: a frame that exists only because of
    // interpolation (accepted: it was drawn).
    std::uint64_t interpolatedOnly = 0;
    // Interpolating, with one or more simulation ticks run in the same frame.
    std::uint64_t interpolatedWithTick = 0;
    // Interpolated frames the prediction refused: it slept until the next tick instead of
    // drawing one that would overrun it (rejected).
    std::uint64_t predictiveRejects = 0;
    // Simulation ticks run, for the simulation rate over a window.
    std::uint64_t simTicks = 0;

    void count_frame(bool interpolationWanted, bool interpolating, bool governorStalled, int numSimTicks) {
        ++frames;
        simTicks += numSimTicks > 0 ? static_cast<std::uint64_t>(numSimTicks) : 0;
        if (!interpolating) {
            ++normal;
            if (interpolationWanted && governorStalled) {
                ++stalled;
            }
        } else if (numSimTicks == 0) {
            ++interpolatedOnly;
        } else {
            ++interpolatedWithTick;
        }
    }
    void count_predictive_reject() { ++predictiveRejects; }
};

// Field by field difference of two readings (later minus earlier).
inline Counters delta(const Counters& before, const Counters& after) {
    return {
        .frames = after.frames - before.frames,
        .normal = after.normal - before.normal,
        .stalled = after.stalled - before.stalled,
        .interpolatedOnly = after.interpolatedOnly - before.interpolatedOnly,
        .interpolatedWithTick = after.interpolatedWithTick - before.interpolatedWithTick,
        .predictiveRejects = after.predictiveRejects - before.predictiveRejects,
        .simTicks = after.simTicks - before.simTicks,
    };
}

}  // namespace dusk::interp_frames
