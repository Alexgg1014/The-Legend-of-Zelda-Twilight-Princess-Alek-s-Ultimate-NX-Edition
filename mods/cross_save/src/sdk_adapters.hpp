#pragma once

// The mod SDK side of the neutral interfaces: LocalStore over SaveSnapshotService, HttpTransport
// over HttpService, SessionStore over a file in the mod's data directory.

#include "../core/sync_types.hpp"
#include "../firebase/firebase_backend.hpp"

#include <string>

namespace crosssave::sdk {

class SnapshotLocalStore : public LocalStore {
public:
    explicit SnapshotLocalStore(std::string dataDir);
    LocalIdentity identity() override;
    LocalSnapshot capture() override;
    LocalCheck inspect(const Bytes& snapshot) override;
    LocalCheck apply(const Bytes& snapshot, const std::optional<std::string>& expectedDigest,
        bool allowConversion) override;
    std::optional<std::string> readRecord() override;
    bool writeRecord(const std::string& text) override;

private:
    std::string m_outPath;  // captures land here
    std::string m_inPath;   // downloads are staged here for inspect/apply
};

class HttpServiceTransport : public firebase::HttpTransport {
public:
    void send(firebase::HttpRequest request, std::function<void(firebase::HttpResponse)> done) override;
};

// The refresh token in <data_dir>/account.json. On every platform this is a plain file the user
// account (PC, Android app sandbox) or anyone with the SD card (Switch) can read: see the token
// storage section of docs/investigaciones/crosssave-resultado-c1.md. It holds no password.
class FileSessionStore : public firebase::SessionStore {
public:
    explicit FileSessionStore(std::string dataDir);
    std::optional<firebase::Session> load() override;
    bool save(const firebase::Session& session) override;
    void clear() override;

private:
    std::string m_path;
};

// Firebase project settings: <data_dir>/firebase.json ({"apiKey": ..., "projectId": ...}) if
// present, else the values compiled in (CROSSSAVE_FIREBASE_API_KEY / _PROJECT_ID; empty in the
// lab build: the project does not exist yet).
firebase::Config load_firebase_config(const std::string& dataDir);

}  // namespace crosssave::sdk
