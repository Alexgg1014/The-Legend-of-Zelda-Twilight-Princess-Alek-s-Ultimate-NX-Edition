// Cross-save engine against the in-memory provider: every case of docs/dev/cross-save-design.md
// "Sincronización y conflictos", offline retries, debounce, fragments, compare-and-swap races,
// lost answers, interrupted uploads and injected local failures. Several engines share one
// FakeBackend: they are the player's devices.

#include "../core/sync_engine.hpp"
#include "fakes.hpp"

#include <cstdio>
#include <memory>
#include <string>

using namespace crosssave;
using namespace crosssave::test;

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

struct Device {
    FakeLocal local;
    FakeBackend& backend;
    std::unique_ptr<SyncEngine> engine;
    int64_t now = 1000;

    Device(FakeBackend& shared, std::string name) : backend{shared} {
        local.game = "GZ2E";
        restart(std::move(name));
    }

    // A new process: the engine is rebuilt; the save and the record survive.
    void restart(std::string name) {
        EngineConfig config;
        config.deviceId = name + "-id";
        config.deviceName = name;
        config.platform = "test";
        config.wallClockMs = [this] { return now + 1'700'000'000'000; };
        config.random = [state = std::hash<std::string>{}(name) | 1]() mutable {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return static_cast<uint64_t>(state);
        };
        engine = std::make_unique<SyncEngine>(local, backend, config);
        engine->setEnabled(true, now);
    }

    // Lets time pass in 250 ms steps, delivering deferred callbacks.
    void run(int64_t ms = 1000) {
        const int64_t end = now + ms;
        while (now <= end) {
            backend.pump();
            engine->tick(now);
            backend.pump();
            now += 250;
        }
    }

    Phase phase() const { return engine->status().phase; }
    const std::string& message() const { return engine->status().message; }
    std::string digest() const { return local.save ? fake_digest(*local.save) : std::string{}; }
};

const RemoteHead* head_of(FakeBackend& backend) {
    const auto it = backend.heads.find(backend.account + "/GZ2-01-gczelda2");
    return it != backend.heads.end() ? &it->second : nullptr;
}

// Fragments no head history names (left by failed or interrupted uploads).
size_t orphan_chunks(FakeBackend& backend) {
    const auto* head = head_of(backend);
    size_t count = 0;
    for (const auto& [name, data] : backend.chunks) {
        bool named = false;
        for (const auto& entry : head ? head->history : std::vector<HistoryEntry>{}) {
            named = named || name.find("/" + entry.snapshotId + "/") != std::string::npos;
        }
        count += named ? 0 : 1;
    }
    return count;
}

#define TEST(name) void name(); struct name##_reg { name##_reg() { tests().push_back({#name, name}); } } name##_instance; void name()

std::vector<std::pair<const char*, void (*)()>>& tests() {
    static std::vector<std::pair<const char*, void (*)()>> list;
    return list;
}

// ---- basic cases --------------------------------------------------------------------------------

TEST(disabled_and_signed_out) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.engine->setEnabled(false, pc.now);
    pc.local.play("A1");
    pc.engine->onSaveWritten(pc.now);
    pc.engine->syncNow(pc.now);
    pc.run(10000);
    CHECK(pc.phase() == Phase::Disabled);
    CHECK(backend.calls.empty());
    backend.account.clear();
    pc.engine->setEnabled(true, pc.now);
    pc.run(2000);
    CHECK(pc.phase() == Phase::SignedOut);
    CHECK(backend.calls.empty());
}

