#pragma once
// aleks2 (H28): the short benchmark behind Settings > Aleks > Diagnostics.
//
// A run loads a heavy known scene, lets it settle, waits for the pipeline queue
// to drain, measures 60 s and writes a versioned JSON report. It never changes
// a setting, never writes the save (the memory-card store is turned into a
// no-op while it runs) and puts the player back where they were.
//
// Everything here runs on the main thread, from the main loop.
#include <string>

struct interface_of_controller_pad;

namespace dusk::benchmark {

inline constexpr const char* kScene = "F_SP122";  // Hyrule Field, outside Castle Town (west)
inline constexpr int kSceneRoom = 8;
inline constexpr int kScenePoint = 0;
inline constexpr int kSceneLayer = -1;
inline constexpr int kWarmupSeconds = 10;
inline constexpr int kMeasureSeconds = 60;
inline constexpr int kPipelineWaitTimeoutSeconds = 90;

// A run is in progress (loading, measuring or putting things back).
bool active();

// Whether Start would do anything right now; if not, why, for the UI to show.
bool can_start(std::string& whyNot);

// Begins a run. Closes the menus; the caller does not need to. (Also true of a matrix.)
bool start(std::string& error);

// H33: Run Profile Matrix. Measures Quality, Balanced and Performance one after the other in
// the current console mode, one report each. The profiles are applied as ConfigVar overrides
// that are never written to config.json and are given back when it finishes, is cancelled,
// fails or the console mode changes. It does not replace the normal run, which still changes
// nothing. Refused (with the reason in `error`) when a profile could not be applied and
// restored safely; see perf_profile_apply.hpp.
bool can_start_matrix(std::string& whyNot);
bool start_matrix(std::string& error);
bool matrix_active();

// H34: Prepare Shaders. Visits the zones in shader_prep.hpp so their pipelines are compiled
// and stored in Aurora's own caches on this console, then goes back. Not a benchmark: it
// measures nothing and changes no setting. Same protections as a run: saves blocked, the
// in-memory save state put back, Start cancels. Writes a report next to the benchmarks.
bool can_start_prep(std::string& whyNot);
bool start_prep(std::string& error);
bool prep_active();

// H35: Run Performance Tour. Measures three zones one after the other -- heavy (Outside Castle
// Town - West), medium (Ordon Village), light (Link's House) -- each with its own load,
// pipeline wait, warm-up and window, with the settings the console already has (profile,
// resolution, companion, Flip Grip, interpolation, uniform reuse mode). Same protections as
// a run; one combined report, tour-<stamp>-<build>.json. See perf_tour.hpp.
bool can_start_tour(std::string& whyNot);
bool start_tour(std::string& error);
bool tour_active();

// Asks the run to stop at the next frame and restore. Safe at any time.
void request_cancel(const char* reason);

// Once per frame from the main loop, after this frame's Aurora stats are read.
void tick();

// From mDoCPd_c::read, after the pads are filled: blocks the game's input for
// the duration of a run (Start cancels it).
void filter_pad(interface_of_controller_pad* pads, int count);

// True from the moment a run starts until the player is back and any autosave
// the return could trigger has had its chance: the card store does nothing.
bool saves_blocked();

// One line for the on-screen status; empty when nothing is running.
std::string status_text();

// The summary of the last finished run, empty before the first.
const std::string& last_summary();

// Where reports are written: <data folder>/benchmarks, e.g.
// sdmc:/switch/dusklight/benchmarks on the console.
std::string report_directory();

// The build identity the report carries, e.g. "H28-c1-1e2574f7b7".
std::string build_id();

}  // namespace dusk::benchmark
