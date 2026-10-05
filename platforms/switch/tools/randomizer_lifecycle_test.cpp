// H38: the Randomizer's game-mode lifecycle (patch 0009, src/session_lifecycle.hpp from the patched source) over fake
// services with counters: full activation, a failure at each stage with rollback, shutdown called twice,
// deactivation followed by mod_shutdown, and a preload thread still running at shutdown.
#include "session_lifecycle.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace randomizer::session;

namespace {

int failures = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                      \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

// --- fake services: a handle is live until freed once; freeing it again is the "stale handle" error -------------
struct Counters {
    int observe = 0, unobserve = 0, texReg = 0, texUnreg = 0, stale = 0;
    ModResult observeResult = MOD_OK, texResult = MOD_OK;
    uint64_t nextHandle = 100;
    std::vector<uint64_t> live;
    std::mutex logMutex;
    std::vector<std::string> events;
    void log(const std::string& e) {
        std::lock_guard lock{logMutex};
        events.push_back(e);
    }
    int count(const std::string& e) {
        std::lock_guard lock{logMutex};
        int n = 0;
        for (const auto& x : events) n += x == e;
        return n;
    }
    long index(const std::string& e) {
        std::lock_guard lock{logMutex};
        for (std::size_t i = 0; i < events.size(); ++i)
            if (events[i] == e) return static_cast<long>(i);
        return -1;
    }
    void reset() {
        observe = unobserve = texReg = texUnreg = stale = 0;
        observeResult = texResult = MOD_OK;
        live.clear();
        std::lock_guard lock{logMutex};
        events.clear();
    }
} C;

bool free_handle(uint64_t h) {
    for (std::size_t i = 0; i < C.live.size(); ++i) {
        if (C.live[i] == h) {
            C.live.erase(C.live.begin() + static_cast<long>(i));
            return true;
        }
    }
    ++C.stale;
    return false;
}

ModResult fake_observe(ModContext*, SaveEventFn, SaveEventFn, SaveEventFn, void*, SaveObserverHandle* out) {
    ++C.observe;
    if (C.observeResult != MOD_OK) return C.observeResult;
    *out = ++C.nextHandle;
    C.live.push_back(*out);
    return MOD_OK;
}
ModResult fake_unobserve(ModContext*, SaveObserverHandle h) {
    ++C.unobserve;
    C.log("unobserve");
    return free_handle(h) ? MOD_OK : MOD_ERROR;
}
ModResult fake_register(ModContext*, const char*, TextureReplacementHandle* out) {
    ++C.texReg;
    if (C.texResult != MOD_OK) return C.texResult;
    *out = ++C.nextHandle;
    C.live.push_back(*out);
    return MOD_OK;
}
ModResult fake_unregister(ModContext*, TextureReplacementHandle h) {
    ++C.texUnreg;
    C.log("texture_unregister");
    return free_handle(h) ? MOD_OK : MOD_ERROR;
}

struct Game {
    int hooksInit = 0, hooksUninstall = 0, uiInit = 0, uiShutdown = 0, seedOff = 0, preloadRuns = 0;
    ModResult hooksResult = MOD_OK, uiResult = MOD_OK;
    std::atomic<bool> preloadStarted{false}, preloadDone{false};
    bool preloadWaitsForCancel = true;
};

GameCallbacks make_callbacks(Game& g) {
    GameCallbacks cb;
    cb.hooks_initialize = [&g] {
        ++g.hooksInit;
        return g.hooksResult;
    };
    cb.hooks_uninstall = [&g] {
        ++g.hooksUninstall;
        C.log("hooks_uninstall");
    };
    cb.ui_initialize = [&g] {
        ++g.uiInit;
        return g.uiResult;
    };
    cb.ui_shutdown = [&g] {
        ++g.uiShutdown;
        C.log("ui_shutdown");
    };
    cb.deactivate_seed = [&g] {
        ++g.seedOff;
        C.log("seed_off");
    };
    cb.preload = [&g](const std::atomic<bool>& cancel) {
        ++g.preloadRuns;
        g.preloadStarted = true;
        while (g.preloadWaitsForCancel && !cancel.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Slow to leave, like a load that cannot be interrupted: shutdown must still wait for it.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        C.log("preload_done");
        g.preloadDone = true;
    };
    cb.on_save_written = nullptr;
    cb.logo_texture = "res/logo.png";
    return cb;
}

struct Fixture {
    SaveService save{};
    TextureService texture{};
    ServiceManager svc{};
    Game game;
    Fixture() {
        C.reset();
        save.observe_saves = fake_observe;
        save.unobserve_saves = fake_unobserve;
        texture.register_file = fake_register;
        texture.unregister = fake_unregister;
        svc.save = &save;
        svc.texture = &texture;
    }
};

void wait_started(Game& g) {
    for (int i = 0; i < 2000 && !g.preloadStarted; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

}  // namespace

int main() {
    {  // full activation, then shutdown: each resource acquired once and released once, in reverse order
        Fixture f;
        SessionResources r(f.svc, make_callbacks(f.game));
        CHECK(r.state() == State::Inactive);
        CHECK(!r.activate().has_value());
        CHECK(r.state() == State::Active);
        for (int i = 0; i < static_cast<int>(kResourceCount); ++i) CHECK(r.held(static_cast<Resource>(i)));
        CHECK(f.game.hooksInit == 1 && C.observe == 1 && C.texReg == 1 && f.game.uiInit == 1);
        CHECK(C.unobserve == 0 && C.texUnreg == 0 && f.game.uiShutdown == 0 && f.game.hooksUninstall == 0);
        wait_started(f.game);
        r.shutdown();
        CHECK(r.state() == State::Inactive);
        CHECK(C.unobserve == 1 && C.texUnreg == 1 && f.game.uiShutdown == 1 && f.game.hooksUninstall == 1 &&
              f.game.seedOff == 1);
        CHECK(C.stale == 0 && C.live.empty());
        // reverse order: preload, seed, ui, texture, save observer, hooks
        CHECK(C.index("preload_done") < C.index("seed_off"));
        CHECK(C.index("seed_off") < C.index("ui_shutdown"));
        CHECK(C.index("ui_shutdown") < C.index("texture_unregister"));
        CHECK(C.index("texture_unregister") < C.index("unobserve"));
        CHECK(C.index("unobserve") < C.index("hooks_uninstall"));
    }
    {  // a failure at each service-driven stage: what came before is released once, in reverse; nothing after exists
        struct Case {
            const char* name;
            Resource stage;
        };
        for (const Case c : {Case{"hooks", Resource::Hooks}, Case{"save", Resource::SaveObserver},
                 Case{"texture", Resource::TextureReplacement}, Case{"ui", Resource::Ui}}) {
            Fixture f;
            if (c.stage == Resource::Hooks) f.game.hooksResult = MOD_ERROR;
            if (c.stage == Resource::SaveObserver) C.observeResult = MOD_ERROR;
            if (c.stage == Resource::TextureReplacement) C.texResult = MOD_ERROR;
            if (c.stage == Resource::Ui) f.game.uiResult = MOD_ERROR;
            SessionResources r(f.svc, make_callbacks(f.game));
            const auto failed = r.activate();
            CHECK(failed.has_value() && failed->resource == c.stage && failed->code == MOD_ERROR);
            CHECK(r.state() == State::Inactive);
            const int stage = static_cast<int>(c.stage);
            // acquired before the failing stage: released exactly once; the failing stage held nothing
            CHECK(f.game.hooksInit == 1);
            CHECK(f.game.hooksUninstall == 1);  // rollback of hooks (partial or full), once
            CHECK(C.observe == (stage >= 1 ? 1 : 0));
            CHECK(C.unobserve == (stage > 1 ? 1 : 0));
            CHECK(C.texReg == (stage >= 2 ? 1 : 0));
            CHECK(C.texUnreg == (stage > 2 ? 1 : 0));
            CHECK(f.game.uiInit == (stage >= 3 ? 1 : 0));
            CHECK(f.game.uiShutdown == 0);  // a failed ui::initialize cleans up inside itself
            CHECK(f.game.seedOff == 0 && f.game.preloadRuns == 0);
            CHECK(C.stale == 0 && C.live.empty());
            r.shutdown();  // nothing left to release
            CHECK(f.game.hooksUninstall == 1 && C.unobserve == (stage > 1 ? 1 : 0) && C.texUnreg == (stage > 2 ? 1 : 0));
            CHECK(C.stale == 0);
        }
    }
    {  // double shutdown, then deactivation followed by mod_shutdown
        Fixture f;
        SessionResources r(f.svc, make_callbacks(f.game));
        r.activate();
        wait_started(f.game);
        r.shutdown();  // onGameModeDeactivated
        r.shutdown();  // mod_shutdown
        r.shutdown();
        CHECK(C.unobserve == 1 && C.texUnreg == 1 && f.game.uiShutdown == 1 && f.game.hooksUninstall == 1);
        CHECK(f.game.seedOff == 1 && f.game.preloadRuns == 1 && C.count("preload_done") == 1);
        CHECK(C.stale == 0);
        // and a later activation starts from scratch
        f.game.preloadStarted = false;
        CHECK(!r.activate().has_value());
        wait_started(f.game);
        CHECK(f.game.hooksInit == 2 && C.observe == 2 && C.texReg == 2 && f.game.preloadRuns == 2);
        r.shutdown();
        CHECK(C.unobserve == 2 && C.texUnreg == 2 && C.stale == 0 && C.live.empty());
        // never activated: shutdown does nothing
        Fixture g;
        SessionResources idle(g.svc, make_callbacks(g.game));
        idle.shutdown();
        CHECK(C.unobserve == 0 && g.game.hooksUninstall == 0 && g.game.seedOff == 0);
    }
    {  // a preload still pending at shutdown is cancelled and joined before anything else is torn down
        Fixture f;
        f.game.preloadWaitsForCancel = true;
        SessionResources r(f.svc, make_callbacks(f.game));
        r.activate();
        wait_started(f.game);
        CHECK(!f.game.preloadDone);
        r.shutdown();
        CHECK(f.game.preloadDone);  // joined: no work left once shutdown returned
        CHECK(C.index("preload_done") >= 0 && C.index("preload_done") < C.index("seed_off"));
        CHECK(C.index("preload_done") < C.index("ui_shutdown"));
        CHECK(C.index("preload_done") < C.index("hooks_uninstall"));
    }
    {  // destruction without an explicit shutdown also releases everything once
        Fixture f;
        {
            SessionResources r(f.svc, make_callbacks(f.game));
            r.activate();
            wait_started(f.game);
        }
        CHECK(C.unobserve == 1 && C.texUnreg == 1 && f.game.hooksUninstall == 1 && f.game.preloadDone);
    }
    {  // Lifecycle itself, with a failure in every stage including seed and thread start
        for (std::size_t failAt = 0; failAt <= kResourceCount; ++failAt) {  // == kResourceCount: no failure
            std::vector<int> acquired(kResourceCount, 0), released(kResourceCount, 0);
            std::vector<int> order;
            Lifecycle::Steps steps;
            for (std::size_t i = 0; i < kResourceCount; ++i) {
                steps[i].acquire = [&, i, failAt]() -> bool {
                    ++acquired[i];
                    if (i == failAt) {
                        if (i % 2) throw std::runtime_error("thread start");  // an exception counts as failure
                        return false;
                    }
                    return true;
                };
                steps[i].release = [&, i] {
                    ++released[i];
                    order.push_back(static_cast<int>(i));
                };
            }
            Lifecycle lc(std::move(steps));
            const auto failed = lc.activate();
            if (failAt == kResourceCount) {
                CHECK(!failed && lc.state() == State::Active);
            } else {
                CHECK(failed && static_cast<std::size_t>(*failed) == failAt && lc.state() == State::Inactive);
                for (std::size_t i = 0; i < kResourceCount; ++i) {
                    CHECK(acquired[i] == (i <= failAt ? 1 : 0));
                    CHECK(released[i] == (i < failAt ? 1 : 0));
                }
            }
            lc.shutdown();
            lc.shutdown();
            for (std::size_t i = 0; i < kResourceCount; ++i) {
                CHECK(released[i] == (i < failAt ? 1 : (failAt == kResourceCount ? 1 : 0)));
            }
            for (std::size_t i = 1; i < order.size(); ++i) CHECK(order[i] < order[i - 1]);  // strictly reverse
        }
    }
    {  // release_handle: freed once, invalid afterwards
        uint64_t h = 7;
        int freed = 0;
        CHECK(release_handle(h, [&](uint64_t) { ++freed; }));
        CHECK(h == 0);
        CHECK(!release_handle(h, [&](uint64_t) { ++freed; }));
        CHECK(freed == 1);
    }
    if (failures == 0) {
        std::puts("randomizer lifecycle: activation, rollback at every stage, double shutdown, deactivate+shutdown, "
                  "pending preload joined: ok");
    }
    return failures == 0 ? 0 : 1;
}
