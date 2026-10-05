// H38-c4: the L3 + R3 recenter chord (src/dusk/wii_recenter_combo.hpp). No game, no hardware.
#include "dusk/wii_recenter_combo.hpp"

#include <cstdio>

using dusk::wii::RecenterCombo;

static int g_failures = 0;
#define CHECK(c)                                                                                  \
    do {                                                                                          \
        if (!(c)) {                                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                              \
            ++g_failures;                                                                         \
        }                                                                                         \
    } while (0)

// Holds the given buttons for `seconds` in 1/60 s ticks; returns how many times the chord fired.
static int hold(RecenterCombo& c, bool l, bool r, double seconds, double dt = 1.0 / 60.0) {
    int fired = 0;
    for (double t = 0.0; t < seconds; t += dt) {
        fired += c.update(l, r, dt) ? 1 : 0;
    }
    return fired;
}

int main() {
    {  // not armed until the sticks have been released once (just enabled, or both already down)
        RecenterCombo c;
        CHECK(hold(c, true, true, 2.0) == 0);
        CHECK(hold(c, false, false, 0.1) == 0);
        CHECK(hold(c, true, true, 0.6) == 1);
    }
    {  // fires once per hold, at 0.5 s, not before
        RecenterCombo c;
        hold(c, false, false, 0.1);
        CHECK(hold(c, true, true, 0.45) == 0);
        CHECK(hold(c, true, true, 0.1) == 1);
        CHECK(hold(c, true, true, 5.0) == 0);  // a long hold does not repeat
    }
    {  // no repeat until BOTH sticks are released
        RecenterCombo c;
        hold(c, false, false, 0.1);
        CHECK(hold(c, true, true, 0.6) == 1);
        CHECK(hold(c, true, false, 0.2) == 0);   // one stick let go
        CHECK(hold(c, true, true, 1.0) == 0);    // pressed together again with the other never fully released
        CHECK(hold(c, false, false, 0.05) == 0); // both released
        CHECK(hold(c, true, true, 0.6) == 1);    // armed again
    }
    {  // one stick alone never fires, however long; a half release restarts the timing
        RecenterCombo c;
        hold(c, false, false, 0.1);
        CHECK(hold(c, true, false, 3.0) == 0);
        CHECK(hold(c, false, true, 3.0) == 0);
        CHECK(hold(c, true, true, 0.3) == 0);
        CHECK(hold(c, true, false, 0.1) == 0);
        CHECK(hold(c, true, true, 0.3) == 0);  // restarted by the half release: 0.3 s is not enough
        CHECK(hold(c, true, true, 0.3) == 1);  // 0.6 s together
    }
    {  // adding the second stick late: the timing starts when both are down
        RecenterCombo c;
        hold(c, false, false, 0.1);
        CHECK(hold(c, true, false, 2.0) == 0);
        CHECK(hold(c, true, true, 0.4) == 0);
        CHECK(hold(c, true, true, 0.2) == 1);
    }
    {  // a hitch: a long tick counts as at most 0.1 s of holding
        RecenterCombo c;
        hold(c, false, false, 0.1);
        CHECK(!c.update(true, true, 2.0));
        CHECK(c.held() > 0.09 && c.held() < 0.11);
        CHECK(!c.update(true, true, -1.0));  // a negative step adds nothing
        CHECK(c.held() > 0.09 && c.held() < 0.11);
    }
    {  // reset (motion controls off): nothing fires while the sticks stay down, and it re-arms on release
        RecenterCombo c;
        hold(c, false, false, 0.1);
        hold(c, true, true, 0.3);
        c.reset();
        CHECK(hold(c, true, true, 2.0) == 0);
        hold(c, false, false, 0.05);
        CHECK(hold(c, true, true, 0.6) == 1);
    }
    if (g_failures == 0) {
        std::printf("wii recenter combo: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
