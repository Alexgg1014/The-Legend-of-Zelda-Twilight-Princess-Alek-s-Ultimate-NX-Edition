#include "sync_engine.hpp"

#include "json.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <span>

namespace crosssave {
namespace {

constexpr int kRecordVersion = 1;

uint32_t chunk_count(size_t size) {
    return static_cast<uint32_t>((size + kChunkBytes - 1) / kChunkBytes);
}

std::string revision_text(int64_t revision) {
    return std::to_string(static_cast<long long>(revision));
}

std::string device_of(const RemoteHead& head) {
    return head.deviceName.empty() ? std::string{"another device"} : head.deviceName;
}

}  // namespace

// ---- record -------------------------------------------------------------------------------------

std::string SyncRecord::serialize() const {
    json::Value value{json::Object{}};
    value["v"] = json::Value{kRecordVersion};
    value["key"] = key;
    value["account"] = accountId;
    value["baseDigest"] = baseDigest;
    value["baseRevision"] = json::Value{static_cast<double>(baseRevision)};
    value["baseSnapshot"] = baseSnapshotId;
    value["pendingUpload"] = json::Value{pendingUpload};
    value["orphan"] = orphanSnapshotId;
    value["orphanChunks"] = json::Value{static_cast<double>(orphanChunks)};
    value["lastSync"] = json::Value{static_cast<double>(lastSyncWallMs)};
    value["deferredRevision"] = json::Value{static_cast<double>(deferredRevision)};
    value["deferredSnapshot"] = deferredSnapshotId;
    return json::dump(value);
}

std::optional<SyncRecord> SyncRecord::parse(const std::string& text) {
    const auto value = json::parse(text, 64 * 1024);
    if (!value || !value->is_object() || value->operator[]("v").as_number() != kRecordVersion) {
        return std::nullopt;
    }
    const auto& v = *value;
    SyncRecord record;
    record.key = v.string_or("key");
    record.accountId = v.string_or("account");
    record.baseDigest = v.string_or("baseDigest");
    record.baseRevision = static_cast<int64_t>(v["baseRevision"].as_number());
    record.baseSnapshotId = v.string_or("baseSnapshot");
    record.pendingUpload = v["pendingUpload"].as_bool();
    record.orphanSnapshotId = v.string_or("orphan");
    record.orphanChunks = static_cast<uint32_t>(v["orphanChunks"].as_number());
    record.lastSyncWallMs = static_cast<int64_t>(v["lastSync"].as_number());
    record.deferredRevision = static_cast<int64_t>(v["deferredRevision"].as_number());
    record.deferredSnapshotId = v.string_or("deferredSnapshot");
    if (record.baseRevision < 0) {
        return std::nullopt;
    }
    return record;
}

// ---- engine -------------------------------------------------------------------------------------

SyncEngine::SyncEngine(LocalStore& local, RemoteBackend& remote, EngineConfig config)
    : m_local{local}, m_remote{remote}, m_config{std::move(config)} {
    if (!m_config.wallClockMs) {
        m_config.wallClockMs = [] { return int64_t{0}; };
    }
    if (!m_config.random) {
        m_config.random = [state = uint64_t{0x9e3779b97f4a7c15}]() mutable {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return state;
        };
    }
    m_status.message = "Cloud sync is off.";
}

void SyncEngine::setEnabled(bool enabled, int64_t now) {
    m_now = now;
    if (enabled == m_enabled) {
        return;
    }
    m_enabled = enabled;
    if (!enabled) {
        ++m_job;  // in-flight callbacks become stale
        m_dueAt.reset();
        m_debounceStart.reset();
        m_pending.reset();
        m_resolution.reset();
        m_status.conflict = {};
        stop(Phase::Disabled, "Cloud sync is off.");
        return;
    }
    onAccountChanged(now);
}

void SyncEngine::setAutomatic(bool automatic) {
    m_automatic = automatic;
}

void SyncEngine::schedule(int64_t at) {
    m_dueAt = m_dueAt ? std::min(*m_dueAt, at) : at;
    m_status.nextAttemptAtMs = *m_dueAt;
}

void SyncEngine::onSaveWritten(int64_t now) {
    m_now = now;
    if (!m_enabled) {
        return;
    }
    m_status.localChangesPending = true;
    m_status.attempts = 0;
    if (!m_automatic) {
        if (!m_busy && m_status.phase == Phase::Idle) {
            m_status.message = "Local changes are not synced yet (manual mode).";
        }
        return;
    }
    if (m_status.phase == Phase::Conflict) {
        return;  // a conflict waits for the player; saving again does not resolve it
    }
    if (!m_debounceStart) {
        m_debounceStart = now;
    }
    // A later save pushes the upload back, up to debounceMaxMs after the first one.
    m_dueAt = std::min(now + m_config.debounceMs, *m_debounceStart + m_config.debounceMaxMs);
    m_status.nextAttemptAtMs = *m_dueAt;
    if (!m_busy && m_status.phase != Phase::WaitingForMenu) {
        m_status.phase = Phase::Scheduled;
        m_status.message = "Saved. Syncing shortly.";
    }
}

void SyncEngine::onSessionStart(int64_t now) {
    m_now = now;
    if (!m_enabled) {
        return;
    }
    m_status.attempts = 0;
    if (m_pending) {
        schedule(now);  // a download already decided waits for this moment
        return;
    }
    if (m_automatic && m_status.phase != Phase::Conflict) {
        schedule(now);
        if (!m_busy) {
            m_status.phase = Phase::Scheduled;
        }
    }
}

void SyncEngine::onAccountChanged(int64_t now) {
    m_now = now;
    ++m_job;
    m_busy = false;
    m_pending.reset();
    m_resolution.reset();
    m_conflictDismissed = false;
    m_deleteRequested = false;
    m_status.conflict = {};
    m_status.attempts = 0;
    if (!m_enabled) {
        stop(Phase::Disabled, "Cloud sync is off.");
        return;
    }
    if (m_remote.accountId().empty()) {
        stop(Phase::SignedOut, "Link an account to sync this save.");
        return;
    }
    m_status.phase = Phase::Scheduled;
    m_status.message = "Checking the cloud copy...";
    schedule(now);
}

void SyncEngine::onNetworkRestored(int64_t now) {
    m_now = now;
    if (!m_enabled || m_busy) {
        return;
    }
    if (m_status.phase == Phase::Retrying || m_status.phase == Phase::Error) {
        m_status.attempts = 0;
        schedule(now);
    }
}

void SyncEngine::syncNow(int64_t now) {
    m_now = now;
    if (!m_enabled) {
        return;
    }
    m_conflictDismissed = false;
    m_status.attempts = 0;
    schedule(now);
    if (!m_busy) {
        m_status.phase = Phase::Scheduled;
        m_status.message = "Syncing...";
    }
}

void SyncEngine::resolve(Choice choice, int64_t now) {
    m_now = now;
    if (m_status.phase != Phase::Conflict) {
        return;
    }
    const auto kind = m_status.conflict.kind;
    if (choice == Choice::Cancel) {
        m_conflictDismissed = true;
        m_pending.reset();
        m_status.message =
            "Both copies were kept. Choose one from the Cloud Save panel, or press Sync now.";
        return;
    }
    m_conflictDismissed = false;
    m_status.attempts = 0;
    if (kind == ConflictKind::NeedsConversion && choice == Choice::KeepRemote && m_pending) {
        m_pending->allowConversion = true;
    } else {
        m_pending.reset();
        m_resolution = choice;
        m_resolutionAllowsConversion = false;
    }
    m_status.phase = Phase::Scheduled;
    m_status.message = choice == Choice::KeepLocal ? "Keeping this device's save..." :
                                                     "Restoring the cloud save...";
    schedule(now);
}

void SyncEngine::deleteRemote(int64_t now) {
    m_now = now;
    if (!m_enabled || m_remote.accountId().empty()) {
        return;
    }
    m_deleteRequested = true;
    m_pending.reset();
    m_resolution.reset();
    schedule(now);
}

void SyncEngine::tick(int64_t now) {
    m_now = now;
    if (m_busy || !m_dueAt || now < *m_dueAt) {
        return;
    }
    m_dueAt.reset();
    m_debounceStart.reset();
    runCycle();
}

// ---- cycle --------------------------------------------------------------------------------------

void SyncEngine::runCycle() {
    if (!m_enabled) {
        stop(Phase::Disabled, "Cloud sync is off.");
        return;
    }
    if (m_remote.accountId().empty()) {
        stop(Phase::SignedOut, "Link an account to sync this save.");
        return;
    }
    m_busy = true;
    const uint64_t job = ++m_job;
    m_status.phase = Phase::Working;
    m_status.activity = Activity::Checking;
    m_status.message = "Checking the cloud copy...";
    m_identity = m_local.identity();
    if (m_identity.status != LocalStatus::Ok) {
        const bool missing = m_identity.status == LocalStatus::NoDisc ||
                             m_identity.status == LocalStatus::NoCard;
        stop(missing ? Phase::Idle : Phase::Error,
            m_identity.message.empty() ? std::string{"The local save is not available."} :
                                         m_identity.message);
        return;
    }
    loadRecord(m_identity.key);
    m_status.localDigest = m_identity.digest;
    if (m_record.deferredRevision > 0) {
        if (auto applied = m_local.takeStartupApply()) {
            // Applied at this start (compatibility mode): that revision is the agreed base.
            m_record.baseDigest = *applied;
            m_record.baseRevision = m_record.deferredRevision;
            m_record.baseSnapshotId = m_record.deferredSnapshotId;
            m_record.pendingUpload = false;
            m_record.lastSyncWallMs = m_config.wallClockMs();
            m_status.lastSyncWallMs = m_record.lastSyncWallMs;
            m_record.deferredRevision = 0;
            m_record.deferredSnapshotId.clear();
            saveRecord();
        } else if (m_local.hasStagedApply()) {
            stop(Phase::WaitingForMenu, "A newer cloud save is ready. Restart the game to apply it.");
            return;
        } else {
            // Refused or failed at start (the save changed meanwhile): decide again.
            m_record.deferredRevision = 0;
            m_record.deferredSnapshotId.clear();
            saveRecord();
        }
    }
    if (m_pending && !m_resolution && !m_deleteRequested) {
        // A download decided earlier (waiting for the menu, or a confirmed conversion).
        m_downloadHead = m_pending->head;
        m_downloadBytes = m_pending->bytes;
        m_downloadAllowsConversion = m_pending->allowConversion;
        m_downloadExpected = m_pending->expectedDigest;
        applyDownloaded();
        return;
    }
    m_remote.getHead(m_identity.key,
        [this, job](RemoteError error, std::optional<RemoteHead> head, std::string message) {
            if (stale(job)) {
                return;
            }
            if (error != RemoteError::None) {
                fail(error, message);
                return;
            }
            onHead(job, std::move(head));
        });
}

void SyncEngine::onHead(uint64_t job, std::optional<RemoteHead> head) {
    if (head) {
        m_status.remoteRevision = head->revision;
        m_status.remoteDevice = head->deviceName;
        m_status.remoteCreatedAtMs = head->createdAtMs;
    } else {
        m_status.remoteRevision = 0;
        m_status.remoteDevice.clear();
        m_status.remoteCreatedAtMs = 0;
    }
    if (!m_record.orphanSnapshotId.empty()) {
        const auto orphan = m_record.orphanSnapshotId;
        const bool committed =
            head && (head->snapshotId == orphan ||
                        std::ranges::any_of(head->history,
                            [&](const HistoryEntry& entry) { return entry.snapshotId == orphan; }));
        if (committed && head->snapshotId == orphan && head->deviceId == m_config.deviceId) {
            // The commit went through but its answer was lost: this device's upload is the head.
            m_record.orphanSnapshotId.clear();
            m_record.orphanChunks = 0;
            adopt(*head, head->digest);
        } else if (committed) {
            m_record.orphanSnapshotId.clear();
            m_record.orphanChunks = 0;
            saveRecord();
        } else {
            // Fragments of an upload that never became the head.
            const auto chunks = m_record.orphanChunks;
            const auto key = m_identity.key;
            m_deleting.push_back(orphan);
            m_remote.deleteSnapshot(key, orphan, chunks,
                [this, key, orphan](RemoteError error, std::string) {
                    std::erase(m_deleting, orphan);
                    if (error == RemoteError::None && m_record.key == key &&
                        m_record.orphanSnapshotId == orphan)
                    {
                        m_record.orphanSnapshotId.clear();
                        m_record.orphanChunks = 0;
                        saveRecord();
                    }
                });
            if (stale(job)) {
                return;
            }
        }
    }
    if (m_deleteRequested) {
        startDelete(job, std::move(head));
        return;
    }
    decide(head);
}

void SyncEngine::decide(const std::optional<RemoteHead>& head) {
    const std::string local = m_identity.hasSave ? m_identity.digest : std::string{};
    if (m_resolution) {
        const Choice choice = *m_resolution;
        m_resolution.reset();
        if (choice == Choice::KeepLocal) {
            if (local.empty()) {
                stop(Phase::Error, "There is no save on this device to keep.");
                return;
            }
            startUpload(head);
            return;
        }
        if (!head) {
            stop(Phase::Error, "There is no cloud copy to restore.");
            return;
        }
        startDownload(*head, m_resolutionAllowsConversion);
        return;
    }
    if (!head) {
        if (local.empty()) {
            succeed("No save to sync yet.");
            return;
        }
        if (m_record.baseRevision == 0) {
            startUpload(std::nullopt);  // first upload of this save to the account
            return;
        }
        conflict(ConflictKind::RemoteMissing, head);
        return;
    }
    if (!local.empty() && local == head->digest) {
        adopt(*head, local);
        succeed("Up to date.");
        return;
    }
    if (local.empty()) {
        if (m_record.baseRevision == 0 && m_record.baseDigest.empty()) {
            startDownload(*head, false);  // new device: take the cloud save
            return;
        }
        conflict(ConflictKind::LocalMissing, head);
        return;
    }
    if (m_record.baseRevision == 0) {
        conflict(ConflictKind::NoCommonBase, head);
        return;
    }
    if (head->revision < m_record.baseRevision) {
        conflict(ConflictKind::RemoteRewound, head);
        return;
    }
    const bool localChanged = local != m_record.baseDigest;
    const bool remoteChanged =
        head->revision != m_record.baseRevision || head->snapshotId != m_record.baseSnapshotId;
    if (!localChanged && !remoteChanged) {
        succeed("Up to date.");
    } else if (!localChanged) {
        startDownload(*head, false);
    } else if (!remoteChanged) {
        startUpload(head);
    } else {
        conflict(ConflictKind::Diverged, head);
    }
}

// ---- upload -------------------------------------------------------------------------------------

void SyncEngine::startUpload(const std::optional<RemoteHead>& expected) {
    m_status.activity = Activity::Uploading;
    m_status.message = "Uploading this save...";
    m_snapshot = m_local.capture();
    switch (m_snapshot.status) {
    case LocalStatus::Ok:
        break;
    case LocalStatus::Busy:
        retryLater("The memory card is busy.");
        return;
    case LocalStatus::NoSave:
        succeed("No save to upload.");
        return;
    default:
        stop(Phase::Error, m_snapshot.message.empty() ? std::string{"The save could not be read."} :
                                                        m_snapshot.message);
        return;
    }
    if (m_snapshot.bytes.empty() || m_snapshot.bytes.size() > kMaxSnapshotBytes) {
        stop(Phase::Error, "The save is too large to sync.");
        return;
    }
    m_expected = expected;
    m_idCollisions = 0;
    m_snapshotId = newSnapshotId();
    m_blobSha = sha256_hex(m_snapshot.bytes);
    m_record.orphanSnapshotId = m_snapshotId;
    m_record.orphanChunks = chunk_count(m_snapshot.bytes.size());
    m_record.pendingUpload = true;
    saveRecord();
    putChunk(m_job, 0);
}

void SyncEngine::putChunk(uint64_t job, size_t index) {
    const size_t count = chunk_count(m_snapshot.bytes.size());
    if (index == count) {
        commitUpload(job);
        return;
    }
    const size_t offset = index * kChunkBytes;
    const size_t size = std::min(kChunkBytes, m_snapshot.bytes.size() - offset);
    const std::span<const uint8_t> piece{m_snapshot.bytes.data() + offset, size};
    Bytes data{piece.begin(), piece.end()};
    const auto sha = sha256_hex(piece);
    m_remote.putChunk(m_identity.key, m_snapshotId, static_cast<uint32_t>(index), data, sha,
        [this, job, index](RemoteError error, std::string message) {
            if (stale(job)) {
                return;
            }
            if (error == RemoteError::Conflict && index == 0 && ++m_idCollisions <= 3) {
                // The ID is taken (a repeated random sequence after a restart): never write into
                // someone else's snapshot, take a new ID.
                m_snapshotId = newSnapshotId();
                m_record.orphanSnapshotId = m_snapshotId;
                saveRecord();
                putChunk(job, 0);
                return;
            }
            if (error != RemoteError::None) {
                fail(error == RemoteError::Conflict ? RemoteError::Invalid : error, message);
                return;
            }
            putChunk(job, index + 1);
        });
}

void SyncEngine::commitUpload(uint64_t job) {
    RemoteHead next;
    next.revision = (m_expected ? m_expected->revision : m_record.baseRevision) + 1;
    next.snapshotId = m_snapshotId;
    next.digest = m_snapshot.digest;
    next.blobSha256 = m_blobSha;
    next.size = m_snapshot.bytes.size();
    next.chunkCount = chunk_count(m_snapshot.bytes.size());
    next.deviceId = m_config.deviceId;
    next.deviceName = m_config.deviceName;
    next.platform = m_config.platform;
    next.game = m_snapshot.game;
    next.createdAtMs = m_config.wallClockMs();
    next.history.push_back({
        .snapshotId = next.snapshotId,
        .revision = next.revision,
        .digest = next.digest,
        .blobSha256 = next.blobSha256,
        .size = next.size,
        .chunkCount = next.chunkCount,
        .deviceName = next.deviceName,
        .createdAtMs = next.createdAtMs,
    });
    std::vector<HistoryEntry> dropped;
    if (m_expected) {
        for (const auto& entry : m_expected->history) {
            if (entry.snapshotId == next.snapshotId) {
                continue;
            }
            if (next.history.size() < kRemoteHistory) {
                next.history.push_back(entry);
            } else {
                dropped.push_back(entry);
            }
        }
    }
    m_remote.commitHead(m_identity.key, m_expected, next,
        [this, job, dropped = std::move(dropped)](
            RemoteError error, RemoteHead committed, std::string message) mutable {
            if (stale(job)) {
                return;
            }
            if (error == RemoteError::Conflict) {
                // Someone else moved the head: re-read and decide again. The fragments stay
                // recorded as an orphan and are removed by the next cycle.
                m_busy = false;
                if (++m_casRetries > m_config.maxCasRetries) {
                    m_casRetries = 0;
                    retryLater("The cloud copy keeps changing.");
                    return;
                }
                m_status.phase = Phase::Scheduled;
                m_status.message = "The cloud copy changed. Checking again...";
                schedule(m_now);
                return;
            }
            if (error != RemoteError::None) {
                fail(error, message);
                return;
            }
            m_record.orphanSnapshotId.clear();
            m_record.orphanChunks = 0;
            adopt(committed, m_snapshot.digest);
            m_status.remoteRevision = committed.revision;
            m_status.remoteDevice = committed.deviceName;
            m_status.remoteCreatedAtMs = committed.createdAtMs;
            cleanupDropped(std::move(dropped));
            succeed("Uploaded revision " + revision_text(committed.revision) + ".");
        });
}

// ---- download -----------------------------------------------------------------------------------

void SyncEngine::startDownload(const RemoteHead& head, bool allowConversion) {
    m_status.activity = Activity::Downloading;
    m_status.message = "Downloading the cloud save from " + device_of(head) + "...";
    if (head.size == 0 || head.size > kMaxSnapshotBytes || head.chunkCount != chunk_count(head.size) ||
        head.blobSha256.size() != 64 || head.snapshotId.empty())
    {
        stop(Phase::Error, "The cloud copy is damaged (bad metadata). Nothing was changed.");
        return;
    }
    m_downloadHead = head;
    m_downloadBytes.clear();
    m_downloadBytes.reserve(head.size);
    m_downloadAllowsConversion = allowConversion;
    m_downloadExpected = m_identity.hasSave ? m_identity.digest : std::string{};
    getChunk(m_job, 0);
}

void SyncEngine::getChunk(uint64_t job, uint32_t index) {
    if (index == m_downloadHead.chunkCount) {
        if (m_downloadBytes.size() != m_downloadHead.size ||
            sha256_hex(m_downloadBytes) != m_downloadHead.blobSha256)
        {
            m_downloadBytes.clear();
            stop(Phase::Error, "The cloud copy failed its integrity check. Nothing was changed.");
            return;
        }
        applyDownloaded();
        return;
    }
    m_remote.getChunk(m_identity.key, m_downloadHead.snapshotId, index,
        [this, job, index](RemoteError error, Bytes data, std::string message) {
            if (stale(job)) {
                return;
            }
            if (error != RemoteError::None) {
                fail(error == RemoteError::Conflict ? RemoteError::Invalid : error, message);
                return;
            }
            const bool last = index + 1 == m_downloadHead.chunkCount;
            if (data.empty() || data.size() > kChunkBytes || (!last && data.size() != kChunkBytes) ||
                m_downloadBytes.size() + data.size() > m_downloadHead.size)
            {
                m_downloadBytes.clear();
                stop(Phase::Error, "The cloud copy is damaged (bad fragment). Nothing was changed.");
                return;
            }
            m_downloadBytes.insert(m_downloadBytes.end(), data.begin(), data.end());
            getChunk(job, index + 1);
        });
}

void SyncEngine::applyDownloaded() {
    m_status.activity = Activity::Applying;
    m_status.message = "Applying the cloud save...";
    const RemoteHead head = m_downloadHead;
    const auto keepPending = [&] {
        m_pending = PendingDownload{
            .head = head,
            .bytes = m_downloadBytes,
            .allowConversion = m_downloadAllowsConversion,
            .expectedDigest = m_downloadExpected,
        };
    };
    const LocalCheck check = m_local.inspect(m_downloadBytes);
    switch (check.status) {
    case LocalStatus::Ok:
        break;
    case LocalStatus::NeedsConversion:
        if (!m_downloadAllowsConversion) {
            keepPending();
            conflict(ConflictKind::NeedsConversion, head);
            return;
        }
        break;
    case LocalStatus::Busy:
        keepPending();
        retryLater("The memory card is busy.");
        return;
    default:
        m_pending.reset();
        stop(Phase::Error, "The cloud copy cannot be used here: " + check.message);
        return;
    }
    const LocalCheck result =
        m_local.apply(m_downloadBytes, m_downloadExpected, m_downloadAllowsConversion);
    switch (result.status) {
    case LocalStatus::Ok:
    case LocalStatus::RemountFailed:
        m_pending.reset();
        m_downloadBytes.clear();
        m_status.lastBackup = result.backupName;
        adopt(head, result.digest);
        if (result.status == LocalStatus::RemountFailed) {
            m_status.restartRequired = true;
            stop(Phase::Error, "The cloud save was written. Restart the game before playing.");
            return;
        }
        succeed("Downloaded revision " + revision_text(head.revision) + " from " + device_of(head) +
                ".");
        return;
    case LocalStatus::InGame:
        keepPending();
        stop(Phase::WaitingForMenu, "A newer save from " + device_of(head) +
                                        " is ready. It is applied from the pre-launch menu.");
        schedule(m_now + m_config.menuRetryMs);
        return;
    case LocalStatus::Deferred:
        m_pending.reset();
        m_downloadBytes.clear();
        m_record.deferredRevision = head.revision;
        m_record.deferredSnapshotId = head.snapshotId;
        saveRecord();
        stop(Phase::WaitingForMenu, "A newer save from " + device_of(head) +
                                        " is ready. It is applied the next time the game starts.");
        return;
    case LocalStatus::LocalChanged:
        // The player saved after the decision: decide again (likely a conflict now).
        m_pending.reset();
        m_busy = false;
        m_status.phase = Phase::Scheduled;
        m_status.message = "This save changed. Checking again...";
        schedule(m_now);
        return;
    case LocalStatus::Busy:
        keepPending();
        retryLater("The memory card is busy.");
        return;
    case LocalStatus::NeedsConversion:
        keepPending();
        conflict(ConflictKind::NeedsConversion, head);
        return;
    default:
        m_pending.reset();
        stop(Phase::Error, "The cloud save could not be applied: " + result.message +
                               " This device's save was kept.");
        return;
    }
}

// ---- delete -------------------------------------------------------------------------------------

void SyncEngine::startDelete(uint64_t job, std::optional<RemoteHead> head) {
    m_deleteRequested = false;
    m_status.activity = Activity::Deleting;
    m_status.message = "Deleting the cloud copies...";
    const auto finish = [this] {
        const auto key = m_record.key;
        const auto account = m_record.accountId;
        m_record = SyncRecord{.key = key, .accountId = account};
        saveRecord();
        // Left on, sync would upload this device's save again right away.
        m_enabled = false;
        m_status.remoteRevision = 0;
        m_status.remoteDevice.clear();
        stop(Phase::Disabled, "Cloud copies deleted. Cloud sync is off on this device.");
    };
    if (!head) {
        finish();
        return;
    }
    auto entries = std::make_shared<std::vector<HistoryEntry>>(head->history);
    auto expected = std::make_shared<RemoteHead>(*head);
    // The head goes first, so no device can download a snapshot whose fragments are being removed.
    m_remote.deleteHead(m_identity.key, *expected,
        [this, job, entries, finish](RemoteError error, std::string message) {
            if (stale(job)) {
                return;
            }
            if (error == RemoteError::Conflict) {
                stop(Phase::Error, "The cloud copy changed while deleting. Try again.");
                return;
            }
            if (error != RemoteError::None) {
                fail(error, message);
                return;
            }
            cleanupDropped(*entries);
            finish();
        });
}

// ---- outcomes -----------------------------------------------------------------------------------

void SyncEngine::conflict(ConflictKind kind, const std::optional<RemoteHead>& head) {
    m_busy = false;
    m_status.phase = Phase::Conflict;
    m_status.activity = Activity::None;
    m_status.conflict = ConflictInfo{
        .kind = kind,
        .hasLocal = m_identity.hasSave,
        .localDigest = m_identity.digest,
        .localGame = m_identity.game,
        .localModifiedTime = m_identity.modifiedTime,
        .remote = head,
    };
    switch (kind) {
    case ConflictKind::Diverged:
        m_status.message = "This save and the cloud copy both changed. Choose which one to keep.";
        break;
    case ConflictKind::NoCommonBase:
        m_status.message =
            "This device and the account each have a different save. Choose which one to keep.";
        break;
    case ConflictKind::RemoteMissing:
        m_status.message = "The cloud copy was removed. Upload this device's save again?";
        break;
    case ConflictKind::RemoteRewound:
        m_status.message = "The cloud copy is older than the one this device last synced.";
        break;
    case ConflictKind::LocalMissing:
        m_status.message = "This device's save is gone. Restore the cloud copy?";
        break;
    case ConflictKind::NeedsConversion:
        m_status.message = "The cloud save comes from another region or platform. Convert it?";
        break;
    case ConflictKind::None:
        break;
    }
}

void SyncEngine::succeed(std::string message) {
    m_busy = false;
    m_casRetries = 0;
    m_conflictDismissed = false;
    m_status.phase = Phase::Idle;
    m_status.activity = Activity::None;
    m_status.attempts = 0;
    m_status.conflict = {};
    m_status.localChangesPending = m_record.pendingUpload;
    m_status.lastSyncWallMs = m_record.lastSyncWallMs;
    m_status.message = std::move(message);
}

void SyncEngine::stop(Phase phase, std::string message) {
    m_busy = false;
    m_status.phase = phase;
    m_status.activity = Activity::None;
    m_status.message = std::move(message);
}

void SyncEngine::fail(RemoteError error, const std::string& message) {
    switch (error) {
    case RemoteError::Network:
        retryLater("Offline, or the server did not answer.");
        return;
    case RemoteError::Quota:
        m_busy = false;
        m_status.phase = Phase::Retrying;
        m_status.activity = Activity::None;
        m_status.message = "The cloud service is over its free quota. Sync resumes later.";
        schedule(m_now + m_config.quotaBackoffMs);
        return;
    case RemoteError::Auth:
        stop(Phase::SignedOut, "The account session ended. Link this device again.");
        return;
    case RemoteError::Denied:
        stop(Phase::Error, "The cloud refused access (device unlinked, or another account).");
        return;
    case RemoteError::Conflict:
        m_busy = false;
        schedule(m_now);
        return;
    case RemoteError::Invalid:
        stop(Phase::Error, "Unexpected answer from the cloud" +
                               (message.empty() ? std::string{"."} : ": " + message));
        return;
    case RemoteError::None:
        break;
    }
    stop(Phase::Error, message);
}

void SyncEngine::retryLater(std::string message) {
    m_busy = false;
    m_status.activity = Activity::None;
    ++m_status.attempts;
    if (m_status.attempts >= m_config.maxAttempts) {
        m_status.phase = Phase::Error;
        m_status.message =
            message + " Sync paused until the next save, a return to the menu, or Sync now.";
        return;
    }
    m_status.phase = Phase::Retrying;
    m_status.message = std::move(message);
    schedule(m_now + backoffDelay());
}

int64_t SyncEngine::backoffDelay() const {
    int64_t delay = m_config.backoffBaseMs;
    for (int i = 1; i < m_status.attempts && delay < m_config.backoffMaxMs; ++i) {
        delay *= 2;
    }
    delay = std::min(delay, m_config.backoffMaxMs);
    // Up to +20% jitter, so devices that lost the network together do not retry together.
    const int64_t jitter = delay / 5;
    return delay + (jitter > 0 ? static_cast<int64_t>(m_config.random() % static_cast<uint64_t>(jitter)) : 0);
}

void SyncEngine::adopt(const RemoteHead& head, const std::string& localDigest) {
    m_record.baseDigest = localDigest;
    m_record.baseRevision = head.revision;
    m_record.baseSnapshotId = head.snapshotId;
    m_record.pendingUpload = false;
    m_record.lastSyncWallMs = m_config.wallClockMs();
    m_status.localDigest = localDigest;
    m_status.localChangesPending = false;
    saveRecord();
}

void SyncEngine::loadRecord(const std::string& key) {
    const auto account = m_remote.accountId();
    std::optional<SyncRecord> stored;
    if (auto text = m_local.readRecord()) {
        stored = SyncRecord::parse(*text);
    }
    if (stored && stored->key == key) {
        m_record = std::move(*stored);
    } else {
        m_record = SyncRecord{.key = key};
    }
    if (m_record.accountId != account) {
        // Another account: nothing agreed with it yet. Its orphans are not ours to clean.
        m_record = SyncRecord{.key = key, .accountId = account};
    }
}

void SyncEngine::saveRecord() {
    m_local.writeRecord(m_record.serialize());
}

void SyncEngine::cleanupDropped(std::vector<HistoryEntry> dropped) {
    if (dropped.empty()) {
        return;
    }
    auto queue = std::make_shared<std::vector<HistoryEntry>>(std::move(dropped));
    const auto key = m_identity.key;
    auto next = std::make_shared<std::function<void()>>();
    *next = [this, key, queue, next] {
        if (queue->empty()) {
            *next = nullptr;  // break the cycle
            return;
        }
        const auto entry = queue->back();
        queue->pop_back();
        m_deleting.push_back(entry.snapshotId);
        m_remote.deleteSnapshot(key, entry.snapshotId, entry.chunkCount,
            [this, next, id = entry.snapshotId](RemoteError, std::string) {
                std::erase(m_deleting, id);
                if (*next) {
                    (*next)();
                }
            });
    };
    (*next)();
}

std::string SyncEngine::newSnapshotId() {
    while (true) {
        std::array<uint8_t, 16> bytes{};
        for (size_t i = 0; i < bytes.size(); i += 8) {
            const uint64_t value = m_config.random();
            for (size_t j = 0; j < 8; ++j) {
                bytes[i + j] = static_cast<uint8_t>(value >> (8 * j));
            }
        }
        auto id = to_hex(bytes);
        // Never reuse an ID whose fragments are being deleted: the delete could land after the
        // new writes.
        if (std::ranges::find(m_deleting, id) == m_deleting.end()) {
            return id;
        }
    }
}

const char* phase_name(Phase phase) {
    switch (phase) {
    case Phase::Disabled:
        return "Off";
    case Phase::SignedOut:
        return "Not linked";
    case Phase::Idle:
        return "Synced";
    case Phase::Scheduled:
        return "Pending";
    case Phase::Working:
        return "Syncing";
    case Phase::WaitingForMenu:
        return "Waiting for the menu";
    case Phase::Conflict:
        return "Conflict";
    case Phase::Retrying:
        return "Retrying";
    case Phase::Error:
        return "Stopped";
    }
    return "?";
}

const char* conflict_name(ConflictKind kind) {
    switch (kind) {
    case ConflictKind::None:
        return "none";
    case ConflictKind::Diverged:
        return "diverged";
    case ConflictKind::NoCommonBase:
        return "no-common-base";
    case ConflictKind::RemoteMissing:
        return "remote-missing";
    case ConflictKind::RemoteRewound:
        return "remote-rewound";
    case ConflictKind::LocalMissing:
        return "local-missing";
    case ConflictKind::NeedsConversion:
        return "needs-conversion";
    }
    return "?";
}

}  // namespace crosssave
