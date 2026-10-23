#include "firebase_backend.hpp"

#include "../core/codec.hpp"
#include "../core/json.hpp"
#include "../core/sha256.hpp"

#include <cstdio>
#include <cstdlib>

namespace crosssave::firebase {
namespace {

using json::Value;

// Firestore typed values ------------------------------------------------------------------------

Value string_value(const std::string& text) {
    Value value{json::Object{}};
    value["stringValue"] = text;
    return value;
}

Value integer_value(int64_t number) {
    Value value{json::Object{}};
    value["integerValue"] = std::to_string(static_cast<long long>(number));
    return value;
}

Value bytes_value(const Bytes& bytes) {
    Value value{json::Object{}};
    value["bytesValue"] = base64_encode(bytes);
    return value;
}

Value timestamp_value(int64_t unixMs) {
    Value value{json::Object{}};
    value["timestampValue"] = rfc3339_utc(unixMs);
    return value;
}

Value map_value(Value fields) {
    Value inner{json::Object{}};
    inner["fields"] = std::move(fields);
    Value value{json::Object{}};
    value["mapValue"] = std::move(inner);
    return value;
}

Value array_value(json::Array values) {
    Value inner{json::Object{}};
    if (!values.empty()) {
        inner["values"] = Value{std::move(values)};
    }
    Value value{json::Object{}};
    value["arrayValue"] = std::move(inner);
    return value;
}

std::string get_string(const Value& fields, std::string_view name) {
    return fields[name]["stringValue"].as_string();
}

std::optional<int64_t> get_integer(const Value& fields, std::string_view name) {
    const auto& text = fields[name]["integerValue"].as_string();
    if (text.empty() || text.size() > 19) {
        return std::nullopt;
    }
    char* end = nullptr;
    const long long number = std::strtoll(text.c_str(), &end, 10);
    if (end != text.c_str() + text.size()) {
        return std::nullopt;
    }
    return static_cast<int64_t>(number);
}

Value history_fields(const HistoryEntry& entry) {
    Value fields{json::Object{}};
    fields["snapshotId"] = string_value(entry.snapshotId);
    fields["revision"] = integer_value(entry.revision);
    fields["digest"] = string_value(entry.digest);
    fields["blobSha256"] = string_value(entry.blobSha256);
    fields["size"] = integer_value(static_cast<int64_t>(entry.size));
    fields["chunkCount"] = integer_value(entry.chunkCount);
    fields["deviceName"] = string_value(entry.deviceName);
    fields["createdAt"] = integer_value(entry.createdAtMs);
    return fields;
}

Value head_fields(const RemoteHead& head) {
    Value fields{json::Object{}};
    fields["revision"] = integer_value(head.revision);
    fields["snapshotId"] = string_value(head.snapshotId);
    fields["digest"] = string_value(head.digest);
    fields["blobSha256"] = string_value(head.blobSha256);
    fields["size"] = integer_value(static_cast<int64_t>(head.size));
    fields["chunkCount"] = integer_value(head.chunkCount);
    fields["deviceId"] = string_value(head.deviceId);
    fields["deviceName"] = string_value(head.deviceName);
    fields["platform"] = string_value(head.platform);
    fields["game"] = string_value(head.game);
    fields["createdAt"] = integer_value(head.createdAtMs);
    fields["format"] = string_value(head.format);
    json::Array history;
    for (const auto& entry : head.history) {
        history.push_back(map_value(history_fields(entry)));
    }
    fields["history"] = array_value(std::move(history));
    return fields;
}

std::optional<HistoryEntry> parse_history(const Value& fields) {
    HistoryEntry entry;
    entry.snapshotId = get_string(fields, "snapshotId");
    entry.digest = get_string(fields, "digest");
    entry.blobSha256 = get_string(fields, "blobSha256");
    entry.deviceName = get_string(fields, "deviceName");
    const auto revision = get_integer(fields, "revision");
    const auto size = get_integer(fields, "size");
    const auto chunks = get_integer(fields, "chunkCount");
    const auto created = get_integer(fields, "createdAt");
    if (entry.snapshotId.empty() || !revision || !size || !chunks || *size < 0 || *chunks < 0) {
        return std::nullopt;
    }
    entry.revision = *revision;
    entry.size = static_cast<uint64_t>(*size);
    entry.chunkCount = static_cast<uint32_t>(*chunks);
    entry.createdAtMs = created.value_or(0);
    return entry;
}

std::optional<RemoteHead> parse_head(const Value& document) {
    const auto& fields = document["fields"];
    RemoteHead head;
    const auto revision = get_integer(fields, "revision");
    const auto size = get_integer(fields, "size");
    const auto chunks = get_integer(fields, "chunkCount");
    head.snapshotId = get_string(fields, "snapshotId");
    head.digest = get_string(fields, "digest");
    head.blobSha256 = get_string(fields, "blobSha256");
    head.deviceId = get_string(fields, "deviceId");
    head.deviceName = get_string(fields, "deviceName");
    head.platform = get_string(fields, "platform");
    head.game = get_string(fields, "game");
    head.format = get_string(fields, "format");
    head.createdAtMs = get_integer(fields, "createdAt").value_or(0);
    head.casToken = document.string_or("updateTime");
    if (!revision || !size || !chunks || *revision < 1 || *size < 0 || *chunks < 0 ||
        head.snapshotId.empty() || head.casToken.empty() || head.format != "dusksave-1")
    {
        return std::nullopt;
    }
    head.revision = *revision;
    head.size = static_cast<uint64_t>(*size);
    head.chunkCount = static_cast<uint32_t>(*chunks);
    for (const auto& item : fields["history"]["arrayValue"]["values"].as_array()) {
        auto entry = parse_history(item["mapValue"]["fields"]);
        if (!entry) {
            return std::nullopt;
        }
        head.history.push_back(std::move(*entry));
    }
    return head;
}

Value write_update(const std::string& name, Value fields) {
    Value document{json::Object{}};
    document["name"] = name;
    document["fields"] = std::move(fields);
    Value write{json::Object{}};
    write["update"] = std::move(document);
    return write;
}

Value precondition_exists(bool exists) {
    Value condition{json::Object{}};
    condition["exists"] = Value{exists};
    return condition;
}

Value precondition_update_time(const std::string& token) {
    Value condition{json::Object{}};
    condition["updateTime"] = token;
    return condition;
}

Value write_delete(const std::string& name) {
    Value write{json::Object{}};
    write["delete"] = name;
    return write;
}

std::string writes_body(json::Array writes) {
    Value body{json::Object{}};
    body["writes"] = Value{std::move(writes)};
    return json::dump(body);
}

// Firebase Auth answers errors as {"error":{"message":"CODE : detail"}}.
std::string auth_error_code(const std::string& body) {
    const auto parsed = json::parse(body, 64 * 1024);
    if (!parsed) {
        return {};
    }
    auto text = (*parsed)["error"].string_or("message");
    if (const auto colon = text.find(' '); colon != std::string::npos) {
        text.resize(colon);
    }
    return text;
}

AuthResult auth_failure(const HttpResponse& response) {
    if (!response.transportOk) {
        return {RemoteError::Network, "No connection to the sign-in service."};
    }
    const auto code = auth_error_code(response.body);
    if (code == "EMAIL_NOT_FOUND" || code == "INVALID_PASSWORD" || code == "INVALID_LOGIN_CREDENTIALS" ||
        code == "INVALID_EMAIL")
    {
        return {RemoteError::Auth, "Wrong email or password."};
    }
    if (code == "USER_DISABLED") {
        return {RemoteError::Auth, "This account is disabled."};
    }
    if (code.starts_with("TOO_MANY_ATTEMPTS") || response.status == 429) {
        return {RemoteError::Quota, "Too many attempts. Try again later."};
    }
    if (code == "OPERATION_NOT_ALLOWED" || code == "ADMIN_ONLY_OPERATION") {
        return {RemoteError::Denied, "This sign-in method is not enabled in the Firebase project."};
    }
    if (response.status >= 500) {
        return {RemoteError::Network, "The sign-in service is unavailable."};
    }
    return {RemoteError::Invalid, code.empty() ? "Sign-in failed (HTTP " + std::to_string(response.status) + ")." :
                                                 "Sign-in failed (" + code + ")."};
}

constexpr char kCodeAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no 0/O, 1/I

}  // namespace

// ---- helpers ------------------------------------------------------------------------------------

std::string rfc3339_utc(int64_t unixMs) {
    int64_t seconds = unixMs >= 0 ? unixMs / 1000 : (unixMs - 999) / 1000;
    const int64_t millis = unixMs - seconds * 1000;
    int64_t days = seconds >= 0 ? seconds / 86400 : (seconds - 86399) / 86400;
    const int64_t secondsOfDay = seconds - days * 86400;
    // Howard Hinnant's civil_from_days.
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t year = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t day = doy - (153 * mp + 2) / 5 + 1;
    const int64_t month = mp < 10 ? mp + 3 : mp - 9;
    year += month <= 2 ? 1 : 0;
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%04lld-%02lld-%02lldT%02lld:%02lld:%02lld.%03lldZ",
        static_cast<long long>(year), static_cast<long long>(month), static_cast<long long>(day),
        static_cast<long long>(secondsOfDay / 3600), static_cast<long long>(secondsOfDay / 60 % 60),
        static_cast<long long>(secondsOfDay % 60), static_cast<long long>(millis));
    return buffer;
}

