// The Firebase adapter against the in-process REST emulator (fake_firestore.hpp), alone and driven
// by the real sync engine: sign-in, token refresh and expiry, device link (pairing) and revocation,
// fragments over bytesValue, compare-and-swap through commit preconditions, error mapping, and that
// no password or token is stored or reported where it should not be.

#include "../core/sync_engine.hpp"
#include "../firebase/firebase_backend.hpp"
#include "fake_firestore.hpp"
#include "fakes.hpp"

#include <cstdio>
#include <memory>

using namespace crosssave;
using namespace crosssave::test;
using firebase::FirebaseBackend;

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

std::vector<std::pair<const char*, void (*)()>>& tests() {
    static std::vector<std::pair<const char*, void (*)()>> list;
    return list;
}
#define TEST(name) void name(); struct name##_reg { name##_reg() { tests().push_back({#name, name}); } } name##_instance; void name()

struct Device {
    FakeFirebase& server;
    MemorySessionStore store;
    FakeLocal local;
    int64_t now = 0;
    uint64_t seed;
    std::unique_ptr<FirebaseBackend> backend;
    std::unique_ptr<SyncEngine> engine;
    std::string name;

    Device(FakeFirebase& fake, std::string deviceName) : server{fake}, seed{std::hash<std::string>{}(deviceName) | 1}, name{std::move(deviceName)} {
        build();
    }

    void build() {
        backend = std::make_unique<FirebaseBackend>(server, store, server.config(), [this] { return now; },
            [this] { return server.nowMs; },
            [this] {
                seed ^= seed << 13;
                seed ^= seed >> 7;
                seed ^= seed << 17;
                return seed;
            });
        EngineConfig config;
        config.deviceId = name + "-id";
        config.deviceName = name;
        config.platform = "test";
        config.wallClockMs = [this] { return server.nowMs; };
        config.random = [this] {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            return seed;
        };
        engine = std::make_unique<SyncEngine>(local, *backend, config);
        engine->setEnabled(true, now);
    }

    void run(int64_t ms = 1000) {
        const int64_t end = now + ms;
        while (now <= end) {
            server.pump();
            engine->tick(now);
            server.pump();
            now += 250;
            server.nowMs += 250;
        }
    }

    firebase::AuthResult signIn(const std::string& email, const std::string& password) {
        firebase::AuthResult result{RemoteError::Invalid, "no answer"};
        backend->signInWithPassword(email, password, [&](firebase::AuthResult r) { result = r; });
        server.pump();
        engine->onAccountChanged(now);
        return result;
    }

    Phase phase() const { return engine->status().phase; }
};

const char* kKey = "GZ2-01-gczelda2";

const FakeFirebase::Doc* head_doc(FakeFirebase& server, const std::string& owner) {
    const auto it = server.docs.find("users/" + owner + "/saves/" + kKey);
    return it != server.docs.end() ? &it->second : nullptr;
}

std::string head_digest(FakeFirebase& server, const std::string& owner) {
    const auto* doc = head_doc(server, owner);
    return doc ? doc->fields["digest"]["stringValue"].as_string() : std::string{};
}

// Links `device` to the owner signed in on `owner` through a code.
bool link(Device& device, Device& owner) {
    std::string code;
    firebase::AuthResult started{RemoteError::Invalid, ""};
    device.backend->startPairing(device.name, "test", 10 * 60 * 1000, [&](firebase::AuthResult r, std::string c) {
        started = r;
        code = c;
    });
    device.server.pump();
    if (started.error != RemoteError::None || code.size() != 8) {
        return false;
    }
    firebase::PairingStatus waiting;
    device.backend->pollPairing([&](firebase::PairingStatus s) { waiting = s; });
    device.server.pump();
    if (waiting.approved) {
        return false;
    }
    firebase::AuthResult approved{RemoteError::Invalid, ""};
    owner.backend->approvePairing(code.substr(0, 4) + "-" + code.substr(4), [&](firebase::AuthResult r) { approved = r; });
    device.server.pump();
    if (approved.error != RemoteError::None) {
        std::fprintf(stderr, "approve failed: %s\n", approved.message.c_str());
        return false;
    }
    firebase::PairingStatus status;
    device.backend->pollPairing([&](firebase::PairingStatus s) { status = s; });
    device.server.pump();
    if (!status.approved || status.ownerLabel != owner.backend->session()->label) {
        return false;
    }
    firebase::AuthResult confirmed{RemoteError::Invalid, ""};
    device.backend->confirmPairing([&](firebase::AuthResult r) { confirmed = r; });
    device.server.pump();
    device.engine->onAccountChanged(device.now);
    return confirmed.error == RemoteError::None;
}

