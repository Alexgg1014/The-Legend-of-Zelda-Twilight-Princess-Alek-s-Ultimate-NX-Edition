#include "sdk_adapters.hpp"

#include "../core/json.hpp"
#include "platform_io.hpp"

#include <mods/service.hpp>
#include <mods/svc/http.h>
#include <mods/svc/save_snapshot.h>

#include <cstring>
#include <vector>

extern const HttpService* svc_http;
extern const SaveSnapshotService* svc_save_snapshot;
extern "C" ModContext* mod_ctx;

#ifndef CROSSSAVE_FIREBASE_API_KEY
#define CROSSSAVE_FIREBASE_API_KEY ""
#endif
#ifndef CROSSSAVE_FIREBASE_PROJECT_ID
#define CROSSSAVE_FIREBASE_PROJECT_ID ""
#endif

namespace crosssave::sdk {
namespace {

LocalStatus to_status(SaveSnapshotStatus status) {
    switch (status) {
    case SAVE_SNAPSHOT_OK:
        return LocalStatus::Ok;
    case SAVE_SNAPSHOT_NO_DISC:
        return LocalStatus::NoDisc;
    case SAVE_SNAPSHOT_NO_CARD:
        return LocalStatus::NoCard;
    case SAVE_SNAPSHOT_CARD_BUSY:
        return LocalStatus::Busy;
    case SAVE_SNAPSHOT_IN_GAME:
        return LocalStatus::InGame;
    case SAVE_SNAPSHOT_NO_SAVE:
        return LocalStatus::NoSave;
    case SAVE_SNAPSHOT_INVALID:
        return LocalStatus::Invalid;
    case SAVE_SNAPSHOT_INCOMPATIBLE:
        return LocalStatus::Incompatible;
    case SAVE_SNAPSHOT_NEEDS_CONVERSION:
        return LocalStatus::NeedsConversion;
    case SAVE_SNAPSHOT_LOCAL_CHANGED:
        return LocalStatus::LocalChanged;
    case SAVE_SNAPSHOT_BACKUP_FAILED:
        return LocalStatus::BackupFailed;
    case SAVE_SNAPSHOT_WRITE_FAILED:
        return LocalStatus::WriteFailed;
    case SAVE_SNAPSHOT_MOD_DATA_FAILED:
        return LocalStatus::ModDataFailed;
    case SAVE_SNAPSHOT_REMOUNT_FAILED:
        return LocalStatus::RemountFailed;
    case SAVE_SNAPSHOT_IO_ERROR:
        break;
    }
    return LocalStatus::IoError;
}

Compat to_compat(SaveSnapshotCompat compat) {
    switch (compat) {
    case SAVE_SNAPSHOT_COMPAT_EXACT:
        return Compat::Exact;
    case SAVE_SNAPSHOT_COMPAT_REGION_CHANGE:
        return Compat::RegionChange;
    case SAVE_SNAPSHOT_COMPAT_PLATFORM_CHANGE:
        return Compat::PlatformChange;
    case SAVE_SNAPSHOT_COMPAT_INCOMPATIBLE:
        break;
    }
    return Compat::Incompatible;
}

LocalCheck from_info(const SaveSnapshotInfo& info) {
    LocalCheck check;
    check.status = to_status(info.status);
    check.message = info.message;
    check.digest = info.digest;
    check.compat = to_compat(info.compat);
    check.backupName = info.backup_name;
    return check;
}

}  // namespace

// ---- local --------------------------------------------------------------------------------------

SnapshotLocalStore::SnapshotLocalStore(std::string dataDir)
    : m_outPath{io::join(dataDir, "snapshot-out.dusksave")},
      m_inPath{io::join(dataDir, "snapshot-in.dusksave")} {}

LocalIdentity SnapshotLocalStore::identity() {
    SaveSnapshotIdentity raw = SAVE_SNAPSHOT_IDENTITY_INIT;
    LocalIdentity id;
    if (svc_save_snapshot->get_identity(mod_ctx, nullptr, &raw) != MOD_OK) {
        id.status = LocalStatus::IoError;
        id.message = "The save service refused the request.";
        return id;
    }
    id.status = to_status(raw.status);
    id.message = raw.message;
    id.key = raw.key;
    id.game = raw.game;
    id.saveName = raw.save_name;
    id.hasSave = raw.has_save;
    id.digest = raw.digest;
    id.changeCount = raw.change_count;
    id.modifiedTime = raw.modified_time;
    return id;
}

LocalSnapshot SnapshotLocalStore::capture() {
    SaveSnapshotInfo info = SAVE_SNAPSHOT_INFO_INIT;
    LocalSnapshot snapshot;
    if (svc_save_snapshot->capture(mod_ctx, nullptr, m_outPath.c_str(), &info) != MOD_OK) {
        snapshot.status = LocalStatus::IoError;
        snapshot.message = "The save service refused the request.";
        return snapshot;
    }
    snapshot.status = to_status(info.status);
    snapshot.message = info.message;
    if (snapshot.status != LocalStatus::Ok) {
        return snapshot;
    }
    auto bytes = io::read_file(m_outPath, kMaxSnapshotBytes);
    io::remove_file(m_outPath);
    if (!bytes) {
        snapshot.status = LocalStatus::IoError;
        snapshot.message = "The captured snapshot could not be read back.";
        return snapshot;
    }
    snapshot.bytes = std::move(*bytes);
    snapshot.digest = info.digest;
    snapshot.game = info.game;
    snapshot.modifiedTime = info.modified_time;
    return snapshot;
}

LocalCheck SnapshotLocalStore::inspect(const Bytes& snapshot) {
    LocalCheck check;
    if (!io::write_file_atomic(m_inPath, snapshot.data(), snapshot.size())) {
        check.status = LocalStatus::IoError;
        check.message = "The downloaded snapshot could not be staged.";
        return check;
    }
    SaveSnapshotInfo info = SAVE_SNAPSHOT_INFO_INIT;
    if (svc_save_snapshot->inspect(mod_ctx, nullptr, m_inPath.c_str(), &info) != MOD_OK) {
        check.status = LocalStatus::IoError;
        check.message = "The save service refused the request.";
        return check;
    }
    return from_info(info);
}

LocalCheck SnapshotLocalStore::apply(
    const Bytes& snapshot, const std::optional<std::string>& expectedDigest, bool allowConversion) {
    LocalCheck check;
    if (!io::write_file_atomic(m_inPath, snapshot.data(), snapshot.size())) {
        check.status = LocalStatus::IoError;
        check.message = "The downloaded snapshot could not be staged.";
        return check;
    }
    SaveSnapshotApplyDesc desc = SAVE_SNAPSHOT_APPLY_DESC_INIT;
    desc.flags = allowConversion ? SAVE_SNAPSHOT_APPLY_ALLOW_CONVERSION : 0u;
    desc.expected_digest = expectedDigest ? expectedDigest->c_str() : nullptr;
    SaveSnapshotInfo info = SAVE_SNAPSHOT_INFO_INIT;
    const auto result = svc_save_snapshot->apply(mod_ctx, nullptr, m_inPath.c_str(), &desc, &info);
    if (result != MOD_OK) {
        check.status = LocalStatus::IoError;
        check.message = "The save service refused the request.";
        return check;
    }
    if (info.status == SAVE_SNAPSHOT_OK || info.status == SAVE_SNAPSHOT_REMOUNT_FAILED) {
        io::remove_file(m_inPath);
    }
    return from_info(info);
}

std::optional<std::string> SnapshotLocalStore::readRecord() {
    size_t size = 0;
    if (svc_save_snapshot->read_record(mod_ctx, nullptr, nullptr, &size) != MOD_OK) {
        return std::nullopt;
    }
    std::string text(size, '\0');
    if (size != 0 && svc_save_snapshot->read_record(mod_ctx, nullptr, text.data(), &size) != MOD_OK) {
        return std::nullopt;
    }
    text.resize(size);
    return text;
}

bool SnapshotLocalStore::writeRecord(const std::string& text) {
    return svc_save_snapshot->write_record(mod_ctx, nullptr, text.data(), text.size()) == MOD_OK;
}

// ---- HTTP ---------------------------------------------------------------------------------------

namespace {

struct PendingRequest {
    std::function<void(firebase::HttpResponse)> done;
};

void on_http_complete(ModContext*, HttpRequestHandle, const HttpResult* result, void* userData) {
    auto* pending = static_cast<PendingRequest*>(userData);
    firebase::HttpResponse response;
    response.transportOk = result->error == HTTP_ERROR_NONE;
    response.status = result->status_code;
    if (result->error_message != nullptr) {
        response.error = result->error_message;
    }
    if (result->body != nullptr && result->body_size != 0) {
        response.body.assign(static_cast<const char*>(result->body), result->body_size);
    }
    auto done = std::move(pending->done);
    delete pending;
    done(std::move(response));
}

}  // namespace

void HttpServiceTransport::send(
    firebase::HttpRequest request, std::function<void(firebase::HttpResponse)> done) {
    std::vector<HttpHeader> headers;
    headers.reserve(request.headers.size());
    for (const auto& [name, value] : request.headers) {
        headers.push_back({name.c_str(), value.c_str()});
    }
    HttpRequestDesc desc = HTTP_REQUEST_DESC_INIT;
    desc.method = request.method == "POST" ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    desc.url = request.url.c_str();
    desc.headers = headers.data();
    desc.header_count = static_cast<uint32_t>(headers.size());
    if (desc.method == HTTP_METHOD_POST) {
        desc.body = request.body.data();
        desc.body_size = request.body.size();
    }
    desc.connect_timeout_ms = 10000;
    desc.idle_timeout_ms = 15000;
    desc.total_timeout_ms = 60000;
    desc.max_body_bytes = request.maxResponseBytes;
    auto* pending = new PendingRequest{std::move(done)};
    HttpRequestHandle handle = 0;
    // HttpService copies what it needs before returning.
    if (svc_http->request(mod_ctx, &desc, on_http_complete, pending, &handle) != MOD_OK) {
        auto callback = std::move(pending->done);
        delete pending;
        firebase::HttpResponse failed;
        failed.transportOk = false;
        failed.error = "the request was refused by the HTTP service";
        callback(std::move(failed));
    }
}

// ---- session ------------------------------------------------------------------------------------

FileSessionStore::FileSessionStore(std::string dataDir) : m_path{io::join(dataDir, "account.json")} {}

std::optional<firebase::Session> FileSessionStore::load() {
    const auto bytes = io::read_file(m_path, 64 * 1024);
    if (!bytes) {
        return std::nullopt;
    }
    const auto value = json::parse(std::string{bytes->begin(), bytes->end()});
    if (!value || (*value)["v"].as_number() != 1) {
        return std::nullopt;
    }
    firebase::Session session;
    session.uid = value->string_or("uid");
    session.refreshToken = value->string_or("refreshToken");
    session.ownerUid = value->string_or("ownerUid");
    session.label = value->string_or("label");
    session.anonymous = (*value)["anonymous"].as_bool();
    if (session.uid.empty() || session.refreshToken.empty()) {
        return std::nullopt;
    }
    return session;
}

bool FileSessionStore::save(const firebase::Session& session) {
    json::Value value{json::Object{}};
    value["v"] = json::Value{1};
    value["uid"] = session.uid;
    value["refreshToken"] = session.refreshToken;
    value["ownerUid"] = session.ownerUid;
    value["label"] = session.label;
    value["anonymous"] = json::Value{session.anonymous};
    const auto text = json::dump(value);
    return io::write_file_atomic(m_path, text.data(), text.size());
}

void FileSessionStore::clear() {
    io::remove_file(m_path);
}

firebase::Config load_firebase_config(const std::string& dataDir) {
    firebase::Config config;
    config.apiKey = CROSSSAVE_FIREBASE_API_KEY;
    config.projectId = CROSSSAVE_FIREBASE_PROJECT_ID;
    if (const auto bytes = io::read_file(io::join(dataDir, "firebase.json"), 64 * 1024)) {
        if (const auto value = json::parse(std::string{bytes->begin(), bytes->end()})) {
            config.apiKey = value->string_or("apiKey", config.apiKey);
            config.projectId = value->string_or("projectId", config.projectId);
            // Optional overrides (an HTTPS mirror or proxy); HttpService only speaks HTTPS.
            config.authBase = value->string_or("authBase", config.authBase);
            config.tokenBase = value->string_or("tokenBase", config.tokenBase);
            config.firestoreBase = value->string_or("firestoreBase", config.firestoreBase);
        }
    }
    return config;
}

}  // namespace crosssave::sdk
