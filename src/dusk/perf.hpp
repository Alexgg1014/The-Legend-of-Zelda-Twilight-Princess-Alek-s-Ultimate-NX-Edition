#pragma once
// The one platform question the companion asks of the console: is it docked?
// (Clock and power profiles are not touched by this program; the session log only
// reads back what the console is running.) Flip Grip is handheld-only, since a dock
// cannot be turned, so dualscreen.cpp falls back to Normal whenever this is true.
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace dusk::perf {

inline bool is_docked() {
#ifdef __SWITCH__
    return appletGetOperationMode() == AppletOperationMode_Console;
#else
    return false;
#endif
}

}  // namespace dusk::perf