TEST(first_upload_then_same_revision_is_quiet) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    pc.run(2000);
    const auto* head = head_of(backend);
    CHECK(head != nullptr);
    CHECK(head && head->revision == 1);
    CHECK(head && head->digest == pc.digest());
    CHECK(head && head->deviceName == "PC");
    CHECK(head && head->history.size() == 1);
    CHECK(pc.phase() == Phase::Idle);
    CHECK(pc.engine->record().baseRevision == 1);
    CHECK(pc.engine->record().orphanSnapshotId.empty());
    // Same revision, same local save: only a head read, no transfer.
    backend.calls.clear();
    pc.engine->onSessionStart(pc.now);
    pc.run(2000);
    CHECK(backend.calls["getHead"] == 1);
    CHECK(backend.calls["putChunk"] == 0 && backend.calls["getChunk"] == 0 && backend.calls["commitHead"] == 0);
    CHECK(pc.phase() == Phase::Idle);
}

TEST(new_device_downloads) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    pc.run(2000);
    Device sw{backend, "Switch"};
    sw.run(2000);
    CHECK(sw.local.save && sw.local.save->content == "A1");
    CHECK(sw.local.backups == 0);  // nothing to back up
    CHECK(sw.engine->record().baseRevision == 1);
    CHECK(sw.engine->record().baseDigest == sw.digest());
    CHECK(sw.phase() == Phase::Idle);
}

TEST(remote_advanced_downloads_with_backup) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    pc.run(2000);
    Device sw{backend, "Switch"};
    sw.run(2000);
    pc.local.play("A2");
    pc.engine->onSaveWritten(pc.now);
    pc.run(6000);
    CHECK(head_of(backend)->revision == 2);
    sw.engine->onSessionStart(sw.now);
    sw.run(2000);
    CHECK(sw.local.save->content == "A2");
    CHECK(sw.local.backups == 1);
    CHECK(sw.local.backupHistory.back().content == "A1");
    CHECK(!sw.engine->status().lastBackup.empty());
    CHECK(sw.engine->record().baseRevision == 2);
}

TEST(local_advanced_uploads_with_cas) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    pc.run(2000);
    const auto token = head_of(backend)->casToken;
    pc.local.play("A2");
    pc.engine->onSaveWritten(pc.now);
    pc.run(6000);
    CHECK(head_of(backend)->revision == 2);
    CHECK(head_of(backend)->casToken != token);
    CHECK(head_of(backend)->digest == pc.digest());
    CHECK(head_of(backend)->history.size() == 2);
}

TEST(debounce_groups_saves) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    pc.run(2000);
    backend.calls.clear();
    for (int i = 0; i < 5; ++i) {
        pc.local.play("B" + std::to_string(i));
        pc.engine->onSaveWritten(pc.now);
        pc.run(1000);  // < debounce: nothing yet
    }
    CHECK(backend.calls["commitHead"] == 0);
    pc.run(6000);
    CHECK(backend.calls["commitHead"] == 1);
    CHECK(head_of(backend)->digest == pc.digest());
    // The cap: saving every 3 s for a minute still uploads within debounceMaxMs.
    backend.calls.clear();
    for (int i = 0; i < 9; ++i) {
        pc.local.play("C" + std::to_string(i));
        pc.engine->onSaveWritten(pc.now);
        pc.run(3000);
    }
    CHECK(backend.calls["commitHead"] >= 1);
}

TEST(manual_mode) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    pc.run(2000);
    pc.engine->setAutomatic(false);
    pc.local.play("A2");
    pc.engine->onSaveWritten(pc.now);
    pc.run(30000);
    CHECK(head_of(backend)->revision == 1);
    CHECK(pc.engine->status().localChangesPending);
    pc.engine->syncNow(pc.now);
    pc.run(2000);
    CHECK(head_of(backend)->revision == 2);
}

// ---- conflicts ----------------------------------------------------------------------------------

void diverge(FakeBackend& backend, Device& pc, Device& sw) {
    pc.local.play("A1");
    pc.run(2000);
    sw.run(2000);
    // Both play offline.
    backend.faults["getHead"] = {RemoteError::Network, 1000};
    pc.local.play("PC-progress");
    pc.engine->onSaveWritten(pc.now);
    pc.run(6000);
    sw.local.play("SW-progress");
    sw.engine->onSaveWritten(sw.now);
    sw.run(6000);
    backend.faults.clear();
    // PC reconnects first and uploads.
    pc.engine->onNetworkRestored(pc.now);
    pc.run(2000);
    sw.engine->onNetworkRestored(sw.now);
    sw.run(2000);
}