std::string mask_email(const std::string& email) {
    const auto at = email.find('@');
    if (at == std::string::npos || at == 0) {
        return "***";
    }
    return email.substr(0, 1) + "***" + email.substr(at);
}

// ---- construction, paths ------------------------------------------------------------------------

FirebaseBackend::FirebaseBackend(HttpTransport& http, SessionStore& store, Config config,
    std::function<int64_t()> monotonicMs, std::function<int64_t()> wallClockMs,
    std::function<uint64_t()> random)
    : m_http{http}, m_store{store}, m_config{std::move(config)}, m_monotonicMs{std::move(monotonicMs)},
      m_wallClockMs{std::move(wallClockMs)}, m_random{std::move(random)} {
    m_session = m_store.load();
    if (m_session && (m_session->uid.empty() || m_session->refreshToken.empty())) {
        m_session.reset();
    }
}

std::string FirebaseBackend::documentsRoot() const {
    return "projects/" + m_config.projectId + "/databases/" + m_config.database + "/documents";
}

std::string FirebaseBackend::documentUrl(const std::string& path) const {
    std::string url = m_config.firestoreBase + "/" + documentsRoot();
    size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        url += "/" + url_encode(path.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return url;
}

std::string FirebaseBackend::headPath(const std::string& key) const {
    return "users/" + accountId() + "/saves/" + key;
}

std::string FirebaseBackend::chunkPath(
    const std::string& key, const std::string& sid, uint32_t index) const {
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "_%04u", index);
    return headPath(key) + "/chunks/" + sid + suffix;
}

