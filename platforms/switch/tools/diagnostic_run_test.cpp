// H34-c2 host test: the start/end order of a diagnostic run (src/dusk/diagnostic_run.hpp),
// the config_json_written baseline of Prepare Shaders, and the diagnostic suspension of
// Dusklight's own achievement system (src/dusk/achievement_signals.hpp, the code
// AchievementSystem::tick() and signal() run).
//   platforms/switch/tools/test-diagnostic-run.sh
//
// Not covered here (needs the game): benchmark.cpp's state machine itself. What is
// covered is the code it calls at both ends, with fakes standing in for the memory card,
// the UI, config::save_count() and the achievement entries.
#include "dusk/achievement_signals.hpp"
#include "dusk/diagnostic_run.hpp"
#include "dusk/perf_tour.hpp"
#include "dusk/shader_prep.hpp"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace dr = dusk::diagnostic_run;
namespace sp = dusk::shader_prep;
namespace pt = dusk::perf_tour;

static int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

namespace {

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// ------------------------------------------------------------------------------------
// A fake of Dusklight's AchievementSystem built on the real signal/tick code: one
// "kill 3 enemies" counter and one "open the letter" one-shot, an achievements.json write
// counter, and the same gates tick() computes.

struct FakeAchievements {
    dusk::AchievementSignals signals;
    int killProgress = 0;
    bool killUnlocked = false;
    bool letterUnlocked = false;
    bool dirty = false;
    int jsonWrites = 0;
    bool gameLaunched = true;

    void signal(const char* key) { signals.signal(key); }
    void suspend() { signals.suspend(); }
    void resume() { signals.resume(); }
    bool suspended() const { return signals.suspended(); }

    void tick() {
        auto gate = dusk::AchievementTickGate::Evaluate;
        if (signals.suspended()) {
            gate = dusk::AchievementTickGate::SkipAndClear;
        } else if (!gameLaunched) {
            gate = dusk::AchievementTickGate::SkipKeep;
        }
        dusk::run_achievement_tick(signals, gate, dirty,
            [this] {
                if (!killUnlocked && signals.count("enemy_killed") > 0) {
                    killProgress += signals.count("enemy_killed");
                    dirty = true;
                    if (killProgress >= 3) {
                        killProgress = 3;
                        killUnlocked = true;
                    }
                }
                if (!letterUnlocked && signals.has("open_letter")) {
                    letterUnlocked = true;
                    dirty = true;
                }
            },
            [this] { ++jsonWrites; });
    }
};

void test_achievements_normal_behaviour() {
    FakeAchievements a;
    a.signal("enemy_killed");
    a.tick();
    CHECK(a.killProgress == 1 && a.jsonWrites == 1);
    a.signal("enemy_killed");
    a.signal("enemy_killed");
    a.tick();
    CHECK(a.killUnlocked && a.killProgress == 3 && a.jsonWrites == 2);
    a.tick();  // nothing new: no write
    CHECK(a.jsonWrites == 2);

    // Before the game launches signals are kept, as before H34-c2.
    FakeAchievements early;
    early.gameLaunched = false;
    early.signal("open_letter");
    early.tick();
    CHECK(!early.letterUnlocked && early.signals.has("open_letter"));
    early.gameLaunched = true;
    early.tick();
    CHECK(early.letterUnlocked);
}

void test_achievements_suspended() {
    FakeAchievements a;
    a.suspend();
    CHECK(a.suspended());
    // A condition that would unlock: dropped as it arrives, nothing evaluated or written.
    a.signal("open_letter");
    for (int i = 0; i < 5; ++i) {
        a.signal("enemy_killed");
    }
    CHECK(!a.signals.has("open_letter") && a.signals.count("enemy_killed") == 0);
    CHECK(a.signals.dropped() == 6);
    a.tick();
    a.tick();
    CHECK(!a.letterUnlocked && !a.killUnlocked);
    CHECK(a.killProgress == 0);
    CHECK(a.jsonWrites == 0);

    // Nothing carried across the resume: the next tick sees no diagnostic signal.
    a.resume();
    CHECK(!a.suspended());
    a.tick();
    CHECK(!a.letterUnlocked && a.killProgress == 0 && a.jsonWrites == 0);

    // After it, a normal signal works exactly as before.
    a.signal("open_letter");
    a.tick();
    CHECK(a.letterUnlocked && a.jsonWrites == 1);
}

void test_achievements_signal_before_suspend_is_cleared() {
    // A signal raised in the frame the run starts does not cross into it, or out of it.
    FakeAchievements a;
    a.signal("open_letter");
    a.suspend();
    CHECK(!a.signals.has("open_letter"));
    a.resume();
    a.tick();
    CHECK(!a.letterUnlocked && a.jsonWrites == 0);
}

void test_achievements_nested_and_unmatched() {
    dusk::AchievementSignals s;
    CHECK(s.suspend());   // outermost
    CHECK(!s.suspend());  // nested
    CHECK(s.depth() == 2);
    CHECK(!s.resume());   // inner resume does not lift it
    CHECK(s.suspended());
    s.signal("x");
    CHECK(!s.has("x"));
    CHECK(s.resume());    // outermost resume lifts it
    CHECK(!s.suspended());
    CHECK(!s.resume());   // an unmatched resume is ignored, never goes negative
    CHECK(s.depth() == 0);
    s.signal("x");
    CHECK(s.has("x"));

    // RAII: scopes nest and unwind in order, also through an exception.
    FakeAchievements a;
    {
        dusk::ScopedAchievementSuspension<FakeAchievements> outer{a};
        {
            dusk::ScopedAchievementSuspension<FakeAchievements> inner{a};
            CHECK(a.signals.depth() == 2);
        }
        CHECK(a.suspended());
    }
    CHECK(!a.suspended());
    try {
        dusk::ScopedAchievementSuspension<FakeAchievements> guard{a};
        throw std::runtime_error("error path");
    } catch (const std::exception&) {
    }
    CHECK(!a.suspended());
}

// ------------------------------------------------------------------------------------
// A fake console for the start and end of a run: the memory-card block, the UI (closing
// Settings saves the config, synchronously, as SettingsWindow::hide does), the config save
// counter, and the achievements. The run itself holds its suspension the way benchmark.cpp's
// Run does (a unique_ptr released in end_run, and destroyed with the run otherwise).

struct FakeConsole {
    bool gameSavesBlocked = false;
    bool settingsOpen = true;
    std::uint64_t configSaves = 0;
    FakeAchievements achievements;
    std::vector<std::string> order;

