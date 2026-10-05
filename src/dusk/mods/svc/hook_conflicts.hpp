#pragma once
// aleks2 (H37): every time two mods' hooks collide on one function, recorded instead of only
// logged, so the loader can say why a mod failed (safe mode's "hook conflict") and the mod
// browser can refuse to enable a pair that cannot run together and say why.
//
// What collides: only replace-hooks. Pre- and post-hooks of any number of mods compose -- each
// runs with its own context, in priority order (svc/hook.cpp) -- so Randomizer and Cosmetics,
// which both post-hook dMenu_Ring_c::_create, run together. A second replace on a target held
// by another mod either fails (HOOK_REPLACE_CONFLICT, or PRIORITY not higher) or displaces the
// first (OVERRIDE, or higher PRIORITY). Displacing is never silent: it is recorded and logged,
// and where displacement is not allowed (Switch V1, see set_displacement_allowed) it is refused
// like a conflict, because the displaced mod would go on running believing its hook is live.
//
// Platform-neutral and independent of the hook engine, so it is tested on its own
// (platforms/switch/tools/hook_conflicts_test.cpp).
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dusk::mods::hooks {

enum class ReplacePolicy : uint8_t { Conflict, Priority, Override };

enum class Outcome : uint8_t {
    Refused,    // the newcomer's replace failed with MOD_CONFLICT; the holder keeps the target
    Displaced,  // the newcomer took the target from the holder (allowed, and recorded)
};

struct Conflict {
    std::string target;    // symbol or address of the hooked function
    std::string holder;    // mod id that had the replace-hook
    std::string newcomer;  // mod id that asked for it
    ReplacePolicy policy = ReplacePolicy::Conflict;
    Outcome outcome = Outcome::Refused;
};

// What hook_replace should do when `newcomer` asks for a target `holder` already replaces. A
// mod replacing its own hook again is not a conflict: no record, the SDK policy decides.
struct Decision {
    bool allow = false;  // true: displace the holder; false: MOD_CONFLICT
    std::optional<Conflict> record;
};
Decision decide(std::string_view target, std::string_view holder, std::string_view newcomer,
    ReplacePolicy policy, int32_t newcomerPriority, int32_t holderPriority);

// Whether one mod's replace may displace another's. Desktop keeps the SDK's semantics (true);
// the Switch build turns it off for V1.
void set_displacement_allowed(bool allowed);
bool displacement_allowed();

void record(Conflict conflict);
std::vector<Conflict> all();
// The conflicts a mod took part in, either side.
std::vector<Conflict> involving(std::string_view modId);
// A one-line explanation for a refusal, for logs, safe mode and the browser.
std::string describe(const Conflict& conflict);
void clear();

}  // namespace dusk::mods::hooks