TEST(diverged_conflict_keeps_both) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    Device sw{backend, "Switch"};
    diverge(backend, pc, sw);
    CHECK(head_of(backend)->digest == pc.digest());
    CHECK(sw.phase() == Phase::Conflict);
    CHECK(sw.engine->status().conflict.kind == ConflictKind::Diverged);
    CHECK(sw.engine->status().conflict.remote && sw.engine->status().conflict.remote->deviceName == "PC");
    CHECK(sw.local.save->content == "SW-progress");  // nothing replaced silently
    // Saving again or starting a session does not resolve it, and transfers nothing.
    backend.calls.clear();
    sw.local.play("SW-more");
    sw.engine->onSaveWritten(sw.now);
    sw.engine->onSessionStart(sw.now);
    sw.run(10000);
    CHECK(sw.phase() == Phase::Conflict);
    CHECK(backend.calls["commitHead"] == 0 && backend.calls["getChunk"] == 0);
    // Cancel: both copies stay.
    sw.engine->resolve(Choice::Cancel, sw.now);
    sw.run(5000);
    CHECK(sw.phase() == Phase::Conflict);
    CHECK(sw.local.save->content == "SW-more");
    CHECK(head_of(backend)->digest == pc.digest());
}

TEST(conflict_keep_local_preserves_remote_in_history) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    Device sw{backend, "Switch"};
    diverge(backend, pc, sw);
    const auto pcSnapshot = head_of(backend)->snapshotId;
    sw.engine->resolve(Choice::KeepLocal, sw.now);
    sw.run(2000);
    CHECK(sw.phase() == Phase::Idle);
    CHECK(head_of(backend)->digest == sw.digest());
    CHECK(head_of(backend)->revision == 3);
    bool kept = false;
    for (const auto& entry : head_of(backend)->history) {
        kept = kept || entry.snapshotId == pcSnapshot;
    }
    CHECK(kept);
    CHECK(backend.chunkCountFor("GZ2-01-gczelda2", pcSnapshot) == 1);
    // PC now sees a newer revision and its own save unchanged since its last sync: it downloads.
    pc.engine->onSessionStart(pc.now);
    pc.run(2000);
    CHECK(pc.local.save->content == "SW-progress");
    CHECK(pc.local.backupHistory.back().content == "PC-progress");
}

TEST(conflict_keep_remote_backs_up_local) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    Device sw{backend, "Switch"};
    diverge(backend, pc, sw);
    sw.engine->resolve(Choice::KeepRemote, sw.now);
    sw.run(2000);
    CHECK(sw.phase() == Phase::Idle);
    CHECK(sw.local.save->content == "PC-progress");
    CHECK(sw.local.backupHistory.back().content == "SW-progress");
    CHECK(sw.engine->record().baseRevision == head_of(backend)->revision);
}

TEST(no_common_base) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    pc.run(2000);
    Device sw{backend, "Switch"};
    sw.local.play("Other");  // played before linking
    sw.engine->onAccountChanged(sw.now);
    sw.run(2000);
    CHECK(sw.phase() == Phase::Conflict);
    CHECK(sw.engine->status().conflict.kind == ConflictKind::NoCommonBase);
    // Same content on both sides: adopted without any transfer.
    FakeBackend other;
    Device a{other, "A"};
    a.local.play("same");
    a.run(2000);
    Device b{other, "B"};
    b.local.play("same");
    other.calls.clear();
    b.engine->onAccountChanged(b.now);
    b.run(2000);
    CHECK(b.phase() == Phase::Idle);
    CHECK(other.calls["getChunk"] == 0 && other.calls["commitHead"] == 0);
    CHECK(b.engine->record().baseRevision == 1);
}

