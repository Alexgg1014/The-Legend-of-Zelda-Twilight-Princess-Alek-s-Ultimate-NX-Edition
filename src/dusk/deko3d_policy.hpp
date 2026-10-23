#pragma once

// aleks2 (deko3d rc12): when the deko3d renderer may run and what happens when it may not. deko3d draws Flip Grip
// and Dual Screen HUD (its present composites them) but does not run mods' gfx service work (draw types, compute
// tasks, offscreen passes, present targets). So a start that would use deko3d uses Vulkan (Dawn/NVK) instead when a
// mod used the gfx service in an earlier deko3d start (cache/needs_dawn.txt), and the player is told why. Nothing
// here runs with Dawn/NVK besides the start-up check.

#include <aurora/aurora.h>

#include <string_view>

namespace dusk::deko3d_policy {

#ifdef __SWITCH__
namespace detail {
extern bool g_running;
}

// Whether Auto means deko3d in this build (ALEKS2_DEKO3D_DEFAULT; rc12: yes). Settings names the backends by it.
bool deko3d_default() noexcept;
// Before aurora_initialize: the backend this start really asks for.
AuroraBackend boot_backend(AuroraBackend desired);
// After aurora_initialize, with the backend that started.
void after_boot(AuroraBackend started);
// After mods_init: forgets the mods in cache/needs_dawn.txt that are not enabled any more.
void after_mods_loaded();
// Settings: the player turned Dual Screen HUD on.
void on_dual_screen_enabled();
// Settings: the player picked a graphics backend.
void on_backend_chosen(AuroraBackend backend);
// The gfx service, with deko3d running: a mod asked for something deko3d does not run (what: "a draw",
// "a compute task", ...). Noted once per mod; the next start uses Dawn/NVK.
void note_gfx_mod(std::string_view modId, std::string_view modName, const char* what);

inline bool deko3d_running() noexcept { return detail::g_running; }
#else
constexpr bool deko3d_default() noexcept { return false; }
inline AuroraBackend boot_backend(AuroraBackend desired) { return desired; }
inline void after_boot(AuroraBackend) {}
inline void after_mods_loaded() {}
inline void on_dual_screen_enabled() {}
inline void on_backend_chosen(AuroraBackend) {}
inline void note_gfx_mod(std::string_view, std::string_view, const char*) {}
constexpr bool deko3d_running() noexcept { return false; }
#endif

}  // namespace dusk::deko3d_policy
