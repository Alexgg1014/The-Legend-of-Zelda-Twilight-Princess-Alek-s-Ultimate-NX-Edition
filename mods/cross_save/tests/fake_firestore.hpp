#pragma once

// An in-process stand-in for Firebase Auth REST and Cloud Firestore REST, enough for the adapter:
// signInWithPassword, signUp (anonymous), securetoken refresh, document GET/list and
// documents:commit with exists/updateTime preconditions and updateMask. It also evaluates a C++
// transcription of firestore.rules (owner or linked device for users/{owner}/**, the pairing
// rules), so a revoked device really gets PERMISSION_DENIED. Faults: offline, forced answers,
// deferred delivery.
//
// It is not Google's emulator: it checks this adapter's requests against the documented REST
// shapes, it does not prove Firestore behaves this way. docs/investigaciones/prompt-sol-crosssave.md
// lists what still needs checking against the real service.

#include "../core/json.hpp"
#include "../firebase/firebase_backend.hpp"

#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace crosssave::test {

using firebase::HttpRequest;
using firebase::HttpResponse;

class FakeFirebase : public firebase::HttpTransport {
public:
    std::string apiKey = "test-api-key";
    std::string project = "demo-dusk";
    std::string authBase = "https://auth.test/v1";
    std::string tokenBase = "https://token.test/v1";
    std::string firestoreBase = "https://firestore.test/v1";
    int64_t nowMs = 1'760'000'000'000;  // server wall clock
    int64_t idTokenTtlSeconds = 3600;

    struct User {
        std::string uid;
        std::string email;
        std::string password;
        bool anonymous = false;
        bool disabled = false;
    };
    struct Doc {
        json::Value fields{json::Object{}};
        std::string updateTime;
    };

    std::map<std::string, User> users;  // by uid
    std::map<std::string, std::string> refreshTokens;  // token -> uid
    struct IdToken {
        std::string uid;
        int64_t expiresAt = 0;
    };
    std::map<std::string, IdToken> idTokens;
    std::map<std::string, Doc> docs;  // path below .../documents/

    // Faults and observation.
    int offline = 0;  // the next N requests fail without an HTTP answer
    struct Forced {
        std::string urlContains;
        int status;
        std::string body;
    };
    std::deque<Forced> forced;
    bool deferred = false;
    std::deque<std::function<void()>> queue;
    std::vector<HttpRequest> log;

    firebase::Config config() const {
        firebase::Config c;
        c.apiKey = apiKey;
        c.projectId = project;
        c.authBase = authBase;
        c.tokenBase = tokenBase;
        c.firestoreBase = firestoreBase;
        return c;
    }

    std::string addUser(const std::string& email, const std::string& password) {
        const std::string uid = "uid-" + std::to_string(users.size() + 1);
        users[uid] = User{uid, email, password, false, false};
        return uid;
    }

    void revokeRefreshTokens(const std::string& uid) {
        std::erase_if(refreshTokens, [&](const auto& item) { return item.second == uid; });
        std::erase_if(idTokens, [&](const auto& item) { return item.second.uid == uid; });
    }

    void pump() {
        while (!queue.empty()) {
            step();
        }
    }

    void step() {
        if (queue.empty()) {
            return;
        }
        auto next = std::move(queue.front());
        queue.pop_front();
        next();
    }

    void send(HttpRequest request, std::function<void(HttpResponse)> done) override {
        log.push_back(request);
        if (deferred) {
            // Like a real network: the server handles the request when it arrives.
            queue.push_back([this, request = std::move(request), done = std::move(done)]() mutable {
                done(handle(request));
            });
        } else {
            done(handle(request));
        }
    }

    std::string documentsRoot() const { return "projects/" + project + "/databases/(default)/documents"; }

private:
    int64_t m_counter = 0;

    static HttpResponse answer(int status, std::string body) {
        HttpResponse response;
        response.transportOk = true;
        response.status = status;
        response.body = std::move(body);
        return response;
    }

    static HttpResponse error(int status, const std::string& code, const std::string& message = "error") {
        json::Value body{json::Object{}};
        json::Value inner{json::Object{}};
        inner["code"] = json::Value{status};
        inner["message"] = message;
        inner["status"] = code;
        body["error"] = std::move(inner);
        return answer(status, json::dump(body));
    }

    static HttpResponse auth_error(int status, const std::string& code) {
        json::Value body{json::Object{}};
        json::Value inner{json::Object{}};
        inner["code"] = json::Value{status};
        inner["message"] = code;
        body["error"] = std::move(inner);
        return answer(status, json::dump(body));
    }

