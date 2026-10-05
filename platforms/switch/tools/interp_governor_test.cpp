// Host test for src/dusk/interp_governor.hpp (H33).
//   platforms/switch/tools/test-interp-governor.sh
#include "dusk/interp_governor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>

static int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

using namespace dusk::governor;

// Simulates a loop. While interpolating a frame takes `interpGap` and sleeps nowhere; while
// stalled the limiter pads the frame up to the 33.33 ms tick, like JFWDisplay::waitForTick.
struct Sim {
    Governor g;
    double now = 0.0;
    int enters = 0, resumes = 0;
    double enteredAt = 0.0;
    static constexpr float kTick = 1.0f / 30.0f;

    // One frame. `interpGap` is what a frame costs while interpolating; `work` is what the
    // plain path costs while stalled (sim + draw, no padding).
    Event frame(float interpGap, float work) {
        float gap, waits;
        if (g.stalled()) {
            gap = std::max(work, kTick);
            waits = gap - work;
        } else {
            gap = interpGap;
            waits = 0.f;
        }
        now += gap;
        const Event e = g.sample(now, gap, waits);
        if (e == Event::Entered) {
            ++enters;
            enteredAt = now;
        }
        resumes += e == Event::Resumed;
        return e;
    }
    void run(double seconds, float interpGap, float work) {
        const double end = now + seconds;
        while (now < end) {
            frame(interpGap, work);
        }
    }
};

// The rule that shipped before H33, for the regression below: stalled until the interval
// EMA falls under 90 % of the sim period.
static bool legacy_resumes(float gapWhileStalled, float work) {
    (void)work;
    return gapWhileStalled < kSimPeriod * 0.90f;
}

static void test_the_dead_end_is_real_and_fixed() {
    // Plain path with 20 ms of real work: the limiter pads the interval to 33.3 ms.
    const float tick = 1.0f / 30.0f;
    CHECK(!legacy_resumes(std::max(0.020f, tick), 0.020f));  // never, at any work < 33 ms
    CHECK(!legacy_resumes(tick, 0.001f));

    Sim s;
    s.run(5.0, 0.040f, 0.0f);  // heavy: interpolating frames of 40 ms
    CHECK(s.g.stalled());
    CHECK(s.enters == 1);
    s.run(20.0, 0.020f, 0.020f);  // the scene lightens: 20 ms of real work, 20 ms interpolated
    CHECK(!s.g.stalled());
    CHECK(s.resumes == 1);
    CHECK(s.g.stats().exits == 1 && s.g.stats().attempts == 1);
}

static void test_heavy_scene_stays_stalled() {
    Sim s;
    s.run(5.0, 0.040f, 0.0f);
    CHECK(s.g.stalled());
    s.run(120.0, 0.040f, 0.040f);  // 40 ms of work: no headroom, ever
    CHECK(s.g.stalled());
    CHECK(s.resumes == 0);
    // Exactly at the threshold is not under it.
    Sim t;
    t.run(5.0, 0.040f, 0.0f);
    t.run(60.0, 0.040f, kResumeSeconds);
    CHECK(t.g.stalled());
}

static void test_entry_needs_dwell_and_threshold() {
    Sim s;
    for (int i = 0; i < kDwellFrames - 1; ++i) {
        s.frame(0.040f, 0.f);
    }
    CHECK(!s.g.stalled());
    s.frame(0.040f, 0.f);
    CHECK(s.g.stalled());
    // At 33 ms (under the 35 ms give-up) it never stalls.
    Sim t;
    t.run(60.0, 0.033f, 0.f);
    CHECK(!t.g.stalled() && t.enters == 0);
}

static void test_cooldown_before_resume() {
    Sim s;
    s.run(5.0, 0.040f, 0.0f);
    CHECK(s.g.stalled());
    const double enteredAt = s.enteredAt;
    // Light from the start: the dwell (30 frames, ~1 s) is met long before 5 s have passed.
    while (s.g.stalled() && s.now < enteredAt + 30.0) {
        s.frame(0.020f, 0.015f);
    }
    CHECK(!s.g.stalled());
    CHECK(s.now - enteredAt >= kCooldownInitial - 1e-6);
    CHECK(s.now - enteredAt < kCooldownInitial + 1.0);
    CHECK(s.g.stats().cooldownBlockedFrames > 0);
}

