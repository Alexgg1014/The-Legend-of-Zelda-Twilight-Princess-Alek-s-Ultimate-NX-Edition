// Compatibility mode (official Dusklight builds without SaveSnapshotService): FileLocalStore on a
// real directory tree laid out like the official PC build's, alone and driven by the sync engine
// against the in-memory provider, with a "native" device (FakeLocal, as the Switch with the
// service) on the other side.

#include "../core/dusksave.hpp"
#include "../core/sync_engine.hpp"
#include "../src/file_local_store.hpp"
#include "../src/platform_io.hpp"
#include "fakes.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

using namespace crosssave;
using namespace crosssave::test;
namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_test = "";

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n", g_test, __FILE__, __LINE__, #cond);      \
        }                                                                                          \
    } while (0)

fs::path g_root;

Bytes make_gci(const std::string& game, const std::string& tag, uint32_t time = 100) {
    Bytes gci(0x40 + 0x2000, static_cast<uint8_t>(tag.size()));
    std::fill(gci.begin(), gci.begin() + 0x40, 0);
    std::copy(game.begin(), game.end(), gci.begin());
    gci[4] = '0';
    gci[5] = '1';
    const std::string name = "gczelda2";
    std::copy(name.begin(), name.end(), gci.begin() + 8);
    gci[0x28] = static_cast<uint8_t>(time >> 24);
    gci[0x29] = static_cast<uint8_t>(time >> 16);
    gci[0x2a] = static_cast<uint8_t>(time >> 8);
    gci[0x2b] = static_cast<uint8_t>(time);
    gci[0x39] = 1;
    std::copy(tag.begin(), tag.end(), gci.begin() + 0x40);
    return gci;
}

void write(const fs::path& path, const Bytes& bytes) {
    fs::create_directories(path.parent_path());
    io::write_file_atomic(path.string(), bytes.data(), bytes.size());
}

std::string read_tag(const fs::path& gciPath) {
    const auto bytes = io::read_file(gciPath.string(), 1 << 20);
    if (!bytes || bytes->size() < 0x48) {
        return {};
    }
    std::string tag;
    for (size_t i = 0x40; i < bytes->size() && (*bytes)[i] >= 'A'; ++i) {
        tag.push_back(static_cast<char>((*bytes)[i]));
    }
    return tag;
}

// An official-build user directory: <user>/mod_data/<id>, <user>/USA/Card A.
struct UserDir {
    fs::path user;
    fs::path dataDir;
    fs::path card;
    explicit UserDir(const std::string& name, const std::string& region = "USA") {
        user = g_root / name;
        fs::remove_all(user);
        dataDir = user / "mod_data" / "dev.aleks2.cross_save";
        card = user / region / "Card A";
        fs::create_directories(dataDir);
        fs::create_directories(card);
    }
    fs::path gci(const std::string& game = "GZ2E") const { return card / ("01-" + game + "-gczelda2.gci"); }
    fs::path sidecars(const std::string& game = "GZ2E") const { return card / ("01-" + game + "-gczelda2.mods"); }
    void play(const std::string& tag, uint32_t time = 100) const { write(gci(), make_gci("GZ2E", tag, time)); }
    size_t backups() const {
        size_t n = 0;
        if (fs::exists(card / "backups")) {
            for (const auto& entry : fs::directory_iterator(card / "backups")) {
                n += entry.path().extension() == ".dusksave" ? 1 : 0;
            }
        }
        return n;
    }
};

std::vector<std::pair<const char*, void (*)()>>& tests() {
    static std::vector<std::pair<const char*, void (*)()>> list;
    return list;
}
#define TEST(name) void name(); struct name##_reg { name##_reg() { tests().push_back({#name, name}); } } name##_instance; void name()

