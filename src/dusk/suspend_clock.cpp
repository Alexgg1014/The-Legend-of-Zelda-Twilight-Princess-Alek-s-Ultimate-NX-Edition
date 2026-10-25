#include "dusk/suspend_clock.hpp"

#include "dusk/logging.h"

#include <aurora/time.hpp>

#include <atomic>
#include <chrono>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace dusk::suspend_clock {
namespace {

using std::chrono::nanoseconds;

// A frame never takes this long while awake; a longer gap at a wake-up is the time asleep. Loading hitches stay
// well under it, and they happen while awake anyway, so they are never mistaken for sleep: only a gap ending at a
// wake-up is skipped.
constexpr nanoseconds kMinimumSuspension = std::chrono::seconds{2};
// The last frame before the sleep still counts.
constexpr nanoseconds kOneFrame = std::chrono::milliseconds{34};

std::atomic<int64_t> s_lastFrameNs{0};

int64_t native_now_ns() {
    return aurora::time::native_clock::now().time_since_epoch().count();
}

#ifdef __SWITCH__
AppletHookCookie s_cookie;
bool s_hooked = false;

void on_applet(AppletHookType type, void*) {
    // OnResume: back from sleep. OnFocusState back in focus: back from the HOME menu, where libnx keeps the game
    // suspended too. Either way no frame ran in between.
    const bool resumed = type == AppletHookType_OnResume ||
                         (type == AppletHookType_OnFocusState && appletGetFocusState() == AppletFocusState_InFocus);
    if (!resumed) {
        return;
    }
    const int64_t last = s_lastFrameNs.load(std::memory_order_relaxed);
    if (last == 0) {
        return;
    }
    const nanoseconds gap{native_now_ns() - last};
    if (gap < kMinimumSuspension) {
        return;
    }
    const nanoseconds asleep = gap - kOneFrame;
    const nanoseconds skipped = aurora::time::skip_suspended(asleep);
    s_lastFrameNs.store(native_now_ns(), std::memory_order_relaxed);
    DuskLog.info("Resumed ({}): away {:.1f} s, {:.1f} s left out of game time (play time, timers)",
        type == AppletHookType_OnResume ? "from sleep" : "from the HOME menu",
        std::chrono::duration<double>(asleep).count(), std::chrono::duration<double>(skipped).count());
}
#endif

}  // namespace

void tick() {
#ifdef __SWITCH__
    if (!s_hooked) {
        // The hook runs from appletMainLoop (SDL's event pump) on the main thread, like switch_net.cpp's.
        appletHook(&s_cookie, on_applet, nullptr);
        s_hooked = true;
    }
#endif
    s_lastFrameNs.store(native_now_ns(), std::memory_order_relaxed);
}

}  // namespace dusk::suspend_clock
