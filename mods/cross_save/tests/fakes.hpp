#pragma once

// Test doubles for the sync engine: a device's save (FakeLocal) and an in-memory provider
// (FakeBackend) shared by several devices, both with injectable failures.

#include "../core/sha256.hpp"
#include "../core/sync_types.hpp"

#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace crosssave::test {

// ---- local --------------------------------------------------------------------------------------

// A save is its game ID plus content. The snapshot bytes are "DSNAP|<game>|<content>"; the content
// digest covers game and content, like the host's (a conversion rewrites the game ID).
struct FakeSave {
    std::string game = "GZ2E";
    std::string content;
};

inline std::string fake_digest(const FakeSave& save) {
    const std::string text = save.game + "|" + save.content;
    return sha256_hex(Bytes{text.begin(), text.end()});
}

class FakeLocal : public LocalStore {
public:
    std::string game = "GZ2E";  // the configured disc
    std::string key = "GZ2-01-gczelda2";
    std::optional<FakeSave> save;
    std::optional<std::string> record;
    uint64_t changes = 0;
    int backups = 0;
    std::vector<FakeSave> backupHistory;

    // Faults.
    bool noDisc = false;
    bool busy = false;
    bool inGame = false;
    LocalStatus applyFailure = LocalStatus::Ok;  // BackupFailed, WriteFailed, ModDataFailed, RemountFailed
    bool failRecordWrite = false;
    // Content with a size, to exercise fragments.
    size_t padTo = 0;

    int captures = 0;
    int applies = 0;

    void play(const std::string& content) {
        save = FakeSave{game, content};
        ++changes;
    }

    LocalIdentity identity() override {
        LocalIdentity id;
        if (noDisc) {
            id.status = LocalStatus::NoDisc;
            id.message = "No disc.";
            return id;
        }
        id.key = key;
        id.game = game;
        id.saveName = "gczelda2";
        id.changeCount = changes;
        if (save) {
            id.hasSave = true;
            id.digest = fake_digest(*save);
            id.modifiedTime = 1000;
        }
        return id;
    }

    Bytes encode(const FakeSave& value) const {
        std::string text = "DSNAP|" + value.game + "|" + value.content;
        if (text.size() < padTo) {
            text += "|";
            while (text.size() < padTo) {
                text.push_back(static_cast<char>('a' + text.size() % 26));
            }
        }
        return {text.begin(), text.end()};
    }

    static std::optional<FakeSave> decode(const Bytes& bytes) {
        const std::string text{bytes.begin(), bytes.end()};
        if (text.rfind("DSNAP|", 0) != 0 || text.size() < 11 || text[10] != '|') {
            return std::nullopt;
        }
        FakeSave value;
        value.game = text.substr(6, 4);
        value.content = text.substr(11);
        const auto pad = value.content.find('|');
        if (pad != std::string::npos) {
            value.content.resize(pad);
        }
        return value;
    }

    LocalSnapshot capture() override {
        ++captures;
        LocalSnapshot snapshot;
        if (busy) {
            snapshot.status = LocalStatus::Busy;
            return snapshot;
        }
        if (!save) {
            snapshot.status = LocalStatus::NoSave;
            return snapshot;
        }
        snapshot.digest = fake_digest(*save);
        snapshot.bytes = encode(*save);
        snapshot.game = save->game;
        return snapshot;
    }

    LocalCheck inspect(const Bytes& bytes) override {
        LocalCheck check;
        const auto value = decode(bytes);
        if (!value) {
            check.status = LocalStatus::Invalid;
            check.message = "not a snapshot";
            return check;
        }
        check.digest = fake_digest(*value);
        if (value->game.substr(0, 3) != game.substr(0, 3)) {
            check.status = LocalStatus::Incompatible;
            check.compat = Compat::Incompatible;
            check.message = "other game";
        } else if (value->game != game) {
            check.status = LocalStatus::NeedsConversion;
            check.compat = Compat::RegionChange;
        }
        return check;
    }

