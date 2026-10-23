#pragma once
// aleks2 (V1-c1): DRS stress test (Settings > Aleks > Diagnostics > Run DRS Stress).
//
// Walks the internal resolution through many changes in a row -- by default 120 changes, one
// every 2 s, bouncing through the DRS steps of the ceiling (100 -> 85 -> 75 -> 85 -> 100 ...);
// perf-lab.ini drs_stress_transitions / drs_stress_interval_ms / drs_stress_pattern=ladder
// (900 -> 720 -> 540 rows) change that without a new build -- with the render-target pool on,
// as DRS Auto uses it. After every change it appends a CSV row (flushed to the card, so a crash
// still leaves everything up to it): rows asked and rendered, process memory, malloc heap,
// render-target allocations / pool hits / evictions, the worst and mean frame of the interval
// and the GPU time. At the end it writes a JSON summary with the memory trend and gives the
// user's resolution back. Start (or any menu) cancels; nothing is saved to the game.
#include <string>

namespace dusk::drs_stress {

bool can_start(std::string& whyNot);
bool start(std::string& error);
bool active();
void request_cancel(const char* reason);

// Once per frame, main thread, after dynres::update().
void tick();

// One line for the on-screen status; empty when idle.
std::string status_text();

}  // namespace dusk::drs_stress
