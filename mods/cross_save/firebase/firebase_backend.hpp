#pragma once

// RemoteBackend over Firebase Authentication REST and Cloud Firestore REST (Spark plan, no server).
// Only GET and POST are used (HttpService has no PATCH/DELETE): reads are GET, every write is a
// POST documents:commit, whose per-write `currentDocument` precondition gives compare-and-swap.
//
// Data layout (see firestore.rules next to this file):
//   users/{owner}/saves/{key}                    head: revision, digest, history, ...
//   users/{owner}/saves/{key}/chunks/{sid}_{n}   fragment n of snapshot sid (<= 512 KiB)
//   users/{owner}/devices/{deviceUid}            devices linked to the owner's account
//   pairings/{code}                              short-lived device-link requests
//
// Two ways to sign in, both supported here (the player-facing choice is open, see
// docs/investigaciones/crosssave-resultado-c1.md):
//   * email + password of the owner account on this device, or
//   * device link: the device signs in anonymously, shows a code, and an owner session elsewhere
//     approves it; the device then reads and writes the owner's saves as a linked device.
// Passwords are sent once to Firebase Auth and never stored; only the refresh token is.

#include "../core/sync_types.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace crosssave::firebase {

struct HttpRequest {
    std::string method;  // "GET" or "POST"
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    size_t maxResponseBytes = 2 * 1024 * 1024;
};

struct HttpResponse {
    bool transportOk = false;  // false: no HTTP answer (offline, timeout, TLS)
    std::string error;
    int status = 0;
    std::string body;
};

// Asynchronous; the callback runs exactly once, on the caller's thread.
class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    virtual void send(HttpRequest request, std::function<void(HttpResponse)> done) = 0;
};

struct Config {
    std::string apiKey;     // the project's Web API key (public by design)
    std::string projectId;
    std::string authBase = "https://identitytoolkit.googleapis.com/v1";
    std::string tokenBase = "https://securetoken.googleapis.com/v1";
    std::string firestoreBase = "https://firestore.googleapis.com/v1";
    std::string database = "(default)";

    bool valid() const { return !apiKey.empty() && !projectId.empty(); }
};

// What survives a restart. The refresh token is a credential: the store decides where it lives
// (see SessionStore implementations per platform).
struct Session {
    std::string uid;           // this device's Firebase user
    std::string refreshToken;
    std::string ownerUid;      // whose saves this device syncs (== uid for an email sign-in)
    std::string label;         // shown to the player, e.g. "a***@example.com"
    bool anonymous = false;    // a linked device rather than the owner's own sign-in
};

class SessionStore {
public:
    virtual ~SessionStore() = default;
    virtual std::optional<Session> load() = 0;
    virtual bool save(const Session& session) = 0;
    virtual void clear() = 0;
};

struct AuthResult {
    RemoteError error = RemoteError::None;
    std::string message;  // safe to show; never contains tokens
};

struct PairingStatus {
    RemoteError error = RemoteError::None;
    std::string message;
    bool approved = false;
    bool expired = false;
    std::string ownerLabel;
};

struct LinkedDevice {
    std::string uid;
    std::string name;
    std::string platform;
    int64_t linkedAtMs = 0;
};

class FirebaseBackend : public RemoteBackend {
public:
    FirebaseBackend(HttpTransport& http, SessionStore& store, Config config,
        std::function<int64_t()> monotonicMs, std::function<int64_t()> wallClockMs,
        std::function<uint64_t()> random);

    const Config& config() const { return m_config; }
    bool signedIn() const { return m_session.has_value() && !m_session->ownerUid.empty(); }
    const std::optional<Session>& session() const { return m_session; }

    // Owner account on this device.
    void signInWithPassword(const std::string& email, const std::string& password,
        std::function<void(AuthResult)> done);
    // Forgets the session on this device. Other devices stay linked; revoke them from the owner.
    void signOut();

    // Device link, device side: signs in anonymously (once) and publishes a code valid for
    // ttlMs. Poll with pollPairing until approved, then confirmPairing to start syncing.
    void startPairing(const std::string& deviceName, const std::string& platform, int64_t ttlMs,
        std::function<void(AuthResult, std::string code)> done);
    void pollPairing(std::function<void(PairingStatus)> done);
    // The player confirmed the owner label: sync as that owner from now on.
    void confirmPairing(std::function<void(AuthResult)> done);
    void cancelPairing();
    const std::string& pairingCode() const { return m_pairingCode; }

    // Device link, owner side (signed in with email): approves a code shown by another device.
    void approvePairing(const std::string& code, std::function<void(AuthResult)> done);
    void listDevices(std::function<void(RemoteError, std::vector<LinkedDevice>, std::string)> done);
    void revokeDevice(const std::string& deviceUid, std::function<void(AuthResult)> done);

    // RemoteBackend
    std::string accountId() const override;
    void getHead(const std::string& key, HeadCallback done) override;
    void putChunk(const std::string& key, const std::string& snapshotId, uint32_t index,
        const Bytes& data, const std::string& sha256, DoneCallback done) override;
    void getChunk(const std::string& key, const std::string& snapshotId, uint32_t index,
        ChunkCallback done) override;
    void commitHead(const std::string& key, const std::optional<RemoteHead>& expected,
        const RemoteHead& next, CommitCallback done) override;
    void deleteSnapshot(const std::string& key, const std::string& snapshotId,
        uint32_t chunkCount, DoneCallback done) override;
    void deleteHead(const std::string& key, const RemoteHead& expected, DoneCallback done) override;

    // Exposed for tests: how an HTTP answer maps to the neutral errors.
    static RemoteError classify(const HttpResponse& response, std::string& message);

private:
    using TokenCallback = std::function<void(RemoteError, std::string message)>;

    std::string documentsRoot() const;  // projects/{p}/databases/{db}/documents
    std::string documentUrl(const std::string& path) const;
    std::string headPath(const std::string& key) const;
    std::string chunkPath(const std::string& key, const std::string& sid, uint32_t index) const;
    void withToken(TokenCallback done);
    void refresh(TokenCallback done);
    void finishRefresh(RemoteError error, const std::string& message);
    // Sends with the current ID token; on 401 refreshes once and resends.
    void authorized(HttpRequest request, std::function<void(HttpResponse)> done, bool retried = false);
    void commit(std::string writesJson, std::function<void(RemoteError, std::string body, std::string message)> done);
    void storeAuthAnswer(const std::string& body, bool anonymous, const std::string& label,
        std::function<void(AuthResult)> done);

    HttpTransport& m_http;
    SessionStore& m_store;
    Config m_config;
    std::function<int64_t()> m_monotonicMs;
    std::function<int64_t()> m_wallClockMs;
    std::function<uint64_t()> m_random;
    std::optional<Session> m_session;
    std::string m_idToken;  // memory only
    int64_t m_idTokenExpiresAt = 0;
    bool m_refreshing = false;
    std::deque<TokenCallback> m_tokenWaiters;
    std::string m_pairingCode;
    std::string m_pairingOwner;
    std::string m_pairingOwnerLabel;
};

// RFC 3339 UTC with milliseconds, as Firestore timestampValue expects.
std::string rfc3339_utc(int64_t unixMs);
// Masks an email for display: "alex@example.com" -> "a***@example.com".
std::string mask_email(const std::string& email);

}  // namespace crosssave::firebase
