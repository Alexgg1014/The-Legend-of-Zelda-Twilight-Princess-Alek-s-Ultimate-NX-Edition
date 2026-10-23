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

// V1-rc12: Capped 60 with interp_floor_fps 54: a 36-40 FPS loop gives up (a steady 30), a 60 FPS one does not, and a
// light scene comes back once its work leaves room for 60. The defaults are unchanged.
static void test_floor_for_capped_60() {
    Sim s;
    CHECK(s.g.give_up_seconds() == kGiveUpSeconds);
    s.g.set_thresholds(1.0f / 54.0f, 0.015f);
    s.run(10.0, 0.026f, 0.020f);  // ~38 FPS while interpolating (rc11 Ordon / Kakariko outskirts)
    CHECK(s.g.stalled() && s.enters == 1);
    Sim fast;
    fast.g.set_thresholds(1.0f / 54.0f, 0.015f);
    fast.run(30.0, 0.0166f, 0.010f);  // 60 FPS: stays interpolating
    CHECK(!fast.g.stalled() && fast.enters == 0);
    Sim back;
    back.g.set_thresholds(1.0f / 54.0f, 0.015f);
    back.run(5.0, 0.026f, 0.020f);
    CHECK(back.g.stalled());
    back.run(30.0, 0.0166f, 0.010f);  // a light room: work 10 ms, under the 15 ms resume threshold
    CHECK(back.resumes >= 1 && !back.g.stalled());
    // Without the floor (rc11), the same 38 FPS loop keeps interpolating.
    Sim old;
    old.run(10.0, 0.026f, 0.020f);
    CHECK(!old.g.stalled());
}

// V1-rc12: the GPU gate. rc12 console (deko3d, Balanced): stalled work 3-13 ms passed the CPU rule, the GPU took
// 17-19 ms a frame, and every resume fell back within a second at 36-50 FPS.
static void test_gpu_gate() {
    auto heavy = [](Sim& s, double seconds, float gpu) {
        const double end = s.now + seconds;
        while (s.now < end) {
            float gap, waits;
            if (s.g.stalled()) {
                gap = Sim::kTick;
                waits = gap - 0.009f;  // 9 ms of work: the CPU rule alone would resume
            } else {
                gap = 0.024f;  // ~42 FPS interpolating: under the 54 floor
                waits = 0.f;
            }
            s.now += gap;
            const Event e = s.g.sample(s.now, gap, waits, gpu);
            s.enters += e == Event::Entered;
            s.resumes += e == Event::Resumed;
        }
    };
    Sim gated;
    gated.g.set_thresholds(1.0f / 54.0f, 0.015f);
    gated.g.set_gpu_gate(0.9f / 54.0f);
    heavy(gated, 120.0, 0.018f);  // GPU 18 ms > 16.7 ms gate
    CHECK(gated.g.stalled() && gated.enters == 1 && gated.resumes == 0);
    CHECK(gated.g.stats().gpuBlockedFrames > 0);
    Sim ungated;  // the rc12 behaviour: resumes and falls back again and again
    ungated.g.set_thresholds(1.0f / 54.0f, 0.015f);
    heavy(ungated, 120.0, 0.018f);
    CHECK(ungated.resumes >= 4 && ungated.enters >= 5);
    Sim unknown;  // no GPU figure: the CPU rule, as before
    unknown.g.set_thresholds(1.0f / 54.0f, 0.015f);
    unknown.g.set_gpu_gate(0.9f / 54.0f);
    heavy(unknown, 120.0, -1.f);
    CHECK(unknown.resumes >= 4);
    Sim light;  // a light room: GPU 10 ms, the floor is held
    light.g.set_thresholds(1.0f / 54.0f, 0.015f);
    light.g.set_gpu_gate(0.9f / 54.0f);
    light.run(5.0, 0.026f, 0.020f);
    CHECK(light.g.stalled());
    {
        const double end = light.now + 30.0;
        while (light.now < end) {
            const bool stalled = light.g.stalled();
            const float gap = stalled ? Sim::kTick : 0.0166f;
            const float waits = stalled ? gap - 0.010f : 0.f;
            light.now += gap;
            light.resumes += light.g.sample(light.now, gap, waits, 0.010f) == Event::Resumed;
        }
    }
    CHECK(light.resumes >= 1 && !light.g.stalled());
    Sim nogate;  // the gate off (no floor): the GPU figure changes nothing
    nogate.run(10.0, 0.026f, 0.020f);
    CHECK(!nogate.g.stalled() && nogate.g.gpu_gate() == 0.f);
}

