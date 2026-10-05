#pragma once

#include "dusk/ui/platform_text_input.hpp"

namespace dusk::sw {

// The console's software keyboard (libnx swkbd) behind the game's PlatformTextInput seam.
dusk::ui::PlatformTextInput& text_input();

}  // namespace dusk::sw
