// Host test for src/dusk/perf_profile.hpp (H33).
//   platforms/switch/tools/test-perf-profile.sh
#include "dusk/perf_profile.hpp"

#include <iostream>

static int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

using namespace dusk::profile;
namespace respol = dusk::respol;

static Knobs applied(int profile) {
    Knobs k = kTable[profile];
    k.maxFps = kAppliedMaxFps;  // choosing a profile always writes the cap
    k.predictive = kTable[profile].predictive >= 0 ? kTable[profile].predictive : 1;
    return k;
}

static void test_table() {
    // Quality: flat 30, no interpolation.
    CHECK(kTable[kQuality].interpolation == kInterpOff);
    // Balanced: Capped 60 + Fancy + predictive. Performance: Capped 60 + Fast + predictive.
    CHECK(kTable[kBalanced].interpolation == kInterpCapped && kTable[kBalanced].interpQuality == kQualityFancy);
    CHECK(kTable[kPerformance].interpolation == kInterpCapped && kTable[kPerformance].interpQuality == kQualityFast);
    CHECK(kTable[kBalanced].predictive == 1 && kTable[kPerformance].predictive == 1);
    CHECK(kTable[kBalanced].maxFps == 60 && kTable[kPerformance].maxFps == 60);
    // No profile locks a cap that is not 60 (a divisor-free cap such as 45 judders on 60 Hz).
    for (int i = 0; i < kCount; ++i) {
        CHECK(kTable[i].maxFps == -1 || kTable[i].maxFps == 60);
        CHECK(kTable[i].resolution == 0);  // profiles use Auto; the policy gives the rows
        CHECK(!kTable[i].dynRes);          // dynamic resolution leaks device memory
    }
    // Each profile carries its own policy.
    CHECK(policy_for(kQuality) == respol::kPolicyQuality);
    CHECK(policy_for(kBalanced) == respol::kPolicyBalanced);
    CHECK(policy_for(kPerformance) == respol::kPolicyPerformance);
    CHECK(policy_for(kCustom) == respol::kPolicyLegacy);
}

static void test_detect() {
    for (int i = 0; i < kCount; ++i) {
        CHECK(detect(applied(i)) == i);
    }
    // A hand change shows Custom...
    Knobs k = applied(kBalanced);
    k.bloom = kPostOff;
    CHECK(detect(k) == kCustom);
    // ...and does not change the policy, which lives in its own setting.
    CHECK(k.policy == respol::kPolicyBalanced);
    // The same knobs with the Legacy policy are Custom until a profile is chosen explicitly.
    Knobs legacy = applied(kQuality);
    legacy.policy = respol::kPolicyLegacy;
    CHECK(detect(legacy) == kCustom);
    // A manual resolution replaces Auto and shows Custom.
    Knobs manual = applied(kPerformance);
    manual.resolution = 15;
    CHECK(detect(manual) == kCustom);
    // Cap and predictive only matter where they are part of the profile.
    Knobs cap = applied(kPerformance);
    cap.maxFps = 45;
    CHECK(detect(cap) == kCustom);
    Knobs q = applied(kQuality);
    q.maxFps = 30;
    q.predictive = 0;
    CHECK(detect(q) == kQuality);
}

static void test_legacy_migration() {
    // Old Quality: Auto + everything at its best. No policy field existed.
    Knobs oldQuality = {0, kInterpOff, kQualityFancy, 4, kPostDusk, kPostDusk, false, false, 60, 60, 1, 0};
    CHECK(detect_legacy(oldQuality) == kQuality);
    Knobs oldBalanced = {13, kInterpOff, kQualityFast, 2, kPostDusk, kPostClassic, false, false, 30, 60, 1, 0};
    CHECK(detect_legacy(oldBalanced) == kBalanced);
    Knobs oldPerformance = {13, kInterpCapped, kQualityFast, 1, kPostClassic, kPostOff, true, false, 60, 60, 1, 0};
    CHECK(detect_legacy(oldPerformance) == kPerformance);
    // The three are told apart; the old Balanced is not the new one.
    CHECK(detect(oldBalanced) == kCustom);
    // Anything that differs, however slightly, is Custom and stays Legacy.
    for (const Knobs base : {oldQuality, oldBalanced, oldPerformance}) {
        Knobs a = base; a.shadow += 1;
        Knobs b = base; b.bloom = (base.bloom + 1) % 3;
        Knobs c = base; c.dof = (base.dof + 1) % 3;
        Knobs d = base; d.noWaterRefraction = !base.noWaterRefraction;
        Knobs e = base; e.resolution = (base.resolution == 0) ? 14 : 0;
        Knobs f = base; f.interpQuality = 1 - base.interpQuality;
        Knobs g = base; g.interpolation = (base.interpolation == 0) ? 2 : 0;
        Knobs h = base; h.dynRes = true;
        for (const Knobs& x : {a, b, c, d, e, f, g, h}) {
            CHECK(detect_legacy(x) == kCustom);
        }
    }
    // A custom Auto config that matches nothing stays Custom.
    Knobs custom = oldQuality;
    custom.shadow = 3;
    CHECK(detect_legacy(custom) == kCustom);
}

int main() {
    test_table();
    test_detect();
    test_legacy_migration();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "perf_profile: all checks passed\n";
    return 0;
}
