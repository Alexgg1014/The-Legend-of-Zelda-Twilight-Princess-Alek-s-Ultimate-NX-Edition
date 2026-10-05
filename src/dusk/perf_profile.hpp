#pragma once

// H33: the three performance profiles ("Optimized Settings") as plain data, plus the logic
// that recognizes them. No game types: the settings enums are mirrored as ints, and
// perf_profile.cpp binds this to the real settings. Tested on a host
// (platforms/switch/tools/test-perf-profile.sh).
//
// A profile is not stored. What is stored is game.autoResolutionPolicy, the resolution policy
// of the last profile the user chose (resolution_policy.hpp). Changing any knob by hand makes
// the row show Custom but does not change that policy, so the docked Auto resolution of the
// chosen profile does not move under the user.
#include "dusk/resolution_policy.hpp"

namespace dusk::profile {

inline constexpr int kCustom = -1;
inline constexpr int kQuality = 0;
inline constexpr int kBalanced = 1;
inline constexpr int kPerformance = 2;
inline constexpr int kCount = 3;

// Mirrors FrameInterpMode, InterpQuality, BloomMode, DepthOfFieldMode (settings.h).
inline constexpr int kInterpOff = 0;
inline constexpr int kInterpCapped = 1;
inline constexpr int kQualityFast = 0;
inline constexpr int kQualityFancy = 1;
inline constexpr int kPostOff = 0;
inline constexpr int kPostClassic = 1;
inline constexpr int kPostDusk = 2;

struct Knobs {
    int resolution;          // internalResolutionScale
    int interpolation;       // FrameInterpMode
    int interpQuality;       // InterpQuality
    int shadow;              // shadowResolutionMultiplier
    int bloom;               // BloomMode
    int dof;                 // DepthOfFieldMode
    bool noWaterRefraction;  // disableWaterRefraction
    bool dynRes;             // dynamicResolution
    int dynTarget;           // dynamicResolutionTarget
    int maxFps;              // video.maxFrameRate, compared only when >= 0
    int predictive;          // predictiveInterpolation, compared only when >= 0
    int policy;              // autoResolutionPolicy, compared only when >= 0
};

// What choosing a profile sets. maxFps is always written (60), but only compared where the
// cap is in force.
inline constexpr Knobs kTable[kCount] = {
    // Quality: everything at its best, presented at the game's own 30 (interpolation off).
    {0, kInterpOff, kQualityFancy, 4, kPostDusk, kPostDusk, false, false, 60, -1, -1,
     respol::kPolicyQuality},
    // Balanced: Capped 60 + Fancy interpolation + predictive skipping.
    {0, kInterpCapped, kQualityFancy, 2, kPostDusk, kPostClassic, false, false, 30, 60, 1,
     respol::kPolicyBalanced},
    // Performance: Capped 60 + Fast interpolation + predictive skipping.
    {0, kInterpCapped, kQualityFast, 1, kPostClassic, kPostOff, true, false, 60, 60, 1,
     respol::kPolicyPerformance},
};

inline constexpr int kAppliedMaxFps = 60;

// The knobs before H33 (the 540p fixed Balanced/Performance, no policy, no predictive/cap
// in the comparison). Used only by the one-shot migration to recognize an old exact profile.
inline constexpr Knobs kLegacyTable[kCount] = {
    {0, kInterpOff, kQualityFancy, 4, kPostDusk, kPostDusk, false, false, 60, -1, -1, -1},
    {13, kInterpOff, kQualityFast, 2, kPostDusk, kPostClassic, false, false, 30, -1, -1, -1},
    {13, kInterpCapped, kQualityFast, 1, kPostClassic, kPostOff, true, false, 60, -1, -1, -1},
};

constexpr bool matches(const Knobs& cur, const Knobs& preset) {
    if (cur.resolution != preset.resolution || cur.interpolation != preset.interpolation ||
        cur.interpQuality != preset.interpQuality || cur.shadow != preset.shadow ||
        cur.bloom != preset.bloom || cur.dof != preset.dof ||
        cur.noWaterRefraction != preset.noWaterRefraction || cur.dynRes != preset.dynRes) {
        return false;
    }
    if (preset.dynRes && cur.dynTarget != preset.dynTarget) {
        return false;
    }
    if (preset.maxFps >= 0 && cur.maxFps != preset.maxFps) {
        return false;
    }
    if (preset.predictive >= 0 && cur.predictive != preset.predictive) {
        return false;
    }
    if (preset.policy >= 0 && cur.policy != preset.policy) {
        return false;
    }
    return true;
}

// Which profile the current knobs are, or kCustom. For the row's label only: it does not
// decide the resolution.
constexpr int detect(const Knobs& cur) {
    for (int i = 0; i < kCount; ++i) {
        if (matches(cur, kTable[i])) {
            return i;
        }
    }
    return kCustom;
}

// Migration 4: the profile an old config matched exactly, or kCustom. `cur.policy` is ignored.
constexpr int detect_legacy(const Knobs& cur) {
    for (int i = 0; i < kCount; ++i) {
        if (matches(cur, kLegacyTable[i])) {
            return i;
        }
    }
    return kCustom;
}

constexpr int policy_for(int profile) {
    return (profile >= 0 && profile < kCount) ? kTable[profile].policy : respol::kPolicyLegacy;
}

}  // namespace dusk::profile
