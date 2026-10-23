#pragma once
// aleks2 (V1-rc12): a sampling profiler for the game (main) thread, developer mode only (perf-lab.ini profile_main = 1).
//
// A thread on core 2 pauses the main thread about every millisecond (svcSetThreadActivity), reads its PC, LR and the
// return addresses of two frame records (svcGetThreadContext3 and the frame-pointer chain, checked against the main
// thread's stack), and lets it go again. Nothing allocates, logs or locks while the main thread is paused, so it cannot
// deadlock on a lock the game holds. Windows: every Performance Tour zone's measurement (zone_begin / zone_end), and
// once profile_delay_s after boot for profile_seconds. Each window writes
// sdmc:/switch/dusklight/cache/logs/profile-<zone>-<n>.txt; platforms/switch/tools/profile_report.py symbolizes it
// against the ELF (top functions, top call stacks).
//
// Off: no thread, no buffer; zone_begin / zone_end return at once (one bool, per tour zone, never per frame).

namespace dusk::sampler {

void start_if_enabled();             ///< once the perf-lab options are read (frame_telemetry.cpp)
void zone_begin(const char* zone);   ///< a tour zone's measurement window opens
void zone_end();                     ///< ... and closes; the file is written by the sampler thread

}  // namespace dusk::sampler