TEST(remote_missing_and_rewound_and_local_missing) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    pc.run(2000);
    pc.local.play("A2");
    pc.engine->onSaveWritten(pc.now);
    pc.run(6000);
    // Remote deleted elsewhere.
    const auto saved = *head_of(backend);
    backend.heads.clear();
    pc.engine->syncNow(pc.now);
    pc.run(2000);
    CHECK(pc.phase() == Phase::Conflict);
    CHECK(pc.engine->status().conflict.kind == ConflictKind::RemoteMissing);
    pc.engine->resolve(Choice::KeepLocal, pc.now);
    pc.run(2000);
    CHECK(head_of(backend) && head_of(backend)->digest == pc.digest());
    CHECK(head_of(backend)->revision == 3);
    // Rewound: someone restored revision 1 directly.
    pc.local.play("A3");
    pc.engine->onSaveWritten(pc.now);
    pc.run(6000);
    CHECK(head_of(backend)->revision == 4);
    RemoteHead old = saved;
    old.revision = 1;
    backend.forceHead("GZ2-01-gczelda2", old);
    pc.engine->syncNow(pc.now);
    pc.run(2000);
    CHECK(pc.engine->status().conflict.kind == ConflictKind::RemoteRewound);
    // Local deleted (Saves window): restore from the cloud.
    FakeBackend b2;
    Device x{b2, "X"};
    x.local.play("keep-me");
    x.run(2000);
    x.local.save.reset();
    x.engine->syncNow(x.now);
    x.run(2000);
    CHECK(x.engine->status().conflict.kind == ConflictKind::LocalMissing);
    CHECK(b2.heads.size() == 1);  // a deleted local save is never synced as a deletion
    x.engine->resolve(Choice::KeepRemote, x.now);
    x.run(2000);
    CHECK(x.local.save && x.local.save->content == "keep-me");
}

// ---- offline, retries, quota, auth --------------------------------------------------------------

TEST(offline_backoff_and_recovery) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    backend.faults["getHead"] = {RemoteError::Network, 3};
    pc.run(1000);
    CHECK(pc.phase() == Phase::Retrying);
    CHECK(pc.engine->status().attempts == 1);
    const auto first = pc.engine->status().nextAttemptAtMs - pc.now;
    CHECK(first > 0 && first <= 6500);
    pc.run(7000);
    CHECK(pc.engine->status().attempts == 2);
    pc.run(40000);  // third failure, then success
    CHECK(pc.phase() == Phase::Idle);
    CHECK(head_of(backend) && head_of(backend)->digest == pc.digest());
    // Pause after maxAttempts; a new save resumes.
    backend.faults["getHead"] = {RemoteError::Network, 100};
    pc.local.play("A2");
    pc.engine->onSaveWritten(pc.now);
    pc.run(60LL * 60 * 1000);
    CHECK(pc.phase() == Phase::Error);
    CHECK(pc.engine->status().attempts == 8);
    const int calls = backend.calls["getHead"];
    pc.run(60LL * 60 * 1000);
    CHECK(backend.calls["getHead"] == calls);  // stopped: no polling
    backend.faults.clear();
    pc.engine->onNetworkRestored(pc.now);
    pc.run(2000);
    CHECK(pc.phase() == Phase::Idle);
    CHECK(head_of(backend)->digest == pc.digest());
    CHECK(pc.local.save->content == "A2");  // the game kept working all along
}

TEST(quota_auth_denied) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    backend.faults["getHead"] = {RemoteError::Quota, 1};
    pc.run(1000);
    CHECK(pc.phase() == Phase::Retrying);
    CHECK(pc.engine->status().nextAttemptAtMs - pc.now > 30LL * 60 * 1000);
    backend.faults["getHead"] = {RemoteError::Auth, 1};
    pc.engine->syncNow(pc.now);
    pc.run(1000);
    CHECK(pc.phase() == Phase::SignedOut);
    backend.faults["getHead"] = {RemoteError::Denied, 1};
    pc.engine->syncNow(pc.now);
    pc.run(1000);
    CHECK(pc.phase() == Phase::Error);
    CHECK(head_of(backend) == nullptr);
}