TEST(locate_and_identity) {
    UserDir dir{"locate"};
    compat::FileLocalStore store{dir.dataDir.string()};
    auto where = store.locate();
    CHECK(where.cardDir == dir.card.string() || fs::path{where.cardDir} == dir.card);
    CHECK(where.game == "GZ2E" && !where.hasSave);
    auto id = store.identity();
    CHECK(id.status == LocalStatus::Ok && !id.hasSave && id.key == "GZ2-01-gczelda2");
    dir.play("FOREST");
    write(dir.sidecars() / "dev.test.alpha.json", Bytes{'{', '}'});
    id = store.identity();
    CHECK(id.hasSave && id.digest.size() == 64);
    const auto gci = io::read_file(dir.gci().string(), 1 << 20);
    dusksave::ModFiles mods{{"dev.test.alpha", Bytes{'{', '}'}}};
    CHECK(id.digest == dusksave::digest(*gci, mods));  // same digest as the host would compute
    // A EUR card and a USA card: the most recently written save wins.
    write(dir.user / "EUR" / "Card A" / "01-GZ2P-gczelda2.gci", make_gci("GZ2P", "EURO", 500));
    CHECK(store.locate().game == "GZ2P");
    // No card at all, or a raw image.
    UserDir none{"none"};
    fs::remove_all(none.card.parent_path());
    compat::FileLocalStore empty{none.dataDir.string()};
    CHECK(empty.identity().status == LocalStatus::NoCard);
    write(none.user / "MemoryCardA.USA.raw", Bytes(16, 0));
    CHECK(empty.identity().message.find("raw card image") != std::string::npos);
}

TEST(capture_inspect_and_staged_apply) {
    UserDir dir{"apply"};
    dir.play("OLD");
    write(dir.sidecars() / "dev.test.alpha.json", Bytes{'o', 'l', 'd'});
    compat::FileLocalStore store{dir.dataDir.string()};
    const auto before = store.identity().digest;
    const auto snapshot = store.capture();
    CHECK(snapshot.status == LocalStatus::Ok && snapshot.digest == before);
    std::string error;
    const auto read = dusksave::read(snapshot.bytes, error);
    CHECK(read && read->modFiles.size() == 1);

    // A newer save from elsewhere, with other mod data.
    const auto incoming = dusksave::pack(make_gci("GZ2E", "NEW"), {{"dev.test.beta", Bytes{'n'}}}, "t");
    CHECK(store.inspect(*incoming).status == LocalStatus::Ok);
    const auto staged = store.apply(*incoming, before, false);
    CHECK(staged.status == LocalStatus::Deferred);
    CHECK(store.hasStagedApply());
    CHECK(read_tag(dir.gci()) == "OLD");  // nothing touched until the next start
    // Next start.
    compat::FileLocalStore restarted{dir.dataDir.string()};
    const auto message = restarted.applyStaged();
    CHECK(message.find("applied") != std::string::npos);
    CHECK(read_tag(dir.gci()) == "NEW");
    CHECK(fs::exists(dir.sidecars() / "dev.test.beta.json"));
    CHECK(!fs::exists(dir.sidecars() / "dev.test.alpha.json"));
    CHECK(dir.backups() == 1);
    CHECK(!restarted.hasStagedApply());
    const auto applied = restarted.takeStartupApply();
    CHECK(applied && *applied == restarted.identity().digest);
    CHECK(!restarted.takeStartupApply());  // once
    // The backup is a .dusksave of the old save that the host (and the Saves window) can read.
    for (const auto& entry : fs::directory_iterator(dir.card / "backups")) {
        const auto bytes = io::read_file(entry.path().string(), 1 << 20);
        const auto backup = dusksave::read(*bytes, error);
        CHECK(backup && backup->modFiles.contains("dev.test.alpha"));
        CHECK(entry.path().filename().string().rfind("01-GZ2E-gczelda2-", 0) == 0);
    }
}

TEST(staged_apply_refuses_changed_local) {
    UserDir dir{"changed"};
    dir.play("OLD");
    compat::FileLocalStore store{dir.dataDir.string()};
    const auto before = store.identity().digest;
    store.apply(*dusksave::pack(make_gci("GZ2E", "NEW"), {}, "t"), before, false);
    dir.play("PLAYEDMORE");  // the player kept playing in this session
    compat::FileLocalStore restarted{dir.dataDir.string()};
    const auto message = restarted.applyStaged();
    CHECK(message.find("not applied") != std::string::npos);
    CHECK(read_tag(dir.gci()) == "PLAYEDMORE");
    CHECK(!restarted.hasStagedApply());
    CHECK(!restarted.takeStartupApply());
    CHECK(dir.backups() == 0);
}

