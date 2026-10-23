#pragma once
// aleks2 (SHADERS-c1): shader preparation in the running game (Switch). The rules live in
// shader_warmup.hpp; this file reads the lab options, asks Aurora how the startup pass is
// going (include/aurora/shader_cache.h), writes the marker when it completes and tells the
// preparation screen and the benchmark what they need.
#include "dusk/shader_warmup.hpp"

#include <cstdint>
#include <string>

namespace dusk::shader_warmup {

/// Before aurora_initialize: reads <data>/shader-lab.ini and exports the cache options
/// for Aurora (AURORA_CACHE_JOURNAL/_SYNC/_CHECK). Without the file nothing changes.
void load_lab_options();
const LabOptions& lab_options();

/// After aurora_initialize: reads the marker and decides whether to show the screen.
void begin();

/// Every frame: notices the end of the pass, writes the marker, logs progress.
void tick();
/// V1-rc7: called every frame while a stage is being created (the black loading screen): true while that stage's
/// shaders are still queued and the wait is under game.shaderSceneWaitMs. The scene's first 2 s are then measured
/// (hitches over 50 ms) and logged.
bool scene_shaders_pending();

struct Progress {
    std::uint32_t done = 0;
    std::uint32_t total = 0;
    bool finished = false;
    double elapsedSeconds = 0.0;
    std::string counter;  ///< "312 / 584"
    std::string eta;      ///< "" until there is an estimate
};
Progress progress();

/// The screen should be shown once at startup (decided by begin()).
bool screen_wanted();
/// The screen is on (between shown() and closed()): for the log and the skip count.
void screen_shown();
void screen_closed(bool skipped);
Reason reason();

/// True while the startup pass is still compiling; `why` says so for the benchmark UI.
bool busy(std::string& why);

}  // namespace dusk::shader_warmup
