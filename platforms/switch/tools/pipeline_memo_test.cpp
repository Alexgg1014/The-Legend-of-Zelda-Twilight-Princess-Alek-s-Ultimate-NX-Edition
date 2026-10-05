// Host test for extern/aurora/lib/gx/lookup_memo.hpp (the H31 pipeline memo).
//   platforms/switch/tools/test-pipeline-memo.sh
#include "gx/lookup_memo.hpp"

#include <cstdio>
#include <iostream>
#include <utility>

using namespace aurora::gx;

static int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

struct Config {  // no padding: 4 + 4 + 8 + 8
    uint32_t version;
    uint32_t a;
    uint64_t b;
    uint64_t c;
};
struct Info {
    uint32_t x = 0;
    uint32_t y = 0;
    bool operator==(const Info&) const = default;
};
using Ref = uint64_t;
using SmallMemo = LookupMemo<Config, Ref, Info, 4, 2>;  // 8 entries, 2 ways per set

constexpr uint32_t kGx = 1;
constexpr uint32_t kOther = 2;

static Config cfg(uint32_t a, uint32_t version = 13) { return {version, a, a * 3ull, a * 7ull}; }

// Counts how often the full lookup runs and hands back deterministic results.
struct Resolver {
    int calls = 0;
    void run(const Config& c, Ref& r, Info& i) {
        ++calls;
        r = 1000 + c.a;
        i = {c.a, c.a + 1};
    }
};

template <class Memo>
static void resolve(Memo& m, bool enabled, const Config& c, Resolver& res, Ref& r, Info& i, uint64_t hash,
                    uint64_t layout = 7, uint32_t generation = 1, uint32_t kind = kGx) {
    memo_resolve(m, enabled, kind, layout, generation, c.version, c, [&] { return hash; }, r, i,
                 [&] { res.run(c, r, i); });
}

static void test_hit_returns_original() {
    SmallMemo m;
    Resolver res;
    Ref r1 = 0, r2 = 0;
    Info i1{}, i2{};
    const Config c = cfg(5);
    resolve(m, true, c, res, r1, i1, 100);
    CHECK(res.calls == 1);
    CHECK(r1 == 1005 && (i1 == Info{5, 6}));
    resolve(m, true, c, res, r2, i2, 100);
    CHECK(res.calls == 1);  // replay, no second full lookup
    CHECK(r2 == r1 && i2 == i1);
    const auto s = m.stats();
    CHECK(s.hits == 1 && s.misses == 1 && s.occupied == 1 && s.maxOccupied == 1);
}

static void test_collision_is_a_miss() {
    SmallMemo m;
    Resolver res;
    Ref r = 0;
    Info i{};
    resolve(m, true, cfg(1), res, r, i, 42);
    CHECK(r == 1001);
    // Different configuration, SAME hash: must not return the first result.
    resolve(m, true, cfg(2), res, r, i, 42);
    CHECK(res.calls == 2);
    CHECK(r == 1002 && (i == Info{2, 3}));
    // Both are now present and each returns its own.
    resolve(m, true, cfg(1), res, r, i, 42);
    CHECK(res.calls == 2 && r == 1001);
    resolve(m, true, cfg(2), res, r, i, 42);
    CHECK(res.calls == 2 && r == 1002);
}

static void test_layout_kind_version_miss() {
    // Wide sets, so the four variants below (same hash) all stay resident and a
    // later hit proves the others did not overwrite the original.
    LookupMemo<Config, Ref, Info, 4, 8> m;
    Resolver res;
    Ref r = 0;
    Info i{};
    resolve(m, true, cfg(1), res, r, i, 9, /*layout*/ 7);
    resolve(m, true, cfg(1), res, r, i, 9, /*layout*/ 8);  // other target layout
    CHECK(res.calls == 2);
    resolve(m, true, cfg(1), res, r, i, 9, 7, 1, kOther);  // other shader type
    CHECK(res.calls == 3);
    resolve(m, true, cfg(1, 14), res, r, i, 9, 7);  // other config version
    CHECK(res.calls == 4);
    resolve(m, true, cfg(1), res, r, i, 9, 7);  // the original still hits
    CHECK(res.calls == 4);
}

static void test_generation_invalidates() {
    SmallMemo m;
    Resolver res;
    Ref r = 0;
    Info i{};
    resolve(m, true, cfg(1), res, r, i, 3, 7, /*gen*/ 1);
    resolve(m, true, cfg(1), res, r, i, 3, 7, 1);
    CHECK(res.calls == 1);
    resolve(m, true, cfg(1), res, r, i, 3, 7, /*gen*/ 2);  // pipeline tables were rebuilt
    CHECK(res.calls == 2);
    CHECK(m.stats().occupied == 1);  // emptied, then refilled
    resolve(m, true, cfg(1), res, r, i, 3, 7, 2);
    CHECK(res.calls == 2);
    // Going back to the old generation is also a change: nothing from before survives.
    resolve(m, true, cfg(1), res, r, i, 3, 7, 1);
    CHECK(res.calls == 3);
}

