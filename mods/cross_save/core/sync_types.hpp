#pragma once

// Provider-neutral contracts of the cross-save mod. Nothing here knows about Firebase, HTTP or the
// mod SDK: the engine (sync_engine.hpp) talks to a LocalStore (the host's SaveSnapshotService on a
// real build, a fake in tests) and a RemoteBackend (Firestore on a real build, an in-memory fake in
// tests).

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace crosssave {

using Bytes = std::vector<uint8_t>;

// Snapshot blobs are split into fragments of at most this size. Firestore stores a document of at
// most 1 MiB and carries bytes as base64 (4/3), and HttpService buffers 1 MiB by default.
constexpr size_t kChunkBytes = 512 * 1024;
// A Twilight Princess save is tens of KiB; anything near this is not a save.
constexpr size_t kMaxSnapshotBytes = 16 * 1024 * 1024;
// Remote snapshots kept per save (the head plus older ones), so a conflict never loses a copy.
constexpr size_t kRemoteHistory = 5;

// ---- local save ---------------------------------------------------------------------------------

enum class LocalStatus {
    Ok,
    NoDisc,
    NoCard,
    Busy,
    InGame,
    NoSave,
    Invalid,
    Incompatible,
    NeedsConversion,
    LocalChanged,
    BackupFailed,
    WriteFailed,
    ModDataFailed,
    RemountFailed,
    IoError,
    Deferred,  // compatibility mode: staged, applied the next time the game starts
};

enum class Compat {
    Exact,
    RegionChange,
    PlatformChange,
    Incompatible,
};

struct LocalIdentity {
    LocalStatus status = LocalStatus::Ok;
    std::string message;
    std::string key;  // sync key, e.g. "GZ2-01-gczelda2"
    std::string game;
    std::string saveName;
    bool hasSave = false;
    std::string digest;  // content digest; empty without a save
    uint64_t changeCount = 0;
    uint32_t modifiedTime = 0;  // GCI header time, seconds since 2000 (shown, never compared)
};

struct LocalSnapshot {
    LocalStatus status = LocalStatus::Ok;
    std::string message;
    std::string digest;
    Bytes bytes;  // the .dusksave
    std::string game;
    uint32_t modifiedTime = 0;
};

struct LocalCheck {
    LocalStatus status = LocalStatus::Ok;
    std::string message;
    std::string digest;  // inspect: of the snapshot; apply: of the save as written
    Compat compat = Compat::Exact;
    std::string backupName;
};

class LocalStore {
public:
    virtual ~LocalStore() = default;
    virtual LocalIdentity identity() = 0;
    virtual LocalSnapshot capture() = 0;
    virtual LocalCheck inspect(const Bytes& snapshot) = 0;
    // expectedDigest: nullopt = no check, "" = there must be no local save.
    virtual LocalCheck apply(
        const Bytes& snapshot, const std::optional<std::string>& expectedDigest, bool allowConversion) = 0;
    // The engine's persistent per-save record (JSON text).
    virtual std::optional<std::string> readRecord() = 0;
    virtual bool writeRecord(const std::string& text) = 0;
    // Compatibility mode only. A download staged by apply (Deferred) still waiting for a restart.
    virtual bool hasStagedApply() const { return false; }
    // Compatibility mode only: once, after a start that applied a staged download, the digest of
    // the save as written.
    virtual std::optional<std::string> takeStartupApply() { return std::nullopt; }
};

// ---- remote -------------------------------------------------------------------------------------

enum class RemoteError {
    None,
    Network,   // transport failure or 5xx: retry with backoff
    Auth,      // not signed in, or the session cannot be refreshed: needs the player
    Quota,     // provider quota/rate limit: retry much later
    Conflict,  // compare-and-swap lost: re-read and decide again, never overwrite blindly
    Invalid,   // malformed or unexpected data
    Denied,    // rules refused the request (wrong account, revoked device)
};

struct HistoryEntry {
    std::string snapshotId;
    int64_t revision = 0;
    std::string digest;
    std::string blobSha256;
    uint64_t size = 0;
    uint32_t chunkCount = 0;
    std::string deviceName;
    int64_t createdAtMs = 0;
};

struct RemoteHead {
    int64_t revision = 0;
    std::string snapshotId;
    std::string digest;      // content digest of the save (what "changed" compares)
    std::string blobSha256;  // SHA-256 of the transferred .dusksave bytes
    uint64_t size = 0;
    uint32_t chunkCount = 0;
    std::string deviceId;
    std::string deviceName;
    std::string platform;
    std::string game;
    int64_t createdAtMs = 0;  // device clock: shown, never compared
    std::string format = "dusksave-1";
    std::vector<HistoryEntry> history;  // newest first, includes the head
    // Opaque compare-and-swap token from the provider (Firestore: the document's updateTime).
    std::string casToken;
};

using HeadCallback = std::function<void(RemoteError, std::optional<RemoteHead>, std::string message)>;
using ChunkCallback = std::function<void(RemoteError, Bytes, std::string message)>;
using DoneCallback = std::function<void(RemoteError, std::string message)>;
using CommitCallback = std::function<void(RemoteError, RemoteHead committed, std::string message)>;

// Callbacks may run before the call returns (a synchronous transport) or later, on the same
// thread. Exactly once each.
class RemoteBackend {
public:
    virtual ~RemoteBackend() = default;
    // Empty when nobody is signed in. Saves live under this account.
    virtual std::string accountId() const = 0;
    virtual void getHead(const std::string& key, HeadCallback done) = 0;
    // Fragments are immutable: writing an existing one fails.
    virtual void putChunk(const std::string& key, const std::string& snapshotId, uint32_t index,
        const Bytes& data, const std::string& sha256, DoneCallback done) = 0;
    virtual void getChunk(
        const std::string& key, const std::string& snapshotId, uint32_t index, ChunkCallback done) = 0;
    // Atomically replaces the head with next if the current head is still expected (nullopt: if
    // there is no head). Conflict otherwise.
    virtual void commitHead(const std::string& key, const std::optional<RemoteHead>& expected,
        const RemoteHead& next, CommitCallback done) = 0;
    // Best effort: removes a snapshot's fragments (orphans of an interrupted upload, or history
    // that fell out of retention).
    virtual void deleteSnapshot(const std::string& key, const std::string& snapshotId,
        uint32_t chunkCount, DoneCallback done) = 0;
    // Removes the head if it is still expected (Conflict otherwise). Used to delete synced data.
    virtual void deleteHead(const std::string& key, const RemoteHead& expected, DoneCallback done) = 0;
};

}  // namespace crosssave