    LocalCheck apply(const Bytes& bytes, const std::optional<std::string>& expected,
        bool allowConversion) override {
        ++applies;
        LocalCheck result = inspect(bytes);
        if (result.status == LocalStatus::NeedsConversion && allowConversion) {
            result.status = LocalStatus::Ok;
        }
        if (result.status != LocalStatus::Ok) {
            return result;
        }
        if (inGame) {
            result.status = LocalStatus::InGame;
            return result;
        }
        if (busy) {
            result.status = LocalStatus::Busy;
            return result;
        }
        const std::string current = save ? fake_digest(*save) : std::string{};
        if (expected && *expected != current) {
            result.status = LocalStatus::LocalChanged;
            return result;
        }
        if (applyFailure == LocalStatus::BackupFailed) {
            result.status = applyFailure;
            result.message = "backup failed";
            return result;
        }
        if (save) {
            ++backups;
            backupHistory.push_back(*save);
            result.backupName = "backup-" + std::to_string(backups) + ".dusksave";
        }
        if (applyFailure == LocalStatus::WriteFailed || applyFailure == LocalStatus::ModDataFailed) {
            result.status = applyFailure;  // the host leaves (or puts back) the previous save
            result.message = "write failed";
            return result;
        }
        auto value = *decode(bytes);
        value.game = game;  // conversion rewrites the game ID
        save = value;
        ++changes;
        result.digest = fake_digest(value);
        result.status = applyFailure == LocalStatus::RemountFailed ? LocalStatus::RemountFailed : LocalStatus::Ok;
        return result;
    }

    std::optional<std::string> readRecord() override { return record; }
    bool writeRecord(const std::string& text) override {
        if (failRecordWrite) {
            return false;
        }
        record = text;
        return true;
    }
};

// ---- remote -------------------------------------------------------------------------------------

class FakeBackend : public RemoteBackend {
public:
    struct Fault {
        RemoteError error = RemoteError::Network;
        int count = 1;
    };

    std::string account = "user-1";
    std::map<std::string, RemoteHead> heads;  // by account/key
    std::map<std::string, Bytes> chunks;      // account/key/sid/index
    int tokenCounter = 0;
    bool deferred = false;  // queue callbacks until pump()
    std::deque<std::function<void()>> queue;
    std::map<std::string, Fault> faults;  // by operation name
    bool loseCommitResponse = false;      // the commit applies but the answer is a network error
    bool corruptNextChunkRead = false;
    // Runs right before a commit is evaluated (another device racing).
    std::function<void()> beforeCommit;
    std::map<std::string, int> calls;

    std::string accountId() const override { return account; }

    std::string headKey(const std::string& key) const { return account + "/" + key; }
    std::string chunkKey(const std::string& key, const std::string& sid, uint32_t index) const {
        return account + "/" + key + "/" + sid + "/" + std::to_string(index);
    }
    size_t chunkCountFor(const std::string& key, const std::string& sid) const {
        size_t n = 0;
        const std::string prefix = account + "/" + key + "/" + sid + "/";
        for (const auto& [name, data] : chunks) {
            n += name.rfind(prefix, 0) == 0 ? 1 : 0;
        }
        return n;
    }

    void pump() {
        while (!queue.empty()) {
            auto next = std::move(queue.front());
            queue.pop_front();
            next();
        }
    }

    void getHead(const std::string& key, HeadCallback done) override {
        ++calls["getHead"];
        if (auto fault = take("getHead")) {
            finish([done, fault] { done(*fault, std::nullopt, "injected"); });
            return;
        }
        std::optional<RemoteHead> head;
        if (const auto it = heads.find(headKey(key)); it != heads.end()) {
            head = it->second;
        }
        finish([done, head] { done(RemoteError::None, head, {}); });
    }

    void putChunk(const std::string& key, const std::string& sid, uint32_t index, const Bytes& data,
        const std::string& sha, DoneCallback done) override {
        ++calls["putChunk"];
        if (auto fault = take("putChunk")) {
            finish([done, fault] { done(*fault, "injected"); });
            return;
        }
        if (data.size() > kChunkBytes || sha256_hex(data) != sha) {
            finish([done] { done(RemoteError::Invalid, "bad chunk"); });
            return;
        }
        const auto name = chunkKey(key, sid, index);
        if (chunks.contains(name)) {
            finish([done] { done(RemoteError::Conflict, "exists"); });
            return;
        }
        chunks[name] = data;
        finish([done] { done(RemoteError::None, {}); });
    }