static void test_disabled_uses_normal_path() {
    SmallMemo m;
    Resolver res;
    Ref r = 0;
    Info i{};
    int hashCalls = 0;
    const Config c = cfg(4);
    for (int n = 0; n < 5; ++n) {
        memo_resolve(
            m, false, kGx, 7, 1, c.version, c,
            [&] {
                ++hashCalls;
                return 1ull;
            },
            r, i, [&] { res.run(c, r, i); });
    }
    CHECK(res.calls == 5);  // every call takes the full lookup
    CHECK(hashCalls == 0);  // and the hash is never even computed
    const auto s = m.stats();
    CHECK(s.hits == 0 && s.misses == 0 && s.occupied == 0 && s.maxOccupied == 0);
    // Turning it on afterwards starts from empty.
    resolve(m, true, c, res, r, i, 1);
    CHECK(res.calls == 6);
    resolve(m, true, c, res, r, i, 1);
    CHECK(res.calls == 6);
}

static void test_capacity_and_eviction() {
    SmallMemo m;  // 4 sets x 2 ways
    Resolver res;
    Ref r = 0;
    Info i{};
    // hash & 3 == 0 for all three: they compete for the same two ways.
    resolve(m, true, cfg(1), res, r, i, 0);
    resolve(m, true, cfg(2), res, r, i, 4);
    CHECK(m.stats().occupied == 2 && m.stats().evictions == 0);
    resolve(m, true, cfg(1), res, r, i, 0);  // touch 1: now 2 is the least recently used
    CHECK(res.calls == 2);
    resolve(m, true, cfg(3), res, r, i, 8);  // evicts 2, not 1
    CHECK(m.stats().evictions == 1 && m.stats().occupied == 2);
    resolve(m, true, cfg(1), res, r, i, 0);
    CHECK(res.calls == 3);  // 1 survived
    resolve(m, true, cfg(2), res, r, i, 4);
    CHECK(res.calls == 4);  // 2 was evicted: full lookup again
    // Fill every set: occupancy never exceeds capacity.
    for (uint32_t k = 0; k < 40; ++k) {
        resolve(m, true, cfg(100 + k), res, r, i, k);
    }
    CHECK(m.stats().occupied == SmallMemo::kCapacity);
    CHECK(m.stats().maxOccupied == SmallMemo::kCapacity);
    CHECK(m.stats().capacity == 8);
}

static void test_deterministic_replacement() {
    // The same call sequence evicts the same entries, run after run.
    auto run = [] {
        SmallMemo m;
        Resolver res;
        Ref r = 0;
        Info i{};
        for (uint32_t k = 0; k < 30; ++k) {
            resolve(m, true, cfg(k % 11), res, r, i, k % 7);
        }
        return std::pair<int, uint64_t>{res.calls, m.stats().evictions};
    };
    CHECK(run() == run());
}

static void test_stats_and_clear() {
    SmallMemo m;
    Resolver res;
    Ref r = 0;
    Info i{};
    resolve(m, true, cfg(1), res, r, i, 1);
    resolve(m, true, cfg(1), res, r, i, 1);
    resolve(m, true, cfg(1), res, r, i, 1);
    auto s = m.stats();
    CHECK(s.hits == 2 && s.misses == 1);
    m.clear();
    s = m.stats();
    CHECK(s.occupied == 0 && s.hits == 2 && s.maxOccupied == 1);  // counters are cumulative
}

static void test_production_size() {
    // The shape the draw path uses: 1024 entries of a 2772-byte config.
    struct Big {
        uint32_t version;
        uint8_t bytes[2768];
    };
    static_assert(sizeof(Big) == 2772);
    using BigMemo = LookupMemo<Big, uint64_t, Info, 256, 4>;
    BigMemo m;
    CHECK(BigMemo::kCapacity == 1024);
    CHECK(m.stats().capacity == 1024);
}

int main() {
    test_hit_returns_original();
    test_collision_is_a_miss();
    test_layout_kind_version_miss();
    test_generation_invalidates();
    test_disabled_uses_normal_path();
    test_capacity_and_eviction();
    test_deterministic_replacement();
    test_stats_and_clear();
    test_production_size();
    if (failures == 0) {
        std::cout << "pipeline_memo: all checks passed\n";
        return 0;
    }
    std::cerr << failures << " check(s) failed\n";
    return 1;
}
