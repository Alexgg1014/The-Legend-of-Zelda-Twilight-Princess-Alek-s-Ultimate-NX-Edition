#include "dusk/perf_profile_apply.hpp"

#include "dusk/dynres.hpp"  // defined on the Switch only; calls below are guarded
#include "dusk/presentation.hpp"
#include "dusk/settings.h"

#include <aurora/gfx.h>

#include <functional>
#include <vector>

namespace dusk::profile {
namespace {

std::vector<std::function<void()>> s_clear;

// Calls fn(var, target) for every knob profile `id` owns.
template <typename Fn>
void forEachKnob(int id, Fn&& fn) {
    const Knobs& k = kTable[id];
    auto& g = getSettings().game;
    fn(g.internalResolutionScale, k.resolution);
    fn(g.autoResolutionPolicy, k.policy);
    fn(g.enableFrameInterpolation, static_cast<FrameInterpMode>(k.interpolation));
    fn(g.interpolationQuality, static_cast<InterpQuality>(k.interpQuality));
    fn(g.shadowResolutionMultiplier, k.shadow);
    fn(g.bloomMode, static_cast<BloomMode>(k.bloom));
    fn(g.depthOfFieldMode, static_cast<DepthOfFieldMode>(k.dof));
    fn(g.disableWaterRefraction, k.noWaterRefraction);
    fn(g.dynamicResolution, k.dynRes);
    fn(g.dynamicResolutionTarget, k.dynTarget);
    fn(getSettings().video.maxFrameRate, kAppliedMaxFps);
    if (k.predictive >= 0) {
        fn(g.predictiveInterpolation, k.predictive != 0);
    }
}

}  // namespace

const char* name(int profile) {
    switch (profile) {
    case kQuality:
        return "Quality";
    case kBalanced:
        return "Balanced";
    case kPerformance:
        return "Performance";
    default:
        return "Custom";
    }
}

Knobs currentKnobs() {
    const auto& g = getSettings().game;
    Knobs k{};
    k.resolution = g.internalResolutionScale.getValue();
    k.interpolation = static_cast<int>(g.enableFrameInterpolation.getValue());
    k.interpQuality = static_cast<int>(g.interpolationQuality.getValue());
    k.shadow = g.shadowResolutionMultiplier.getValue();
    k.bloom = static_cast<int>(g.bloomMode.getValue());
    k.dof = static_cast<int>(g.depthOfFieldMode.getValue());
    k.noWaterRefraction = g.disableWaterRefraction.getValue();
    k.dynRes = g.dynamicResolution.getValue();
    k.dynTarget = g.dynamicResolutionTarget.getValue();
    k.maxFps = getSettings().video.maxFrameRate.getValue();
    k.predictive = g.predictiveInterpolation.getValue() ? 1 : 0;
    k.policy = g.autoResolutionPolicy.getValue();
    return k;
}

int detectCurrent() {
    return detect(currentKnobs());
}

void applyPersistent(int profile) {
    if (profile < 0 || profile >= kCount) {
        return;
    }
    forEachKnob(profile, [](auto& var, auto target) { var.setValue(target); });
    // Profiles run with vsync on: FIFO presentation is what makes 30 and 60 even.
    if (!getSettings().video.enableVsync.getValue()) {
        getSettings().video.enableVsync.setValue(true);
        aurora_enable_vsync(true);
    }
    presentation::update_frame_rate_preference();
#ifdef __SWITCH__
    dynres::reset();
#endif
}

bool canApplyOverride(int profile, std::string& whyNot) {
    if (profile < 0 || profile >= kCount) {
        whyNot = "unknown profile";
        return false;
    }
    if (!s_clear.empty()) {
        whyNot = "a profile override is already active";
        return false;
    }
    bool ok = true;
    forEachKnob(profile, [&](auto& var, auto target) {
        if (!ok || var.getValue() == target) {
            return;  // nothing to change on this knob
        }
        if (var.getLayer() != config::ConfigVarLayer::Value) {
            ok = false;
            whyNot = std::string("'") + var.getName() +
                     "' is not at a saved value (launch argument, speedrun mode or never set); choose "
                     "a profile once in Optimized Settings first";
        }
    });
    return ok;
}

bool applyOverride(int profile, std::string& whyNot) {
    if (!canApplyOverride(profile, whyNot)) {
        return false;
    }
    forEachKnob(profile, [](auto& var, auto target) {
        if (var.getValue() == target) {
            return;
        }
        var.setOverrideValue(target);
        s_clear.emplace_back([&var] { var.clearOverride(); });
    });
#ifdef __SWITCH__
    dynres::reset();
#endif
    return true;
}

void clearOverrides() {
    if (s_clear.empty()) {
        return;  // nothing was overridden: touch nothing (a normal benchmark changes no setting)
    }
    // Reverse order, like unwinding.
    for (auto it = s_clear.rbegin(); it != s_clear.rend(); ++it) {
        (*it)();
    }
    s_clear.clear();
#ifdef __SWITCH__
    dynres::reset();
#endif
}

bool overridesActive() {
    return !s_clear.empty();
}

}  // namespace dusk::profile
