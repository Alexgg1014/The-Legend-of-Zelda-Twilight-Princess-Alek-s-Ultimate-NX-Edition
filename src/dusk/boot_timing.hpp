#pragma once

#include <cstdint>

// aleks2 (rc17): the boot split into timed phases, one log line each, from the process entry to the first frame,
// and one summary line ("boot work: X s (excluding N s in menus)"). Logging only: nothing here changes what the
// boot does. Definitions at the end of session_log.cpp (Switch); no-ops elsewhere.
//
// The calls sit on existing lines of the files they time, so no line of a PGO-profiled unit moves and the rc17
// profile still matches; they are noexcept, so no call adds an exception edge to a profiled function.
//
//   a phase:  const auto t = boot_timing::now(); <the work>; boot_timing::phase("what", t);
//   mods:     mod_loaded / mod_activated for each mod, mods_report() once: the totals and the slowest three;
//   menus:    menu_wait() for the prelaunch loop that runs before the game loop (no usable disc), play_pressed()
//             for the prelaunch menu shown over the first frames; neither counts as boot work;
//   frame():  once per main-loop frame; the second call (the first frame done) prints the summary.
namespace dusk::boot_timing {

#ifdef __SWITCH__
std::uint64_t now() noexcept;
// From the process entry (userAppInit, switch_startup.cpp) to game_main: the romfs mount and the rest.
void entry_report() noexcept;
void phase(const char* what, std::uint64_t since) noexcept;
// phase() when `when` (a value, not a branch at the call site).
void phase_if(bool when, const char* what, std::uint64_t since) noexcept;
// romfs:/dusklight.symdb, read at the first by-name lookup; left out of the mod it happens in.
void symbol_manifest(std::uint64_t since) noexcept;
void mod_loaded(const char* id, std::uint64_t since) noexcept;
void mod_activated(const char* id, std::uint64_t since) noexcept;
void mods_report() noexcept;
void menu_wait(std::uint64_t since) noexcept;
void play_pressed() noexcept;
void frame() noexcept;
#else
inline std::uint64_t now() noexcept { return 0; }
inline void entry_report() noexcept {}
inline void phase(const char*, std::uint64_t) noexcept {}
inline void phase_if(bool, const char*, std::uint64_t) noexcept {}
inline void symbol_manifest(std::uint64_t) noexcept {}
inline void mod_loaded(const char*, std::uint64_t) noexcept {}
inline void mod_activated(const char*, std::uint64_t) noexcept {}
inline void mods_report() noexcept {}
inline void menu_wait(std::uint64_t) noexcept {}
inline void play_pressed() noexcept {}
inline void frame() noexcept {}
#endif

}  // namespace dusk::boot_timing
