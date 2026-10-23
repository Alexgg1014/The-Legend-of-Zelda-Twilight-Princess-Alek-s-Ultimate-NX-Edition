#pragma once

// H33: decides when predictive interpolation gives up ("stalled") and when it comes back.
// Pure: no settings, no clocks, no logging; game_clock.cpp feeds it one sample per frame and
// acts on the event it returns. Tested on a host (platforms/switch/tools/test-interp-governor.sh).
//
// Why this exists. Interpolation gives up when the loop interval stays over ~35 ms (it cannot
// beat the 30 Hz sim). While it is given up the game runs its plain path, in which the
// frame limiter (JFWDisplay::waitForTick) sleeps until the 33.3 ms tick. The loop interval is
// then >= 33.3 ms by construction, so a "resume under 30 ms" rule measured on the interval
// can never fire: it was a dead end that only turning the option off and on cleared.
//
// So the way back is decided on the real work of a frame, `interval - waits`, where waits is
// the time the loop spent sleeping in the limiters. The way in still uses the interval (while
// interpolating there is no tick limiter, and an interval over 35 ms is genuinely too slow).
//
//   * Enter stalled: interval EMA > kGiveUpSeconds for kDwellFrames consecutive samples.
//   * Resume: work EMA < kResumeSeconds for kDwellFrames consecutive samples AND the cooldown
//     since entering has elapsed. The same threshold for Fast and Fancy: a threshold per
//     quality needs measurements (the H33 JSON reports), not a guess.
//   * Cooldown: 5 s; doubled (5, 10, 20, capped at 20) when it stalls again within 15 s of a
//     resume; back to 5 s after 15 s of interpolating without a relapse.
//   * Discontinuity (a load, a pause, a sim-rate change): the state is kept, the estimates and
//     votes are dropped and the cooldown starts again from 5 s, counted from the discontinuity.
//
// H36: the cap was 60 s and the stable reset 30 s. On H35-c1 hardware (Balanced 960x540,
// handheld) the zones stayed stalled 87-100 % of their 30 s windows with 767-901 frames each
// that met the resume rule and waited on the cooldown, and the penalty earned in one scene
// carried over into the next after a load. A cap of 20 s still keeps a scene that flaps to a
// few transitions a minute (interp_governor_test.cpp), and a load is a new scene, so it
// starts from the initial cooldown. Thresholds, dwell and the initial cooldown are unchanged.
#include <algorithm>
#include <cstdint>

namespace dusk::governor {

inline constexpr float kSimPeriod = 1.0f / 30.0f;
inline constexpr float kGiveUpSeconds = kSimPeriod * 1.05f;  // ~35.0 ms
inline constexpr float kResumeSeconds = 0.030f;              // 30 ms, work, Fast and Fancy alike
inline constexpr int kDwellFrames = 30;
inline constexpr float kEmaWeight = 0.2f;
inline constexpr float kMaxSampleSeconds = 0.25f;  // a longer gap is a load or a pause, not a frame

inline constexpr double kCooldownInitial = 5.0;
inline constexpr double kCooldownMax = 20.0;
inline constexpr double kRelapseWindow = 15.0;
inline constexpr double kStableReset = 15.0;

enum class Event { None, Entered, Resumed };

// post-rc12: how long a decision waits, per use. The defaults are the H33/H36 behaviour above. Capped 60 with a floor
// (game_clock.cpp) uses kCapped60Pacing: a miss has to last ~1.5 s, a first retry comes after 2 s, and a scene that keeps
// failing backs off to one try a minute (2, 4, 8 ... 60 s) instead of flapping every 5-20 s (rc12 console, log 155101:
// nine stalls in 167 s, Ordon ranch held at 30 while it interpolated at 43-51 FPS between them).
struct Pacing {
    int enterDwellFrames = kDwellFrames;
    int resumeDwellFrames = kDwellFrames;
    double cooldownInitial = kCooldownInitial;
    double cooldownMax = kCooldownMax;
    double relapseWindow = kRelapseWindow;
    double stableReset = kStableReset;
    // While interpolating, a sample under the give-up line takes one vote back instead of clearing them, so a scene
    // that misses most of the time (noisy, around the line) does stall; false keeps the clear-on-any-good-sample rule.
    bool decayVotes = false;
    // A miss this many times over the line counts severeVotes per sample: a scene far under the floor (a retry in
    // Hyrule Field at 33 FPS) goes back to 30 in ~enterDwellFrames / severeVotes samples. 0 = off.
    float severeFactor = 0.f;
    int severeVotes = 1;
};
inline constexpr Pacing kCapped60Pacing{
    .enterDwellFrames = 90,
    .resumeDwellFrames = 30,
    .cooldownInitial = 2.0,
    .cooldownMax = 60.0,
    .relapseWindow = 10.0,
    .stableReset = 20.0,
    .decayVotes = true,
    .severeFactor = 1.25f,
    .severeVotes = 3,
};

struct Stats {
    uint32_t enters = 0;
    uint32_t exits = 0;
    uint32_t attempts = 0;  // resumes attempted; every one is an exit
    uint32_t failures = 0;  // stalled again within kRelapseWindow of a resume
    double stalledSeconds = 0.0;
    float lastWorkSeconds = 0.f;  // EMAs at the last transition or sample
    float lastGapSeconds = 0.f;
    double cooldownSeconds = kCooldownInitial;
    uint32_t cooldownBlockedFrames = 0;  // frames that met the resume rule but waited on the cooldown
    uint32_t gpuBlockedFrames = 0;       // V1-rc12: frames that met it but whose GPU frame was too long (gpu gate)
    float lastGpuSeconds = 0.f;          // V1-rc12: GPU EMA at the last sample (0: unknown)
};

class Governor {
public:
    bool stalled() const { return mStalled; }
    const Stats& stats() const { return mStats; }

