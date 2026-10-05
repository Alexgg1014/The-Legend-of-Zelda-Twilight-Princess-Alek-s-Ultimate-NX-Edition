#pragma once

// aleks2 H38-c4: the chord that recenters the Wii-style motion sensors without opening the settings: both stick buttons
// (L3 and R3) held together for kHoldSeconds. Pure (no game, no hardware): tested on the host
// (platforms/switch/tools/test-wii-recenter-combo.sh).
//
// One activation per hold: after it fires, it is armed again only when both sticks have been released. A hold that began
// before the combo could be armed (the motion controls were just switched on, or a stick was already down when the game
// started reading) does not fire until the sticks have been released once.

namespace dusk::wii {

class RecenterCombo {
public:
    static constexpr double kHoldSeconds = 0.5;
    static constexpr double kMaxStepSeconds = 0.1;  // a hitch is not half a second of holding

    // Once per tick with the stick buttons as they are now. Returns true on the tick the hold reaches kHoldSeconds, once.
    bool update(bool left, bool right, double dt) {
        if (!left && !right) {
            mArmed = true;
            mFired = false;
            mHeld = 0.0;
            return false;
        }
        if (!(left && right)) {
            mHeld = 0.0;  // one stick alone is not the chord, and a half release restarts the timing
            return false;
        }
        if (!mArmed || mFired) {
            return false;
        }
        mHeld += dt < 0.0 ? 0.0 : (dt > kMaxStepSeconds ? kMaxStepSeconds : dt);
        if (mHeld >= kHoldSeconds) {
            mFired = true;
            return true;
        }
        return false;
    }

    // The motion controls were switched off or the layout changed: nothing in flight, and the combo waits for a release.
    void reset() {
        mArmed = false;
        mFired = false;
        mHeld = 0.0;
    }

    double held() const { return mHeld; }

private:
    bool mArmed = false;
    bool mFired = false;
    double mHeld = 0.0;
};

}  // namespace dusk::wii
