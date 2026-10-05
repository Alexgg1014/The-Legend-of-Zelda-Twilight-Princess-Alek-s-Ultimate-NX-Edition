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
};

class Governor {
public:
    bool stalled() const { return mStalled; }
    const Stats& stats() const { return mStats; }
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
    }

    // A break in the loop: a load or pause gap, an unpause, a sim-rate change. What comes next
    // is another scene, so nothing measured or earned before it applies: the estimates and the
    // votes go, the cooldown goes back to kCooldownInitial and a resume before the break no
    // longer makes the next stall a relapse. The state stays as it is -- a stalled loop is not
    // let out here, it has to show kDwellFrames light samples taken after the break, and the
    // cooldown is counted from the break. Counters are cumulative and are kept.
    void discontinuity(double now) {
        flush_estimates();
        mStats.cooldownSeconds = kCooldownInitial;
        mHasResumed = false;
        if (mStalled) {
            mStalledAt = now;
        }
    }

    // One frame. `now` is a monotonic time in seconds, `gap` the native interval since the
    // previous sample, `waits` the limiter sleep that happened inside that interval. A sample
    // outside (0, kMaxSampleSeconds) is ignored, as a load or pause would be.
    Event sample(double now, float gap, float waits) {
        if (!(gap > 0.f) || gap >= kMaxSampleSeconds) {
            return Event::None;
        }
        waits = std::clamp(waits, 0.f, gap);
        const float work = std::max(0.f, gap - waits);
        mGapEma = mGapEma <= 0.f ? gap : mGapEma + (gap - mGapEma) * kEmaWeight;
        mWorkEma = mWorkEma <= 0.f ? work : mWorkEma + (work - mWorkEma) * kEmaWeight;
        mStats.lastGapSeconds = mGapEma;
        mStats.lastWorkSeconds = mWorkEma;

        if (mStalled) {
            mStats.stalledSeconds += gap;
            if (!(mWorkEma < kResumeSeconds)) {
                mVotes = 0;
                return Event::None;
            }
            if (now < mStalledAt + mStats.cooldownSeconds) {
                // The rule is met; the cooldown has not elapsed. Hold the votes at the dwell.
                mVotes = std::min(mVotes + 1, kDwellFrames);
                ++mStats.cooldownBlockedFrames;
                return Event::None;
            }
            if (++mVotes < kDwellFrames) {
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
        if (mHasResumed && now - mResumedAt >= kStableReset) {
            mStats.cooldownSeconds = kCooldownInitial;
        }
        if (!(mGapEma > kGiveUpSeconds)) {
            mVotes = 0;
            return Event::None;
        }
        if (++mVotes < kDwellFrames) {
            return Event::None;
        }
        if (mHasResumed && now - mResumedAt < kRelapseWindow) {
            mStats.cooldownSeconds = std::min(mStats.cooldownSeconds * 2.0, kCooldownMax);
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
    Stats mStats{};
};

}  // namespace dusk::governor