// post-rc12: log 155101 (rc12 console, Balanced, Capped 60, deko3d) replayed. Interpolation was on 571-738 s; per scene,
// the interval while interpolating at the logged stalls, the work while stalled at the logged resumes, and the GPU
// frame from the rc12 tour (Hyrule Field 18.5 ms, Ordon 17.2 ms). Per-frame noise +-15 % and a 64 ms hitch every 600
// frames. "Before" is rc12 (floor 54, no GPU gate, 5/10/20 s); "after" is floor 45, GPU gate, kCapped60Pacing.
struct ReplayScene {
    const char* name;
    double seconds;
    float interpGap, work, gpu;
};
static constexpr ReplayScene kLog155101[] = {
    {"F_SP121 Hyrule Field", 64.0, 0.0300f, 0.006f, 0.0185f},
    {"F_SP103 Ordon Village", 5.0, 0.0285f, 0.006f, 0.0172f},
    {"F_SP00  Ordon Ranch", 43.0, 0.0205f, 0.009f, 0.0170f},
    {"F_SP103 Ordon Village", 19.0, 0.0230f, 0.013f, 0.0172f},
    {"F_SP00  Ordon Ranch", 36.0, 0.0210f, 0.011f, 0.0170f},
};
struct ReplayResult {
    int enters = 0, resumes = 0;
    double interpolatingSeconds[5] = {};
    double sceneSeconds[5] = {};
};
static ReplayResult replay_log_155101(bool after, bool print) {
    Governor g;
    if (after) {
        g.set_thresholds(1.0f / 45.0f, 0.015f);
        g.set_gpu_gate(0.9f / 45.0f);
        g.set_pacing(kCapped60Pacing);
    } else {
        g.set_thresholds(1.0f / 54.0f, 0.015f);
    }
    ReplayResult r;
    uint32_t rng = 155101u;
    auto noise = [&rng]() {
        rng = rng * 1664525u + 1013904223u;
        return (static_cast<float>(rng >> 8) / 16777216.0f) * 0.30f - 0.15f;  // +-15 %
    };
    double now = 571.0;
    int frame = 0;
    if (print) {
        std::printf("  %s:\n", after ? "after" : "before");
    }
    for (int s = 0; s < 5; ++s) {
        const auto& sc = kLog155101[s];
        if (s > 0) {
            g.discontinuity(now);  // a stage load
        }
        const double end = now + sc.seconds;
        while (now < end) {
            float gap, waits;
            const bool hitch = ++frame % 600 == 0;
            if (g.stalled()) {
                const float work = sc.work * (1.f + noise());
                gap = std::max(work, 1.0f / 30.0f) + (hitch ? 0.064f : 0.f);
                waits = gap - work;
            } else {
                gap = sc.interpGap * (1.f + noise()) + (hitch ? 0.064f : 0.f);
                waits = 0.f;
            }
            const float gpu = sc.gpu * (1.f + noise() * 0.5f);
            if (!g.stalled()) {
                r.interpolatingSeconds[s] += gap;
            }
            r.sceneSeconds[s] += gap;
            now += gap;
            const Event e = g.sample(now, gap, waits, gpu);
            if (e != Event::None) {
                r.enters += e == Event::Entered;
                r.resumes += e == Event::Resumed;
                if (print) {
                    std::printf("    t=%6.1f %-22s %s (cooldown %.0f s)\n", now, sc.name,
                        e == Event::Entered ? "stalled -> 30" : "resumed -> interpolating", g.stats().cooldownSeconds);
                }
            }
        }
    }
    if (print) {
        for (int s = 0; s < 5; ++s) {
            std::printf("    %-22s interpolating %3.0f %%\n", kLog155101[s].name,
                100.0 * r.interpolatingSeconds[s] / r.sceneSeconds[s]);
        }
        std::printf("    %d stalls, %d resumes\n", r.enters, r.resumes);
    }
    return r;
}

