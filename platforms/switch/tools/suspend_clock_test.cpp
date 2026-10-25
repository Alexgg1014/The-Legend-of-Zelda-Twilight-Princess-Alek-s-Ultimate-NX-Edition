// aleks2 (1.0.1): aurora::time::skip_suspended leaves a suspended span out of game_clock (GitHub issue #5: play time
// counted the time the console slept). Drives the clock with a fake native time source.
#include <aurora/time.hpp>

#include "time_internal.hpp"

#include <chrono>
#include <cstdio>

using namespace std::chrono_literals;
namespace at = aurora::time;

static at::native_clock::duration g_now{};
static at::native_clock::time_point fake_now() noexcept { return at::native_clock::time_point{g_now}; }

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    g_fail += !ok;
}

static std::chrono::nanoseconds game() { return at::game_clock::now().time_since_epoch(); }

int main() {
    g_now = 10s;
    at::internal::set_now_function(fake_now);  // also resets: game time 0 at native 10 s
    g_now = 15s;
    check(game() == 5s, "game time follows the native clock while awake");
    g_now = 75s;  // asleep for 60 s
    const auto skipped = at::skip_suspended(60s);
    check(game() == 5s, "a skipped 60 s sleep does not count");
    check(skipped == 60s, "skip_suspended reports the 60 s it left out");
    g_now = 77s;
    check(game() == 7s, "game time runs on after the wake-up");
    at::skip_suspended(10min);  // longer than anything since the last rebase: clamped, never goes backwards
    check(game() == 7s, "a span reaching before the last rebase never moves the clock backwards");
    g_now = 90s;  // asleep 13 s, but something read the clock at 90 s before the wake-up hook ran
    check(game() == 20s, "a read before the hook sees the sleep");
    check(at::skip_suspended(13s) == 0s && game() == 20s, "the hook then skips nothing rather than going backwards");
    at::skip_suspended(0s);
    at::skip_suspended(-5s);
    check(game() == 20s, "zero and negative spans change nothing");
    at::set_scale(2.0f);
    g_now = 91s;
    check(game() == 22s, "the time scale still applies");
    at::internal::set_now_function(nullptr);
    std::printf("suspend-clock: %s\n", g_fail ? "FAIL" : "ok");
    return g_fail ? 1 : 0;
}