    // V1-rc12: the give-up (interval EMA) and resume (work EMA) thresholds, in seconds. The defaults are
    // kGiveUpSeconds/kResumeSeconds: give up only when the loop cannot beat the simulation rate. On a 60 Hz panel
    // with Capped 60 that leaves a 36-40 FPS loop interpolating, an uneven cadence that looks worse than a steady
    // 30 (rc11 console, Ordon / Kakariko outskirts): game_clock.cpp raises the floor there.
    void set_thresholds(float giveUpSeconds, float resumeSeconds) {
        mGiveUp = giveUpSeconds;
        mResume = resumeSeconds;
    }
    float give_up_seconds() const { return mGiveUp; }

    // V1-rc12: with a floor, an interpolating loop presents one rendered frame per interval, so it can only hold
    // the floor when one GPU frame fits in it. The resume rule measures the CPU (work) only: on the rc12 console
    // (deko3d, 540-720 rows) the GPU took 17-19 ms a frame, the rule let it resume every 5-20 s and each try fell
    // back within a second at 36-50 FPS -- short bursts of uneven frames. With a gate set, a resume also needs the
    // GPU EMA under it; without GPU figures (gpu < 0 in sample()) the rule is the CPU one, as before. 0 = no gate.
    void set_gpu_gate(float maxGpuSeconds) { mGpuGate = maxGpuSeconds; }
    // post-rc12: the dwell and cooldown schedule (Pacing). A change takes effect from the next decision; a cooldown
    // already longer than the new maximum is cut to it.
    void set_pacing(const Pacing& pacing) {
        const bool atInitial = mStats.cooldownSeconds == mPacing.cooldownInitial;
        mPacing = pacing;
        mStats.cooldownSeconds =
            atInitial ? mPacing.cooldownInitial : std::min(mStats.cooldownSeconds, mPacing.cooldownMax);
    }
    const Pacing& pacing() const { return mPacing; }
    float gpu_gate() const { return mGpuGate; }
    float gapEma() const { return mGapEma; }
    float workEma() const { return mWorkEma; }

    // Interpolation is off or predictive is disabled: nothing to decide, forget the state.
    // Counters are cumulative and are kept.
    void disable() {
        mStalled = false;
        mVotes = 0;
        mGapEma = 0.f;
        mWorkEma = 0.f;
    }

    // Drop the running estimates without leaving the current state (time scale change, mode
    // change, epoch bump): what was measured no longer describes the loop.
    void flush_estimates() {
        mVotes = 0;
        mGapEma = 0.f;
        mWorkEma = 0.f;
        mGpuEma = 0.f;
    }

    // A break in the loop: a load or pause gap, an unpause, a sim-rate change. What comes next
    // is another scene, so nothing measured or earned before it applies: the estimates and the
    // votes go, the cooldown goes back to kCooldownInitial and a resume before the break no
    // longer makes the next stall a relapse. The state stays as it is -- a stalled loop is not
    // let out here, it has to show kDwellFrames light samples taken after the break, and the
    // cooldown is counted from the break. Counters are cumulative and are kept.
    void discontinuity(double now) {
        flush_estimates();
        mStats.cooldownSeconds = mPacing.cooldownInitial;
        mHasResumed = false;
        if (mStalled) {
            mStalledAt = now;
        }
    }