    std::string token(const std::string& prefix) { return prefix + std::to_string(++m_counter) + "x"; }

    std::string timestamp() {
        // Monotonic, microsecond precision, like Firestore's updateTime.
        char nanos[16];
        std::snprintf(nanos, sizeof(nanos), ".%09lld", static_cast<long long>(++m_counter % 1000000000));
        return firebase::rfc3339_utc(nowMs).substr(0, 19) + nanos + "Z";
    }

    json::Value session_answer(const std::string& uid, bool snake) {
        const auto id = token("id-");
        const auto refresh = token("rt-");
        idTokens[id] = {uid, nowMs + idTokenTtlSeconds * 1000};
        refreshTokens[refresh] = uid;
        json::Value body{json::Object{}};
        body[snake ? "id_token" : "idToken"] = id;
        body[snake ? "refresh_token" : "refreshToken"] = refresh;
        body[snake ? "expires_in" : "expiresIn"] = std::to_string(idTokenTtlSeconds);
        body[snake ? "user_id" : "localId"] = uid;
        return body;
    }

    static std::string query_key(const std::string& url) {
        const auto at = url.find("key=");
        return at == std::string::npos ? std::string{} : url.substr(at + 4);
    }

    HttpResponse handle(const HttpRequest& request) {
        if (offline > 0) {
            --offline;
            HttpResponse response;
            response.transportOk = false;
            response.error = "offline";
            return response;
        }
        for (auto it = forced.begin(); it != forced.end(); ++it) {
            if (request.url.find(it->urlContains) != std::string::npos) {
                auto response = answer(it->status, it->body);
                forced.erase(it);
                return response;
            }
        }
        if (request.url.rfind(tokenBase + "/token", 0) == 0) {
            return refresh(request);
        }
        if (request.url.rfind(authBase + "/accounts:", 0) == 0) {
            return accounts(request);
        }
        if (request.url.rfind(firestoreBase + "/", 0) == 0) {
            return firestore(request);
        }
        return error(404, "NOT_FOUND", "no such endpoint");
    }

    HttpResponse refresh(const HttpRequest& request) {
        if (request.method != "POST" || query_key(request.url) != apiKey) {
            return auth_error(400, "API_KEY_INVALID");
        }
        const std::string prefix = "grant_type=refresh_token&refresh_token=";
        if (request.body.rfind(prefix, 0) != 0) {
            return auth_error(400, "INVALID_GRANT_TYPE");
        }
        const auto it = refreshTokens.find(request.body.substr(prefix.size()));
        if (it == refreshTokens.end()) {
            return auth_error(400, "INVALID_REFRESH_TOKEN");
        }
        const auto uid = it->second;
        if (users[uid].disabled) {
            return auth_error(400, "USER_DISABLED");
        }
        refreshTokens.erase(it);  // rotate
        return answer(200, json::dump(session_answer(uid, true)));
    }

    HttpResponse accounts(const HttpRequest& request) {
        if (request.method != "POST" || query_key(request.url) != apiKey) {
            return auth_error(400, "API_KEY_INVALID");
        }
        const auto body = json::parse(request.body);
        if (!body) {
            return auth_error(400, "INVALID_JSON");
        }
        if (request.url.find("accounts:signInWithPassword") != std::string::npos) {
            for (const auto& [uid, user] : users) {
                if (!user.anonymous && user.email == body->string_or("email")) {
                    if (user.password != body->string_or("password")) {
                        return auth_error(400, "INVALID_LOGIN_CREDENTIALS");
                    }
                    if (user.disabled) {
                        return auth_error(400, "USER_DISABLED");
                    }
                    return answer(200, json::dump(session_answer(uid, false)));
                }
            }
            return auth_error(400, "INVALID_LOGIN_CREDENTIALS");
        }
        if (request.url.find("accounts:signUp") != std::string::npos) {
            const std::string uid = "anon-" + std::to_string(users.size() + 1);
            users[uid] = User{uid, {}, {}, true, false};
            return answer(200, json::dump(session_answer(uid, false)));
        }
        return auth_error(400, "UNKNOWN");
    }

    std::optional<std::string> caller(const HttpRequest& request) {
        for (const auto& [name, value] : request.headers) {
            if (name == "Authorization" && value.rfind("Bearer ", 0) == 0) {
                const auto it = idTokens.find(value.substr(7));
                if (it != idTokens.end() && it->second.expiresAt > nowMs) {
                    return it->second.uid;
                }
            }
        }
        return std::nullopt;
    }