    void getChunk(const std::string& key, const std::string& sid, uint32_t index, ChunkCallback done) override {
        ++calls["getChunk"];
        if (auto fault = take("getChunk")) {
            finish([done, fault] { done(*fault, {}, "injected"); });
            return;
        }
        const auto it = chunks.find(chunkKey(key, sid, index));
        if (it == chunks.end()) {
            finish([done] { done(RemoteError::Invalid, {}, "missing chunk"); });
            return;
        }
        Bytes data = it->second;
        if (corruptNextChunkRead && !data.empty()) {
            corruptNextChunkRead = false;
            data[data.size() / 2] ^= 0x40;
        }
        finish([done, data] { done(RemoteError::None, data, {}); });
    }

    void commitHead(const std::string& key, const std::optional<RemoteHead>& expected,
        const RemoteHead& next, CommitCallback done) override {
        ++calls["commitHead"];
        if (beforeCommit) {
            auto hook = std::move(beforeCommit);
            beforeCommit = nullptr;
            hook();
        }
        if (auto fault = take("commitHead")) {
            finish([done, fault] { done(*fault, {}, "injected"); });
            return;
        }
        const auto name = headKey(key);
        const auto it = heads.find(name);
        const bool matches = expected ? (it != heads.end() && it->second.casToken == expected->casToken) :
                                        it == heads.end();
        if (!matches) {
            finish([done] { done(RemoteError::Conflict, {}, "precondition failed"); });
            return;
        }
        // Every fragment the head names must exist (Firestore rules can't check this; the
        // engine's order guarantees it, and this asserts it).
        for (uint32_t i = 0; i < next.chunkCount; ++i) {
            if (!chunks.contains(chunkKey(key, next.snapshotId, i))) {
                finish([done] { done(RemoteError::Invalid, {}, "head names a missing fragment"); });
                return;
            }
        }
        RemoteHead stored = next;
        stored.casToken = "t" + std::to_string(++tokenCounter);
        heads[name] = stored;
        if (loseCommitResponse) {
            loseCommitResponse = false;
            finish([done] { done(RemoteError::Network, {}, "connection reset"); });
            return;
        }
        finish([done, stored] { done(RemoteError::None, stored, {}); });
    }

    void deleteSnapshot(const std::string& key, const std::string& sid, uint32_t count, DoneCallback done) override {
        ++calls["deleteSnapshot"];
        if (auto fault = take("deleteSnapshot")) {
            finish([done, fault] { done(*fault, "injected"); });
            return;
        }
        for (uint32_t i = 0; i < count; ++i) {
            chunks.erase(chunkKey(key, sid, i));
        }
        finish([done] { done(RemoteError::None, {}); });
    }

    void deleteHead(const std::string& key, const RemoteHead& expected, DoneCallback done) override {
        ++calls["deleteHead"];
        if (auto fault = take("deleteHead")) {
            finish([done, fault] { done(*fault, "injected"); });
            return;
        }
        const auto it = heads.find(headKey(key));
        if (it == heads.end() || it->second.casToken != expected.casToken) {
            finish([done] { done(RemoteError::Conflict, "precondition failed"); });
            return;
        }
        heads.erase(it);
        finish([done] { done(RemoteError::None, {}); });
    }

    // Another device's direct commit (for races and rewinds).
    void forceHead(const std::string& key, RemoteHead head) {
        head.casToken = "t" + std::to_string(++tokenCounter);
        heads[headKey(key)] = head;
    }

private:
    std::optional<RemoteError> take(const std::string& op) {
        const auto it = faults.find(op);
        if (it == faults.end() || it->second.count <= 0) {
            return std::nullopt;
        }
        --it->second.count;
        return it->second.error;
    }

    void finish(std::function<void()> callback) {
        if (deferred) {
            queue.push_back(std::move(callback));
        } else {
            callback();
        }
    }
};

}  // namespace crosssave::test
