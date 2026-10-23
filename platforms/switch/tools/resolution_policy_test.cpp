// Host test for src/dusk/resolution_policy.hpp (H33).
//   platforms/switch/tools/test-resolution-policy.sh
#include "dusk/resolution_policy.hpp"

#include <iostream>

static int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

using namespace dusk::respol;

static constexpr Mode kModes[] = {Mode::Handheld, Mode::Flip, Mode::Docked};

static void test_policy_tables() {
    // Legacy is what Auto meant before H33.
    CHECK(policy_rows(kPolicyLegacy, Mode::Docked) == 1080);
    CHECK(policy_rows(kPolicyLegacy, Mode::Handheld) == 720);
    CHECK(policy_rows(kPolicyLegacy, Mode::Flip) == 540);
    CHECK(policy_rows(kPolicyQuality, Mode::Docked) == 900);
    CHECK(policy_rows(kPolicyQuality, Mode::Handheld) == 720);
    CHECK(policy_rows(kPolicyQuality, Mode::Flip) == 540);
    // post-rc12: Balanced handheld is 720 rows with deko3d only; Flip, docked and Performance unchanged.
    CHECK(policy_rows(kPolicyBalanced, Mode::Handheld, true) == 720);
    CHECK(policy_rows(kPolicyBalanced, Mode::Handheld, false) == 540);
    CHECK(policy_rows(kPolicyBalanced, Mode::Flip, true) == 540);
    CHECK(policy_rows(kPolicyBalanced, Mode::Docked, true) == 720);
    CHECK(policy_rows(kPolicyPerformance, Mode::Handheld, true) == 540);
    CHECK(resolve(0, kPolicyBalanced, Mode::Handheld, true).rows == 720);
    CHECK(resolve(0, kPolicyBalanced, Mode::Handheld).rows == 540);
    for (const int p : {kPolicyBalanced, kPolicyPerformance}) {
        CHECK(policy_rows(p, Mode::Docked) == 720);
        CHECK(policy_rows(p, Mode::Handheld) == 540);
        CHECK(policy_rows(p, Mode::Flip) == 540);
    }
    // Anything else is Legacy.
    for (const int bad : {-1, 4, 99, -1000, 1 << 20}) {
        CHECK(sanitize_policy(bad) == kPolicyLegacy);
        CHECK(policy_rows(bad, Mode::Docked) == 1080);
    }
    // Every policy row count is itself an exact ladder height and within the mode limit.
    for (int p = 0; p <= 3; ++p) {
        for (const Mode m : kModes) {
            CHECK(dusk::ladder::exact_value(policy_rows(p, m)) != 0);
            CHECK(policy_rows(p, m) <= max_rows(m));
        }
    }
}

static void test_classification() {
    CHECK(classify(0) == StoredClass::Auto);
    CHECK(classify(1) == StoredClass::Valid);   // 528
    CHECK(classify(2) == StoredClass::Valid);   // 1056, valid on the dock
    for (int v = 3; v <= 12; ++v) {
        CHECK(classify(v) == StoredClass::UnsafeSwitch);
    }
    CHECK(classify(13) == StoredClass::Valid);
    CHECK(classify(14) == StoredClass::Valid);
    CHECK(classify(15) == StoredClass::Valid);
    CHECK(classify(16) == StoredClass::Valid);
    CHECK(classify(17) == StoredClass::UnsafeSwitch);
    CHECK(classify(18) == StoredClass::UnsafeSwitch);
    for (const int v : {-1, 19, 99, -100000, 1 << 30}) {
        CHECK(classify(v) == StoredClass::Corrupt);
    }
}

static void test_never_exceeds_cap() {
    // Exhaustive over a wide range, including the values that crashed or could be corrupt (12, 99).
    for (int stored = -200; stored <= 200; ++stored) {
        for (int policy = -2; policy <= 6; ++policy) {
            for (const Mode mode : kModes) {
                const Resolved r = resolve(stored, policy, mode);
                CHECK(r.rows <= max_rows(mode));
                CHECK(r.rows >= 1);
                // What is handed to the renderer renders exactly those rows.
                CHECK(r.rows == stored_rows(r.value));
                CHECK(classify(r.value) == StoredClass::Valid);
            }
        }
    }
    for (const int stored : {12, 99, 1 << 30, -(1 << 30)}) {
        for (const Mode mode : kModes) {
            const Resolved r = resolve(stored, kPolicyQuality, mode);
            CHECK(r.rows <= max_rows(mode));
            CHECK(r.repaired);
        }
    }
}

static void test_precedence() {
    // Manual wins over Auto, whatever the policy.
    CHECK(resolve(15, kPolicyPerformance, Mode::Docked).rows == 900);
    CHECK(resolve(13, kPolicyQuality, Mode::Docked).rows == 540);
    // Auto uses the policy.
    CHECK(resolve(0, kPolicyQuality, Mode::Docked).rows == 900);
    CHECK(resolve(0, kPolicyLegacy, Mode::Docked).rows == 1080);
    // Auto is not "repaired", a corrupt value is.
    CHECK(!resolve(0, kPolicyQuality, Mode::Docked).repaired);
    CHECK(resolve(99, kPolicyQuality, Mode::Docked).repaired);
    CHECK(resolve(99, kPolicyQuality, Mode::Docked).rows == 900);
}

static void test_runtime_clamp_only() {
    // 1080p stored: valid docked, limited in handheld and Flip without being rewritten.
    Resolved docked = resolve(16, kPolicyLegacy, Mode::Docked);
    CHECK(docked.rows == 1080 && !docked.clamped && docked.value == 16);
    for (const Mode m : {Mode::Handheld, Mode::Flip}) {
        Resolved r = resolve(16, kPolicyLegacy, m);
        CHECK(r.rows == 720 && r.clamped && r.value == 14);
    }
    // 900p likewise.
    CHECK(resolve(15, 0, Mode::Handheld).rows == 720 && resolve(15, 0, Mode::Handheld).clamped);
    CHECK(!resolve(15, 0, Mode::Docked).clamped);
    // 2x (1056 rows) is valid on the dock only.
    CHECK(!resolve(2, 0, Mode::Docked).clamped && resolve(2, 0, Mode::Docked).value == 2);
    CHECK(resolve(2, 0, Mode::Handheld).clamped && resolve(2, 0, Mode::Handheld).rows == 720);
    // 540p never clamps.
    for (const Mode m : kModes) {
        CHECK(!resolve(13, 0, m).clamped);
    }
}

static void test_carousel() {
    CHECK(kCarouselCount == 5);
    CHECK(carousel_index(0) == 0 && carousel_index(13) == 1 && carousel_index(14) == 2);
    CHECK(carousel_index(15) == 3 && carousel_index(16) == 4);
    // Old factors and unsafe values are not offered.
    for (const int v : {1, 2, 3, 12, 17, 18, 99, -1}) {
        CHECK(carousel_index(v) == -1);
    }
    // 540p -> left is Auto, never a factor (the step that gave x12 before H33).
    CHECK(kCarouselValues[carousel_index(13) - 1] == 0);
}

int main() {
    test_policy_tables();
    test_classification();
    test_never_exceeds_cap();
    test_precedence();
    test_runtime_clamp_only();
    test_carousel();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "resolution_policy: all checks passed\n";
    return 0;
}