std::string FirebaseBackend::accountId() const {
    return m_session ? m_session->ownerUid : std::string{};
}

// ---- errors -------------------------------------------------------------------------------------

RemoteError FirebaseBackend::classify(const HttpResponse& response, std::string& message) {
    if (!response.transportOk) {
        message = response.error.empty() ? "network error" : response.error;
        return RemoteError::Network;
    }
    if (response.status >= 200 && response.status < 300) {
        message.clear();
        return RemoteError::None;
    }
    std::string status;
    if (const auto parsed = json::parse(response.body, 64 * 1024)) {
        status = (*parsed)["error"].string_or("status");
    }
    message = "HTTP " + std::to_string(response.status) + (status.empty() ? "" : " " + status);
    if (response.status == 401 || status == "UNAUTHENTICATED") {
        return RemoteError::Auth;
    }
    if (response.status == 403 || status == "PERMISSION_DENIED") {
        return RemoteError::Denied;
    }
    if (response.status == 429 || status == "RESOURCE_EXHAUSTED") {
        return RemoteError::Quota;
    }
    if (response.status == 409 || status == "ALREADY_EXISTS" || status == "ABORTED" ||
        status == "FAILED_PRECONDITION")
    {
        return RemoteError::Conflict;
    }
    if (response.status >= 500 || status == "UNAVAILABLE" || status == "DEADLINE_EXCEEDED") {
        return RemoteError::Network;
    }
    return RemoteError::Invalid;
}

// ---- tokens -------------------------------------------------------------------------------------

void FirebaseBackend::withToken(TokenCallback done) {
    if (!m_session) {
        done(RemoteError::Auth, "not signed in");
        return;
    }
    if (!m_idToken.empty() && m_monotonicMs() + 60'000 < m_idTokenExpiresAt) {
        done(RemoteError::None, {});
        return;
    }
    refresh(std::move(done));
}