static void test_backoff_and_reset() {
    Sim s;
    double expected = kCooldownInitial;
    CHECK(s.g.stats().cooldownSeconds == expected);
    // Relapse over and over: stall, resume, stall again inside the 15 s window.
    // H36: capped at 20 s (it was 60 s).
    const double doubled[] = {10.0, 20.0, 20.0, 20.0, 20.0, 20.0};
    s.run(5.0, 0.040f, 0.0f);
    CHECK(s.g.stalled());
    int failures0 = 0;
    for (const double want : doubled) {
        // Resume as soon as the cooldown lets it.
        for (int guard = 0; s.g.stalled() && guard < 100000; ++guard) {
            s.frame(0.040f, 0.015f);
        }
        CHECK(!s.g.stalled());
        // Heavy again straight away: stalls within the relapse window.
        for (int guard = 0; !s.g.stalled() && guard < 100000; ++guard) {
            s.frame(0.040f, 0.015f);
        }
        CHECK(s.g.stalled());
        ++failures0;
        CHECK(s.g.stats().cooldownSeconds == want);
        CHECK(s.g.stats().failures == static_cast<uint32_t>(failures0));
    }
    // A long stable stretch resets the cooldown to its start.
    for (int guard = 0; s.g.stalled() && guard < 1000000; ++guard) {
        s.frame(0.020f, 0.015f);
    }
    CHECK(!s.g.stalled());
    s.run(kStableReset + 1.0, 0.020f, 0.0f);  // interpolating comfortably
    CHECK(!s.g.stalled());
    CHECK(s.g.stats().cooldownSeconds == kCooldownInitial);
    // And a stall after that is not a failure.
    const uint32_t before = s.g.stats().failures;
    s.run(5.0, 0.040f, 0.0f);
    CHECK(s.g.stalled());
    CHECK(s.g.stats().failures == before);
    CHECK(s.g.stats().cooldownSeconds == kCooldownInitial);
}

static void test_oscillation_is_bounded() {
    // 10 s heavy / 10 s light for 10 minutes: transitions stay a handful per minute.
    Sim s;
    for (int cycle = 0; cycle < 30; ++cycle) {
        s.run(10.0, 0.040f, 0.040f);
        s.run(10.0, 0.020f, 0.015f);
    }
    const double minutes = s.now / 60.0;
    CHECK((s.enters + s.resumes) / minutes <= 6.0);
    // And a pathological scene that is heavy only while interpolating (work 15 ms, interpolated
    // frames 40 ms): the cooldown backoff keeps the flapping rare.
    Sim t;
    t.run(600.0, 0.040f, 0.015f);
    CHECK((t.enters + t.resumes) / (t.now / 60.0) <= 6.0);
    CHECK(t.g.stats().cooldownSeconds <= kCooldownMax);
}

static void test_samples_and_waits() {
    Governor g;
    // Out-of-range samples are ignored: no votes, no EMA.
    for (int i = 0; i < 1000; ++i) {
        CHECK(g.sample(i * 0.5, 0.0f, 0.f) == Event::None);
        CHECK(g.sample(i * 0.5, -1.0f, 0.f) == Event::None);
        CHECK(g.sample(i * 0.5, 0.25f, 0.f) == Event::None);   // exactly a load
        CHECK(g.sample(i * 0.5, 5.0f, 0.f) == Event::None);
    }
    CHECK(!g.stalled() && g.gapEma() == 0.f && g.stats().enters == 0);
    // Waits larger than the interval are bounded by it: work is never negative.
    Governor h;
    h.sample(1.0, 0.020f, 0.5f);
    CHECK(h.workEma() == 0.f || h.workEma() >= 0.f);
    CHECK(h.workEma() < 1e-6f);
    // Negative waits are treated as zero.
    Governor k;
    k.sample(1.0, 0.020f, -3.f);
    CHECK(std::abs(k.workEma() - 0.020f) < 1e-6f);
}

static void test_flush_and_disable() {
    Sim s;
    s.run(5.0, 0.040f, 0.0f);
    CHECK(s.g.stalled());
    // flush keeps the state, drops the estimates and the votes.
    s.g.flush_estimates();
    CHECK(s.g.stalled() && s.g.gapEma() == 0.f && s.g.workEma() == 0.f);
    // disable clears the state but keeps the counters.
    const auto enters = s.g.stats().enters;
    s.g.disable();
    CHECK(!s.g.stalled() && s.g.stats().enters == enters);
}

static void test_same_threshold_for_every_quality() {
    // The governor has no notion of quality: identical samples give identical decisions.
    Sim fast, fancy;
    for (int i = 0; i < 3000; ++i) {
        const float w = (i % 400) < 200 ? 0.045f : 0.018f;
        fast.frame(0.040f, w);
        fancy.frame(0.040f, w);
    }
    CHECK(fast.enters == fancy.enters && fast.resumes == fancy.resumes);
    CHECK(kResumeSeconds == 0.030f);
}

// --- H36: cooldown cap, stable reset and discontinuities -----------------------------------

// Counters a discontinuity must leave alone.
struct Counters {
    uint32_t enters, exits, attempts, failures, blocked;
    double stalledSeconds;
    explicit Counters(const Stats& s)
        : enters(s.enters), exits(s.exits), attempts(s.attempts), failures(s.failures),
          blocked(s.cooldownBlockedFrames), stalledSeconds(s.stalledSeconds) {}
    bool operator==(const Counters&) const = default;
};