    // One frame. `now` is a monotonic time in seconds, `gap` the native interval since the
    // previous sample, `waits` the limiter sleep that happened inside that interval. A sample
    // outside (0, kMaxSampleSeconds) is ignored, as a load or pause would be.
    Event sample(double now, float gap, float waits, float gpu = -1.f) {
        if (!(gap > 0.f) || gap >= kMaxSampleSeconds) {
            return Event::None;
        }
        // V1-rc12: the GPU frame time, when known (gpu < 0: unknown, the estimate is dropped).
        if (gpu >= 0.f && gpu < kMaxSampleSeconds) {
            mGpuEma = mGpuEma <= 0.f ? gpu : mGpuEma + (gpu - mGpuEma) * kEmaWeight;
        } else if (gpu < 0.f) {
            mGpuEma = 0.f;
        }
        mStats.lastGpuSeconds = mGpuEma;
        waits = std::clamp(waits, 0.f, gap);
        const float work = std::max(0.f, gap - waits);
        mGapEma = mGapEma <= 0.f ? gap : mGapEma + (gap - mGapEma) * kEmaWeight;
        mWorkEma = mWorkEma <= 0.f ? work : mWorkEma + (work - mWorkEma) * kEmaWeight;
        mStats.lastGapSeconds = mGapEma;
        mStats.lastWorkSeconds = mWorkEma;

        if (mStalled) {
            mStats.stalledSeconds += gap;
            if (!(mWorkEma < mResume)) {
                mVotes = 0;
                return Event::None;
            }
            if (mGpuGate > 0.f && mGpuEma > 0.f && !(mGpuEma < mGpuGate)) {
                mVotes = 0;  // V1-rc12: the CPU would allow it, the GPU would not hold the floor
                ++mStats.gpuBlockedFrames;
                return Event::None;
            }
            if (now < mStalledAt + mStats.cooldownSeconds) {
                // The rule is met; the cooldown has not elapsed. Hold the votes at the dwell.
                mVotes = std::min(mVotes + 1, mPacing.resumeDwellFrames);
                ++mStats.cooldownBlockedFrames;
                return Event::None;
            }
            if (++mVotes < mPacing.resumeDwellFrames) {
                return Event::None;
            }
            mStalled = false;
            mVotes = 0;
            mResumedAt = now;
            mHasResumed = true;
            ++mStats.exits;
            ++mStats.attempts;
            // The estimates were taken in the other mode; start from interpolating samples.
            mGapEma = 0.f;
            mWorkEma = 0.f;
            return Event::Resumed;
        }

        // Interpolating: after a stable stretch the cooldown goes back to its start.
        if (mHasResumed && now - mResumedAt >= mPacing.stableReset) {
            mStats.cooldownSeconds = mPacing.cooldownInitial;
        }
        if (!(mGapEma > mGiveUp)) {
            mVotes = mPacing.decayVotes ? std::max(0, mVotes - 1) : 0;
            return Event::None;
        }
        mVotes += mPacing.severeFactor > 0.f && mGapEma > mGiveUp * mPacing.severeFactor ? mPacing.severeVotes : 1;
        if (mVotes < mPacing.enterDwellFrames) {
            return Event::None;
        }
        if (mHasResumed && now - mResumedAt < mPacing.relapseWindow) {
            mStats.cooldownSeconds = std::min(mStats.cooldownSeconds * 2.0, mPacing.cooldownMax);
            ++mStats.failures;
        }
        mStalled = true;
        mVotes = 0;
        mStalledAt = now;
        ++mStats.enters;
        mGapEma = 0.f;
        mWorkEma = 0.f;
        return Event::Entered;
    }

private:
    bool mStalled = false;
    int mVotes = 0;
    float mGapEma = 0.f;
    float mWorkEma = 0.f;
    double mStalledAt = 0.0;
    double mResumedAt = 0.0;
    bool mHasResumed = false;
    float mGiveUp = kGiveUpSeconds;
    float mResume = kResumeSeconds;
    float mGpuGate = 0.f;
    float mGpuEma = 0.f;
    Pacing mPacing{};
    Stats mStats{};
};

}  // namespace dusk::governor
