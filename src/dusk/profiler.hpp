#pragma once
// The frame profiler behind the profiler overlay and the benchmark.
//
// Code names the parts of a frame it wants timed with DUSK_PROFSCOPE("name"). Each
// name claims a slot once; every time a scope for it closes, its duration and a call
// are added to that slot. At the end of a frame the slots are folded into running
// figures and emptied, and the overlay reads whatever the figures are. It is not
// Tracy on purpose: this survives into a release build, costs two clock reads per
// scope, and answers the question that keeps coming up on this console, which part
// of the frame grew, without a host attached.
//
// Scopes can close on the render thread as well as the main one, so a slot's running
// totals are relaxed atomics: nothing is published through them, they are only
// summed. Everything else here belongs to the thread that calls beginFrame() and
// endFrame().
//
// A capture is the benchmark's view of the same data (see benchmark.cpp). The
// running figures answer "what is it doing now"; a capture keeps every frame
// interval and sums each slot over a fixed window, so the window can be reported
// afterwards. It is off outside a benchmark and costs one branch per frame and per
// slot while off.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dusk::profiler {

inline constexpr int kMaxZones = 24;

namespace detail {

using Clock = std::chrono::steady_clock;

inline double to_ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

// An exponential moving average. A window would need a ring per zone and answers a
// question nobody asks; the overlay is for watching a number move while something
// changes on screen.
class Smoothed {
public:
    explicit constexpr Smoothed(double weight) : mWeight(weight) {}
    void add(double sample) { mValue += (sample - mValue) * mWeight; }
    double value() const { return mValue; }

private:
    double mWeight;
    double mValue = 0.0;
};

// The largest recent value. It decays rather than latching, so one stutter half an
// hour ago does not sit at the top of the column for the rest of the session.
class PeakHold {
public:
    void add(double sample) { mValue = sample > mValue ? sample : mValue * kDecay; }
    double value() const { return mValue; }

private:
    static constexpr double kDecay = 0.995;
    double mValue = 0.0;
};

// The last kSize frame times, and their 95th percentile. The percentile is worked
// out on the frame thread once every kEvery frames: that thread is the one writing
// the ring, and sorting a few hundred values every frame would put the cost inside
// the frame being measured.
class FrameTimes {
public:
    void add(double ms) {
        mRing[mNext] = static_cast<float>(ms);
        mNext = (mNext + 1) % kSize;
        ++mCount;
        if (mCount < kMinFrames) {
            // Too few to rank: report the latest rather than a number made of the
            // zeroes the ring started with.
            mP95 = ms;
        } else if (mCount % kEvery == 0) {
            refresh_p95();
        }
    }
    double p95() const { return mP95; }
    std::uint64_t count() const { return mCount; }

private:
    static constexpr std::size_t kSize = 180;
    static constexpr std::uint64_t kMinFrames = 30;
    static constexpr std::uint64_t kEvery = 30;

    void refresh_p95() {
        const std::size_t filled = static_cast<std::size_t>(std::min<std::uint64_t>(mCount, kSize));
        std::array<float, kSize> scratch = mRing;
        const auto pick = scratch.begin() + static_cast<std::ptrdiff_t>(filled * 95 / 100);
        std::nth_element(scratch.begin(), pick, scratch.begin() + static_cast<std::ptrdiff_t>(filled));
        mP95 = *pick;
    }

    std::array<float, kSize> mRing{};
    std::size_t mNext = 0;
    std::uint64_t mCount = 0;
    double mP95 = 0.0;
};

struct Zone {
    const char* name = nullptr;
    // Filled by scopes during a frame, emptied by fold().
    std::atomic<std::uint64_t> nanos{0};
    std::atomic<std::uint32_t> calls{0};
    // The figures read between frames.
    Smoothed avgMs{0.06};
    PeakHold peakMs;
    Smoothed avgCalls{0.06};
};

struct ZoneSum {
    double sumMs = 0.0;
    double maxMs = 0.0;
    std::uint64_t calls = 0;
};

struct Capture {
    // Enough for two hours at 30 frames a second; past it, intervals stop being
    // kept and the overflow flag is raised.
    static constexpr std::size_t kMaxFrames = 65536;

    bool active = false;
    bool overflowed = false;
    std::uint64_t frames = 0;
    // Begin-to-begin interval of consecutive loop iterations, so everything the
    // loop spends, the frame limiter and the wait for the display included, is in
    // it. That is the frame time a player sees; the zone sums are not.
    std::vector<float> intervalsMs;
    std::array<ZoneSum, kMaxZones> zones{};
    Clock::time_point lastBegin{};
    bool haveLastBegin = false;
};

struct State {
    std::array<Zone, kMaxZones> zones{};
    // Slots are claimed from whichever thread reaches a scope first, and two
    // threads can reach two different scopes at once.
    std::atomic<int> claimed{0};
    FrameTimes frames;
    Smoothed frameAvgMs{0.05};
    Clock::time_point frameStart{};
    Capture capture;
};

// A plain inline variable, not a function-local static: every scope in the game
// reads this, and a local static would put an initialization-guard check in front of
// each of them. Everything in State is constant-initialized.
inline State g_state;
inline State& state() { return g_state; }

// Claims a slot. The index may be past the end; ScopedZone treats that as "not
// timed", so running out of slots costs the extra scopes and nothing else.
inline int claim(const char* name) {
    State& s = state();
    const int index = s.claimed.fetch_add(1, std::memory_order_relaxed);
    if (index < kMaxZones) {
        s.zones[static_cast<std::size_t>(index)].name = name;
    }
    return index;
}

inline int zone_count() { return std::min(state().claimed.load(std::memory_order_relaxed), kMaxZones); }

// Folds the frame's scopes into the running figures and, if capturing, into the
// capture, and empties the slots for the next frame.
inline void fold(double frameMs) {
    State& s = state();
    s.frameAvgMs.add(frameMs);
    s.frames.add(frameMs);

    for (int i = 0; i < zone_count(); ++i) {
        Zone& zone = s.zones[static_cast<std::size_t>(i)];
        const double ms = static_cast<double>(zone.nanos.exchange(0, std::memory_order_relaxed)) / 1e6;
        const std::uint32_t calls = zone.calls.exchange(0, std::memory_order_relaxed);
        zone.avgMs.add(ms);
        zone.peakMs.add(ms);
        zone.avgCalls.add(calls);
        if (s.capture.active) {
            ZoneSum& sum = s.capture.zones[static_cast<std::size_t>(i)];
            sum.sumMs += ms;
            sum.maxMs = std::max(sum.maxMs, ms);
            sum.calls += calls;
        }
    }
    if (s.capture.active) {
        ++s.capture.frames;
    }
}

class ScopedZone {
public:
    explicit ScopedZone(int index) noexcept : mIndex(index < kMaxZones ? index : -1), mStart(Clock::now()) {}