    void close_ui() {
        order.push_back("close_ui");
        if (settingsOpen) {
            settingsOpen = false;
            ++configSaves;  // SettingsWindow::hide -> config::save()
        }
    }
};

struct FakeRun {
    std::uint64_t configBaseline = 0;
    std::unique_ptr<dusk::ScopedAchievementSuspension<FakeAchievements>> achievements;
};

std::uint64_t start(FakeConsole& c, FakeRun& run) {
    return dr::begin({
        .blockGameSaves =
            [&] {
                c.order.push_back("block_saves");
                c.gameSavesBlocked = true;
            },
        .suspendAchievements =
            [&] {
                c.order.push_back("suspend_achievements");
                run.achievements =
                    std::make_unique<dusk::ScopedAchievementSuspension<FakeAchievements>>(c.achievements);
            },
        .closeUi = [&] { c.close_ui(); },
        .configSaveCount =
            [&] {
                c.order.push_back("read_save_count");
                return c.configSaves;
            },
    });
}

void finish(FakeConsole& c, FakeRun& run, sp::TourReport* report) {
    dr::end({
        .reenableGameSaves =
            [&] {
                c.order.push_back("reenable_saves");
                c.gameSavesBlocked = false;
            },
        .finish =
            [&] {
                c.order.push_back("finish");
                if (report != nullptr) {
                    report->savesReenabled = !c.gameSavesBlocked;
                    report->configSavesDuringRun = dr::config_writes_since(run.configBaseline, c.configSaves);
                }
            },
        .resumeAchievements =
            [&] {
                c.order.push_back("resume_achievements");
                run.achievements.reset();
            },
    });
}

void test_start_order_and_baseline() {
    FakeConsole c;
    FakeRun run;
    run.configBaseline = start(c, run);
    const std::vector<std::string> expected{"block_saves", "suspend_achievements", "close_ui", "read_save_count"};
    CHECK(c.order == expected);
    CHECK(c.gameSavesBlocked && c.achievements.suspended());
    // The Settings save happened, and is inside the baseline, not after it.
    CHECK(c.configSaves == 1 && run.configBaseline == 1);
    CHECK(dr::config_writes_since(run.configBaseline, c.configSaves) == 0);

    // A real write after the run began is still caught.
    ++c.configSaves;
    CHECK(dr::config_writes_since(run.configBaseline, c.configSaves) == 1);
    CHECK(dr::config_writes_since(5, 3) == 0);  // never negative
}

// Every way a run can end goes through the same end: completed, cancelled while loading,
// cancelled while drawing, a zone that would not load, left early, could not return.
void test_every_exit_restores(const char* outcome, sp::Outcome reportOutcome, bool writeDuringRun) {
    FakeConsole c;
    FakeRun run;
    run.configBaseline = start(c, run);
    // The run warps and the game raises signals that would unlock things.
    c.achievements.signal("open_letter");
    c.achievements.signal("enemy_killed");
    c.achievements.tick();
    if (writeDuringRun) {
        ++c.configSaves;
    }
    sp::TourReport report;
    report.outcome = reportOutcome;
    report.zones.assign(sp::zones().size(), sp::ZoneResult{});
    c.order.clear();
    finish(c, run, &report);

    const std::vector<std::string> expected{"reenable_saves", "finish", "resume_achievements"};
    CHECK(c.order == expected);
    CHECK(!c.gameSavesBlocked);
    CHECK(!c.achievements.suspended());
    CHECK(report.savesReenabled);
    CHECK(c.achievements.jsonWrites == 0 && !c.achievements.letterUnlocked && c.achievements.killProgress == 0);
    const std::string json = sp::build_report(report, sp::zones());
    CHECK(contains(json, writeDuringRun ? "\"config_json_written\": true" : "\"config_json_written\": false"));
    CHECK(contains(json, std::string("\"status\": \"") + sp::outcome_name(reportOutcome) + "\""));
    // And afterwards achievements work normally again.
    c.achievements.signal("open_letter");
    c.achievements.tick();
    CHECK(c.achievements.letterUnlocked && c.achievements.jsonWrites == 1);
    if (failures != 0) {
        std::cerr << "  (exit: " << outcome << ")\n";
    }
}

void test_run_destroyed_without_end() {
    // If the run object is replaced without end_run (g_run = Run{}), the suspension still
    // ends with it.
    FakeConsole c;
    {
        FakeRun run;
        run.configBaseline = start(c, run);
        CHECK(c.achievements.suspended());
    }
    CHECK(!c.achievements.suspended());

    // The H34-c1 order (baseline read before the UI closed) is what blamed the run.
    FakeConsole old;
    const std::uint64_t badBaseline = old.configSaves;
    old.close_ui();
    CHECK(dr::config_writes_since(badBaseline, old.configSaves) == 1);
}

// ------------------------------------------------------------------------------------
// H35: the saves block as a guard the run holds (benchmark.cpp's Run::savesBlock), and the
// Performance Tour going through the same start and end.

void test_saves_guard() {
    std::atomic<bool> blocked{false};
    {
        dr::ScopedFlag guard{blocked};
        CHECK(blocked.load() && guard.held());
        guard.release();
        CHECK(!blocked.load() && !guard.held());
        guard.release();  // twice is harmless
        CHECK(!blocked.load());
    }
    // Destroyed while held: released.
    {
        dr::ScopedFlag guard{blocked};
        CHECK(blocked.load());
    }
    CHECK(!blocked.load());
    // Moved: only the new owner releases it, once.
    {
        dr::ScopedFlag outer;
        {
            dr::ScopedFlag inner{blocked};
            outer = std::move(inner);
            CHECK(!inner.held() && outer.held());
        }
        CHECK(blocked.load());
    }
    CHECK(!blocked.load());
    // An exception on the way out of a run releases it too.
    try {
        dr::ScopedFlag guard{blocked};
        throw std::runtime_error("error path");
    } catch (const std::exception&) {
    }
    CHECK(!blocked.load());
}

// The run as benchmark.cpp lays it out: achievements first, the saves guard after, so a run
// destroyed without end_run gives saving back before achievements resume.
struct GuardedRun {
    std::unique_ptr<dusk::ScopedAchievementSuspension<FakeAchievements>> achievements;
    dr::ScopedFlag savesBlock;
};

void test_tour_run_destroyed_unexpectedly() {
    std::atomic<bool> blocked{false};
    FakeConsole c;
    {
        GuardedRun run;
        dr::begin({
            .blockGameSaves = [&] { run.savesBlock = dr::ScopedFlag(blocked); },
            .suspendAchievements =
                [&] {
                    run.achievements =
                        std::make_unique<dusk::ScopedAchievementSuspension<FakeAchievements>>(c.achievements);
                },
            .closeUi = [&] { c.close_ui(); },
            .configSaveCount = [&] { return c.configSaves; },
        });
        CHECK(blocked.load() && c.achievements.suspended());
        c.achievements.signal("open_letter");
        c.achievements.tick();
    }
    // Neither the card nor the achievements stay locked once the run object is gone, and
    // nothing the tour did was evaluated or written.
    CHECK(!blocked.load());
    CHECK(!c.achievements.suspended());
    CHECK(!c.achievements.letterUnlocked && c.achievements.jsonWrites == 0);
}

// Every way the tour ends: completed, cancelled with Start in each step, a zone that does not
// load, the player leaving a zone. Same end order as every run; no config write, no
// achievement, saves back, and a report that says so.
void test_tour_every_exit(const char* name, pt::Outcome outcome, int stoppedZone, pt::ZoneStatus stoppedStatus) {
    std::atomic<bool> blocked{false};
    FakeConsole c;
    GuardedRun run;
    const std::uint64_t baseline = dr::begin({
        .blockGameSaves = [&] { run.savesBlock = dr::ScopedFlag(blocked); },
        .suspendAchievements =
            [&] {
                run.achievements = std::make_unique<dusk::ScopedAchievementSuspension<FakeAchievements>>(c.achievements);
            },
        .closeUi = [&] { c.close_ui(); },
        .configSaveCount = [&] { return c.configSaves; },
    });
    // The warps raise game signals.
    for (int i = 0; i < 3; ++i) {
        c.achievements.signal("enemy_killed");
        c.achievements.tick();
    }
    pt::TourReport report;
    report.outcome = outcome;
    report.uniformReuseModeAtStart = "off";
    report.zones.assign(pt::zones().size(), pt::ZoneResult{});
    for (int i = 0; i < static_cast<int>(report.zones.size()); ++i) {
        if (stoppedZone < 0 || i < stoppedZone) {
            report.zones[static_cast<std::size_t>(i)].status = pt::ZoneStatus::Completed;
            report.zones[static_cast<std::size_t>(i)].measured = true;
        } else if (i == stoppedZone) {
            report.zones[static_cast<std::size_t>(i)].status = stoppedStatus;
        }
    }
    if (outcome != pt::Outcome::Completed) {
        report.detail = name;
    }
    c.order.clear();
    std::string json;
    dr::end({
        .reenableGameSaves =
            [&] {
                c.order.push_back("reenable_saves");
                run.savesBlock.release();
            },
        .finish =
            [&] {
                c.order.push_back("finish");
                report.savesReenabled = !blocked.load();
                report.configSavesDuringRun = dr::config_writes_since(baseline, c.configSaves);
                report.achievementsSuspendedDuringRun = c.achievements.suspended();
                report.returned = true;
                report.stateRestored = true;
                json = pt::build_report(report, pt::zones());
            },
        .resumeAchievements =
            [&] {
                c.order.push_back("resume_achievements");
                run.achievements.reset();
            },
    });
    const std::vector<std::string> expected{"reenable_saves", "finish", "resume_achievements"};
    CHECK(c.order == expected);
    CHECK(!blocked.load() && !c.achievements.suspended());
    CHECK(c.achievements.jsonWrites == 0 && c.achievements.killProgress == 0);
    CHECK(contains(json, "\"config_json_written\": false"));
    CHECK(contains(json, "\"saves_reenabled\": true"));
    CHECK(contains(json, "\"achievements_suspended_during_run\": true"));
    CHECK(contains(json, std::string("\"status\": \"") + sp::outcome_name(outcome) + "\""));
    // The script runs these through a real JSON parser.
    std::string file = std::string("diagnostic_run_test.tour.") + name + ".json";
    for (auto& ch : file) {
        if (ch == ' ') {
            ch = '_';
        }
    }
    std::ofstream(file, std::ios::binary) << json;
    if (failures != 0) {
        std::cerr << "  (tour exit: " << name << ")\n";
    }
}

}  // namespace