void FirebaseBackend::refresh(TokenCallback done) {
    m_tokenWaiters.push_back(std::move(done));
    if (m_refreshing) {
        return;
    }
    m_refreshing = true;
    HttpRequest request;
    request.method = "POST";
    request.url = m_config.tokenBase + "/token?key=" + url_encode(m_config.apiKey);
    request.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
    request.body = "grant_type=refresh_token&refresh_token=" + url_encode(m_session->refreshToken);
    request.maxResponseBytes = 64 * 1024;
    m_http.send(std::move(request), [this](HttpResponse response) {
        if (!m_session) {
            finishRefresh(RemoteError::Auth, "signed out");
            return;
        }
        if (!response.transportOk || response.status >= 500) {
            finishRefresh(RemoteError::Network, "token refresh: network");
            return;
        }
        if (response.status == 429) {
            finishRefresh(RemoteError::Quota, "token refresh: rate limited");
            return;
        }
        const auto parsed = json::parse(response.body, 64 * 1024);
        if (response.status != 200 || !parsed) {
            // TOKEN_EXPIRED, USER_DISABLED, USER_NOT_FOUND, INVALID_REFRESH_TOKEN...: the session
            // is over; the player must link again. The stored token is useless and is dropped.
            const auto code = auth_error_code(response.body);
            m_store.clear();
            m_session.reset();
            m_idToken.clear();
            finishRefresh(RemoteError::Auth, "session ended" + (code.empty() ? "" : " (" + code + ")"));
            return;
        }
        const auto idToken = parsed->string_or("id_token");
        const auto refreshToken = parsed->string_or("refresh_token");
        const auto expires = std::atoll(parsed->string_or("expires_in", "3600").c_str());
        const auto userId = parsed->string_or("user_id");
        if (idToken.empty() || (!userId.empty() && userId != m_session->uid)) {
            finishRefresh(RemoteError::Invalid, "token refresh: unexpected answer");
            return;
        }
        m_idToken = idToken;
        m_idTokenExpiresAt = m_monotonicMs() + (expires > 0 ? expires : 3600) * 1000;
        if (!refreshToken.empty() && refreshToken != m_session->refreshToken) {
            m_session->refreshToken = refreshToken;
            m_store.save(*m_session);
        }
        finishRefresh(RemoteError::None, {});
    });
}

void FirebaseBackend::finishRefresh(RemoteError error, const std::string& message) {
    m_refreshing = false;
    auto waiters = std::move(m_tokenWaiters);
    m_tokenWaiters.clear();
    for (auto& waiter : waiters) {
        waiter(error, message);
    }
}

void FirebaseBackend::authorized(
    HttpRequest request, std::function<void(HttpResponse)> done, bool retried) {
    withToken([this, request = std::move(request), done = std::move(done), retried](
                  RemoteError error, std::string message) mutable {
        if (error != RemoteError::None) {
            // Report as a synthetic answer the classifier maps back to the same error.
            HttpResponse synthetic;
            synthetic.transportOk = error != RemoteError::Network;
            synthetic.error = message;
            synthetic.status = error == RemoteError::Auth ? 401 : error == RemoteError::Quota ? 429 : 400;
            done(std::move(synthetic));
            return;
        }
        HttpRequest sent = request;
        sent.headers.emplace_back("Authorization", "Bearer " + m_idToken);
        m_http.send(std::move(sent),
            [this, request = std::move(request), done = std::move(done), retried](HttpResponse response) mutable {
                if (response.transportOk && response.status == 401 && !retried) {
                    m_idToken.clear();  // expired early or revoked: refresh once
                    authorized(std::move(request), std::move(done), true);
                    return;
                }
                done(std::move(response));
            });
    });
}

void FirebaseBackend::commit(
    std::string writesJson, std::function<void(RemoteError, std::string, std::string)> done) {
    HttpRequest request;
    request.method = "POST";
    request.url = m_config.firestoreBase + "/" + documentsRoot() + ":commit";
    request.headers = {{"Content-Type", "application/json"}};
    request.body = std::move(writesJson);
    request.maxResponseBytes = 256 * 1024;
    authorized(std::move(request), [done = std::move(done)](HttpResponse response) {
        std::string message;
        auto error = classify(response, message);
        // In a commit, NOT_FOUND means an updateTime/exists precondition met a deleted document:
        // the state moved under us, exactly like a lost compare-and-swap.
        if (error == RemoteError::Invalid && response.status == 404) {
            error = RemoteError::Conflict;
        }
        done(error, std::move(response.body), std::move(message));
    });
}

// ---- sign-in ------------------------------------------------------------------------------------

void FirebaseBackend::storeAuthAnswer(const std::string& body, bool anonymous,
    const std::string& label, std::function<void(AuthResult)> done) {
    const auto parsed = json::parse(body, 64 * 1024);
    const auto uid = parsed ? parsed->string_or("localId") : std::string{};
    const auto refreshToken = parsed ? parsed->string_or("refreshToken") : std::string{};
    const auto idToken = parsed ? parsed->string_or("idToken") : std::string{};
    if (uid.empty() || refreshToken.empty() || idToken.empty()) {
        done({RemoteError::Invalid, "Unexpected answer from the sign-in service."});
        return;
    }
    Session session;
    session.uid = uid;
    session.refreshToken = refreshToken;
    session.anonymous = anonymous;
    session.ownerUid = anonymous ? std::string{} : uid;  // a device link sets it on confirmation
    session.label = label;
    m_session = session;
    m_idToken = idToken;
    const auto expires = std::atoll(parsed->string_or("expiresIn", "3600").c_str());
    m_idTokenExpiresAt = m_monotonicMs() + (expires > 0 ? expires : 3600) * 1000;
    if (!m_store.save(session)) {
        done({RemoteError::Invalid, "Signed in, but the session could not be stored on this device."});
        return;
    }
    done({});
}

