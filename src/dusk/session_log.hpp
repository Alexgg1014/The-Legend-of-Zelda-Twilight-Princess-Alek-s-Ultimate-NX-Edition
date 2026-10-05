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

// H28: the clocks clkrst reports right now, in MHz, for reports that need to say
// what the console was running at. Read-only: nothing here sets a clock. False
// (and zeros) off Switch or when clkrst could not be opened.
bool readClocksMHz(unsigned& cpu, unsigned& gpu, unsigned& emc);

}  // namespace dusk::session_log
