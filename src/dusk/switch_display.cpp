#include "dusk/switch_display.hpp"

#include "dusk/dualscreen.h"
#include "dusk/perf.hpp"
#include "dusk/settings.h"

#include <aurora/aurora.h>

namespace dusk::display {

respol::Mode mode() {
#ifdef __SWITCH__
    if (perf::is_docked()) {
        return respol::Mode::Docked;
    }
    // flipWanted() rather than flipActive(): right before the first frame and in the
    // pre-launch screen, where the panel split has not been pushed yet.
    return dualscreen::flipWanted() ? respol::Mode::Flip : respol::Mode::Handheld;
#else
    return respol::Mode::Handheld;
#endif
}

int auto_rows() {
#ifdef __SWITCH__
    return respol::resolve(0, getSettings().game.autoResolutionPolicy.getValue(), mode()).rows;
#else
    return 0;
#endif
}

void poll() {
#ifdef __SWITCH__
    // The applet's operation mode is a value libnx keeps up to date, so asking
    // every frame costs nothing.
    dualscreen::applyAspectPolicy();

    static int s_appliedValue = -1;
    static int s_appliedMode = -1;
    respol::g_deko3dRenderer = aurora_get_backend() == BACKEND_DEKO3D;  // post-rc12: Balanced handheld 720 rows
    const auto& game = getSettings().game;
    const auto m = mode();
    const auto resolved = respol::resolve(
        game.internalResolutionScale.getValue(), game.autoResolutionPolicy.getValue(), m);
    if (resolved.value == s_appliedValue && static_cast<int>(m) == s_appliedMode) {
        return;
    }
    s_appliedValue = resolved.value;
    s_appliedMode = static_cast<int>(m);
    applyInternalResolutionScale(game.internalResolutionScale.getValue());
#endif
}

}  // namespace dusk::display