// ---- fragments and integrity --------------------------------------------------------------------

TEST(fragments_roundtrip_and_corruption) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.padTo = 1300 * 1024;  // 1.27 MiB -> 3 fragments
    pc.local.play("big");
    pc.run(2000);
    const auto* head = head_of(backend);
    CHECK(head && head->chunkCount == 3);
    CHECK(head && head->size == 1300 * 1024);
    size_t largest = 0;
    for (const auto& [name, data] : backend.chunks) {
        largest = std::max(largest, data.size());
    }
    CHECK(largest == kChunkBytes);
    Device sw{backend, "Switch"};
    sw.run(2000);
    CHECK(sw.local.save && sw.local.save->content == "big");
    // A damaged fragment: nothing applied, local save untouched.
    pc.local.play("big2");
    pc.engine->onSaveWritten(pc.now);
    pc.run(6000);
    backend.corruptNextChunkRead = true;
    sw.engine->syncNow(sw.now);
    sw.run(2000);
    CHECK(sw.phase() == Phase::Error);
    CHECK(sw.local.save->content == "big");
    CHECK(sw.engine->record().baseRevision == 1);
    // Next attempt works.
    sw.engine->syncNow(sw.now);
    sw.run(2000);
    CHECK(sw.local.save->content == "big2");
}

TEST(bad_head_metadata) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    pc.run(2000);
    RemoteHead bad = *head_of(backend);
    bad.revision = 2;
    bad.chunkCount = 7;
    backend.forceHead("GZ2-01-gczelda2", bad);
    Device sw{backend, "Switch"};
    sw.run(2000);
    CHECK(sw.phase() == Phase::Error);
    CHECK(!sw.local.save);
    CHECK(sw.local.applies == 0);
}

// ---- races, lost answers, interruptions ---------------------------------------------------------

TEST(cas_lost_is_reread_not_overwritten) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    Device sw{backend, "Switch"};
    pc.local.play("A1");
    pc.run(2000);
    sw.run(2000);
    pc.local.play("PC-2");
    sw.local.play("SW-2");
    // The Switch commits between the PC's head read and the PC's commit.
    backend.beforeCommit = [&] {
        sw.engine->syncNow(sw.now);
        sw.run(500);
    };
    pc.engine->syncNow(pc.now);
    pc.run(3000);
    CHECK(head_of(backend)->digest == sw.digest());  // the Switch's commit was not overwritten
    CHECK(pc.phase() == Phase::Conflict);
    CHECK(pc.engine->status().conflict.kind == ConflictKind::Diverged);
    // The PC's fragments were orphaned and removed by the cycle that found the conflict.
    CHECK(orphan_chunks(backend) == 0);
    CHECK(pc.engine->record().orphanSnapshotId.empty());
    pc.engine->resolve(Choice::KeepRemote, pc.now);
    pc.run(2000);
    CHECK(pc.local.save->content == "SW-2");
}

TEST(lost_commit_answer_is_adopted) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    backend.loseCommitResponse = true;
    pc.run(1000);
    CHECK(pc.phase() == Phase::Retrying);
    CHECK(head_of(backend) && head_of(backend)->revision == 1);  // it did commit
    pc.run(10000);
    CHECK(pc.phase() == Phase::Idle);
    CHECK(head_of(backend)->revision == 1);  // no duplicate revision
    CHECK(pc.engine->record().baseRevision == 1);
    CHECK(backend.chunkCountFor("GZ2-01-gczelda2", head_of(backend)->snapshotId) == 1);  // not deleted
}

