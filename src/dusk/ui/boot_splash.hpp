#pragma once
// aleks2 (V1-rc19): the boot splash, which is also the loading screen while the mods load (about 18 s on rc18, a few
// with mods/.libcache). Shown once the GPU and RmlUi are up, closed before the game loop's first frame.
//
// No thread of its own: progress() presents one frame itself (aurora_update, begin_frame, the UI, end_frame, as the
// prelaunch loop does), called from the loader between mods. If a frame cannot be presented it is skipped; nothing
// else depends on the splash. Our own text and fonts only: no game art.
namespace dusk::ui::boot_splash {

void show() noexcept;
/// "Loading mods 3/10: Dawnlight" and a bar at done/total; total 0: the label alone. At most one frame per ~30 ms.
void progress(const char* label, int done, int total) noexcept;
void hide() noexcept;

}  // namespace dusk::ui::boot_splash