int main() {
    test_achievements_normal_behaviour();
    test_achievements_suspended();
    test_achievements_signal_before_suspend_is_cleared();
    test_achievements_nested_and_unmatched();
    test_start_order_and_baseline();
    test_every_exit_restores("completed", sp::Outcome::Completed, false);
    test_every_exit_restores("cancelled while loading", sp::Outcome::Cancelled, false);
    test_every_exit_restores("cancelled while drawing", sp::Outcome::Cancelled, false);
    test_every_exit_restores("zone did not load", sp::Outcome::Failed, false);
    test_every_exit_restores("real config write during the run", sp::Outcome::Completed, true);
    test_every_exit_restores("cancelled after a real config write", sp::Outcome::Cancelled, true);
    test_run_destroyed_without_end();
    test_saves_guard();
    test_tour_run_destroyed_unexpectedly();
    test_tour_every_exit("completed", pt::Outcome::Completed, -1, pt::ZoneStatus::Completed);
    test_tour_every_exit("cancelled while loading", pt::Outcome::Cancelled, 0, pt::ZoneStatus::Cancelled);
    test_tour_every_exit("cancelled while measuring", pt::Outcome::Cancelled, 1, pt::ZoneStatus::Cancelled);
    test_tour_every_exit("zone did not load", pt::Outcome::Failed, 2, pt::ZoneStatus::Failed);
    test_tour_every_exit("left the zone", pt::Outcome::Completed, 1, pt::ZoneStatus::Skipped);
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "diagnostic_run: all checks passed\n";
    return 0;
}