TEST(interrupted_upload_leaves_orphan_that_is_cleaned) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.padTo = 1100 * 1024;
    pc.local.play("A1");
    backend.faults["putChunk"] = {RemoteError::Network, 1};
    // First fragment written, second fails; then the process dies.
    backend.faults["putChunk"].count = 0;
    int puts = 0;
    backend.deferred = true;
    pc.engine->syncNow(pc.now);
    pc.engine->tick(pc.now);
    // Deliver only the getHead answer and the first putChunk answer, then "crash".
    for (int i = 0; i < 2 && !backend.queue.empty(); ++i) {
        auto next = std::move(backend.queue.front());
        backend.queue.pop_front();
        next();
        ++puts;
    }
    backend.queue.clear();
    backend.deferred = false;
    const auto orphan = SyncRecord::parse(*pc.local.record)->orphanSnapshotId;
    CHECK(!orphan.empty());
    CHECK(head_of(backend) == nullptr);
    CHECK(backend.chunkCountFor("GZ2-01-gczelda2", orphan) >= 1);
    CHECK(orphan_chunks(backend) >= 1);
    pc.restart("PC");
    pc.run(2000);
    CHECK(orphan_chunks(backend) == 0);
    CHECK(head_of(backend) && head_of(backend)->digest == pc.digest());
    CHECK(pc.engine->record().orphanSnapshotId.empty());
    (void)puts;
}

TEST(restart_resumes_without_reupload) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    pc.run(2000);
    pc.restart("PC");
    backend.calls.clear();
    pc.run(2000);
    CHECK(backend.calls["commitHead"] == 0);
    CHECK(pc.phase() == Phase::Idle);
    // A save made while the game was closed (another session) is detected at start.
    pc.local.play("A2");
    pc.restart("PC");
    pc.run(2000);
    CHECK(head_of(backend)->digest == pc.digest());
}

// ---- local failures -----------------------------------------------------------------------------

TEST(local_apply_failures_keep_local_save) {
    for (const auto failure : {LocalStatus::BackupFailed, LocalStatus::WriteFailed, LocalStatus::ModDataFailed}) {
        FakeBackend backend;
        Device pc{backend, "PC"};
        pc.local.play("A1");
        pc.run(2000);
        Device sw{backend, "Switch"};
        sw.run(2000);
        pc.local.play("A2");
        pc.engine->onSaveWritten(pc.now);
        pc.run(6000);
        sw.local.applyFailure = failure;
        sw.engine->syncNow(sw.now);
        sw.run(2000);
        CHECK(sw.phase() == Phase::Error);
        CHECK(sw.local.save->content == "A1");
        CHECK(sw.engine->record().baseRevision == 1);
        sw.local.applyFailure = LocalStatus::Ok;
        sw.engine->syncNow(sw.now);
        sw.run(2000);
        CHECK(sw.local.save->content == "A2");
        CHECK(sw.phase() == Phase::Idle);
    }
}

TEST(remount_failure_needs_restart) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    pc.run(2000);
    Device sw{backend, "Switch"};
    sw.local.applyFailure = LocalStatus::RemountFailed;
    sw.run(2000);
    CHECK(sw.engine->status().restartRequired);
    CHECK(sw.local.save && sw.local.save->content == "A1");
    CHECK(sw.engine->record().baseRevision == 1);
    sw.local.applyFailure = LocalStatus::Ok;
    sw.restart("Switch");
    backend.calls.clear();
    sw.run(2000);
    CHECK(sw.phase() == Phase::Idle);
    CHECK(backend.calls["getChunk"] == 0);
}

TEST(busy_card_retries) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    pc.local.busy = true;
    pc.run(1000);
    CHECK(pc.phase() == Phase::Retrying);
    CHECK(head_of(backend) == nullptr);
    pc.local.busy = false;
    pc.run(10000);
    CHECK(head_of(backend) && head_of(backend)->digest == pc.digest());
}

TEST(no_disc_is_idle) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.noDisc = true;
    pc.run(2000);
    CHECK(pc.phase() == Phase::Idle);
    CHECK(backend.calls.empty());
}

// ---- in game, menu, conversion ------------------------------------------------------------------