TEST(conversion_and_incompatible) {
    UserDir dir{"convert"};
    dir.play("NTSC");
    compat::FileLocalStore store{dir.dataDir.string()};
    const auto pal = *dusksave::pack(make_gci("GZ2P", "PAL"), {}, "t");
    CHECK(store.inspect(pal).status == LocalStatus::NeedsConversion);
    CHECK(store.apply(pal, std::nullopt, false).status == LocalStatus::NeedsConversion);
    CHECK(store.apply(pal, std::nullopt, true).status == LocalStatus::Deferred);
    compat::FileLocalStore restarted{dir.dataDir.string()};
    restarted.applyStaged();
    const auto bytes = io::read_file(dir.gci().string(), 1 << 20);
    CHECK(bytes && std::string(bytes->begin(), bytes->begin() + 4) == "GZ2E");  // game ID rewritten
    CHECK(read_tag(dir.gci()) == "PAL");
    const auto windWaker = *dusksave::pack([] {
        auto gci = make_gci("GZ2E", "X");
        std::copy_n("GALE", 4, gci.begin());
        return gci;
    }(), {}, "t");
    CHECK(store.inspect(windWaker).status == LocalStatus::Incompatible);
}

TEST(backup_retention) {
    UserDir dir{"retention"};
    for (int i = 0; i < 14; ++i) {
        // Fake old backups with earlier stamps.
        char name[96];
        std::snprintf(name, sizeof(name), "01-GZ2E-gczelda2-2020010%dT0000%02dZ.dusksave", 1 + i / 10, i);
        write(dir.card / "backups" / name, Bytes{'P', 'K'});
    }
    dir.play("OLD");
    compat::FileLocalStore store{dir.dataDir.string()};
    store.apply(*dusksave::pack(make_gci("GZ2E", "NEW"), {}, "t"), store.identity().digest, false);
    compat::FileLocalStore restarted{dir.dataDir.string()};
    restarted.applyStaged();
    CHECK(dir.backups() == 10);
}

// ---- with the engine ----------------------------------------------------------------------------

struct CompatDevice {
    UserDir dir;
    FakeBackend& backend;
    std::unique_ptr<compat::FileLocalStore> store;
    std::unique_ptr<SyncEngine> engine;
    int64_t now = 1000;
    std::string startup;

    CompatDevice(FakeBackend& shared, const std::string& name) : dir{name}, backend{shared} { start(); }

    // A game start: mod_initialize applies what was staged, then the engine starts.
    void start() {
        store = std::make_unique<compat::FileLocalStore>(dir.dataDir.string());
        startup = store->applyStaged();
        EngineConfig config;
        config.deviceId = "pc-id";
        config.deviceName = "PC";
        config.wallClockMs = [] { return int64_t{1'700'000'000'000}; };
        config.random = [state = static_cast<uint64_t>(now)]() mutable {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            return state;
        };
        engine = std::make_unique<SyncEngine>(*store, backend, config);
        engine->setEnabled(true, now);
        engine->onSessionStart(now);
    }
    void run(int64_t ms = 2000) {
        for (const int64_t end = now + ms; now <= end; now += 250) {
            engine->tick(now);
        }
    }
};

struct NativeDevice {
    FakeLocal local;
    std::unique_ptr<SyncEngine> engine;
    int64_t now = 1000;
    explicit NativeDevice(FakeBackend& backend) {
        EngineConfig config;
        config.deviceId = "switch-id";
        config.deviceName = "Switch";
        engine = std::make_unique<SyncEngine>(local, backend, config);
        engine->setEnabled(true, now);
    }
    void run(int64_t ms = 2000) {
        for (const int64_t end = now + ms; now <= end; now += 250) {
            engine->tick(now);
        }
    }
};

// A native device whose saves are real .dusksave (as SaveSnapshotService captures them), so both
// kinds of device exchange the same bytes.
class DusksaveLocal : public FakeLocal {
public:
    Bytes gci = make_gci("GZ2E", "SWITCHSTART");
    LocalIdentity identity() override {
        auto id = FakeLocal::identity();
        id.hasSave = true;
        id.digest = dusksave::digest(gci, {});
        return id;
    }
    LocalSnapshot capture() override {
        LocalSnapshot snapshot;
        snapshot.bytes = *dusksave::pack(gci, {}, "t");
        snapshot.digest = dusksave::digest(gci, {});
        snapshot.game = "GZ2E";
        return snapshot;
    }
    LocalCheck inspect(const Bytes& bytes) override {
        LocalCheck check;
        std::string error;
        const auto snapshot = dusksave::read(bytes, error);
        check.status = snapshot ? LocalStatus::Ok : LocalStatus::Invalid;
        if (snapshot) {
            check.digest = dusksave::digest(snapshot->gci, snapshot->modFiles);
        }
        return check;
    }
    LocalCheck apply(const Bytes& bytes, const std::optional<std::string>&, bool) override {
        LocalCheck check = inspect(bytes);
        std::string error;
        gci = dusksave::read(bytes, error)->gci;
        check.digest = dusksave::digest(gci, {});
        return check;
    }
};

