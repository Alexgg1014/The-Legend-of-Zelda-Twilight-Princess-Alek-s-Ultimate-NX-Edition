#pragma once

#include "dusk/resolution_policy.hpp"

namespace dusk::display {

// The console's display mode right now: docked, Flip Grip, or plain handheld. Handheld on
// platforms other than the Switch (where the resolution policy is not applied).
respol::Mode mode();

// How many rows the "auto" internal resolution means right now (resolution_policy.hpp):
// the base policy of the last chosen performance profile, or Legacy (1080 docked, 720
// handheld, 540 under Flip Grip, where the game occupies 720x540 of the panel and anything
// taller is rendered only to be shrunk). Returns 0 on platforms where "auto" has no row
// count of its own.
int auto_rows();

// Call once a frame. When what the current resolution setting resolves to has changed (docked
// or undocked, Flip Grip on or off, a different base policy), applies it again -- whether the
// setting is Auto or a manual value that the mode limits. Nothing is written to the
// configuration. Also keeps the aspect policy in step with Flip Grip. Does nothing on
// platforms other than the Switch.
void poll();

}  // namespace dusk::display
