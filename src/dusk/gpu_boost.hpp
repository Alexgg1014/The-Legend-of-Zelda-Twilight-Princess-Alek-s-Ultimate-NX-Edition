#pragma once
// aleks2 (V1-rc16): the opt-in handheld GPU boost (decided with Alex, 2026-10-20: presets never touch the clocks; every
// preset runs at stock clocks unless the player turns "GPU Boost" on). With it on, in handheld, the game asks the system
// for its official handheld configuration 0x92220008 (CPU 1020 MHz, GPU 460.8 MHz, memory 1331.2 MHz) -- the GPU up
// from 307.2, the memory unchanged: not an overclock, but more battery. Idea from nfsmw-nx (our own code; credited in
// THIRD_PARTY_NOTICES).
//
// Asked for only while the setting is on, the console is in handheld and the game is in front; given back (the
// configuration in force before) when it is turned off, on docking, HOME and quit. 1.5 s after asking, the clocks are
// read back: if the memory clock moved the request is undone and logged; if the GPU is not at 460.8 something else
// owns the clocks (sys-clk, an overclock): logged, and this steps aside for the rest of the session.

namespace dusk::gpu_boost {

// Once per presented frame (main loop).
void tick();
// Quit: the configuration in force before, if it was changed.
void restore();
// For the session log and the tours: is the boost being asked for right now.
bool active();
// The clocks in effect, MHz (clkrst; 0 when unknown).
void clocks_mhz(unsigned& cpu, unsigned& gpu, unsigned& emc);

}  // namespace dusk::gpu_boost
