#pragma once
// aleks2 (H34-c2): the order in which a diagnostic run (Run Benchmark, Run Profile Matrix,
// Prepare Shaders, and since H35 Run Performance Tour) starts and ends. Kept apart from
// benchmark.cpp, with the steps passed in, so the order itself is what the host test checks
// (platforms/switch/tools/test-diagnostic-run.sh).
//
// Start, before the first warp:
//   1. block the game's saves (the memory-card store does nothing from here on);
//   2. suspend the achievement system (no evaluation, no progress, no achievements.json,
//      signals dropped as they arrive);
//   3. close the UI documents -- closing Settings runs its own synchronous config::save();
//   4. only then read config::save_count(): the baseline the run's report compares
//      against, so that save is not blamed on the run.
// End, once the player is back where they were (every outcome ends here):
//   1. re-enable the game's saves;
//   2. finish the run (report, summary);
//   3. resume the achievement system, last.
#include <atomic>
#include <cstdint>
#include <functional>
#include <utility>

namespace dusk::diagnostic_run {

struct StartSteps {
    std::function<void()> blockGameSaves;
    std::function<void()> suspendAchievements;
    std::function<void()> closeUi;
    std::function<std::uint64_t()> configSaveCount;
};

/// Runs the start steps in order and returns the config save baseline.
inline std::uint64_t begin(const StartSteps& steps) {
    steps.blockGameSaves();
    steps.suspendAchievements();
    steps.closeUi();
    return steps.configSaveCount();
}

struct EndSteps {
    std::function<void()> reenableGameSaves;
    std::function<void()> finish;
    std::function<void()> resumeAchievements;
};

inline void end(const EndSteps& steps) {
    steps.reenableGameSaves();
    steps.finish();
    steps.resumeAchievements();
}

/// H35: holds a run's "saves blocked" flag. Set when constructed on a flag, cleared by
/// release() -- which end() reaches through EndSteps::reenableGameSaves -- or, if the run is
/// destroyed without ending (replaced, torn down), by the destructor, so saving can never
/// stay disabled after the run object is gone. Move only.
class ScopedFlag {
public:
    ScopedFlag() = default;
    explicit ScopedFlag(std::atomic<bool>& flag) : mFlag(&flag) { flag.store(true, std::memory_order_release); }
    ~ScopedFlag() { release(); }
    ScopedFlag(ScopedFlag&& other) noexcept : mFlag(std::exchange(other.mFlag, nullptr)) {}
    ScopedFlag& operator=(ScopedFlag&& other) noexcept {
        if (this != &other) {
            release();
            mFlag = std::exchange(other.mFlag, nullptr);
        }
        return *this;
    }
    ScopedFlag(const ScopedFlag&) = delete;
    ScopedFlag& operator=(const ScopedFlag&) = delete;

    void release() {
        if (mFlag != nullptr) {
            mFlag->store(false, std::memory_order_release);
            mFlag = nullptr;
        }
    }
    bool held() const { return mFlag != nullptr; }

private:
    std::atomic<bool>* mFlag = nullptr;
};

/// config::save() calls since the baseline (0 if the counter somehow went back).
inline std::uint64_t config_writes_since(std::uint64_t baseline, std::uint64_t now) {
    return now >= baseline ? now - baseline : 0;
}

}  // namespace dusk::diagnostic_run