void FirebaseBackend::signInWithPassword(
    const std::string& email, const std::string& password, std::function<void(AuthResult)> done) {
    if (!m_config.valid()) {
        done({RemoteError::Invalid, "The Firebase project is not configured."});
        return;
    }
    Value body{json::Object{}};
    body["email"] = email;
    body["password"] = password;
    body["returnSecureToken"] = Value{true};
    HttpRequest request;
    request.method = "POST";
    request.url = m_config.authBase + "/accounts:signInWithPassword?key=" + url_encode(m_config.apiKey);
    request.headers = {{"Content-Type", "application/json"}};
    request.body = json::dump(body);  // the only place the password exists; never logged or kept
    request.maxResponseBytes = 64 * 1024;
    m_http.send(std::move(request), [this, label = mask_email(email), done = std::move(done)](HttpResponse response) {
        if (!response.transportOk || response.status != 200) {
            done(auth_failure(response));
            return;
        }
        storeAuthAnswer(response.body, false, label, done);
    });
}

void FirebaseBackend::signOut() {
    m_store.clear();
    m_session.reset();
    m_idToken.clear();
    m_idTokenExpiresAt = 0;
    m_pairingCode.clear();
    m_pairingOwner.clear();
    m_pairingOwnerLabel.clear();
}

// ---- device link --------------------------------------------------------------------------------

void FirebaseBackend::startPairing(const std::string& deviceName, const std::string& platform,
    int64_t ttlMs, std::function<void(AuthResult, std::string)> done) {
    if (!m_config.valid()) {
        done({RemoteError::Invalid, "The Firebase project is not configured."}, {});
        return;
    }
    const auto publish = [this, deviceName, platform, ttlMs, done]() {
        // The local random source may be weak (homebrew has no reliable getrandom through the
        // Linux bridge); the ID token is a server-signed JWT, so mixing it in makes the code
        // unpredictable to anyone without this device's session.
        Sha256 mixer;
        for (int i = 0; i < 4; ++i) {
            const uint64_t value = m_random();
            mixer.update(std::string_view{reinterpret_cast<const char*>(&value), sizeof(value)});
        }
        mixer.update(m_session->uid);
        mixer.update(m_idToken);
        const auto digest = mixer.finish();
        std::string code;
        for (int i = 0; i < 8; ++i) {
            code.push_back(kCodeAlphabet[digest[static_cast<size_t>(i)] % (sizeof(kCodeAlphabet) - 1)]);
        }
        const int64_t now = m_wallClockMs();
        Value fields{json::Object{}};
        fields["deviceUid"] = string_value(m_session->uid);
        fields["deviceName"] = string_value(deviceName);
        fields["platform"] = string_value(platform);
        fields["createdAt"] = timestamp_value(now);
        fields["expiresAt"] = timestamp_value(now + ttlMs);
        auto write = write_update(documentsRoot() + "/pairings/" + code, std::move(fields));
        write["currentDocument"] = precondition_exists(false);
        commit(writes_body({std::move(write)}),
            [this, code, done](RemoteError error, std::string, std::string message) {
                if (error == RemoteError::Conflict) {
                    done({RemoteError::Conflict, "That code is taken. Try again."}, {});
                    return;
                }
                if (error != RemoteError::None) {
                    done({error, "Could not publish the link code (" + message + ")."}, {});
                    return;
                }
                m_pairingCode = code;
                m_pairingOwner.clear();
                m_pairingOwnerLabel.clear();
                done({}, code);
            });
    };
    if (m_session && m_session->anonymous) {
        publish();  // reuse this device's identity
        return;
    }
    HttpRequest request;
    request.method = "POST";
    request.url = m_config.authBase + "/accounts:signUp?key=" + url_encode(m_config.apiKey);
    request.headers = {{"Content-Type", "application/json"}};
    request.body = R"({"returnSecureToken":true})";
    request.maxResponseBytes = 64 * 1024;
    m_http.send(std::move(request), [this, publish, done](HttpResponse response) {
        if (!response.transportOk || response.status != 200) {
            done(auth_failure(response), {});
            return;
        }
        storeAuthAnswer(response.body, true, {}, [publish, done](AuthResult result) {
            if (result.error != RemoteError::None) {
                done(result, {});
                return;
            }
            publish();
        });
    });
}