TEST(in_game_waits_for_menu) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    pc.run(2000);
    Device sw{backend, "Switch"};
    sw.run(2000);
    pc.local.play("A2");
    pc.engine->onSaveWritten(pc.now);
    pc.run(6000);
    sw.local.inGame = true;
    sw.engine->onSessionStart(sw.now);
    sw.run(2000);
    CHECK(sw.phase() == Phase::WaitingForMenu);
    CHECK(sw.local.save->content == "A1");
    const int downloads = backend.calls["getChunk"];
    sw.run(20000);
    CHECK(backend.calls["getChunk"] == downloads);  // retries the apply, not the download
    sw.local.inGame = false;
    sw.engine->onSessionStart(sw.now);
    sw.run(1000);
    CHECK(sw.local.save->content == "A2");
    CHECK(sw.phase() == Phase::Idle);
}

TEST(in_game_save_while_waiting_becomes_conflict) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    pc.run(2000);
    Device sw{backend, "Switch"};
    sw.run(2000);
    pc.local.play("A2");
    pc.engine->onSaveWritten(pc.now);
    pc.run(6000);
    sw.local.inGame = true;
    sw.engine->onSessionStart(sw.now);
    sw.run(2000);
    CHECK(sw.phase() == Phase::WaitingForMenu);
    sw.local.play("SW-ingame");  // the player keeps playing and saves
    sw.local.inGame = false;
    sw.engine->onSessionStart(sw.now);
    sw.run(3000);
    CHECK(sw.local.save->content == "SW-ingame");  // never clobbered
    CHECK(sw.phase() == Phase::Conflict);
    CHECK(sw.engine->status().conflict.kind == ConflictKind::Diverged);
}

TEST(region_conversion_needs_confirmation) {
    FakeBackend backend;
    Device pal{backend, "PAL"};
    pal.local.game = "GZ2P";
    pal.local.play("P1");
    pal.run(2000);
    Device ntsc{backend, "NTSC"};
    ntsc.run(2000);
    CHECK(ntsc.phase() == Phase::Conflict);
    CHECK(ntsc.engine->status().conflict.kind == ConflictKind::NeedsConversion);
    CHECK(!ntsc.local.save);
    ntsc.engine->resolve(Choice::KeepRemote, ntsc.now);
    ntsc.run(2000);
    CHECK(ntsc.local.save && ntsc.local.save->game == "GZ2E" && ntsc.local.save->content == "P1");
    CHECK(ntsc.phase() == Phase::Idle);
    // The converted save differs from the cloud digest, but it is the agreed base: no upload loop.
    backend.calls.clear();
    ntsc.engine->onSessionStart(ntsc.now);
    ntsc.run(5000);
    CHECK(backend.calls["commitHead"] == 0);
    CHECK(ntsc.phase() == Phase::Idle);
    // Another game entirely is refused.
    FakeBackend b2;
    Device wind{b2, "Wind"};
    wind.local.game = "GALE";
    wind.local.play("W");
    wind.run(2000);
    Device tp{b2, "TP"};
    tp.run(2000);
    CHECK(tp.phase() == Phase::Error);
    CHECK(!tp.local.save);
}

// ---- accounts, deletion, retention, async -------------------------------------------------------

TEST(account_switch_resets_base) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    pc.run(2000);
    backend.account = "user-2";
    pc.engine->onAccountChanged(pc.now);
    pc.run(2000);
    CHECK(pc.engine->record().accountId == "user-2");
    CHECK(head_of(backend) && head_of(backend)->revision == 1);  // first upload to user-2
    backend.account = "user-1";
    pc.engine->onAccountChanged(pc.now);
    pc.run(2000);
    // Back to user-1: the record was for user-2, so no common base, but the content matches.
    CHECK(pc.phase() == Phase::Idle);
}

