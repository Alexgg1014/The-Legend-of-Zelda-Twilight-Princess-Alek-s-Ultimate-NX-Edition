// H38-c3: from the sword gesture to the cut (src/dusk/wii_cut_select.hpp): the mapping with Mirror Mode and Swap Swing
// Sides as an explicit XOR, which branches take a gesture, and the one-shot slot (exact consumption, special actions
// discard, no leaking into the next attack).
#include "dusk/wii_cut_select.hpp"

#include <cstdio>

using namespace dusk::wii;

namespace {
int failures = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                      \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)
}  // namespace

int main() {
    // the XOR, all four combinations
    CHECK(!flip_sides(false, false));
    CHECK(flip_sides(true, false));
    CHECK(flip_sides(false, true));
    CHECK(!flip_sides(true, true));

    // gesture x mirror x swap x branch
    const SwordGesture gestures[] = {SwordGesture::None, SwordGesture::Generic, SwordGesture::Vertical,
        SwordGesture::LeftToRight, SwordGesture::RightToLeft, SwordGesture::Thrust};
    const CutBranch branches[] = {CutBranch::Normal, CutBranch::Targeted, CutBranch::Chain, CutBranch::Finisher,
        CutBranch::StunnedEnemy, CutBranch::Other};
    for (SwordGesture g : gestures) {
        for (CutBranch b : branches) {
            for (int mirror = 0; mirror < 2; ++mirror) {
                for (int swap = 0; swap < 2; ++swap) {
                    const CutStyle s = select_cut(g, b, mirror != 0, swap != 0);
                    if (!branch_takes_gesture(b)) {
                        CHECK(s == CutStyle::Keep);  // finishers, stunned-enemy hits and everything else keep the game's cut
                        continue;
                    }
                    switch (g) {
                    case SwordGesture::None:
                    case SwordGesture::Generic: CHECK(s == CutStyle::Keep); break;
                    case SwordGesture::Vertical: CHECK(s == CutStyle::Vertical); break;  // sides do not matter
                    case SwordGesture::LeftToRight: CHECK(s == ((mirror != swap) ? CutStyle::Right : CutStyle::Left)); break;
                    case SwordGesture::RightToLeft: CHECK(s == ((mirror != swap) ? CutStyle::Left : CutStyle::Right)); break;
                    // V1-rc7: a thrust is the stab, except in a chain (which alternates left and right)
                    case SwordGesture::Thrust: CHECK(s == (b == CutBranch::Chain ? CutStyle::Keep : CutStyle::Stab)); break;
                    }
                }
            }
        }
    }
    // H38-c4, confirmed on the console: with both options off a stroke from the left plays the game's LEFT cut
    CHECK(select_cut(SwordGesture::LeftToRight, CutBranch::Normal, false, false) == CutStyle::Left);
    CHECK(select_cut(SwordGesture::RightToLeft, CutBranch::Normal, false, false) == CutStyle::Right);
    // Swap Swing Sides alone, or Mirror Mode alone, turns it around; both cancel
    CHECK(select_cut(SwordGesture::LeftToRight, CutBranch::Chain, false, true) == CutStyle::Right);
    CHECK(select_cut(SwordGesture::LeftToRight, CutBranch::Targeted, true, false) == CutStyle::Right);
    CHECK(select_cut(SwordGesture::LeftToRight, CutBranch::Normal, true, true) == CutStyle::Left);
    // opposite gestures give opposite cuts under every setting
    for (int mirror = 0; mirror < 2; ++mirror) {
        for (int swap = 0; swap < 2; ++swap) {
            CHECK(select_cut(SwordGesture::LeftToRight, CutBranch::Normal, mirror, swap) !=
                  select_cut(SwordGesture::RightToLeft, CutBranch::Normal, mirror, swap));
        }
    }

    // the slot: one consumer, once
    {
        GestureSlot slot;
        CHECK(!slot.take(0.0).valid());
        slot.offer(1, SwordGesture::Vertical, 10.0);
        CHECK(slot.pending(10.1));
        const auto t = slot.take(10.1);
        CHECK(t.valid() && t.serial == 1 && t.gesture == SwordGesture::Vertical);
        CHECK(!slot.take(10.1).valid() && !slot.pending(10.1));  // spent
        CHECK(slot.offered() == 1 && slot.taken() == 1);
    }
    // a special action (finisher, jump, charged attack, spin...) discards the gesture: it does not reach the next attack,
    // even well inside its lifetime
    {
        GestureSlot slot;
        slot.offer(1, SwordGesture::RightToLeft, 20.0);
        slot.discard();
        CHECK(!slot.take(20.05).valid());
        slot.offer(2, SwordGesture::Vertical, 20.1);  // the next swing
        const auto t = slot.take(20.15);
        CHECK(t.valid() && t.serial == 2 && t.gesture == SwordGesture::Vertical);
        CHECK(slot.discarded() == 1);
        // discarding with nothing pending changes nothing
        slot.discard();
        CHECK(slot.discarded() == 1);
    }
    // a gesture that is never used expires; it cannot be applied to an attack made later
    {
        GestureSlot slot;
        slot.offer(1, SwordGesture::Vertical, 30.0);
        CHECK(!slot.take(30.0 + GestureSlot::kLifetimeS + 0.01).valid());
        CHECK(slot.expired() == 1);
        slot.offer(2, SwordGesture::LeftToRight, 31.0);
        CHECK(slot.take(31.0 + GestureSlot::kLifetimeS - 0.01).valid());
    }
    // a newer swing replaces an unused older one (and says so); serials are the engine's and are not reused here
    {
        GestureSlot slot;
        slot.offer(7, SwordGesture::Vertical, 40.0);
        slot.offer(8, SwordGesture::LeftToRight, 40.1);
        const auto t = slot.take(40.12);
        CHECK(t.serial == 8 && t.gesture == SwordGesture::LeftToRight && slot.replaced() == 1);
        CHECK(!slot.take(40.12).valid());
    }
    // special action first, then a plain attack with no new gesture (a button press): nothing leaks
    {
        GestureSlot slot;
        slot.offer(5, SwordGesture::Vertical, 50.0);
        slot.discard();                 // commonProcInit of a special proc
        CHECK(!slot.take(50.2).valid());  // the next normal attack, by button, finds nothing
    }
    // a physical B press is its own attack: it discards a pending gesture (same call as a special action)
    {
        GestureSlot slot;
        slot.offer(5, SwordGesture::Vertical, 60.0);
        slot.discard();
        CHECK(!slot.pending(60.01));
    }
    if (failures == 0) {
        std::puts("wii cut select: mirror XOR swap, only plain cuts take a gesture, one consumer once, special actions and "
                  "expiry discard, no leak into the next attack: ok");
    }
    return failures == 0 ? 0 : 1;
}