static void test_replay_log_155101() {
    std::printf("replay of log 155101 (rc12 console, Capped 60, deko3d):\n");
    const ReplayResult before = replay_log_155101(false, true);
    const ReplayResult after = replay_log_155101(true, true);
    auto pct = [](const ReplayResult& r, int s) { return r.interpolatingSeconds[s] / r.sceneSeconds[s]; };
    CHECK(before.enters >= 5);                          // the model flaps as the console did (9 stalls in 167 s)
    CHECK(pct(after, 2) > 0.8 && pct(after, 4) > 0.8);  // Ordon Ranch keeps its interpolated frames
    CHECK(pct(after, 0) < 0.15);                        // Hyrule Field: a clean 30
    CHECK(pct(after, 3) < 0.5);                         // Ordon Village at ~43 FPS: under the floor, mostly 30
    CHECK(after.enters <= before.enters);
    CHECK(pct(after, 2) > pct(before, 2) && pct(after, 4) > pct(before, 4));
}

// V1-rc16: Boldiatti's rc14 console (FW 22.5, deko3d, Capped/Fast 60), log 085043: on entering D_MN06B (~1000
// draws/frame) the first frames after the load ran at 28.8 ms, the floor rule (45) stalled, and the way back needed work
// under 15 ms; the room's work is ~18 ms (Unlimited ran it at 51.6-56.5 FPS), so it stayed at 30 for minutes. Modelled:
// 2 s at 28.8 ms after the load, then 18.5 ms +-15 % (GPU ~18 ms, work ~18 ms while stalled), 150 s. And Ordon Ranch
// GPU-bound (his logs: interval and GPU 40-42 ms): worse than 30 interpolated, so a clean 30 must stay.
// "Before" = rc14 (floor 45, GPU gate, kCapped60Pacing); "after" = rc16 (no floor: the Unlimited rule).
struct BoldiattiResult {
    int enters = 0, resumes = 0;
    double interpolating = 0.0, total = 0.0;
};
static BoldiattiResult replay_boldiatti(bool after, float steadyGap, float transientGap, float gpu, float stalledWork,
                                        double seconds) {
    Governor g;
    if (!after) {
        g.set_thresholds(1.0f / 45.0f, 0.015f);
        g.set_gpu_gate(0.9f / 45.0f);
        g.set_pacing(kCapped60Pacing);
    }
    BoldiattiResult r;
    uint32_t rng = 85043u;
    auto noise = [&rng]() {
        rng = rng * 1664525u + 1013904223u;
        return (static_cast<float>(rng >> 8) / 16777216.0f) * 0.30f - 0.15f;
    };
    double now = 100.0;
    g.discontinuity(now);  // the stage load
    const double start = now;
    while (now < start + seconds) {
        float gap, waits;
        if (g.stalled()) {
            const float work = stalledWork * (1.f + noise());
            gap = std::max(work, 1.0f / 30.0f);
            waits = gap - work;
        } else {
            gap = (now - start < 2.0 ? transientGap : steadyGap) * (1.f + noise());
            waits = 0.f;
            r.interpolating += gap;
        }
        r.total += gap;
        now += gap;
        const Event e = g.sample(now, gap, waits, gpu * (1.f + noise() * 0.5f));
        r.enters += e == Event::Entered;
        r.resumes += e == Event::Resumed;
    }
    return r;
}

static void test_replay_boldiatti_capped_60() {
    std::printf("replay of Boldiatti's rc14 log 085043 (Capped 60):\n");
    const auto before = replay_boldiatti(false, 0.0185f, 0.0288f, 0.018f, 0.018f, 150.0);
    const auto after = replay_boldiatti(true, 0.0185f, 0.0288f, 0.018f, 0.018f, 150.0);
    std::printf("  D_MN06B   before: interpolating %3.0f %%, %d stalls, %d resumes\n",
        100.0 * before.interpolating / before.total, before.enters, before.resumes);
    std::printf("  D_MN06B   after:  interpolating %3.0f %%, %d stalls, %d resumes\n",
        100.0 * after.interpolating / after.total, after.enters, after.resumes);
    CHECK(before.enters >= 1 && before.interpolating / before.total < 0.2);  // the console's lock at 30
    CHECK(after.enters == 0 && after.interpolating / after.total > 0.95);    // like Unlimited: ~54 FPS throughout
    const auto ranch = replay_boldiatti(true, 0.041f, 0.041f, 0.040f, 0.040f, 60.0);
    std::printf("  F_SP00    after:  interpolating %3.0f %%, %d stalls (GPU-bound, worse than 30)\n",
        100.0 * ranch.interpolating / ranch.total, ranch.enters);
    CHECK(ranch.enters >= 1 && ranch.interpolating / ranch.total < 0.2);  // a real failure still gives a clean 30
}

int main() {
    test_replay_boldiatti_capped_60();
    test_replay_log_155101();
    test_floor_for_capped_60();
    test_gpu_gate();
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
