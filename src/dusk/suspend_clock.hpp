#pragma once

// aleks2 (1.0.1, GitHub issue #5): time the console spends asleep does not count as game time. The save's play time
// (d_save.cpp: OSGetTime() - start), achievements and the speedrun timer all read OSGetTime, which follows Aurora's
// game_clock; on Horizon the steady clock under it keeps running while the console sleeps. Each frame records when it
// ran; when libnx reports the wake-up (AppletHookType_OnResume), the gap since the last frame is skipped in the game
// clock. Wall-clock dates (OSGetSystemTime: the save date, the in-game calendar) are untouched.
namespace dusk::suspend_clock {

// Once per game frame (m_Do_main's loop).
void tick();

}  // namespace dusk::suspend_clock
