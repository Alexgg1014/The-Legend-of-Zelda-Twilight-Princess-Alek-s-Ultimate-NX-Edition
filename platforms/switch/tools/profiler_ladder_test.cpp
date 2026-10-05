// Host test for two small header-only pieces that were rewritten in H32:
//   src/dusk/resolution_ladder.hpp   the named internal resolutions
//   src/dusk/profiler.hpp            the frame profiler behind the overlay and the benchmark
//   platforms/switch/tools/test-profiler-ladder.sh
#include "dusk/profiler.hpp"
#include "dusk/resolution_ladder.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>

static int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static void test_ladder() {
    using namespace dusk::ladder;
    // Values 13..18 are exact heights; everything else is not.
    CHECK(exact_height(12) == 0);
    CHECK(exact_height(13) == 540);
    CHECK(exact_height(14) == 720);
    CHECK(exact_height(15) == 900);
    CHECK(exact_height(16) == 1080);
    CHECK(exact_height(17) == 1440);
    CHECK(exact_height(18) == 2160);
    CHECK(exact_height(19) == 0);
    CHECK(exact_height(-1) == 0);
    CHECK(exact_height(0) == 0);
    CHECK(kFirstExact == 13 && kLastValue == 18);
    // And back: a height finds its value; a height that is not on the ladder finds none.
    for (int v = kFirstExact; v <= kLastValue; ++v) {
        CHECK(exact_value(exact_height(v)) == v);
    }
    CHECK(exact_value(0) == 0);
    CHECK(exact_value(480) == 0);
    CHECK(exact_value(721) == 0);
    static_assert(exact_height(14) == 720 && exact_value(1080) == 16);
}

static void test_profiler() {
    namespace p = dusk::profiler;
    const int before = p::numZones();
    for (int frame = 0; frame < 3; ++frame) {
        DUSK_PROF_FRAME_BEGIN();
        {
            DUSK_PROFSCOPE("test.a");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        {
            DUSK_PROFSCOPE("test.b");
            DUSK_PROFSCOPE("test.c");
        }
        DUSK_PROF_FRAME_END();
    }
    // Each scope site claims a slot once, however many frames run through it.
    CHECK(p::numZones() == before + 3);
    CHECK(p::frameCount() == 3);
    CHECK(p::frameAvgMs() > 0.0);
    // The slot that slept shows it, and its call count is one per frame.
    int a = -1;
    for (int i = 0; i < p::numZones(); ++i) {
        if (std::string_view(p::zoneName(i)) == "test.a") {
            a = i;
        }
    }
    CHECK(a >= 0);
    CHECK(p::zoneAvgMs(a) > 0.0 && p::zonePeakMs(a) >= 2.0);
    CHECK(p::zoneAvgCalls(a) > 0.0);

    // A capture records frame-to-frame intervals and per-zone sums over its window, and
    // not before it starts.
    p::captureBegin();
    CHECK(p::captureActive() && p::captureFrameIntervalsMs().empty());
    for (int frame = 0; frame < 5; ++frame) {
        DUSK_PROF_FRAME_BEGIN();
        {
            DUSK_PROFSCOPE("test.a");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        DUSK_PROF_FRAME_END();
    }
    p::captureEnd();
    CHECK(!p::captureActive());
    // The interval that straddles the start is dropped: five frames, four intervals.
    CHECK(p::captureFrameIntervalsMs().size() == 4);
    CHECK(p::captureProfiledFrames() == 5);
    // Every scope site claims a slot of its own, so the figures for a name are the sum
    // over the slots that carry it (the benchmark report does the same).
    double sumMs = 0.0;
    uint64_t calls = 0;
    for (int i = 0; i < p::numZones(); ++i) {
        if (std::string_view(p::zoneName(i)) == "test.a") {
            sumMs += p::captureZoneSumMs(i);
            calls += p::captureZoneCalls(i);
        }
    }
    CHECK(sumMs >= 5 * 2.0 * 0.9);
    CHECK(calls == 5);
    CHECK(!p::captureOverflowed());
    // After a stop the capture stays readable; a new one starts empty.
    p::captureBegin();
    CHECK(p::captureProfiledFrames() == 0 && p::captureFrameIntervalsMs().empty());
    p::captureEnd();

    // The 95th percentile needs enough frames; before that it follows the last frame.
    for (int frame = 0; frame < 100; ++frame) {
        DUSK_PROF_FRAME_BEGIN();
        DUSK_PROF_FRAME_END();
    }
    CHECK(p::frameP95Ms() >= 0.0);
}

int main() {
    test_ladder();
    test_profiler();
    if (failures == 0) {
        std::cout << "profiler_ladder: all checks passed\n";
        return 0;
    }
    std::cerr << failures << " check(s) failed\n";
    return 1;
}