    ~ScopedZone() {
        if (mIndex < 0) {
            return;
        }
        const auto spent = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - mStart);
        Zone& zone = state().zones[static_cast<std::size_t>(mIndex)];
        zone.nanos.fetch_add(static_cast<std::uint64_t>(spent.count()), std::memory_order_relaxed);
        zone.calls.fetch_add(1, std::memory_order_relaxed);
    }

    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;

private:
    int mIndex;
    Clock::time_point mStart;
};

}  // namespace detail

// --- Running figures ---------------------------------------------------------
inline int numZones() { return detail::zone_count(); }
inline const char* zoneName(int i) { return detail::state().zones[static_cast<std::size_t>(i)].name; }
inline double zoneAvgMs(int i) { return detail::state().zones[static_cast<std::size_t>(i)].avgMs.value(); }
inline double zonePeakMs(int i) { return detail::state().zones[static_cast<std::size_t>(i)].peakMs.value(); }
inline double zoneAvgCalls(int i) { return detail::state().zones[static_cast<std::size_t>(i)].avgCalls.value(); }

inline std::uint64_t frameCount() { return detail::state().frames.count(); }
inline double frameAvgMs() { return detail::state().frameAvgMs.value(); }
inline double frameP95Ms() { return detail::state().frames.p95(); }

// --- Frame boundaries (main thread) -----------------------------------------
inline void beginFrame() {
    detail::State& s = detail::state();
    const auto now = detail::Clock::now();
    if (s.capture.active && s.capture.haveLastBegin) {
        if (s.capture.intervalsMs.size() < detail::Capture::kMaxFrames) {
            s.capture.intervalsMs.push_back(static_cast<float>(detail::to_ms(now - s.capture.lastBegin)));
        } else {
            s.capture.overflowed = true;
        }
    }
    s.capture.lastBegin = now;
    s.capture.haveLastBegin = true;
    s.frameStart = now;
}

inline void endFrame() {
    detail::State& s = detail::state();
    detail::fold(detail::to_ms(detail::Clock::now() - s.frameStart));
}

// --- Capture (main thread) --------------------------------------------------
// Starting discards the previous capture; stopping keeps it readable until the
// next start.
inline void captureBegin() {
    detail::Capture& c = detail::state().capture;
    c.intervalsMs.clear();
    c.intervalsMs.reserve(8192);
    c.zones.fill({});
    c.frames = 0;
    c.overflowed = false;
    // The interval that straddles the start belongs to the time before it.
    c.haveLastBegin = false;
    c.active = true;
}
inline void captureEnd() { detail::state().capture.active = false; }
inline bool captureActive() { return detail::state().capture.active; }
inline bool captureOverflowed() { return detail::state().capture.overflowed; }
inline const std::vector<float>& captureFrameIntervalsMs() { return detail::state().capture.intervalsMs; }
inline std::uint64_t captureProfiledFrames() { return detail::state().capture.frames; }
inline double captureZoneSumMs(int i) { return detail::state().capture.zones[static_cast<std::size_t>(i)].sumMs; }
inline double captureZoneMaxMs(int i) { return detail::state().capture.zones[static_cast<std::size_t>(i)].maxMs; }
inline std::uint64_t captureZoneCalls(int i) {
    return detail::state().capture.zones[static_cast<std::size_t>(i)].calls;
}

}  // namespace dusk::profiler

#ifndef DUSK_PROFILER_ENABLED
#define DUSK_PROFILER_ENABLED 1
#endif

// Two levels of macro so that __LINE__ is replaced by the line number before the
// names are pasted together; pasted directly it would be the literal token
// __LINE__ and two scopes in one block would collide.
#define DUSK_PROF_JOIN_(a, b) a##b
#define DUSK_PROF_JOIN(a, b) DUSK_PROF_JOIN_(a, b)

#if DUSK_PROFILER_ENABLED
#define DUSK_PROFSCOPE(name)                                                                       \
    static const int DUSK_PROF_JOIN(duskZone_, __LINE__) = ::dusk::profiler::detail::claim(name);  \
    const ::dusk::profiler::detail::ScopedZone DUSK_PROF_JOIN(duskScope_, __LINE__)(              \
        DUSK_PROF_JOIN(duskZone_, __LINE__))
#define DUSK_PROF_FRAME_BEGIN() ::dusk::profiler::beginFrame()
#define DUSK_PROF_FRAME_END() ::dusk::profiler::endFrame()
#else
#define DUSK_PROFSCOPE(name) ((void)0)
#define DUSK_PROF_FRAME_BEGIN() ((void)0)
#define DUSK_PROF_FRAME_END() ((void)0)
#endif