    static std::vector<std::string> split(const std::string& path) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= path.size()) {
            const auto end = path.find('/', start);
            parts.push_back(path.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        return parts;
    }

    static std::string decode(const std::string& text) {
        std::string out;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '%' && i + 2 < text.size()) {
                out.push_back(static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16)));
                i += 2;
            } else {
                out.push_back(text[i]);
            }
        }
        return out;
    }

    // ---- rules (a transcription of firestore.rules) ---------------------------------------------

    bool linked(const std::string& owner, const std::string& uid) const {
        return docs.contains("users/" + owner + "/devices/" + uid);
    }

    static std::string field_string(const json::Value& fields, const std::string& name) {
        return fields[name]["stringValue"].as_string();
    }

    bool allowed(const std::string& uid, const std::string& op, const std::string& path,
        const Doc* before, const json::Value* after) const {
        const auto parts = split(path);
        const bool anonymous = users.contains(uid) && users.at(uid).anonymous;
        if (parts.size() >= 2 && parts[0] == "users") {
            const auto& owner = parts[1];
            if (parts.size() == 4 && parts[2] == "devices") {
                return uid == owner || (op == "get" && uid == parts[3]);
            }
            if (parts.size() == 3 && parts[2] == "devices") {
                return op == "list" && uid == owner;
            }
            if (parts.size() >= 4 && parts[2] == "saves") {
                if (!(uid == owner || linked(owner, uid))) {
                    return false;
                }
                // firestore.rules: a head's revision only moves forward.
                if (parts.size() == 4 && op == "update" && before && after) {
                    const auto revision = [](const json::Value& fields) {
                        return std::atoll(fields["revision"]["integerValue"].as_string().c_str());
                    };
                    return revision(*after) > revision(before->fields);
                }
                return true;
            }
            return false;
        }
        if (parts.size() == 2 && parts[0] == "pairings") {
            if (op == "get") {
                return before && (field_string(before->fields, "deviceUid") == uid || !anonymous);
            }
            if (op == "create") {
                return after && field_string(*after, "deviceUid") == uid && field_string(*after, "ownerUid").empty();
            }
            if (op == "update") {
                return before && after && !anonymous && field_string(before->fields, "ownerUid").empty() &&
                       field_string(*after, "ownerUid") == uid &&
                       field_string(*after, "deviceUid") == field_string(before->fields, "deviceUid");
            }
            if (op == "delete") {
                return before && field_string(before->fields, "deviceUid") == uid;
            }
        }
        return false;
    }

    json::Value document_json(const std::string& path, const Doc& doc) const {
        json::Value value{json::Object{}};
        value["name"] = documentsRoot() + "/" + path;
        value["fields"] = doc.fields;
        value["createTime"] = doc.updateTime;
        value["updateTime"] = doc.updateTime;
        return value;
    }

    HttpResponse firestore(const HttpRequest& request) {
        const auto uid = caller(request);
        if (!uid) {
            return error(401, "UNAUTHENTICATED", "missing or expired token");
        }
        const std::string root = firestoreBase + "/" + documentsRoot();
        if (request.method == "POST" && request.url == root + ":commit") {
            return commit(*uid, request.body);
        }
        if (request.method != "GET" || request.url.rfind(root + "/", 0) != 0) {
            return error(400, "INVALID_ARGUMENT", "unsupported");
        }
        std::string path = request.url.substr(root.size() + 1);
        const auto query = path.find('?');
        if (query != std::string::npos) {
            path.resize(query);
        }
        std::string decoded;
        for (const auto& part : split(path)) {
            decoded += (decoded.empty() ? "" : "/") + decode(part);
        }
        const auto parts = split(decoded);
        if (parts.size() % 2 == 1) {  // a collection: list
            if (!allowed(*uid, "list", decoded, nullptr, nullptr)) {
                return error(403, "PERMISSION_DENIED", "denied");
            }
            json::Array documents;
            for (const auto& [name, doc] : docs) {
                if (name.rfind(decoded + "/", 0) == 0 && split(name).size() == parts.size() + 1) {
                    documents.push_back(document_json(name, doc));
                }
            }
            json::Value body{json::Object{}};
            if (!documents.empty()) {
                body["documents"] = json::Value{std::move(documents)};
            }
            return answer(200, json::dump(body));
        }
        const auto it = docs.find(decoded);
        // Firestore evaluates rules against the (possibly missing) document; reading a missing
        // document you could read is a 404.
        if (!allowed(*uid, "get", decoded, it != docs.end() ? &it->second : nullptr, nullptr)) {
            if (it == docs.end() && decoded.rfind("pairings/", 0) == 0) {
                return error(404, "NOT_FOUND", "no document");
            }
            return error(403, "PERMISSION_DENIED", "denied");
        }
        if (it == docs.end()) {
            return error(404, "NOT_FOUND", "no document");
        }
        return answer(200, json::dump(document_json(decoded, it->second)));
    }

    HttpResponse commit(const std::string& uid, const std::string& text) {
        const auto body = json::parse(text);
        if (!body || !(*body)["writes"].is_array() || (*body)["writes"].as_array().size() > 500) {
            return error(400, "INVALID_ARGUMENT", "bad commit");
        }
        if (text.size() > 10 * 1024 * 1024) {
            return error(400, "INVALID_ARGUMENT", "request too large");
        }
        const std::string prefix = documentsRoot() + "/";
        struct Planned {
            std::string path;
            bool remove = false;
            json::Value fields;
        };
        std::vector<Planned> plan;
        std::set<std::string> touched;
        for (const auto& write : (*body)["writes"].as_array()) {
            const bool remove = write.contains("delete");
            const std::string name = remove ? write["delete"].as_string() : write["update"].string_or("name");
            if (name.rfind(prefix, 0) != 0) {
                return error(400, "INVALID_ARGUMENT", "bad document name");
            }
            const std::string path = name.substr(prefix.size());
            if (split(path).size() % 2 != 0 || !touched.insert(path).second) {
                return error(400, "INVALID_ARGUMENT", "bad or repeated document");
            }
            const auto it = docs.find(path);
            const Doc* before = it != docs.end() ? &it->second : nullptr;
            const auto& condition = write["currentDocument"];
            if (condition.contains("exists")) {
                const bool exists = condition["exists"].as_bool();
                if (exists && !before) {
                    return error(404, "NOT_FOUND", "no document to update");
                }
                if (!exists && before) {
                    return error(409, "ALREADY_EXISTS", "document exists");
                }
            }
            if (condition.contains("updateTime")) {
                if (!before) {
                    return error(404, "NOT_FOUND", "no document to update");
                }
                if (before->updateTime != condition["updateTime"].as_string()) {
                    return error(400, "FAILED_PRECONDITION", "the stored version does not match");
                }
            }
            Planned planned{path, remove, {}};
            if (!remove) {
                json::Value fields = write["update"]["fields"];
                if (write.contains("updateMask")) {
                    json::Value merged = before ? before->fields : json::Value{json::Object{}};
                    for (const auto& field : write["updateMask"]["fieldPaths"].as_array()) {
                        const auto& key = field.as_string();
                        if (fields.contains(key)) {
                            merged[key] = fields[key];
                        } else {
                            merged.object().erase(key);
                        }
                    }
                    fields = merged;
                }
                if (json::dump(fields).size() > 1024 * 1024) {
                    return error(400, "INVALID_ARGUMENT", "document larger than 1 MiB");
                }
                planned.fields = std::move(fields);
            }
            const std::string op = remove ? "delete" : before ? "update" : "create";
            if (!(remove && !before) && !allowed(uid, op, path, before, remove ? nullptr : &planned.fields)) {
                return error(403, "PERMISSION_DENIED", "denied");
            }
            plan.push_back(std::move(planned));
        }
        // All preconditions and rules passed: apply atomically.
        const auto time = timestamp();
        json::Array results;
        for (auto& planned : plan) {
            if (planned.remove) {
                docs.erase(planned.path);
                results.push_back(json::Value{json::Object{}});
            } else {
                docs[planned.path] = Doc{std::move(planned.fields), time};
                json::Value result{json::Object{}};
                result["updateTime"] = time;
                results.push_back(std::move(result));
            }
        }
        json::Value answerBody{json::Object{}};
        answerBody["writeResults"] = json::Value{std::move(results)};
        answerBody["commitTime"] = time;
        return answer(200, json::dump(answerBody));
    }
};

class MemorySessionStore : public firebase::SessionStore {
public:
    std::optional<firebase::Session> stored;
    std::string serialized;  // what a file would hold, to check no password lands there
    std::optional<firebase::Session> load() override { return stored; }
    bool save(const firebase::Session& session) override {
        stored = session;
        serialized = session.uid + "|" + session.refreshToken + "|" + session.ownerUid + "|" + session.label;
        return true;
    }
    void clear() override {
        stored.reset();
        serialized.clear();
    }
};

}  // namespace crosssave::test
