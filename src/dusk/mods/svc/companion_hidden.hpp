#pragma once

// aleks2 (1.0.3): names a mod's by-name lookup (svc_hook->resolve) does not find while the companion screen is off.
//
// Twilight HD HUD (2.7.x, from GameBanana) decides that the dual-screen companion owns the item slots as soon as these
// two functions resolve, without asking whether the companion is on. They always exist in this port (the companion is
// built in), and the companion has been off by default since 1.0.1, so the mod gave its third item slot, its Fixed
// Bindings and its D-pad shortcuts to a screen nobody had open; Dawnlight 5.3.x copies the same test before deciding
// who provides Z items. Hidden while the companion is off, both behave as on upstream Dusklight, which has no
// companion. Whether they are hidden is decided once, when the mods are activated at startup (the setting then),
// and logged; turning the companion on later takes a restart for those mods.
//
// Lookups only: nothing here changes hooks, imports or the game's own calls. Add a name only with a reason like the
// above; the 1.0.3 catalog audit found no mod that imports these two, and only Dawnlight 5.3.3 looks them up by name.

#include <string_view>

namespace dusk::mods::svc::companion_hidden {

inline constexpr std::string_view kNames[] = {
    "dusk::companion::slotTriggerBits",
    "dusk::companion::slotHoldBits",
    "_ZN4dusk9companion15slotTriggerBitsEv",
    "_ZN4dusk9companion12slotHoldBitsEv",
};

inline bool hidden_while_off(std::string_view name) {
    for (const auto hidden : kNames) {
        if (name == hidden) {
            return true;
        }
    }
    return false;
}

// The lookup's answer for `name`, given the companion setting at startup: true = report it as not found.
inline bool hide(bool companionOnAtStartup, std::string_view name) {
    return !companionOnAtStartup && hidden_while_off(name);
}

}  // namespace dusk::mods::svc::companion_hidden
