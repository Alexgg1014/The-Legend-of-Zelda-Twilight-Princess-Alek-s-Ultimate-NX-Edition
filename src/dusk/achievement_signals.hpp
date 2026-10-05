#pragma once
// aleks2 (H34-c2): the per-tick signals of Dusklight's own AchievementSystem, and the
// diagnostic suspension. Header-only and free of game headers so the host test
// (platforms/switch/tools/test-diagnostic-run.sh) runs the same code the game does.
//
// While suspended (a benchmark, profile matrix or Prepare Shaders run is warping the player
// around and putting the game in states it would not otherwise be in):
//   - signal() drops the signal at once; nothing accumulates for later;
//   - run_tick() evaluates nothing, changes no progress and never saves achievements.json;
//   - suspensions nest: only the last resume() lifts it, and it clears whatever is left.
// Outside a suspension nothing changes: run_tick() behaves exactly as
// AchievementSystem::tick() did before (including keeping signals while the game has not
// launched yet).
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace dusk {

class AchievementSignals {
public:
    void signal(std::string_view key) {
        if (mSuspendDepth != 0) {
            ++mDropped;
            return;
        }
        ++mCounts[key];
    }
    bool has(std::string_view key) const { return count(key) > 0; }
    int count(std::string_view key) const {
        const auto it = mCounts.find(key);
        return it != mCounts.end() ? it->second : 0;
    }
    void clear() { mCounts.clear(); }

    /// Returns true for the outermost suspension.
    bool suspend() {
        mCounts.clear();
        return mSuspendDepth++ == 0;
    }
    /// Returns true when this resume lifted the suspension. An unmatched resume is ignored.
    bool resume() {
        if (mSuspendDepth == 0) {
            return false;
        }
        if (--mSuspendDepth != 0) {
            return false;
        }
        mCounts.clear();
        return true;
    }
    bool suspended() const { return mSuspendDepth != 0; }
    unsigned depth() const { return mSuspendDepth; }
    std::uint64_t dropped() const { return mDropped; }

private:
    // Keys are the string literals the game passes, as before.
    std::unordered_map<std::string_view, int> mCounts;
    unsigned mSuspendDepth = 0;
    std::uint64_t mDropped = 0;
};

enum class AchievementTickGate {
    Evaluate,      ///< check every achievement, then clear the signals and save if dirty
    SkipAndClear,  ///< evaluate nothing and drop this tick's signals
    SkipKeep,      ///< evaluate nothing and keep the signals (game not launched yet)
};

/// One AchievementSystem tick. A suspension overrides the gate: nothing is evaluated,
/// nothing is kept, nothing is saved.
template <typename Evaluate, typename Save>
void run_achievement_tick(
    AchievementSignals& signals, AchievementTickGate gate, bool& dirty, Evaluate&& evaluate, Save&& save) {
    if (signals.suspended()) {
        gate = AchievementTickGate::SkipAndClear;
    }
    if (gate == AchievementTickGate::SkipAndClear) {
        signals.clear();
        return;
    }
    if (gate == AchievementTickGate::SkipKeep) {
        return;
    }
    evaluate();
    signals.clear();
    if (dirty) {
        save();
        dirty = false;
    }
}

/// Holds one suspension for its lifetime. `System` is anything with suspend()/resume():
/// AchievementSystem in the game, AchievementSignals in the test.
template <typename System>
class ScopedAchievementSuspension {
public:
    explicit ScopedAchievementSuspension(System& system) : mSystem(&system) { mSystem->suspend(); }
    ~ScopedAchievementSuspension() {
        if (mSystem != nullptr) {
            mSystem->resume();
        }
    }
    ScopedAchievementSuspension(const ScopedAchievementSuspension&) = delete;
    ScopedAchievementSuspension& operator=(const ScopedAchievementSuspension&) = delete;

private:
    System* mSystem;
};

}  // namespace dusk