void FirebaseBackend::pollPairing(std::function<void(PairingStatus)> done) {
    if (m_pairingCode.empty() || !m_session) {
        done({.error = RemoteError::Invalid, .message = "No link code is active."});
        return;
    }
    HttpRequest request;
    request.method = "GET";
    request.url = documentUrl("pairings/" + m_pairingCode);
    request.maxResponseBytes = 64 * 1024;
    authorized(std::move(request), [this, done](HttpResponse response) {
        PairingStatus status;
        if (response.transportOk && response.status == 404) {
            status.expired = true;
            status.message = "The link code expired.";
            done(status);
            return;
        }
        std::string message;
        status.error = classify(response, message);
        if (status.error != RemoteError::None) {
            status.message = message;
            done(status);
            return;
        }
        const auto parsed = json::parse(response.body);
        if (!parsed) {
            done({.error = RemoteError::Invalid, .message = "bad pairing answer"});
            return;
        }
        const auto& fields = (*parsed)["fields"];
        const auto owner = get_string(fields, "ownerUid");
        if (get_string(fields, "deviceUid") != m_session->uid) {
            done({.error = RemoteError::Invalid, .message = "pairing for another device"});
            return;
        }
        if (!owner.empty()) {
            m_pairingOwner = owner;
            m_pairingOwnerLabel = get_string(fields, "ownerLabel");
            status.approved = true;
            status.ownerLabel = m_pairingOwnerLabel;
        }
        done(status);
    });
}

void FirebaseBackend::confirmPairing(std::function<void(AuthResult)> done) {
    if (!m_session || m_pairingOwner.empty()) {
        done({RemoteError::Invalid, "The link was not approved yet."});
        return;
    }
    m_session->ownerUid = m_pairingOwner;
    m_session->label = m_pairingOwnerLabel;
    if (!m_store.save(*m_session)) {
        done({RemoteError::Invalid, "The link could not be stored on this device."});
        return;
    }
    // The code has done its job; remove it (best effort).
    commit(writes_body({write_delete(documentsRoot() + "/pairings/" + m_pairingCode)}),
        [](RemoteError, std::string, std::string) {});
    m_pairingCode.clear();
    m_pairingOwner.clear();
    m_pairingOwnerLabel.clear();
    done({});
}

void FirebaseBackend::cancelPairing() {
    if (!m_pairingCode.empty() && m_session) {
        commit(writes_body({write_delete(documentsRoot() + "/pairings/" + m_pairingCode)}),
            [](RemoteError, std::string, std::string) {});
    }
    m_pairingCode.clear();
    m_pairingOwner.clear();
    m_pairingOwnerLabel.clear();
}

void FirebaseBackend::approvePairing(const std::string& rawCode, std::function<void(AuthResult)> done) {
    std::string code;
    for (const char c : rawCode) {
        if (c == ' ' || c == '-') {
            continue;
        }
        code.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
    }
    if (!m_session || m_session->anonymous || m_session->ownerUid.empty()) {
        done({RemoteError::Auth, "Sign in with the account's email on this device first."});
        return;
    }
    if (code.size() != 8 || code.find_first_not_of(kCodeAlphabet) != std::string::npos) {
        done({RemoteError::Invalid, "That is not a link code."});
        return;
    }
    HttpRequest request;
    request.method = "GET";
    request.url = documentUrl("pairings/" + code);
    request.maxResponseBytes = 64 * 1024;
    authorized(std::move(request), [this, code, done](HttpResponse response) {
        if (response.transportOk && response.status == 404) {
            done({RemoteError::Invalid, "No device is waiting with that code (it may have expired)."});
            return;
        }
        std::string message;
        const auto error = classify(response, message);
        if (error != RemoteError::None) {
            done({error, "Could not read the link code (" + message + ")."});
            return;
        }
        const auto parsed = json::parse(response.body);
        const auto& fields = parsed ? (*parsed)["fields"] : json::Value{};
        const auto deviceUid = get_string(fields, "deviceUid");
        if (deviceUid.empty() || !get_string(fields, "ownerUid").empty()) {
            done({RemoteError::Invalid, "That code was already used."});
            return;
        }
        const auto owner = m_session->ownerUid;
        Value device{json::Object{}};
        device["name"] = string_value(get_string(fields, "deviceName"));
        device["platform"] = string_value(get_string(fields, "platform"));
        device["linkedAt"] = integer_value(m_wallClockMs());
        device["pairingCode"] = string_value(code);
        Value approval{json::Object{}};
        approval["ownerUid"] = string_value(owner);
        approval["ownerLabel"] = string_value(m_session->label);
        auto approve = write_update(documentsRoot() + "/pairings/" + code, std::move(approval));
        Value mask{json::Object{}};
        mask["fieldPaths"] = Value{json::Array{Value{"ownerUid"}, Value{"ownerLabel"}}};
        approve["updateMask"] = std::move(mask);
        approve["currentDocument"] = precondition_update_time(parsed->string_or("updateTime"));
        // One commit: the device is listed under the owner exactly when the code says so.
        commit(writes_body({write_update(documentsRoot() + "/users/" + owner + "/devices/" + deviceUid,
                                std::move(device)),
                   std::move(approve)}),
            [done](RemoteError error, std::string, std::string message) {
                if (error == RemoteError::Conflict) {
                    done({RemoteError::Conflict, "That code changed while approving. Try again."});
                    return;
                }
                if (error != RemoteError::None) {
                    done({error, "Could not approve the device (" + message + ")."});
                    return;
                }
                done({});
            });
    });
}

