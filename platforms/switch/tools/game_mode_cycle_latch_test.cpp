// H38-c5: the Play / Randomizer carousel changes once per press, however long the direction is held
// (src/dusk/ui/game_mode_cycle_latch.hpp). No game, no UI.
#include "dusk/ui/game_mode_cycle_latch.hpp"

#include <cstdio>

using dusk::ui::GameModeCycleLatch;

static int g_failures = 0;
#define CHECK(c)                                                                                  \
    do {                                                                                          \
        if (!(c)) {                                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                              \
            ++g_failures;                                                                         \
        }                                                                                         \
    } while (0)

// A two-entry carousel driven the way the UI drives it: the first key-down cycles, later key-downs of the same hold (the
// global auto-repeat) are ignored, the key-up rearms. Returns how many times the carousel moved.
static int hold(GameModeCycleLatch& latch, int direction, int keyDowns, int& index) {
    int moves = 0;
    for (int i = 0; i < keyDowns; ++i) {
        if (latch.press(direction)) {
            index = ((index + direction) % 2 + 2) % 2;
            ++moves;
        }
    }
    latch.release(direction);
    return moves;
}

int main() {
    {  // the first key-down changes once; repeats before the key-up do not (Play -> Randomizer, not back to Play)
        GameModeCycleLatch latch;
        int index = 0;  // 0 = Play, 1 = Randomizer
        CHECK(hold(latch, 1, 1, index) == 1 && index == 1);
        index = 0;
        CHECK(hold(latch, 1, 40, index) == 1 && index == 1);  // a long hold: 40 key-downs, one change
    }
    {  // the key-up rearms, for the same direction
        GameModeCycleLatch latch;
        int index = 0;
        CHECK(hold(latch, 1, 5, index) == 1 && index == 1);
        CHECK(hold(latch, 1, 5, index) == 1 && index == 0);
        CHECK(hold(latch, 1, 1, index) == 1 && index == 1);
    }
    {  // the same for left
        GameModeCycleLatch latch;
        int index = 0;
        CHECK(hold(latch, -1, 12, index) == 1 && index == 1);
        CHECK(hold(latch, -1, 12, index) == 1 && index == 0);
    }
    {  // left and right are independent, including when they overlap
        GameModeCycleLatch latch;
        CHECK(latch.press(1));
        CHECK(!latch.press(1));
        CHECK(latch.press(-1));   // left goes down while right is still held: it counts
        CHECK(!latch.press(-1));
        CHECK(!latch.press(1));
        latch.release(-1);
        CHECK(latch.press(-1));   // left rearmed, right still held
        CHECK(!latch.press(1));
        latch.release(1);
        CHECK(latch.press(1));
        latch.release(1);
        latch.release(-1);
        CHECK(latch.press(1) && latch.press(-1));
    }
    {  // a key-up with nothing held changes nothing
        GameModeCycleLatch latch;
        latch.release(1);
        latch.release(-1);
        CHECK(latch.press(1) && latch.press(-1));
    }
    {  // focus lost while a direction was down: reset rearms both, so the next press is not swallowed
        GameModeCycleLatch latch;
        CHECK(latch.press(1));
        CHECK(latch.press(-1));
        latch.reset();
        CHECK(latch.press(1));
        CHECK(latch.press(-1));
        CHECK(!latch.press(1));
    }
    if (g_failures == 0) {
        std::printf("game-mode carousel: one cycle per press, repeats ignored, release rearms, directions independent: ok\n");
    }
    return g_failures == 0 ? 0 : 1;
}
