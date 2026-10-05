#pragma once

namespace dusk::ui {

// Direction keys repeat while held (src/dusk/ui/input.cpp calls ProcessKeyDown again every 0.12..0.045 s after 0.32 s). A
// two-entry carousel would otherwise advance twice and appear not to move: Play -> Randomizer -> Play. Keep each direction
// disarmed until its matching key-up arrives. Left and right are independent. A touch tap is one click, not a key, and does
// not go through here.
class GameModeCycleLatch {
public:
    // A key-down: true only for the first of a hold.
    bool press(int direction) noexcept {
        bool& held = direction < 0 ? mLeftHeld : mRightHeld;
        if (held) {
            return false;
        }
        held = true;
        return true;
    }

    // The matching key-up.
    void release(int direction) noexcept {
        (direction < 0 ? mLeftHeld : mRightHeld) = false;
    }

    // The carousel lost focus (or its input was reset) while a direction was down: the key-up will not reach it, so nothing
    // may stay held.
    void reset() noexcept {
        mLeftHeld = false;
        mRightHeld = false;
    }

private:
    bool mLeftHeld = false;
    bool mRightHeld = false;
};

}  // namespace dusk::ui
