#pragma once

// H33: the single place that decides which internal resolution is applied.
//
// Every route that changes the framebuffer height (startup, the setting's callback, a
// performance profile, dock/undock, dynamic resolution) ends in applyInternalResolutionScale,
// which asks resolve() here. Standard library plus resolution_ladder.hpp only, so it is
// tested on a host (platforms/switch/tools/test-resolution-policy.sh).
//
// Order of decisions:
//   1. A manual resolution (setting != 0) wins over Auto.
//   2. Auto uses the base policy remembered by the last chosen performance profile
//      (game.autoResolutionPolicy); Legacy is what Auto meant before H33.
//   3. The result never exceeds the console's limit for the current mode: 1280x720 handheld
//      (Flip Grip included), 1920x1080 docked. Aurora clamps again as a second barrier.
//
// A stored value that is valid on the dock but above the handheld limit is only limited at
// run time; it is not rewritten, so docking again brings it back.
#include "dusk/resolution_ladder.hpp"

namespace dusk::respol {

enum class Mode { Handheld, Flip, Docked };

// game.autoResolutionPolicy
inline constexpr int kPolicyLegacy = 0;
inline constexpr int kPolicyQuality = 1;
inline constexpr int kPolicyBalanced = 2;
inline constexpr int kPolicyPerformance = 3;

inline constexpr int kBaseRows = 528;  // 1x: the GameCube frame buffer
inline constexpr int kMaxRowsHandheld = 720;
inline constexpr int kMaxRowsDocked = 1080;
inline constexpr int kMaxValue = ladder::kLastValue;

// Values offered by the Switch carousel, in order. Index 0 is Auto.
inline constexpr int kCarouselValues[] = {0, ladder::kFirstExact + 0, ladder::kFirstExact + 1,
                                          ladder::kFirstExact + 2, ladder::kFirstExact + 3};
inline constexpr int kCarouselCount = static_cast<int>(sizeof(kCarouselValues) / sizeof(kCarouselValues[0]));

// post-rc12: whether the renderer in use is deko3d (switch_display.cpp sets it every frame from aurora_get_backend()).
// The default of policy_rows/resolve below, so every caller follows the running backend; the host tests pass it.
inline bool g_deko3dRenderer = false;

constexpr int sanitize_policy(int policy) {
    return (policy >= kPolicyLegacy && policy <= kPolicyPerformance) ? policy : kPolicyLegacy;
}

constexpr int max_rows(Mode mode) {
    return mode == Mode::Docked ? kMaxRowsDocked : kMaxRowsHandheld;
}

// Rows Auto stands for. Legacy is the pre-H33 behavior.
// post-rc12: `deko3d` -- the renderer is deko3d. Balanced in handheld is then 720 rows: the rc12 tour (Quality, 720
// rows handheld, deko3d) measured a GPU frame of 17.1-19.0 ms p50 and 20.5 ms p99 with heavier effects than Balanced's
// (shadows x4, DoF Dusk), so one interpolated frame fits under the Capped 60 floor (45 FPS, 22 ms). With Dawn/NVK there
// is no such measurement and its render worker costs 3-5x as much per draw: 540 as before. Flip and docked unchanged.
constexpr int policy_rows(int policy, Mode mode, bool deko3d = g_deko3dRenderer) {
    switch (sanitize_policy(policy)) {
    case kPolicyQuality:
        return mode == Mode::Docked ? 900 : (mode == Mode::Flip ? 540 : 720);
    case kPolicyBalanced:
        return mode == Mode::Docked ? 720 : (mode == Mode::Handheld && deko3d ? 720 : 540);
    case kPolicyPerformance:
        return mode == Mode::Docked ? 720 : 540;
    default:
        return mode == Mode::Docked ? 1080 : (mode == Mode::Flip ? 540 : 720);
    }
}

// Rows a stored setting value stands for. 0 for Auto and for values outside the domain.
constexpr int stored_rows(int value) {
    if (value >= 1 && value < ladder::kFirstExact) {
        return value * kBaseRows;
    }
    return ladder::exact_height(value);
}

enum class StoredClass {
    Auto,          // 0
    Valid,         // inside the domain and within the largest mode limit (1080 rows)
    Corrupt,       // outside 0..kMaxValue
    UnsafeSwitch,  // inside the domain but above 1080 rows: never legal on the console
};

constexpr StoredClass classify(int value) {
    if (value < 0 || value > kMaxValue) {
        return StoredClass::Corrupt;
    }
    if (value == 0) {
        return StoredClass::Auto;
    }
    return stored_rows(value) > kMaxRowsDocked ? StoredClass::UnsafeSwitch : StoredClass::Valid;
}

// The largest exact ladder value whose height does not exceed `rows`.
constexpr int largest_exact_within(int rows) {
    int best = 0;
    for (int v = ladder::kFirstExact; v <= ladder::kLastValue; ++v) {
        if (ladder::exact_height(v) <= rows) {
            best = v;
        }
    }
    return best;
}

struct Resolved {
    int value;     // what applyInternalResolutionScale hands to the renderer
    int rows;      // the picture height that value renders
    bool clamped;  // the stored selection was limited by the mode
    bool repaired; // the stored value was corrupt or unsafe and was treated as Auto
};

constexpr Resolved resolve(int stored, int policy, Mode mode, bool deko3d = g_deko3dRenderer) {
    const StoredClass cls = classify(stored);
    const int cap = max_rows(mode);
    if (cls == StoredClass::Auto || cls == StoredClass::Corrupt || cls == StoredClass::UnsafeSwitch) {
        int rows = policy_rows(policy, mode, deko3d);
        if (rows > cap) {
            rows = cap;
        }
        return {ladder::exact_value(rows), rows, false, cls != StoredClass::Auto};
    }
    const int rows = stored_rows(stored);
    if (rows <= cap) {
        return {stored, rows, false, false};
    }
    const int value = largest_exact_within(cap);
    return {value, ladder::exact_height(value), true, false};
}

// Carousel position of a stored value, or -1 when it is not one of the offered ones
// (an old factor such as 1x or 2x, which still loads but is not offered).
constexpr int carousel_index(int stored) {
    for (int i = 0; i < kCarouselCount; ++i) {
        if (kCarouselValues[i] == stored) {
            return i;
        }
    }
    return -1;
}

}  // namespace dusk::respol
