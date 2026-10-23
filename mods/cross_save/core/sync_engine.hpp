#pragma once

// The cross-save decision engine: when to upload, download, ask, or wait. Provider-neutral and
// single-threaded (the game thread); driven by tick() and a few events. See
// docs/dev/cross-save-design.md, "Sincronización y conflictos", for the cases it implements.

#include "sync_types.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace crosssave {

struct EngineConfig {
    int64_t debounceMs = 4000;        // after a game save, wait this long for more saves
    int64_t debounceMaxMs = 20000;    // ...but never longer than this after the first one
    int64_t backoffBaseMs = 5000;     // first retry after a transport error
    int64_t backoffMaxMs = 10 * 60 * 1000;
    int64_t quotaBackoffMs = 60 * 60 * 1000;
    int64_t menuRetryMs = 5000;       // how often a download waiting for the menu is retried
    int maxAttempts = 8;              // transport retries before pausing until the next trigger
    int maxCasRetries = 3;            // immediate re-reads after a lost compare-and-swap
    std::string deviceId;
    std::string deviceName;
    std::string platform;
    std::function<int64_t()> wallClockMs;  // only for display fields (createdAt, last sync)
    std::function<uint64_t()> random;      // snapshot IDs and backoff jitter
};

enum class Phase {
    Disabled,        // sync is off (nothing linked or the player turned it off)
    SignedOut,       // enabled, but no account session: link again
    Idle,            // up to date, or nothing to do
    Scheduled,       // a sync will start soon (debounce, retry or manual request)
    Working,         // talking to the provider or the local save
    WaitingForMenu,  // a newer save is downloaded; it is applied from the pre-launch menu
    Conflict,        // the player must choose
    Retrying,        // transient failure; next attempt at nextAttemptAtMs
    Error,           // stopped until the next trigger (message says why)
};

enum class Activity { None, Checking, Uploading, Downloading, Applying, Deleting };

enum class ConflictKind {
    None,
    Diverged,         // both changed since the last sync
    NoCommonBase,     // this device never synced and both copies exist and differ
    RemoteMissing,    // synced before, but the remote copy is gone
    RemoteRewound,    // the remote revision went backwards (data reset elsewhere)
    LocalMissing,     // synced before, but the local save is gone
    NeedsConversion,  // the remote copy is from another region/platform
};

enum class Choice { KeepLocal, KeepRemote, Cancel };

struct ConflictInfo {
    ConflictKind kind = ConflictKind::None;
    bool hasLocal = false;
    std::string localDigest;
    std::string localGame;
    uint32_t localModifiedTime = 0;
    std::optional<RemoteHead> remote;
};

struct Status {
    Phase phase = Phase::Disabled;
    Activity activity = Activity::None;
    std::string message;
    int attempts = 0;
    int64_t nextAttemptAtMs = 0;
    int64_t lastSyncWallMs = 0;
    int64_t remoteRevision = 0;
    std::string remoteDevice;
    int64_t remoteCreatedAtMs = 0;
    std::string localDigest;
    bool localChangesPending = false;
    ConflictInfo conflict;
    std::string lastBackup;  // backup made by the last download
    bool restartRequired = false;
};

// What the engine persists per save, through LocalStore::writeRecord.
struct SyncRecord {
    std::string key;
    std::string accountId;
    std::string baseDigest;  // local content digest both sides agreed on
    int64_t baseRevision = 0;
    std::string baseSnapshotId;
    bool pendingUpload = false;
    std::string orphanSnapshotId;  // upload in progress or interrupted: fragments to clean up
    uint32_t orphanChunks = 0;
    int64_t deferredRevision = 0;  // a download staged for the next start (compatibility mode)
    std::string deferredSnapshotId;
    int64_t lastSyncWallMs = 0;

    std::string serialize() const;
    static std::optional<SyncRecord> parse(const std::string& text);
};

class SyncEngine {
public:
    SyncEngine(LocalStore& local, RemoteBackend& remote, EngineConfig config);

    void setEnabled(bool enabled, int64_t now);
    void setAutomatic(bool automatic);
    bool enabled() const { return m_enabled; }
    bool automatic() const { return m_automatic; }

    // A game save finished writing (SaveService on_save_written).
    void onSaveWritten(int64_t now);
    // The game started, or returned to the pre-launch menu.
    void onSessionStart(int64_t now);
    // The account session changed (signed in, signed out, switched).
    void onAccountChanged(int64_t now);
    void onNetworkRestored(int64_t now);
    void syncNow(int64_t now);
    void resolve(Choice choice, int64_t now);
    // Deletes every synced copy of this save from the account (local saves are untouched).
    void deleteRemote(int64_t now);
    void tick(int64_t now);

    const Status& status() const { return m_status; }
    bool busy() const { return m_busy; }
    const SyncRecord& record() const { return m_record; }

private:
    struct PendingDownload {
        RemoteHead head;
        Bytes bytes;
        bool allowConversion = false;
        std::string expectedDigest;  // the local digest the download was decided against
    };

    void schedule(int64_t at);
    void runCycle();
    void onHead(uint64_t job, std::optional<RemoteHead> head);
    void decide(const std::optional<RemoteHead>& head);
    void startUpload(const std::optional<RemoteHead>& expected);
    void putChunk(uint64_t job, size_t index);
    void commitUpload(uint64_t job);
    void startDownload(const RemoteHead& head, bool allowConversion);
    void getChunk(uint64_t job, uint32_t index);
    void applyDownloaded();
    void startDelete(uint64_t job, std::optional<RemoteHead> head);
    void conflict(ConflictKind kind, const std::optional<RemoteHead>& head);
    void succeed(std::string message);
    void stop(Phase phase, std::string message);
    void fail(RemoteError error, const std::string& message);
    void retryLater(std::string message);
    void adopt(const RemoteHead& head, const std::string& localDigest);
    void loadRecord(const std::string& key);
    void saveRecord();
    void cleanupDropped(std::vector<HistoryEntry> dropped);
    std::string newSnapshotId();
    int64_t backoffDelay() const;
    bool stale(uint64_t job) const { return job != m_job; }

    LocalStore& m_local;
    RemoteBackend& m_remote;
    EngineConfig m_config;
    bool m_enabled = false;
    bool m_automatic = true;
    bool m_busy = false;
    uint64_t m_job = 0;
    int64_t m_now = 0;
    std::optional<int64_t> m_dueAt;
    std::optional<int64_t> m_debounceStart;
    Status m_status;
    SyncRecord m_record;
    LocalIdentity m_identity;
    std::optional<Choice> m_resolution;
    bool m_resolutionAllowsConversion = false;
    bool m_conflictDismissed = false;
    bool m_deleteRequested = false;
    int m_casRetries = 0;
    int m_idCollisions = 0;
    std::vector<std::string> m_deleting;  // snapshot IDs with a delete in flight

    // Upload in flight.
    std::optional<RemoteHead> m_expected;
    LocalSnapshot m_snapshot;
    std::string m_snapshotId;
    std::string m_blobSha;
    // Download in flight / waiting for the menu.
    RemoteHead m_downloadHead;
    Bytes m_downloadBytes;
    bool m_downloadAllowsConversion = false;
    std::string m_downloadExpected;  // "" = there was no local save
    std::optional<PendingDownload> m_pending;
};

const char* phase_name(Phase phase);
const char* conflict_name(ConflictKind kind);

}  // namespace crosssave
