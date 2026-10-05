#pragma once

// aleks2 (performance): stepped dynamic resolution.
//
// Not per-frame scaling -- Aurora's frame buffer is resized through the
// deferred surface-resize path, which costs a synchronize and a target
// rebuild, so a change is a small hitch. This controller therefore moves one
// rung at a time on a 2 s cadence with hysteresis, and refuses to sacrifice
// resolution when the drop did not buy any frames (main-thread bound), which
// on this console is most of the heavy places.
//
// Rungs are the existing internalResolutionScale values 1x (~480 rows),
// 540, 720, 900, 1080. The ceiling is the user's own internalResolutionScale
// (or what "auto" resolves to); the floor is 1x.

namespace dusk::dynres {

// Once per presented frame, main thread, after the frame was presented.
void update();

// Forget history and return to the ceiling (preset applied, feature toggled).
void reset();

// The internalResolutionScale value in effect, 0 when the controller is idle
// (disabled, or at the ceiling with nothing overridden).
int currentOverride();

// H28: how many times the controller has changed the resolution since launch.
// Only ever grows, so a benchmark reads it before and after its window.
unsigned transitionCount();

}  // namespace dusk::dynres
