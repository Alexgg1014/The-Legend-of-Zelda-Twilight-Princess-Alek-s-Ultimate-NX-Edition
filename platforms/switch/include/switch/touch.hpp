#ifndef DUSK_SWITCH_TOUCH_H
#define DUSK_SWITCH_TOUCH_H

// libnx touchscreen -> companion::touchEvent, plus the psm battery readout.
// No-ops off Switch so the call sites need no #ifdef.

namespace dusk::switch_touch {

#ifdef __SWITCH__
void initialize();
void update();
void updateBattery();
#else
inline void initialize() {}
inline void update() {}
inline void updateBattery() {}
#endif

}  // namespace dusk::switch_touch

#endif  // DUSK_SWITCH_TOUCH_H
