#pragma once

// aleks2 H38-c3: from the sword gesture to the cut the game plays, and the one-shot slot the gesture waits in. Pure (no
// game, no hardware) so the rules are tested on the host (platforms/switch/tools/test-wii-cut-select.sh).
//
// The game picks the cut in daAlink_c::checkCutAction() from the stick direction, the combo and the target. A gesture does
// not move the stick: it only replaces the cut that checkCutAction would have started, in the plain cut branches, and
// every special action (finisher, jump attack, charged attack, spin, horse, wolf, an event) leaves the cut it chose.

#include "dusk/wii_gesture_detector.hpp"

#include <cstdint>

namespace dusk::wii {

enum class CutStyle { Keep, Vertical, Left, Right, Stab };

// Where in checkCutAction a cut is being chosen.
enum class CutBranch {
    Normal,           // combo 1..3, no target locked
    Targeted,         // combo 1..3, target locked
    Chain,            // the next hit of a PROC_CUT_NORMAL chain
    Finisher,         // combo 4, any finisher or mortal draw
    StunnedEnemy,     // the hit-an-enemy-down branch (FLG0_UNK_8000)
    Other,            // anything else
};

inline bool branch_takes_gesture(CutBranch b) {
    return b == CutBranch::Normal || b == CutBranch::Targeted || b == CutBranch::Chain;
}

// Left and right are swapped when exactly one of Mirror Mode (the world is mirrored) and Swap Swing Sides is on. An
// explicit XOR: both on cancel, as both off do.
inline bool flip_sides(bool mirrorMode, bool swapSwingSides) { return mirrorMode != swapSwingSides; }

// H38-c4: confirmed on the console (H38-c3): a stroke of the hand from the left to the right plays the game's LEFT cut and a
// stroke from the right to the left plays RIGHT (CUT_NM_PARAM_LEFT/RIGHT are named for the side the blade starts from). The
// mapping is the same for either sword hand: the detector already reports the stroke as the hand moved it. Swap Swing Sides
// (XOR Mirror Mode) turns it around.
inline CutStyle select_cut(SwordGesture g, CutBranch branch, bool mirrorMode, bool swapSwingSides) {
    if (!branch_takes_gesture(branch)) {
        return CutStyle::Keep;
    }
    const bool flip = flip_sides(mirrorMode, swapSwingSides);
    switch (g) {
    case SwordGesture::Vertical: return CutStyle::Vertical;
    case SwordGesture::LeftToRight: return flip ? CutStyle::Right : CutStyle::Left;
    case SwordGesture::RightToLeft: return flip ? CutStyle::Left : CutStyle::Right;
    // V1-rc7: a thrust is the stab -- not in a chain, which alternates left and right cuts.
    case SwordGesture::Thrust: return branch == CutBranch::Chain ? CutStyle::Keep : CutStyle::Stab;
    default: return CutStyle::Keep;  // Generic and None: the game's own choice
    }
}

// The gesture a sword pulse leaves for the game. Exactly one consumer takes it, once; anything that is not a plain cut
// discards it; a physical B press (its own attack) discards it; it expires. Each gesture has a serial that is never reused.
class GestureSlot {
public:
    static constexpr double kLifetimeS = 0.35;  // from the B press to the cut: a charge release can take a few ticks

    struct Taken {
        std::uint32_t serial = 0;
        SwordGesture gesture = SwordGesture::None;
        bool valid() const { return serial != 0; }
    };

    void offer(std::uint32_t serial, SwordGesture g, double now) {
        if (mActive) {
            ++mReplaced;  // a new swing before the last was used: the newer one counts
        }
        mActive = true;
        mSerial = serial;
        mGesture = g;
        mExpires = now + kLifetimeS;
        ++mOffered;
    }

    Taken take(double now) {
        if (!mActive) {
            return {};
        }
        if (now > mExpires) {
            clear();
            ++mExpired;
            return {};
        }
        const Taken t{mSerial, mGesture};
        clear();
        ++mTaken;
        return t;
    }

    // A special action started (or the cut was resolved some other way): the gesture is spent without being applied.
    void discard() {
        if (mActive) {
            clear();
            ++mDiscarded;
        }
    }

    bool pending(double now) const { return mActive && now <= mExpires; }
    std::uint32_t offered() const { return mOffered; }
    std::uint32_t taken() const { return mTaken; }
    std::uint32_t discarded() const { return mDiscarded; }
    std::uint32_t expired() const { return mExpired; }
    std::uint32_t replaced() const { return mReplaced; }
    void reset() { *this = GestureSlot{}; }

private:
    void clear() {
        mActive = false;
        mSerial = 0;
        mGesture = SwordGesture::None;
    }
    bool mActive = false;
    std::uint32_t mSerial = 0;
    SwordGesture mGesture = SwordGesture::None;
    double mExpires = 0.0;
    std::uint32_t mOffered = 0, mTaken = 0, mDiscarded = 0, mExpired = 0, mReplaced = 0;
};

}  // namespace dusk::wii
