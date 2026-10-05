// H33 audit of the ConfigVar Override layer, which Run Profile Matrix relies on to change
// settings without persisting them. Builds the real src/dusk/config_var.hpp with only the
// out-of-line base-class members stubbed. The matrix is allowed only if these hold:
//   1. an override is never what getValueForSave() returns;
//   2. clearOverride() gives a Value-layer variable back its saved value;
//   3. the layer is visible (getLayer) so a variable that would not restore safely is refused.
// It also documents the one case that does NOT restore (check 4), which is why the matrix
// refuses to touch a variable that is still on the Default layer.
//   platforms/switch/tools/test-config-override-audit.sh
#include "dusk/config_var.hpp"

#include <iostream>

namespace dusk::config {
ConfigVarBase::ConfigVarBase(std::string n, const ConfigImplBase* i)
    : name(std::move(n)), registered(false), layer(ConfigVarLayer::Default), impl(i) {}
ConfigVarBase::~ConfigVarBase() {}
const char* ConfigVarBase::getName() const noexcept { return name.c_str(); }
const ConfigImplBase* ConfigVarBase::getImpl() const noexcept { return impl; }
void ConfigVarBase::markRegistered() { registered = true; }
void ConfigVarBase::unmarkRegistered() { registered = false; }
bool ConfigVarBase::has_subscribers() const { return false; }
void ConfigVarBase::notify_changed(const void*) {}
template <>
const ConfigImplBase* GetConfigImpl<int>() { return nullptr; }
template <>
const ConfigImplBase* GetConfigImpl<bool>() { return nullptr; }
}  // namespace dusk::config

static int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

using namespace dusk::config;

int main() {
    // A saved value (Value layer): what a user who chose a profile has.
    ConfigVar<int> saved{"t.saved", 7};
    saved.markRegistered();
    saved.setValue(3);
    CHECK(saved.getLayer() == ConfigVarLayer::Value);

    saved.setOverrideValue(99);
    CHECK(saved.getLayer() == ConfigVarLayer::Override);
    CHECK(saved.getValue() == 99);            // the game sees the override
    CHECK(saved.getValueForSave() == 3);      // 1: the file never would
    saved.clearOverride();
    CHECK(saved.getLayer() == ConfigVarLayer::Value);
    CHECK(saved.getValue() == 3);             // 2: restored
    CHECK(saved.getValueForSave() == 3);

    // Overriding twice and clearing once restores as well (the matrix swaps profiles).
    saved.setOverrideValue(11);
    saved.setOverrideValue(12);
    CHECK(saved.getValueForSave() == 3);
    saved.clearOverride();
    CHECK(saved.getValue() == 3);

    // 4: a variable still on the Default layer does NOT restore: clearOverride() lands on
    // Value, whose stored `value` was never set, so the variable reads a zero and would be
    // saved as one. The matrix therefore refuses to override such a variable.
    ConfigVar<int> neverSet{"t.never", 7};
    neverSet.markRegistered();
    CHECK(neverSet.getLayer() == ConfigVarLayer::Default);
    CHECK(neverSet.getValue() == 7);
    neverSet.setOverrideValue(99);
    neverSet.clearOverride();
    CHECK(neverSet.getLayer() == ConfigVarLayer::Value);
    CHECK(neverSet.getValue() == 0);  // not 7: documented hazard
    CHECK(neverSet.getValue() != neverSet.getDefaultValue());

    // 3: the layers the matrix must refuse are all distinguishable.
    ConfigVar<bool> launchArg{"t.arg", false};
    launchArg.markRegistered();
    launchArg.setOverrideValue(true);
    CHECK(launchArg.getLayer() == ConfigVarLayer::Override);
    ConfigVar<bool> speedrun{"t.speedrun", false};
    speedrun.markRegistered();
    speedrun.setSpeedrunValue(true);
    CHECK(speedrun.getLayer() == ConfigVarLayer::Speedrun);

    // clearOverride() on a variable that is not overridden changes nothing.
    ConfigVar<int> plain{"t.plain", 5};
    plain.markRegistered();
    plain.setValue(8);
    plain.clearOverride();
    CHECK(plain.getValue() == 8 && plain.getLayer() == ConfigVarLayer::Value);

    // A launch-argument override would be clobbered by the matrix and handed back as Value:
    // the matrix refuses it (layer != Value). Shown here for the record.
    launchArg.setOverrideValue(false);
    launchArg.clearOverride();
    CHECK(launchArg.getLayer() == ConfigVarLayer::Value);  // the arg override is gone

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "config override audit: all checks passed\n";
    return 0;
}
