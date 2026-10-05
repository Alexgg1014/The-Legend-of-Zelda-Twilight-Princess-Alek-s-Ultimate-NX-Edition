#pragma once

// H33: binds perf_profile.hpp (plain data) to the real settings.
#include "dusk/perf_profile.hpp"

#include <string>

namespace dusk::profile {

const char* name(int profile);  // "Quality", "Balanced", "Performance" or "Custom"

// The current value of every knob a profile owns, as the plain struct perf_profile.hpp wants.
Knobs currentKnobs();

// Which profile the current knobs are, or kCustom. Only for the row's label: the resolution
// is decided by game.autoResolutionPolicy, never by this.
int detectCurrent();

// Choosing a profile in Settings: sets every knob it owns (and the Auto resolution policy),
// as a normal persistent selection. The caller saves the configuration.
void applyPersistent(int profile);

// --- Run Profile Matrix: the same knobs, but only as ConfigVar Override layer ----------
// An override is never written to config.json (ConfigVar::getValueForSave ignores it) and
// does not survive the process. It is refused unless every knob it would have to change is
// on the Value layer: ConfigVar::clearOverride() hands back whatever `value` holds, and for a
// variable still on the Default layer that is a zero, not the default. A knob that already
// has the target value is not touched at all.
bool canApplyOverride(int profile, std::string& whyNot);
bool applyOverride(int profile, std::string& whyNot);
// Gives every knob overridden by applyOverride back to its own layer. Safe to call any time.
void clearOverrides();
bool overridesActive();

}  // namespace dusk::profile