void FirebaseBackend::listDevices(
    std::function<void(RemoteError, std::vector<LinkedDevice>, std::string)> done) {
    if (!m_session || m_session->anonymous) {
        done(RemoteError::Auth, {}, "Only the account owner can list devices.");
        return;
    }
    HttpRequest request;
    request.method = "GET";
    request.url = documentUrl("users/" + m_session->ownerUid + "/devices") + "?pageSize=100";
    request.maxResponseBytes = 256 * 1024;
    authorized(std::move(request), [done](HttpResponse response) {
        std::string message;
        const auto error = classify(response, message);
        if (error != RemoteError::None) {
            done(error, {}, message);
            return;
        }
        std::vector<LinkedDevice> devices;
        const auto parsed = json::parse(response.body);
        for (const auto& document : parsed ? (*parsed)["documents"].as_array() : json::Array{}) {
            const auto& name = document.as_object().contains("name") ? document["name"].as_string() : std::string{};
            const auto& fields = document["fields"];
            LinkedDevice device;
            device.uid = name.substr(name.rfind('/') + 1);
            device.name = get_string(fields, "name");
            device.platform = get_string(fields, "platform");
            device.linkedAtMs = get_integer(fields, "linkedAt").value_or(0);
            devices.push_back(std::move(device));
        }
        done(RemoteError::None, std::move(devices), {});
    });
}

void FirebaseBackend::revokeDevice(const std::string& deviceUid, std::function<void(AuthResult)> done) {
    if (!m_session || m_session->anonymous || deviceUid.empty() ||
        deviceUid.find('/') != std::string::npos)
    {
        done({RemoteError::Auth, "Only the account owner can unlink devices."});
        return;
    }
    commit(writes_body({write_delete(documentsRoot() + "/users/" + m_session->ownerUid + "/devices/" + deviceUid)}),
        [done](RemoteError error, std::string, std::string message) {
            done({error, error == RemoteError::None ? std::string{} : "Could not unlink (" + message + ")."});
        });
}

// ---- RemoteBackend ------------------------------------------------------------------------------

void FirebaseBackend::getHead(const std::string& key, HeadCallback done) {
    if (accountId().empty()) {
        done(RemoteError::Auth, std::nullopt, "not signed in");
        return;
    }
    HttpRequest request;
    request.method = "GET";
    request.url = documentUrl(headPath(key));
    request.maxResponseBytes = 256 * 1024;
    authorized(std::move(request), [done](HttpResponse response) {
        if (response.transportOk && response.status == 404) {
            done(RemoteError::None, std::nullopt, {});
            return;
        }
        std::string message;
        const auto error = classify(response, message);
        if (error != RemoteError::None) {
            done(error, std::nullopt, message);
            return;
        }
        const auto parsed = json::parse(response.body);
        auto head = parsed ? parse_head(*parsed) : std::nullopt;
        if (!head) {
            done(RemoteError::Invalid, std::nullopt, "the cloud head is malformed");
            return;
        }
        done(RemoteError::None, std::move(head), {});
    });
}