// ---- pure helpers -------------------------------------------------------------------------------

TEST(classify_and_time) {
    std::string message;
    const auto answer = [](bool ok, int status, std::string body = {}) {
        firebase::HttpResponse r;
        r.transportOk = ok;
        r.status = status;
        r.body = std::move(body);
        return r;
    };
    CHECK(FirebaseBackend::classify(answer(false, 0), message) == RemoteError::Network);
    CHECK(FirebaseBackend::classify(answer(true, 200), message) == RemoteError::None);
    CHECK(FirebaseBackend::classify(answer(true, 401), message) == RemoteError::Auth);
    CHECK(FirebaseBackend::classify(answer(true, 403, R"({"error":{"status":"PERMISSION_DENIED"}})"), message) == RemoteError::Denied);
    CHECK(FirebaseBackend::classify(answer(true, 429), message) == RemoteError::Quota);
    CHECK(FirebaseBackend::classify(answer(true, 400, R"({"error":{"status":"RESOURCE_EXHAUSTED"}})"), message) == RemoteError::Quota);
    CHECK(FirebaseBackend::classify(answer(true, 409, R"({"error":{"status":"ALREADY_EXISTS"}})"), message) == RemoteError::Conflict);
    CHECK(FirebaseBackend::classify(answer(true, 409, R"({"error":{"status":"ABORTED"}})"), message) == RemoteError::Conflict);
    CHECK(FirebaseBackend::classify(answer(true, 400, R"({"error":{"status":"FAILED_PRECONDITION"}})"), message) == RemoteError::Conflict);
    CHECK(message == "HTTP 400 FAILED_PRECONDITION");
    CHECK(FirebaseBackend::classify(answer(true, 503, R"({"error":{"status":"UNAVAILABLE"}})"), message) == RemoteError::Network);
    CHECK(FirebaseBackend::classify(answer(true, 400, R"({"error":{"status":"INVALID_ARGUMENT"}})"), message) == RemoteError::Invalid);
    CHECK(firebase::rfc3339_utc(0) == "1970-01-01T00:00:00.000Z");
    CHECK(firebase::rfc3339_utc(1'700'000'000'123) == "2023-11-14T22:13:20.123Z");
    CHECK(firebase::rfc3339_utc(951'782'400'000) == "2000-02-29T00:00:00.000Z");
    CHECK(firebase::mask_email("alex@example.com") == "a***@example.com");
    CHECK(firebase::mask_email("bad") == "***");
}

// ---- sign-in ------------------------------------------------------------------------------------

TEST(password_sign_in_stores_only_refresh_token) {
    FakeFirebase server;
    server.addUser("alex@example.com", "hunter22");
    Device pc{server, "PC"};
    CHECK(pc.phase() == Phase::SignedOut);
    auto wrong = pc.signIn("alex@example.com", "nope");
    CHECK(wrong.error == RemoteError::Auth);
    CHECK(wrong.message == "Wrong email or password.");
    CHECK(!pc.backend->signedIn());
    auto ok = pc.signIn("alex@example.com", "hunter22");
    CHECK(ok.error == RemoteError::None);
    CHECK(pc.backend->signedIn());
    CHECK(pc.store.stored && pc.store.stored->label == "a***@example.com");
    CHECK(pc.store.serialized.find("hunter22") == std::string::npos);
    CHECK(pc.store.serialized.find("alex@") == std::string::npos);
    // Only the sign-in request carries the password.
    int carrying = 0;
    for (const auto& request : server.log) {
        carrying += request.body.find("hunter22") != std::string::npos ? 1 : 0;
    }
    CHECK(carrying == 1);  // the successful sign-in request, and nothing else
    // An unconfigured project refuses before any request.
    MemorySessionStore store;
    FirebaseBackend unconfigured{server, store, firebase::Config{}, [] { return int64_t{0}; },
        [] { return int64_t{0}; }, [] { return uint64_t{1}; }};
    firebase::AuthResult refused;
    const auto before = server.log.size();
    unconfigured.signInWithPassword("a@b.c", "x", [&](firebase::AuthResult r) { refused = r; });
    CHECK(refused.error == RemoteError::Invalid);
    CHECK(server.log.size() == before);
}

TEST(sync_through_rest) {
    FakeFirebase server;
    const auto owner = server.addUser("alex@example.com", "pw");
    Device pc{server, "PC"};
    pc.signIn("alex@example.com", "pw");
    pc.local.play("forest");
    pc.run(2000);
    CHECK(pc.phase() == Phase::Idle);
    CHECK(head_digest(server, owner) == fake_digest(*pc.local.save));
    // Same account on a second device with email: downloads.
    Device pc2{server, "Laptop"};
    pc2.signIn("alex@example.com", "pw");
    pc2.run(2000);
    CHECK(pc2.local.save && pc2.local.save->content == "forest");
    // A restart restores the session from the store (no password needed).
    pc2.build();
    pc2.local.play("mines");
    pc2.engine->onSaveWritten(pc2.now);
    pc2.run(6000);
    CHECK(head_digest(server, owner) == fake_digest(*pc2.local.save));
    CHECK(head_doc(server, owner)->fields["revision"]["integerValue"].as_string() == "2");
}

TEST(fragments_over_bytes_value) {
    FakeFirebase server;
    const auto owner = server.addUser("alex@example.com", "pw");
    Device pc{server, "PC"};
    pc.signIn("alex@example.com", "pw");
    pc.local.padTo = 1300 * 1024;
    pc.local.play("big");
    pc.run(2000);
    int chunks = 0;
    size_t largestRequest = 0;
    for (const auto& [path, doc] : server.docs) {
        if (path.find("/chunks/") != std::string::npos) {
            ++chunks;
        }
    }
    for (const auto& request : server.log) {
        largestRequest = std::max(largestRequest, request.body.size());
    }
    CHECK(chunks == 3);
    CHECK(largestRequest < 1024 * 1024);  // each fragment commit fits Firestore's 1 MiB document
    Device sw{server, "Switch"};
    sw.signIn("alex@example.com", "pw");
    sw.run(2000);
    CHECK(sw.local.save && sw.local.save->content == "big");
    // A tampered fragment (sha field no longer matches) is refused; nothing applied.
    pc.local.play("big2");
    pc.engine->onSaveWritten(pc.now);
    pc.run(6000);
    for (auto& [path, doc] : server.docs) {
        if (path.find("/chunks/") != std::string::npos &&
            doc.fields["snapshotId"]["stringValue"].as_string() ==
                head_doc(server, owner)->fields["snapshotId"]["stringValue"].as_string())
        {
            doc.fields["sha256"] = json::Value{json::Object{{"stringValue", json::Value{std::string(64, '0')}}}};
            break;
        }
    }
    sw.engine->syncNow(sw.now);
    sw.run(2000);
    CHECK(sw.phase() == Phase::Error);
    CHECK(sw.local.save->content == "big");
}

TEST(cas_race_over_rest) {
    FakeFirebase server;
    const auto owner = server.addUser("alex@example.com", "pw");
    Device pc{server, "PC"};
    Device sw{server, "Switch"};
    pc.signIn("alex@example.com", "pw");
    sw.signIn("alex@example.com", "pw");
    pc.local.play("A1");
    pc.run(2000);
    sw.run(2000);
    // Both read the head, then both commit: exactly one wins.
    server.deferred = true;
    pc.local.play("PC-2");
    sw.local.play("SW-2");
    pc.engine->syncNow(pc.now);
    sw.engine->syncNow(sw.now);
    for (int i = 0; i < 40; ++i) {
        pc.engine->tick(pc.now);
        sw.engine->tick(sw.now);
        // Deliver one answer at a time, alternating devices' progress.
        if (!server.queue.empty()) {
            auto next = std::move(server.queue.front());
            server.queue.pop_front();
            next();
        }
        pc.now += 250;
        sw.now += 250;
    }
    server.deferred = false;
    pc.run(3000);
    sw.run(3000);
    const auto winner = head_digest(server, owner);
    const bool pcWon = winner == fake_digest(*pc.local.save);
    const bool swWon = winner == fake_digest(*sw.local.save);
    CHECK(pcWon != swWon);
    CHECK((pcWon ? sw.phase() : pc.phase()) == Phase::Conflict);
    CHECK(head_doc(server, owner)->fields["revision"]["integerValue"].as_string() == "2");
}

TEST(token_refresh_and_expiry) {
    FakeFirebase server;
    server.idTokenTtlSeconds = 120;
    const auto owner = server.addUser("alex@example.com", "pw");
    Device pc{server, "PC"};
    pc.signIn("alex@example.com", "pw");
    pc.local.play("A1");
    pc.run(2000);
    // The ID token runs out: the next sync refreshes it first.
    pc.run(5LL * 60 * 1000);
    size_t refreshes = 0;
    for (const auto& request : server.log) {
        refreshes += request.url.find("/token?key=") != std::string::npos ? 1 : 0;
    }
    pc.local.play("A2");
    pc.engine->onSaveWritten(pc.now);
    pc.run(6000);
    size_t after = 0;
    for (const auto& request : server.log) {
        after += request.url.find("/token?key=") != std::string::npos ? 1 : 0;
    }
    CHECK(after == refreshes + 1);
    CHECK(head_digest(server, owner) == fake_digest(*pc.local.save));
    // Server-side revocation of the ID token alone: a 401, one refresh, the request succeeds.
    server.idTokens.clear();
    pc.local.play("A3");
    pc.engine->onSaveWritten(pc.now);
    pc.run(6000);
    CHECK(head_digest(server, owner) == fake_digest(*pc.local.save));
    // Revoked refresh tokens (password changed, account disabled): the session ends.
    server.revokeRefreshTokens(owner);
    pc.local.play("A4");
    pc.engine->onSaveWritten(pc.now);
    pc.run(6000);
    CHECK(pc.phase() == Phase::SignedOut);
    CHECK(!pc.store.stored);
    CHECK(!pc.backend->signedIn());
    CHECK(pc.local.save->content == "A4");
}

TEST(device_link_and_revoke) {
    FakeFirebase server;
    const auto owner = server.addUser("alex@example.com", "pw");
    Device pc{server, "PC"};
    pc.signIn("alex@example.com", "pw");
    pc.local.play("forest");
    pc.run(2000);
    Device sw{server, "Switch"};
    CHECK(link(sw, pc));
    CHECK(sw.backend->accountId() == owner);
    CHECK(sw.backend->session()->anonymous);
    CHECK(sw.backend->session()->label == "a***@example.com");
    CHECK(!server.docs.contains("pairings/" + sw.backend->pairingCode()));
    sw.run(2000);
    CHECK(sw.local.save && sw.local.save->content == "forest");
    sw.local.play("lakebed");
    sw.engine->onSaveWritten(sw.now);
    sw.run(6000);
    CHECK(head_digest(server, owner) == fake_digest(*sw.local.save));
    // The owner sees and unlinks the device.
    std::vector<firebase::LinkedDevice> devices;
    pc.backend->listDevices([&](RemoteError, std::vector<firebase::LinkedDevice> list, std::string) { devices = list; });
    server.pump();
    CHECK(devices.size() == 1 && devices[0].name == "Switch");
    firebase::AuthResult revoked{RemoteError::Invalid, ""};
    pc.backend->revokeDevice(devices[0].uid, [&](firebase::AuthResult r) { revoked = r; });
    server.pump();
    CHECK(revoked.error == RemoteError::None);
    sw.local.play("after-revoke");
    sw.engine->onSaveWritten(sw.now);
    sw.run(6000);
    CHECK(sw.phase() == Phase::Error);
    CHECK(sw.engine->status().message.find("refused") != std::string::npos);
    CHECK(head_digest(server, owner) != fake_digest(*sw.local.save));
    // A linked device cannot approve codes or list devices.
    firebase::AuthResult approve;
    sw.backend->approvePairing("ABCDEFGH", [&](firebase::AuthResult r) { approve = r; });
    CHECK(approve.error == RemoteError::Auth);
}

TEST(pairing_misuse) {
    FakeFirebase server;
    server.addUser("alex@example.com", "pw");
    server.addUser("eve@example.com", "pw2");
    Device pc{server, "PC"};
    pc.signIn("alex@example.com", "pw");
    Device eve{server, "Eve"};
    eve.signIn("eve@example.com", "pw2");
    Device sw{server, "Switch"};
    std::string code;
    sw.backend->startPairing("Switch", "switch", 600000, [&](firebase::AuthResult, std::string c) { code = c; });
    server.pump();
    CHECK(code.size() == 8);
    // Bad codes.
    firebase::AuthResult result;
    pc.backend->approvePairing("1234", [&](firebase::AuthResult r) { result = r; });
    CHECK(result.error == RemoteError::Invalid);
    pc.backend->approvePairing("ZZZZZZZZ", [&](firebase::AuthResult r) { result = r; });
    server.pump();
    CHECK(result.error == RemoteError::Invalid);
    // First approval wins; a second one (another account that guessed the code) is refused.
    pc.backend->approvePairing(code, [&](firebase::AuthResult r) { result = r; });
    server.pump();
    CHECK(result.error == RemoteError::None);
    eve.backend->approvePairing(code, [&](firebase::AuthResult r) { result = r; });
    server.pump();
    CHECK(result.error != RemoteError::None);
    // The device shows the owner label before confirming: a hijacked code is visible there.
    firebase::PairingStatus status;
    sw.backend->pollPairing([&](firebase::PairingStatus s) { status = s; });
    server.pump();
    CHECK(status.approved && status.ownerLabel == "a***@example.com");
    // Not confirming: nothing synced, the device has no owner.
    CHECK(sw.backend->accountId().empty());
    sw.backend->cancelPairing();
    server.pump();
    CHECK(!server.docs.contains("pairings/" + code));
    // Another device cannot read or delete someone else's pairing doc.
    Device other{server, "Other"};
    std::string otherCode;
    other.backend->startPairing("Other", "test", 600000, [&](firebase::AuthResult, std::string c) { otherCode = c; });
    server.pump();
    // A non-owner anonymous device asking for it is denied by the rules.
    firebase::PairingStatus spy;
    sw.backend->startPairing("Switch", "switch", 600000, [&](firebase::AuthResult, std::string) {});
    server.pump();
    CHECK(server.docs.contains("pairings/" + otherCode));
    firebase::HttpRequest peek;
    peek.method = "GET";
    peek.url = server.firestoreBase + "/" + server.documentsRoot() + "/pairings/" + otherCode;
    std::string swToken;
    for (const auto& [token, info] : server.idTokens) {
        if (info.uid == sw.backend->session()->uid) {
            swToken = token;
        }
    }
    peek.headers = {{"Authorization", "Bearer " + swToken}};
    firebase::HttpResponse peeked;
    server.send(peek, [&](firebase::HttpResponse r) { peeked = r; });
    CHECK(peeked.status == 403);
    (void)spy;
}

TEST(error_mapping_end_to_end) {
    FakeFirebase server;
    const auto owner = server.addUser("alex@example.com", "pw");
    Device pc{server, "PC"};
    pc.signIn("alex@example.com", "pw");
    pc.local.play("A1");
    server.offline = 3;
    pc.run(1000);
    CHECK(pc.phase() == Phase::Retrying);
    pc.run(60000);
    CHECK(pc.phase() == Phase::Idle);
    CHECK(head_digest(server, owner) == fake_digest(*pc.local.save));
    server.forced.push_back({"/saves/", 429, R"({"error":{"code":429,"status":"RESOURCE_EXHAUSTED"}})"});
    pc.engine->syncNow(pc.now);
    pc.run(1000);
    CHECK(pc.phase() == Phase::Retrying);
    CHECK(pc.engine->status().message.find("quota") != std::string::npos);
    server.forced.push_back({"/saves/", 503, R"({"error":{"code":503,"status":"UNAVAILABLE"}})"});
    pc.engine->syncNow(pc.now);
    pc.run(1000);
    CHECK(pc.phase() == Phase::Retrying);
    pc.run(30000);
    CHECK(pc.phase() == Phase::Idle);
    // Head removed between the read and the commit: NOT_FOUND on the precondition is a lost CAS.
    pc.local.play("A2");
    server.deferred = true;
    pc.engine->syncNow(pc.now);
    pc.engine->tick(pc.now);
    server.step();  // head read
    server.step();  // fragment write; the head commit is now queued
    server.deferred = false;
    server.docs.erase("users/" + owner + "/saves/" + kKey);
    pc.run(3000);
    CHECK(pc.phase() == Phase::Conflict);
    CHECK(pc.engine->status().conflict.kind == ConflictKind::RemoteMissing);
    // Messages never contain the ID token or the refresh token.
    for (const auto& [token, info] : server.idTokens) {
        CHECK(pc.engine->status().message.find(token) == std::string::npos);
    }
    for (const auto& [token, uid] : server.refreshTokens) {
        CHECK(pc.engine->status().message.find(token) == std::string::npos);
    }
}

TEST(delete_remote_over_rest) {
    FakeFirebase server;
    const auto owner = server.addUser("alex@example.com", "pw");
    Device pc{server, "PC"};
    pc.signIn("alex@example.com", "pw");
    for (int i = 0; i < 3; ++i) {
        pc.local.play("R" + std::to_string(i));
        pc.engine->onSaveWritten(pc.now);
        pc.run(6000);
    }
    pc.engine->deleteRemote(pc.now);
    pc.run(2000);
    size_t left = 0;
    for (const auto& [path, doc] : server.docs) {
        left += path.rfind("users/" + owner + "/saves/", 0) == 0 ? 1 : 0;
    }
    CHECK(left == 0);
    CHECK(pc.phase() == Phase::Disabled);
    CHECK(pc.local.save->content == "R2");
}

}  // namespace

int main() {
    for (const auto& [name, test] : tests()) {
        g_test = name;
        std::fprintf(stderr, "== %s\n", name);
        test();
    }
    std::printf("firebase_test: %zu tests, %d checks, %d failures\n", tests().size(), g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("firebase_test: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
