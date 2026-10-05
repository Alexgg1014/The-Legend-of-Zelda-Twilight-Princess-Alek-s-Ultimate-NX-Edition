#pragma once

// aleks2 H38: Wii-style motion controls on the Switch.
//
// The detector (wii_gesture_detector.hpp) is pure; this is the runtime around it: it reads the Joy-Con motion queues
// (accelerometer and gyroscope, with the time each sample covers) once per simulated tick, before mDoCPd_c::read() consumes
// the results, and turns them into
//   * a one-frame B press for the sword (injected in mDoCPd_c::read(), after convert(), before the benchmark filter,
//     so it goes through the game's ordinary rules);
//   * with it, the way the sword was swung, held in a one-shot slot (wii_cut_select.hpp) that daAlink_c::checkCutAction
//     takes to choose the vertical, left or right cut; every other cut or special action discards it, so a gesture cannot
//     reach the next attack. The stick is never simulated;
//   * a short latch the spin attack reads (daAlink_c::checkCutTurnInputTrigger), so the vanilla validations decide;
//   * a short latch the shield bash reads, next to the R press, inside the block that already checks the shield, the
//     ground and the unlocked technique (daAlink_c::checkItemActionInitStart's shield-attack branch).
// Which Joy-Con swings the sword is a setting (Motion Sword Hand); the other is the Nunchuk. Aiming by motion is the existing
// Gyro Aim; there is no Wii pointer cursor for menus (out of scope).
// Off by default. The pulses are never raised while a Dusklight document is open, during benchmarks and tours, outside
// plain gameplay, while paused or during the recenter window.

#include "dusk/wii_cut_select.hpp"
#include "dusk/wii_gesture_detector.hpp"

#include <string>

namespace dusk::wii {

// Once per simulated tick, before mDoCPd_c::read(). dt is the tick length in seconds.
void sample(float dt);

// Consumed by the game; each returns true at most once per gesture.
bool take_sword_pulse();
bool consume_spin();
bool consume_shield_bash();

// The way the sword was swung, for daAlink_c::checkCutAction: taken (and so spent) exactly once, valid() false when the
// attack was not made by a gesture. cut_style_for() turns it into the cut for the branch being decided, using Mirror Mode
// and Swap Swing Sides (their XOR); Keep leaves the game's own choice.
GestureSlot::Taken take_sword_gesture();
CutStyle cut_style_for(const GestureSlot::Taken& taken, CutBranch branch);
// A special action started (any procedure other than the cut charge), or a physical B press: the pending gesture is spent.
void discard_sword_gesture();

// Settings actions and status.
void recalibrate();                 // re-estimates the resting pose and holds gestures for a moment
std::string status_text();          // sensors detected and which gestures are available
bool available();                   // at least the sword gesture can work with the current controller

}  // namespace dusk::wii