void FirebaseBackend::putChunk(const std::string& key, const std::string& snapshotId,
    uint32_t index, const Bytes& data, const std::string& sha256, DoneCallback done) {
    if (accountId().empty()) {
        done(RemoteError::Auth, "not signed in");
        return;
    }
    Value fields{json::Object{}};
    fields["data"] = bytes_value(data);
    fields["sha256"] = string_value(sha256);
    fields["size"] = integer_value(static_cast<int64_t>(data.size()));
    fields["snapshotId"] = string_value(snapshotId);
    fields["index"] = integer_value(index);
    auto write = write_update(documentsRoot() + "/" + chunkPath(key, snapshotId, index), std::move(fields));
    write["currentDocument"] = precondition_exists(false);  // fragments are immutable
    commit(writes_body({std::move(write)}),
        [done](RemoteError error, std::string, std::string message) { done(error, std::move(message)); });
}

void FirebaseBackend::getChunk(
    const std::string& key, const std::string& snapshotId, uint32_t index, ChunkCallback done) {
    if (accountId().empty()) {
        done(RemoteError::Auth, {}, "not signed in");
        return;
    }
    HttpRequest request;
    request.method = "GET";
    request.url = documentUrl(chunkPath(key, snapshotId, index));
    request.maxResponseBytes = 1536 * 1024;  // 512 KiB as base64 plus the JSON envelope
    authorized(std::move(request), [done, snapshotId, index](HttpResponse response) {
        std::string message;
        if (response.transportOk && response.status == 404) {
            done(RemoteError::Invalid, {}, "a fragment is missing");
            return;
        }
        const auto error = classify(response, message);
        if (error != RemoteError::None) {
            done(error, {}, message);
            return;
        }
        const auto parsed = json::parse(response.body);
        if (!parsed) {
            done(RemoteError::Invalid, {}, "malformed fragment");
            return;
        }
        const auto& fields = (*parsed)["fields"];
        auto data = base64_decode(fields["data"]["bytesValue"].as_string());
        const auto size = get_integer(fields, "size");
        if (!data || !size || static_cast<int64_t>(data->size()) != *size ||
            get_string(fields, "snapshotId") != snapshotId || get_integer(fields, "index") != int64_t{index} ||
            sha256_hex(*data) != get_string(fields, "sha256"))
        {
            done(RemoteError::Invalid, {}, "a fragment failed its check");
            return;
        }
        done(RemoteError::None, std::move(*data), {});
    });
}

void FirebaseBackend::commitHead(const std::string& key, const std::optional<RemoteHead>& expected,
    const RemoteHead& next, CommitCallback done) {
    if (accountId().empty()) {
        done(RemoteError::Auth, {}, "not signed in");
        return;
    }
    auto write = write_update(documentsRoot() + "/" + headPath(key), head_fields(next));
    write["currentDocument"] =
        expected ? precondition_update_time(expected->casToken) : precondition_exists(false);
    commit(writes_body({std::move(write)}),
        [done, next](RemoteError error, std::string body, std::string message) {
            if (error != RemoteError::None) {
                done(error, {}, std::move(message));
                return;
            }
            const auto parsed = json::parse(body);
            const auto results = parsed ? (*parsed)["writeResults"].as_array() : json::Array{};
            const auto token = results.empty() ? std::string{} : results.front().string_or("updateTime");
            if (token.empty()) {
                // Committed, but without the new token the next update cannot be conditional.
                // The engine re-reads the head, which carries it.
                done(RemoteError::Network, {}, "commit answer without updateTime");
                return;
            }
            RemoteHead committed = next;
            committed.casToken = token;
            done(RemoteError::None, std::move(committed), {});
        });
}

void FirebaseBackend::deleteSnapshot(const std::string& key, const std::string& snapshotId,
    uint32_t chunkCount, DoneCallback done) {
    if (accountId().empty() || chunkCount == 0) {
        done(accountId().empty() ? RemoteError::Auth : RemoteError::None, {});
        return;
    }
    // A commit takes at most 500 writes; a snapshot has at most 32 fragments (16 MiB).
    json::Array writes;
    for (uint32_t i = 0; i < chunkCount && i < 500; ++i) {
        writes.push_back(write_delete(documentsRoot() + "/" + chunkPath(key, snapshotId, i)));
    }
    commit(writes_body(std::move(writes)),
        [done](RemoteError error, std::string, std::string message) { done(error, std::move(message)); });
}

void FirebaseBackend::deleteHead(const std::string& key, const RemoteHead& expected, DoneCallback done) {
    if (accountId().empty()) {
        done(RemoteError::Auth, "not signed in");
        return;
    }
    auto write = write_delete(documentsRoot() + "/" + headPath(key));
    write["currentDocument"] = precondition_update_time(expected.casToken);
    commit(writes_body({std::move(write)}),
        [done](RemoteError error, std::string, std::string message) { done(error, std::move(message)); });
}

}  // namespace crosssave::firebase
