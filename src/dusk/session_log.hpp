#pragma once

// aleks2: make the on-card log self-describing. Every line carries the
// seconds since launch (borealis' sink has no timestamp), and three kinds of
// stamps cross the log with the Horizon-monitor captures:
//   - settings: the performance-relevant knobs (preset, interpolation,
//     resolution, shadows, post) plus the clocks clkrst reports, at launch and
//     whenever the config is saved;
//   - stage: every scene change with the stage the game is heading to;
//   - heartbeat: every 30 s, the average fps Aurora measured, the stage and the
//     clocks.
// Switch only; no-ops elsewhere.

namespace dusk::session_log {

void stampSettings(const char* why);
void stampStage(const char* stageName, int procName);
void tick();  // once per frame; emits the heartbeat when due
// V1-rc12: the next tick() emits the heartbeat now (Check Tour: one full set of lines per spot).
void requestHeartbeat();
// V1-rc1: one line per startup step, with the seconds since launch and since the previous step, so
// the time before the first menu frame can be split (it was ~37 s of black screen on the console).
void stampBoot(const char* step);
// V1-rc1: CPU boost (Horizon's FastLoad mode, the one games use for loading screens) while the
// shader preparation screen is up; off restores the normal clocks. No-op elsewhere.
void cpuBoost(bool on);
// V1-rc3: std::terminate writes what was thrown (and in which thread) to the log before aborting. rc1/rc2 died
// that way in a helper thread and the only trace was the side effect: abort -> __appExit unmounts sdmc while
// the other threads keep using it.
void installTerminateLogger();

// H28: the clocks clkrst reports right now, in MHz, for reports that need to say
// what the console was running at. Read-only: nothing here sets a clock. False
// (and zeros) off Switch or when clkrst could not be opened.
bool readClocksMHz(unsigned& cpu, unsigned& gpu, unsigned& emc);

}  // namespace dusk::session_log

// aleks2 (rc17): the boot phase timing, here at the end so no line above moves (PGO).
#include "dusk/boot_timing.hpp"
