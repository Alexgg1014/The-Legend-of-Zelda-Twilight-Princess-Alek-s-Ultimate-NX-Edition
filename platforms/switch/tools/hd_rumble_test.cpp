// aleks2 (V1-rc16): host test for src/dusk/hd_rumble_core.hpp -- the envelopes stay in the actuators' range, every mode
// has one, a shock plays once when it starts, a quake loops until the game stops it, and a stop ends everything.
#include "dusk/hd_rumble_core.hpp"

#include <cstdio>

using namespace dusk::hd_rumble;

// d_vibration.h's numbering.
constexpr int kShockPower2 = 2, kShockPower3 = 3, kShockPower5 = 5, kShockPower6 = 6;
constexpr int kQuakePower2 = 2;

static int g_failures = 0;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                                          \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

static void check_envelope(const Envelope& e, bool loop, int maxMs) {
    CHECK(e.count > 0 && e.count <= kMaxKeys);
    CHECK(e.loop == loop);
    int total = 0;
    bool some = false;
    for (int i = 0; i < e.count; ++i) {
        const Key& k = e.keys[static_cast<size_t>(i)];
        CHECK(k.ampLow >= 0.f && k.ampLow <= 1.f && k.ampHigh >= 0.f && k.ampHigh <= 1.f);
        CHECK(k.freqLow >= 40.f && k.freqLow <= 640.f && k.freqHigh >= 80.f && k.freqHigh <= 1280.f);
        CHECK(k.pan >= -1.f && k.pan <= 1.f);
        total += k.ms;
        some |= k.ampLow > 0.f || k.ampHigh > 0.f;
    }
    CHECK(some);
    CHECK(total > 0 && total <= maxMs);
    if (!loop) {
        const Key& last = e.keys[static_cast<size_t>(e.count - 1)];
        CHECK(last.ampLow == 0.f && last.ampHigh == 0.f);  // a hit ends still
    }
}

int main() {
    for (int m = kShockCustom; m <= kShockFor2D; ++m) {
        check_envelope(shock_envelope(m), false, 400);
    }
    for (int m = kQuakeCustom; m <= kQuakeHorse; ++m) {
        check_envelope(quake_envelope(m), true, 1000);
    }
    // Stronger shocks hit harder (low band) and last longer.
    auto peak = [](const Envelope& e) {
        float p = 0.f;
        for (int i = 0; i < e.count; ++i) p = std::max(p, e.keys[static_cast<size_t>(i)].ampLow);
        return p;
    };
    auto length = [](const Envelope& e) {
        int t = 0;
        for (int i = 0; i < e.count; ++i) t += e.keys[static_cast<size_t>(i)].ms;
        return t;
    };
    CHECK(peak(shock_envelope(kShockPower1)) < peak(shock_envelope(kShockPower6)));
    CHECK(length(shock_envelope(kShockPower1)) < length(shock_envelope(kShockPower8)));
    CHECK(shock_envelope(kShockPower8).keys[0].freqLow < shock_envelope(kShockPower2).keys[0].freqLow);  // a thud
    // The gallop alternates sides.
    const Envelope horse = quake_envelope(kQuakeHorse);
    bool left = false, right = false;
    for (int i = 0; i < horse.count; ++i) {
        const Key& k = horse.keys[static_cast<size_t>(i)];
        left |= k.pan < -0.5f && k.ampLow > 0.f;
        right |= k.pan > 0.5f && k.ampLow > 0.f;
    }
    CHECK(left && right);

    // The tracker, frame by frame as dVibration_c::Run leaves the motor.
    Tracker t;
    Command c = t.frame({});  // nothing running
    CHECK(!c.playShot && !c.playLoop && !c.stopLoop && !c.stopAll);
    c = t.frame({kShockPower3, 1, kQuakeNone, -99});  // a shock started this frame
    CHECK(c.playShot && !c.playLoop);
    c = t.frame({kShockPower3, 2, kQuakeNone, -99});  // still running: not played again
    CHECK(!c.playShot);
    c = t.frame({kShockNone, -99, kQuakePower2, 1});  // a quake starts
    CHECK(c.playLoop && c.loop.loop && t.quake_on());
    c = t.frame({kShockNone, -99, kQuakePower2, 40});  // keeps going: nothing new
    CHECK(!c.playLoop && !c.stopLoop);
    c = t.frame({kShockPower5, 1, kQuakePower2, 0});  // a hit during the quake: the quake goes on
    CHECK(c.playShot && !c.playLoop && !c.stopLoop);
    c = t.frame({kShockNone, -99, kQuakeHorse, 3});  // the quake changes to the gallop
    CHECK(c.playLoop);
    c = t.frame({kShockNone, -99, kQuakeNone, -99});  // the game stops it
    CHECK(c.stopLoop && !t.quake_on());
    t.frame({kShockNone, -99, kQuakePower1, 1});
    c = t.stop();  // pause, kill, a load, HOME
    CHECK(c.stopAll && !t.quake_on());
    c = t.frame({kShockNone, -99, kQuakePower1, 5});  // after a pause the game's quake resumes: it plays again
    CHECK(c.playLoop);

    if (g_failures != 0) {
        std::fprintf(stderr, "hd_rumble_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("hd_rumble_test: all checks passed\n");
    return 0;
}