TEST(pc_compat_and_switch_native_round_trip) {
    FakeBackend backend;
    CompatDevice pc{backend, "roundtrip-pc"};
    pc.dir.play("PCFOREST");
    pc.engine->onSaveWritten(pc.now);
    pc.run(6000);
    CHECK(backend.heads.size() == 1);
    const auto pcDigest = pc.store->identity().digest;
    CHECK(backend.heads.begin()->second.digest == pcDigest);

    // The Switch (service) downloads the PC's .dusksave and agrees on the digest.
    DusksaveLocal swLocal;
    EngineConfig config;
    config.deviceId = "switch-id";
    config.deviceName = "Switch";
    SyncEngine sw{swLocal, backend, config};
    sw.setEnabled(true, 0);
    swLocal.gci.clear();
    // New device without save: fake identity says hasSave; give it the PC's content path instead.
    for (int64_t t = 0; t < 3000; t += 250) {
        sw.tick(t);
    }
    // Both had a save and they differ: conflict; the player keeps the cloud copy.
    CHECK(sw.status().phase == Phase::Conflict);
    sw.resolve(Choice::KeepRemote, 3000);
    for (int64_t t = 3000; t < 6000; t += 250) {
        sw.tick(t);
    }
    CHECK(sw.status().phase == Phase::Idle);
    CHECK(dusksave::digest(swLocal.gci, {}) == pcDigest);

    // The Switch plays on and uploads; the PC sees it, stages it, and applies it at its next start.
    std::copy_n("SWITCHLAKE", 10, swLocal.gci.begin() + 0x40);
    sw.onSaveWritten(6000);
    for (int64_t t = 6000; t < 12000; t += 250) {
        sw.tick(t);
    }
    CHECK(backend.heads.begin()->second.revision == 2);
    pc.engine->onSessionStart(pc.now);
    pc.run();
    CHECK(pc.engine->status().phase == Phase::WaitingForMenu);
    CHECK(read_tag(pc.dir.gci()) == "PCFOREST");
    pc.start();  // the player restarts the game
    CHECK(pc.startup.find("applied") != std::string::npos);
    CHECK(read_tag(pc.dir.gci()) == "SWITCHLAKE");
    backend.calls.clear();
    pc.run();
    CHECK(pc.engine->status().phase == Phase::Idle);
    CHECK(pc.engine->record().baseRevision == 2);
    CHECK(backend.calls["commitHead"] == 0);  // adopted, not re-uploaded
    CHECK(pc.dir.backups() == 1);
}

TEST(pc_compat_restart_without_apply_keeps_waiting) {
    FakeBackend backend;
    CompatDevice pc{backend, "waiting-pc"};
    pc.dir.play("A");
    pc.run(6000);
    // Someone else uploads.
    RemoteHead head = backend.heads.begin()->second;
    DusksaveLocal other;
    other.gci = make_gci("GZ2E", "REMOTE");
    EngineConfig config;
    config.deviceId = "other";
    SyncEngine otherEngine{other, backend, config};
    otherEngine.setEnabled(true, 0);
    for (int64_t t = 0; t < 3000; t += 250) {
        otherEngine.tick(t);
    }
    otherEngine.resolve(Choice::KeepLocal, 3000);
    for (int64_t t = 3000; t < 6000; t += 250) {
        otherEngine.tick(t);
    }
    CHECK(backend.heads.begin()->second.revision == 2);
    pc.engine->syncNow(pc.now);
    pc.run();
    CHECK(pc.engine->status().phase == Phase::WaitingForMenu);
    // Same session, more checks: still waiting, nothing downloaded again, nothing uploaded.
    backend.calls.clear();
    pc.engine->syncNow(pc.now);
    pc.run();
    CHECK(pc.engine->status().phase == Phase::WaitingForMenu);
    CHECK(backend.calls["getChunk"] == 0 && backend.calls["commitHead"] == 0);
    (void)head;
}

}  // namespace

int main(int argc, char** argv) {
    g_root = argc > 1 ? fs::path{argv[1]} : fs::temp_directory_path() / "cross-save-compat";
    fs::create_directories(g_root);
    for (const auto& [name, test] : tests()) {
        g_test = name;
        test();
    }
    std::printf("compat_test: %zu tests, %d checks, %d failures\n", tests().size(), g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("compat_test: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
