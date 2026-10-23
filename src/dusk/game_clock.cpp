#include "dusk/game_clock.h"

#include <aurora/time.hpp>

#include "dusk/interp_governor.hpp"
#include "dusk/interp_frames.hpp"
#include "dusk/logging.h"
#include "dusk/settings.h"
#ifdef __SWITCH__
#include "dusk/frame_telemetry.hpp"  // V1-rc12: interp_floor_fps
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace dusk::game_clock {

using native_clock = aurora::time::native_clock;
using game_clock = aurora::time::game_clock;

FrameTiming g_frameTiming{.dt = kUiInitialDt};

namespace {
bool s_initialized = false;
bool s_simTickActive = false;
native_clock::time_point s_previousNativeSample{};
game_clock::time_point s_latestGameSample{};
game_clock::time_point s_currentSnapshotTime{};
game_clock::time_point s_pendingSimTime{};
uint64_t s_presentationEpoch = 1;
bool s_timingModeInitialized = false;
bool s_previousSeparatePresentation = false;
bool s_previousInterpolating = false;
bool s_previousTimeStopped = false;

constexpr game_clock::duration kSimPeriodDuration =
    std::chrono::duration_cast<game_clock::duration>(std::chrono::duration<float>(kSimPeriod));
constexpr native_clock::duration kAbnormalGapResetThreshold = std::chrono::milliseconds(250);
constexpr int kMaxSimTicksPerFrame = static_cast<int>(aurora::time::kMaximumTimeScale) * 4;

// aleks2 (performance): predictive interpolation. An interpolated frame costs
// the render worker as much as a real one; when it does not fit before the
// next sim tick is due it only delays that tick, and the frames land at
// 33/50/33/50 ms instead of a clean 30. So the average native frame time is
// tracked and, when an interpolated frame is predicted to overrun the tick,
// the loop sleeps until the tick is due and draws the real frame instead.
// 60 where the console keeps up, 30 where it does not, same preset.
constexpr float kFrameTimeEmaWeight = 0.2f;
constexpr float kPredictMargin = 0.85f;
float s_avgFrameSeconds = 0.f;
bool s_skipFrameTimeSample = false;

// aleks2 (performance): and the case the prediction above cannot reach.
//
// The simulation runs at 30 Hz and interpolation fills the gaps between ticks.
// Below 30 fps there are no gaps to fill -- the sim is already behind every
// frame, so numSimTicks is never zero and the prediction never runs -- but the
// machinery stays on and keeps paying for the state capture and the blend. The
// result is the wrong way round: unlocking the frame rate in a heavy place
// measured *slower* than leaving it capped, because uncapped was doing the
// interpolation work and getting nothing for it.
//
// So interpolation gives up while the console cannot beat the sim rate, and
// comes back when it can. H33: the decision lives in interp_governor.hpp (pure, host
// tested). The way back is decided on the real work of a frame -- the loop interval minus
// the time the limiters slept -- because while it is given up the tick limiter pads every
// frame to 33.3 ms and an interval-based resume rule could never fire.
//
// The limiters report their sleep through note_wait(). It accumulates in s_pendingWaitNs
// and advance() consumes it on every call, so a wait is only ever attributed to the
// interval it happened in: the one that ends at the next advance(). Anything that makes an
// interval unrepresentative (a reset, a rate change, a predictive sleep, a load or pause
// gap) discards the sample; the wait it carried is consumed with it.
dusk::governor::Governor s_governor;
std::atomic<int64_t> s_pendingWaitNs{0};
std::atomic<int64_t> s_waitTotalNs[2] = {0, 0};
bool s_discardNextSample = false;
float s_lastTimeScale = 1.0f;
// H35: frames by kind (interp_frames.hpp). Main thread only, like advance().
dusk::interp_frames::Counters s_frameCounters;

float ui_dt() {
    if (s_simTickActive) {
        return kSimPeriod;
    }

    const float maximumDt = kUiMaximumDt * aurora::time::scale();
    return std::clamp(g_frameTiming.dt, 0.0f, maximumDt);
}
}  // namespace

void initialize() {
    if (s_initialized) {
        return;
    }
    s_previousNativeSample = native_clock::now();
    s_latestGameSample = game_clock::now();
    s_currentSnapshotTime = s_latestGameSample;
    s_pendingSimTime = s_latestGameSample;
    s_initialized = true;
}

void reset() {
    const auto nativeNow = native_clock::now();
    s_previousNativeSample = nativeNow;
    s_latestGameSample = game_clock::now();
    s_currentSnapshotTime = s_latestGameSample - kSimPeriodDuration;
    s_pendingSimTime = s_currentSnapshotTime;
    s_simTickActive = false;
    ++s_presentationEpoch;
    s_discardNextSample = true;
    s_pendingWaitNs.store(0, std::memory_order_relaxed);
    // H36: an unpause or a rate change is a break in the loop, not just stale estimates.
    s_governor.discontinuity(std::chrono::duration<double>(nativeNow.time_since_epoch()).count());
}

void set_sim_rate(float hz) {
    const float maximumHz = aurora::time::kMaximumTimeScale / kSimPeriod;
    aurora::time::set_scale(std::clamp(hz, 1.0f, maximumHz) * kSimPeriod);
    reset();
}

float get_sim_rate() {
    return aurora::time::scale() / kSimPeriod;
}

namespace {
const FrameTiming& advance_impl();
}  // namespace

const FrameTiming& advance() {
    const FrameTiming& out = advance_impl();
    s_frameCounters.count_frame(getSettings().game.enableFrameInterpolation.getValue() != FrameInterpMode::Off,
        out.interpolating, s_governor.stalled(), out.numSimTicks);
    return out;
}

namespace {
const FrameTiming& advance_impl() {
    const auto nativeNow = native_clock::now();
    const auto gameNow = game_clock::now();
    const auto nativeFrameGap = nativeNow - s_previousNativeSample;
    const auto gameFrameGap = gameNow - s_latestGameSample;
    s_previousNativeSample = nativeNow;
    s_latestGameSample = gameNow;

    auto& out = g_frameTiming;
    out = {
        .dt = std::chrono::duration<float>(gameFrameGap).count(),
        .presentationEpoch = s_presentationEpoch,
    };
    // Every call consumes the wait the limiters reported since the previous one.
    const int64_t waitNs = s_pendingWaitNs.exchange(0, std::memory_order_relaxed);
    const float gapSeconds = std::chrono::duration<float>(nativeFrameGap).count();
    bool governorSample = false;
    {
        if (s_skipFrameTimeSample) {
            s_skipFrameTimeSample = false;  // the gap was a predictive sleep, not a frame
        } else if (gapSeconds > 0.f && gapSeconds < 0.25f) {
            s_avgFrameSeconds = s_avgFrameSeconds <= 0.f ?
                                    gapSeconds :
                                    s_avgFrameSeconds + (gapSeconds - s_avgFrameSeconds) * kFrameTimeEmaWeight;
            governorSample = !s_discardNextSample;
        } else if (gapSeconds >= dusk::governor::kMaxSampleSeconds) {
            // H36: a load or pause long enough to stop the loop. The scene after it starts
            // with the initial cooldown (interp_governor.hpp, discontinuity).
            const bool wasStalled = s_governor.stalled();
            s_governor.discontinuity(std::chrono::duration<double>(nativeNow.time_since_epoch()).count());
            if (wasStalled) {
                DuskLog.info("interp: discontinuity while stalled (gap={:.0f} ms); cooldown back to {:.0f} s",
                    gapSeconds * 1000.f, dusk::governor::kCooldownInitial);
            }
        }
        s_discardNextSample = false;
    }

    const float timeScale = aurora::time::scale();
    if (timeScale != s_lastTimeScale) {
        // A rate change makes the previous estimates describe another loop.
        s_lastTimeScale = timeScale;
        s_governor.flush_estimates();
        governorSample = false;
    }
    const bool interpolationWanted =
        getSettings().game.enableFrameInterpolation.getValue() != FrameInterpMode::Off;

    // See interp_governor.hpp. Shares the Predictive Interpolation switch, because it is
    // the same idea: do not draw an interpolated frame that cannot pay for itself.
    if (!interpolationWanted || s_avgFrameSeconds <= 0.f ||
        !getSettings().game.predictiveInterpolation.getValue())
    {
        s_governor.disable();
    } else if (governorSample) {
#ifdef __SWITCH__
        // V1-rc12: Capped at 60 on the 60 Hz panel: interpolate only where 60 is within reach (interp_floor_fps).
        // V1-rc16: the floor is off by default, so Capped 60 gives up only where Unlimited does (worse than 30).
        {
            const unsigned floorFps = dusk::frame_telemetry::lab().interpFloorFps;
            const bool capped60 = getSettings().game.enableFrameInterpolation.getValue() == FrameInterpMode::Capped &&
                                  getSettings().video.maxFrameRate.getValue() >= 55.0;
            if (floorFps != 0 && capped60) {
                s_governor.set_thresholds(1.0f / static_cast<float>(floorFps), 0.015f);
                // V1-rc12: one GPU frame has to fit in the floor's interval, with a 10 % margin (interp_governor.hpp).
                s_governor.set_gpu_gate(0.9f / static_cast<float>(floorFps));
                // post-rc12: a sustained miss to stall, a quick first retry, back-off in a scene that keeps failing.
                if (s_governor.pacing().enterDwellFrames != dusk::governor::kCapped60Pacing.enterDwellFrames) {
                    s_governor.set_pacing(dusk::governor::kCapped60Pacing);
                }
            } else {
                s_governor.set_thresholds(dusk::governor::kGiveUpSeconds, dusk::governor::kResumeSeconds);
                s_governor.set_gpu_gate(0.f);
                if (s_governor.pacing().enterDwellFrames != dusk::governor::Pacing{}.enterDwellFrames) {
                    s_governor.set_pacing(dusk::governor::Pacing{});
                }
            }
        }
#endif
        const double nowSeconds =
            std::chrono::duration<double>(nativeNow.time_since_epoch()).count();
        const float waitSeconds = static_cast<float>(waitNs) * 1e-9f;
        float gpuSeconds = -1.f;
#ifdef __SWITCH__
        // V1-rc12: the last frame's GPU time (deko3d's timestamps; Dawn's when GPU timing is on), for the gpu gate.
        if (const float gpuMs = dusk::frame_telemetry::last().gpuMs; gpuMs >= 0.f) {
            gpuSeconds = gpuMs * 1e-3f;
        }
#endif
        const auto event = s_governor.sample(nowSeconds, gapSeconds, waitSeconds, gpuSeconds);
        if (event != dusk::governor::Event::None) {
            const auto& st = s_governor.stats();
            DuskLog.info(
                "interp: {} (quality={} work={:.1f} ms gap={:.1f} ms gpu={:.1f} ms cooldown={:.0f} s enters={} "
                "exits={} failures={} gpu_blocked={})",
                event == dusk::governor::Event::Entered ? "stalled" : "resumed",
                getSettings().game.interpolationQuality.getValue() == InterpQuality::Fancy ? "Fancy" : "Fast",
                st.lastWorkSeconds * 1000.f, st.lastGapSeconds * 1000.f, st.lastGpuSeconds * 1000.f,
                st.cooldownSeconds, st.enters, st.exits, st.failures, st.gpuBlockedFrames);
        }
    }
    const bool interpolating = interpolationWanted && !s_governor.stalled();
    const bool separatePresentation = interpolating || timeScale != 1.0f;
    out.interpolating = interpolating;
    out.separatePresentation = separatePresentation;

    const bool timeStopped = timeScale == 0.0f;
    const bool timingModeChanged =
        s_timingModeInitialized &&
        (separatePresentation != s_previousSeparatePresentation ||
            interpolating != s_previousInterpolating || timeStopped != s_previousTimeStopped);
    const bool abnormalGap = nativeFrameGap > kAbnormalGapResetThreshold;
    if (timingModeChanged || abnormalGap) {
        ++s_presentationEpoch;
        out.presentationEpoch = s_presentationEpoch;
    }
    s_timingModeInitialized = true;
    s_previousSeparatePresentation = separatePresentation;
    s_previousInterpolating = interpolating;
    s_previousTimeStopped = timeStopped;

    if (!separatePresentation) {
        s_currentSnapshotTime = gameNow;
        out.numSimTicks = 1;
        return out;
    }

    const auto simulationTarget = interpolating ? gameNow - kSimPeriodDuration : gameNow;
    if (timeStopped || abnormalGap) {
        s_currentSnapshotTime = simulationTarget;
        out.numSimTicks = 0;
        return out;
    }

    int numSimTicks = 0;
    auto projectedSnapshotTime = s_currentSnapshotTime;
    while (numSimTicks < kMaxSimTicksPerFrame) {
        const bool tickDue = interpolating ?
                                 projectedSnapshotTime < simulationTarget :
                                 projectedSnapshotTime + kSimPeriodDuration <= simulationTarget;
        if (!tickDue) {
            break;
        }
        projectedSnapshotTime += kSimPeriodDuration;
        numSimTicks++;
    }
    out.numSimTicks = numSimTicks;

    if (interpolating && numSimTicks == 0 && timeScale > 0.f && s_avgFrameSeconds > 0.f &&
        getSettings().game.predictiveInterpolation.getValue())
    {
        // Game time until the next tick is due, in native seconds.
        const auto dueIn = (s_currentSnapshotTime + kSimPeriodDuration) - simulationTarget;
        const float dueSeconds = std::chrono::duration<float>(dueIn).count() / timeScale;
        if (dueSeconds > 0.f && dueSeconds < s_avgFrameSeconds * kPredictMargin) {
            std::this_thread::sleep_for(std::chrono::duration<float>(dueSeconds));
            // Re-sample with the tick now due. The sleep is not a frame, so
            // the next gap sample is discarded; the sim frame after it is
            // what the prediction should learn from.
            s_skipFrameTimeSample = true;
            s_frameCounters.count_predictive_reject();
            return advance_impl();
        }
    }
    return out;
}
}  // namespace

bool interpolation_stalled() {
    return s_governor.stalled();
}

void note_wait(WaitKind kind, uint64_t ns) {
    const auto value = static_cast<int64_t>(ns);
    s_pendingWaitNs.fetch_add(value, std::memory_order_relaxed);
    s_waitTotalNs[static_cast<int>(kind)].fetch_add(value, std::memory_order_relaxed);
}

uint64_t wait_total_ns(WaitKind kind) {
    return static_cast<uint64_t>(s_waitTotalNs[static_cast<int>(kind)].load(std::memory_order_relaxed));
}

interp_frames::Counters interp_frame_counters() {
    return s_frameCounters;
}

InterpTelemetry interp_telemetry() {
    const auto& st = s_governor.stats();
    InterpTelemetry t{};
    t.stalled = s_governor.stalled();
    t.enters = st.enters;
    t.exits = st.exits;
    t.attempts = st.attempts;
    t.failures = st.failures;
    t.cooldownBlockedFrames = st.cooldownBlockedFrames;
    t.stalledSeconds = st.stalledSeconds;
    t.workMs = st.lastWorkSeconds * 1000.f;
    t.gapMs = st.lastGapSeconds * 1000.f;
    t.cooldownSeconds = static_cast<float>(st.cooldownSeconds);
    return t;
}

void begin_sim_tick() {
    s_pendingSimTime = g_frameTiming.separatePresentation ? s_currentSnapshotTime + kSimPeriodDuration :
                                                            s_latestGameSample;
    s_simTickActive = true;
}

void commit_sim_tick() {
    if (s_simTickActive) {
        s_currentSnapshotTime = s_pendingSimTime;
        s_simTickActive = false;
    } else {
        s_currentSnapshotTime += kSimPeriodDuration;
    }
}

bool is_sim_frame() {
    return !g_frameTiming.separatePresentation || s_simTickActive;
}

bool is_presentation_frame() {
    return !g_frameTiming.separatePresentation || !s_simTickActive;
}

float sample_interpolation_step() {
    const float step =
        std::chrono::duration<float>(game_clock::now() - s_currentSnapshotTime).count() /
        kSimPeriod;
    return std::clamp(step, 0.0f, 1.0f);
}

double sample_time() {
    const auto now = s_simTickActive ? s_pendingSimTime : game_clock::now();
    return std::chrono::duration<double>(now.time_since_epoch()).count();
}

float original_frames() {
    return ui_dt() / kSimPeriod;
}

float consume_interval(double& lastSample) {
    const double now = sample_time();
    const float dt = std::max(0.0, now - lastSample);
    lastSample = now;
    return dt;
}

}  // namespace dusk::game_clock