TEST(delete_remote) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    pc.run(2000);
    pc.local.play("A2");
    pc.engine->onSaveWritten(pc.now);
    pc.run(6000);
    pc.engine->deleteRemote(pc.now);
    pc.run(2000);
    CHECK(head_of(backend) == nullptr);
    CHECK(backend.chunks.empty());
    CHECK(pc.phase() == Phase::Disabled);
    CHECK(!pc.engine->enabled());
    CHECK(pc.local.save->content == "A2");
    pc.run(30000);
    CHECK(head_of(backend) == nullptr);  // not re-uploaded
}

TEST(retention_drops_old_fragments) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    std::vector<std::string> ids;
    for (int i = 0; i < 8; ++i) {
        pc.local.play("R" + std::to_string(i));
        pc.engine->onSaveWritten(pc.now);
        pc.run(6000);
        ids.push_back(head_of(backend)->snapshotId);
    }
    CHECK(head_of(backend)->revision == 8);
    CHECK(head_of(backend)->history.size() == kRemoteHistory);
    CHECK(backend.chunks.size() == kRemoteHistory);
    CHECK(backend.chunkCountFor("GZ2-01-gczelda2", ids[0]) == 0);
    CHECK(backend.chunkCountFor("GZ2-01-gczelda2", ids[7]) == 1);
}

TEST(async_disable_drops_stale_callbacks) {
    FakeBackend backend;
    backend.deferred = true;
    Device pc{backend, "PC"};
    pc.local.play("A1");
    pc.engine->syncNow(pc.now);
    pc.engine->tick(pc.now);  // getHead queued
    CHECK(pc.engine->busy());
    pc.engine->setEnabled(false, pc.now);
    backend.pump();
    CHECK(head_of(backend) == nullptr);
    CHECK(pc.phase() == Phase::Disabled);
    pc.engine->setEnabled(true, pc.now);
    pc.run(2000);
    CHECK(head_of(backend) && head_of(backend)->digest == pc.digest());
}

TEST(pc_switch_android_round_trip) {
    FakeBackend backend;
    Device pc{backend, "PC"};
    Device sw{backend, "Switch"};
    Device an{backend, "Android"};
    pc.local.play("forest");
    pc.engine->onSaveWritten(pc.now);
    pc.run(6000);
    an.engine->onSessionStart(an.now);
    an.run(2000);
    an.local.play("mines");
    an.engine->onSaveWritten(an.now);
    an.run(6000);
    sw.engine->onSessionStart(sw.now);
    sw.run(2000);
    sw.local.play("lakebed");
    sw.engine->onSaveWritten(sw.now);
    sw.run(6000);
    pc.engine->onSessionStart(pc.now);
    pc.run(2000);
    CHECK(pc.local.save->content == "lakebed");
    CHECK(an.local.save->content == "mines");
    CHECK(head_of(backend)->revision == 3);
    CHECK(head_of(backend)->deviceName == "Switch");
    CHECK(pc.phase() == Phase::Idle && sw.phase() == Phase::Idle);
}

TEST(record_roundtrip) {
    SyncRecord record{.key = "k", .accountId = "a", .baseDigest = "d", .baseRevision = 42,
        .baseSnapshotId = "s", .pendingUpload = true, .orphanSnapshotId = "o", .orphanChunks = 3,
        .lastSyncWallMs = 1'700'000'000'123};
    const auto parsed = SyncRecord::parse(record.serialize());
    CHECK(parsed && parsed->baseRevision == 42 && parsed->orphanChunks == 3 &&
          parsed->lastSyncWallMs == 1'700'000'000'123 && parsed->pendingUpload &&
          parsed->baseDigest == "d" && parsed->orphanSnapshotId == "o");
    CHECK(!SyncRecord::parse("{\"v\":2}"));
    CHECK(!SyncRecord::parse("not json"));
}

}  // namespace

int main() {
    for (const auto& [name, test] : tests()) {
        g_test = name;
        test();
    }
    std::printf("sync_engine_test: %zu tests, %d checks, %d failures\n", tests().size(), g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("sync_engine_test: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
