#pragma once
// aleks2 (V1-rc16): HD Rumble on the Switch's own controllers (hd_rumble_core.hpp says what each vibration becomes).
// Settings > Input > Rumble: "HD Rumble" (on) and "Rumble Intensity" (0-100 %, also the classic rumble's).

namespace dusk::hd_rumble {

// HD Rumble is on and pad 1 vibrates through the console's own controllers (a GameCube adapter keeps its motor):
// dVibration_c then leaves the GameCube motor still and the envelopes play instead.
bool active();
// dVibration_c::Run(), once per game frame, after its motor state is updated.
void vibration_frame(int shockMode, int shockFrame, int quakeMode, int quakeFrame);
// dVibration_c::Pause / Kill: nothing keeps buzzing.
void stop();
// The main loop, once per presented frame: the intensity setting, and a stop when the game stops running its
// vibration (a load, a pause menu) or the app leaves the foreground (HOME).
void tick();

}  // namespace dusk::hd_rumble
