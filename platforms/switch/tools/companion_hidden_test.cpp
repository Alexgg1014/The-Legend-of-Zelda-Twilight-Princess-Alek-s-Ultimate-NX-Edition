// aleks2 (1.0.3): the names svc_hook->resolve does not offer while the companion screen is off
// (src/dusk/mods/svc/companion_hidden.hpp): exactly the two slot queries Twilight HD HUD and Dawnlight probe.
#include "dusk/mods/svc/companion_hidden.hpp"

#include <cstdio>

namespace ch = dusk::mods::svc::companion_hidden;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    g_fail += !ok;
}

int main() {
    check(ch::hidden_while_off("dusk::companion::slotTriggerBits"), "slotTriggerBits is hidden");
    check(ch::hidden_while_off("dusk::companion::slotHoldBits"), "slotHoldBits is hidden");
    check(ch::hidden_while_off("_ZN4dusk9companion15slotTriggerBitsEv"), "and its mangled name");
    check(ch::hidden_while_off("_ZN4dusk9companion12slotHoldBitsEv"), "and its mangled name");
    check(!ch::hidden_while_off("dusk::dualscreen::hudOnCompanion"), "hudOnCompanion stays visible (mods ask it)");
    check(!ch::hidden_while_off("dusk::companion::measureText"), "other companion functions stay visible");
    check(!ch::hidden_while_off("dusk::getActionBindTrig"), "unrelated names stay visible");
    check(!ch::hidden_while_off("dusk::companion::slotTriggerBit"), "a prefix is not a match");
    check(!ch::hidden_while_off(""), "empty is not a match");
    // The lookup's answer, by the companion setting at startup.
    check(ch::hide(false, "dusk::companion::slotTriggerBits") && ch::hide(false, "dusk::companion::slotHoldBits"),
        "companion off at startup: the lookup by name fails for both");
    check(!ch::hide(true, "dusk::companion::slotTriggerBits") && !ch::hide(true, "dusk::companion::slotHoldBits"),
        "companion on at startup: the lookup by name goes on to the manifest (found)");
    check(!ch::hide(false, "dusk::dualscreen::hudOnCompanion") && !ch::hide(false, "daAlink_c::execute"),
        "companion off: every other name is looked up as usual");
    std::printf("companion-hidden: %s\n", g_fail ? "FAIL" : "ok");
    return g_fail ? 1 : 0;
}