// Telemetry that has to hold after any sequence of frames.
static void check_coherent(const Governor& g) {
    const Stats& st = g.stats();
    CHECK(st.exits == st.attempts);                         // every resume is an attempt
    CHECK(st.enters == st.exits + (g.stalled() ? 1u : 0u));  // in and out alternate
    CHECK(st.failures <= st.enters);
    CHECK(st.cooldownSeconds >= kCooldownInitial && st.cooldownSeconds <= kCooldownMax);
    CHECK(st.stalledSeconds >= 0.0);
}

// Stall, then relapse until the cooldown is at its cap: the state H35-c1 left a player in.
static void drive_to_cap(Sim& s) {
    s.run(5.0, 0.040f, 0.0f);
    for (int round = 0; round < 6; ++round) {
        for (int guard = 0; s.g.stalled() && guard < 100000; ++guard) {
            s.frame(0.040f, 0.015f);
        }
        for (int guard = 0; !s.g.stalled() && guard < 100000; ++guard) {
            s.frame(0.040f, 0.015f);
        }
    }
    CHECK(s.g.stalled());
    CHECK(s.g.stats().cooldownSeconds == kCooldownMax);
}

static void test_h36_constants() {
    CHECK(kCooldownInitial == 5.0);
    CHECK(kCooldownMax == 20.0);
    CHECK(kStableReset == 15.0);
    CHECK(kRelapseWindow == 15.0);
    CHECK(kDwellFrames == 30);
    CHECK(std::abs(kGiveUpSeconds - 0.035f) < 1e-4f);  // enter over ~35 ms
    CHECK(kResumeSeconds == 0.030f);                    // resume under 30 ms of work
}

static void test_heavy_scene_stays_stalled_across_a_discontinuity() {
    Sim s;
    drive_to_cap(s);
    s.g.discontinuity(s.now);
    CHECK(s.g.stalled());
    s.run(120.0, 0.040f, 0.040f);  // the scene after the load is just as heavy
    CHECK(s.g.stalled());
    check_coherent(s.g);
}

static void test_discontinuity_does_not_force_interpolation() {
    Sim s;
    s.run(5.0, 0.040f, 0.0f);
    s.run(60.0, 0.040f, 0.040f);  // stalled for a minute: its cooldown is long past
    CHECK(s.g.stalled());
    const Counters before{s.g.stats()};
    s.now += 2.0;  // the load itself: no samples
    s.g.discontinuity(s.now);
    // State kept, estimates and votes dropped, counters untouched.
    CHECK(s.g.stalled());
    CHECK(s.g.gapEma() == 0.f && s.g.workEma() == 0.f);
    CHECK(Counters{s.g.stats()} == before);
    CHECK(s.g.stats().cooldownSeconds == kCooldownInitial);
    // Light frames right after: not out on the next frame, nor within the dwell, nor before
    // the cooldown counted from the discontinuity.
    const double at = s.now;
    for (int i = 0; i < kDwellFrames; ++i) {
        CHECK(s.frame(0.020f, 0.015f) == Event::None);
    }
    while (s.g.stalled() && s.now < at + 60.0) {
        s.frame(0.020f, 0.015f);
        if (s.g.stalled()) {
            CHECK(s.now < at + kCooldownInitial + 0.1);
        }
    }
    CHECK(!s.g.stalled());
    CHECK(s.now - at >= kCooldownInitial - 1e-6);
    check_coherent(s.g);
}

static void test_light_scene_after_a_load_recovers_on_the_initial_cooldown() {
    // H35-c1: the cooldown at its cap in a heavy zone, then a load into a lighter one.
    // Without the discontinuity the penalty carries over; with it the new scene gets 5 s.
    Sim carried, fresh;
    drive_to_cap(carried);
    drive_to_cap(fresh);
    const double carriedAt = carried.now;
    while (carried.g.stalled() && carried.now < carriedAt + 60.0) {
        carried.frame(0.020f, 0.015f);
    }
    fresh.now += 1.0;
    fresh.g.discontinuity(fresh.now);
    const double freshAt = fresh.now;
    while (fresh.g.stalled() && fresh.now < freshAt + 60.0) {
        fresh.frame(0.020f, 0.015f);
    }
    CHECK(!fresh.g.stalled());
    // Cooldown, plus at most the dwell (~1 s of padded frames) if the light samples came late.
    CHECK(fresh.now - freshAt >= kCooldownInitial - 1e-6);
    CHECK(fresh.now - freshAt <= kCooldownInitial + kDwellFrames * (1.0 / 30.0) + 0.1);
    CHECK(!carried.g.stalled());
    CHECK(carried.now - carriedAt <= kCooldownMax + 0.1);  // never longer than the cap either
    CHECK(fresh.now - freshAt < carried.now - carriedAt);
    // A stall soon after the load is not a relapse of a resume from before it.
    Sim t;
    t.run(5.0, 0.040f, 0.0f);
    for (int guard = 0; t.g.stalled() && guard < 100000; ++guard) {
        t.frame(0.020f, 0.015f);
    }
    t.run(1.0, 0.020f, 0.0f);
    t.g.discontinuity(t.now);
    t.run(5.0, 0.040f, 0.0f);
    CHECK(t.g.stalled());
    CHECK(t.g.stats().failures == 0);
    CHECK(t.g.stats().cooldownSeconds == kCooldownInitial);
    check_coherent(t.g);
}

