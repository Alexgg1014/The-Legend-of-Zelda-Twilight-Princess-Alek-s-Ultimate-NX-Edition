#pragma once

namespace dusk::sw {

// Checks the program was launched the way it can run, and does the per-process
// setup that has to happen on the main thread before anything else: the card
// folders, and the core the main thread runs on.
//
// Returns false when it was not launched that way. Horizon starts homebrew from
// the album as an "applet", with a fraction of the memory and no way to take over
// the screen, and the game cannot run like that; in that case the player has been
// shown how to start it as a title takeover, has pressed Plus, and the caller
// should simply return from main.
bool begin_session();

}  // namespace dusk::sw