static void test_no_relapse_exceeds_the_cap() {
    // The flapping scene for 20 minutes: the cooldown is checked after every frame.
    Sim s;
    double highest = 0.0;
    double longestStall = 0.0;
    double stallStart = -1.0;
    while (s.now < 1200.0) {
        const Event e = s.frame(0.040f, 0.015f);
        highest = std::max(highest, s.g.stats().cooldownSeconds);
        if (e == Event::Entered) {
            stallStart = s.now;
        } else if (e == Event::Resumed && stallStart >= 0.0) {
            longestStall = std::max(longestStall, s.now - stallStart);
        }
    }
    CHECK(highest == kCooldownMax);
    CHECK(longestStall <= kCooldownMax + 0.1);  // after any relapse it is reconsidered within 20 s
    CHECK(s.g.stats().failures > 3);
    check_coherent(s.g);
}

static void test_fifteen_stable_seconds_restore_the_initial_cooldown() {
    Sim s;
    drive_to_cap(s);
    for (int guard = 0; s.g.stalled() && guard < 1000000; ++guard) {
        s.frame(0.020f, 0.015f);
    }
    CHECK(!s.g.stalled());
    s.run(kStableReset - 1.0, 0.020f, 0.0f);
    CHECK(s.g.stats().cooldownSeconds == kCooldownMax);  // 14 s: not yet
    s.run(1.5, 0.020f, 0.0f);
    CHECK(s.g.stats().cooldownSeconds == kCooldownInitial);  // 15 s: back to 5
    check_coherent(s.g);
}

static void test_pathological_oscillation_stays_bounded() {
    // Heavy only while interpolating (40 ms interpolated, 15 ms plain): each cycle is a resume,
    // ~1 s to relapse and a cooldown at the 20 s cap, so about 2 transitions per 21 s.
    Sim s;
    s.run(1200.0, 0.040f, 0.015f);
    const double perMinute = (s.enters + s.resumes) / (s.now / 60.0);
    CHECK(perMinute <= 6.0);
    // Even with a load every 30 s handing back the initial cooldown, it stays a handful.
    Sim t;
    for (int load = 0; load < 40; ++load) {
        t.run(30.0, 0.040f, 0.015f);
        t.now += 1.0;
        t.g.discontinuity(t.now);
    }
    const double withLoads = (t.enters + t.resumes) / (t.now / 60.0);
    CHECK(withLoads <= 10.0);
    check_coherent(s.g);
    check_coherent(t.g);
    std::cout << "flapping scene: " << perMinute << " transitions/min; with a load every 30 s: "
              << withLoads << "/min\n";
}

static void test_discontinuity_while_interpolating() {
    Sim s;
    s.run(30.0, 0.020f, 0.0f);
    CHECK(!s.g.stalled());
    const Counters before{s.g.stats()};
    s.g.discontinuity(s.now);
    CHECK(!s.g.stalled() && Counters{s.g.stats()} == before);
    // Entering still needs the full dwell of fresh samples.
    for (int i = 0; i < kDwellFrames - 1; ++i) {
        s.frame(0.040f, 0.f);
    }
    CHECK(!s.g.stalled());
    s.frame(0.040f, 0.f);
    CHECK(s.g.stalled());
    check_coherent(s.g);
}

int main() {
    test_the_dead_end_is_real_and_fixed();
    test_heavy_scene_stays_stalled();
    test_entry_needs_dwell_and_threshold();
    test_cooldown_before_resume();
    test_backoff_and_reset();
    test_oscillation_is_bounded();
    test_samples_and_waits();
    test_flush_and_disable();
    test_same_threshold_for_every_quality();
    test_h36_constants();
    test_heavy_scene_stays_stalled_across_a_discontinuity();
    test_discontinuity_does_not_force_interpolation();
    test_light_scene_after_a_load_recovers_on_the_initial_cooldown();
    test_no_relapse_exceeds_the_cap();
    test_fifteen_stable_seconds_restore_the_initial_cooldown();
    test_pathological_oscillation_stays_bounded();
    test_discontinuity_while_interpolating();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "interp_governor: all checks passed\n";
    return 0;
}
