// Cloud Save smoke test against the REAL Firebase project (Windows console). The production
// engine and Firebase adapter, two simulated devices with fake saves, under save keys of their own
// (GZ2-01-smoketest, GZ2-01-smokeprobe): the real save (GZ2-01-gczelda2) is never touched.
//
// It checks what the in-process emulator cannot: the real security rules, the real answers to
// commit preconditions, token refresh, device link and revocation, fragments over bytesValue.
// Everything it creates is deleted at the end (the linked test device's anonymous user stays in
// Authentication > Users; delete it there if you like).
//
// The password is read without echo, sent once to Firebase Auth and never stored or printed.
//
// usage: cloudsave_smoke.exe firebase.json [email] [--approve CODE]

#include "../core/json.hpp"
#include "../core/sync_engine.hpp"
#include "../firebase/firebase_backend.hpp"
#include "../tests/fakes.hpp"
#include "winhttp_transport.hpp"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>

using namespace crosssave;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string& what, const std::string& detail = {}) {
    (ok ? g_pass : g_fail)++;
    std::printf("  [%s] %s%s%s\n", ok ? "OK  " : "FAIL", what.c_str(), detail.empty() ? "" : " -- ",
        detail.c_str());
}

void note(const std::string& text) {
    std::printf("  %s\n", text.c_str());
}

class MemoryStore : public firebase::SessionStore {
public:
    std::optional<firebase::Session> session;
    std::optional<firebase::Session> load() override { return session; }
    bool save(const firebase::Session& value) override {
        session = value;
        return true;
    }
    void clear() override { session.reset(); }
};

std::mt19937_64& rng() {
    static std::mt19937_64 engine{std::random_device{}()};
    return engine;
}

int64_t ticks() {
    return static_cast<int64_t>(GetTickCount64());
}

int64_t wall() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const uint64_t value = (uint64_t{ft.dwHighDateTime} << 32) | ft.dwLowDateTime;
    return static_cast<int64_t>(value / 10000 - 11644473600000ULL);
}

struct Device {
    std::string name;
    MemoryStore store;
    test::FakeLocal local;
    std::unique_ptr<firebase::FirebaseBackend> backend;
    std::unique_ptr<SyncEngine> engine;
    int64_t now = 1000;

    Device(firebase::HttpTransport& http, const firebase::Config& config, std::string deviceName)
        : name{std::move(deviceName)} {
        local.key = "GZ2-01-smoketest";
        backend = std::make_unique<firebase::FirebaseBackend>(http, store, config, ticks, wall, [] { return rng()(); });
        EngineConfig engineConfig;
        engineConfig.deviceId = name + "-" + std::to_string(rng()() % 100000);
        engineConfig.deviceName = name;
        engineConfig.platform = "smoke";
        engineConfig.wallClockMs = wall;
        engineConfig.random = [] { return rng()(); };
        engine = std::make_unique<SyncEngine>(local, *backend, engineConfig);
        engine->setEnabled(true, now);
    }

    // Simulated time: the transport is synchronous, so every step completes inside tick().
    void run(int64_t ms = 6000) {
        for (const int64_t end = now + ms; now <= end; now += 250) {
            engine->tick(now);
        }
    }
    Phase phase() const { return engine->status().phase; }
    std::string status() const {
        return std::string{phase_name(engine->status().phase)} + ": " + engine->status().message;
    }
};

std::string read_password() {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(input, &mode);
    SetConsoleMode(input, mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
    std::string password;
    std::getline(std::cin, password);
    SetConsoleMode(input, mode);
    std::printf("\n");
    return password;
}

RemoteHead probe_head(int64_t revision) {
    RemoteHead head;
    head.revision = revision;
    head.snapshotId = std::string(32, 'a');
    head.digest = std::string(64, 'b');
    head.blobSha256 = std::string(64, 'c');
    head.size = 1;
    head.chunkCount = 1;
    head.deviceId = "probe";
    head.deviceName = "probe";
    head.platform = "smoke";
    head.game = "GZ2E";
    head.createdAtMs = wall();
    head.history.push_back({head.snapshotId, revision, head.digest, head.blobSha256, 1, 1, "probe", head.createdAtMs});
    return head;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        std::printf("usage: cloudsave_smoke.exe firebase.json [email] [--approve CODE]\n");
        return 2;
    }
    std::ifstream file{argv[1]};
    std::stringstream text;
    text << file.rdbuf();
    const auto json = json::parse(text.str());
    if (!json) {
        std::printf("cannot read %s\n", argv[1]);
        return 2;
    }
    firebase::Config config;
    config.apiKey = json->string_or("apiKey");
    config.projectId = json->string_or("projectId");
    const char* approveCode = nullptr;
    std::string email;
    for (int i = 2; i < argc; ++i) {
        if (std::string{argv[i]} == "--approve" && i + 1 < argc) {
            approveCode = argv[++i];
        } else {
            email = argv[i];
        }
    }
    if (email.empty()) {
        std::printf("Email of the owner account: ");
        std::getline(std::cin, email);
    }
    std::printf("Password (not shown, not stored): ");
    std::string password = read_password();

    smoke::WinHttpTransport http;
    std::printf("Project %s\n\n1. Owner sign-in (PC)\n", config.projectId.c_str());
    Device pc{http, config, "Smoke-PC"};
    firebase::AuthResult auth{RemoteError::Invalid, "no answer"};
    pc.backend->signInWithPassword(email, password, [&](firebase::AuthResult r) { auth = r; });
    password.assign(password.size(), '\0');
    password.clear();
    check(auth.error == RemoteError::None, "sign in with email/password", auth.message);
    if (auth.error != RemoteError::None) {
        return 1;
    }
    const std::string owner = pc.backend->accountId();
    if (approveCode) {
        // --approve CODE: only approve a device that shows this link code (e.g. the Switch).
        pc.backend->approvePairing(approveCode, [&](firebase::AuthResult r) { auth = r; });
        check(auth.error == RemoteError::None, std::string{"approve link code "} + approveCode, auth.message);
        pc.backend->signOut();
        std::printf("\nNow confirm the account on the device.\n");
        return g_fail == 0 ? 0 : 1;
    }
    pc.engine->onAccountChanged(pc.now);

    std::printf("\n2. Commit preconditions (key GZ2-01-smokeprobe)\n");
    const std::string probe = "GZ2-01-smokeprobe";
    RemoteHead created;
    std::string message;
    RemoteError error = RemoteError::Invalid;
    pc.backend->commitHead(probe, std::nullopt, probe_head(1), [&](RemoteError e, RemoteHead h, std::string m) {
        error = e;
        created = h;
        message = m;
    });
    check(error == RemoteError::None, "create head with exists:false", message);
    pc.backend->commitHead(probe, std::nullopt, probe_head(1), [&](RemoteError e, RemoteHead, std::string m) {
        error = e;
        message = m;
    });
    check(error == RemoteError::Conflict, "exists:false on an existing head is a lost CAS", "answer: " + message);
    RemoteHead stale = created;
    stale.casToken = "2020-01-01T00:00:00.000000Z";
    pc.backend->commitHead(probe, stale, probe_head(2), [&](RemoteError e, RemoteHead, std::string m) {
        error = e;
        message = m;
    });
    check(error == RemoteError::Conflict, "stale updateTime is a lost CAS", "answer: " + message);
    RemoteHead second;
    pc.backend->commitHead(probe, created, probe_head(2), [&](RemoteError e, RemoteHead h, std::string m) {
        error = e;
        second = h;
        message = m;
    });
    check(error == RemoteError::None, "matching updateTime commits", message);
    pc.backend->commitHead(probe, second, probe_head(2), [&](RemoteError e, RemoteHead, std::string m) {
        error = e;
        message = m;
    });
    check(error == RemoteError::Denied, "rules refuse a revision that does not move forward", "answer: " + message);
    pc.backend->deleteHead(probe, second, [&](RemoteError e, std::string m) {
        error = e;
        message = m;
    });
    check(error == RemoteError::None, "delete head with matching updateTime", message);
    pc.backend->commitHead(probe, second, probe_head(3), [&](RemoteError e, RemoteHead, std::string m) {
        error = e;
        message = m;
    });
    check(error == RemoteError::Conflict, "updateTime on a deleted head is a lost CAS", "answer: " + message);
    pc.backend->putChunk(probe, std::string(32, 'd'), 0, Bytes{}, std::string(64, 'e'), [&](RemoteError e, std::string m) {
        error = e;
        message = m;
    });
    check(error == RemoteError::Denied, "rules refuse an empty fragment", "answer: " + message);

    std::printf("\n3. First upload from the PC (key GZ2-01-smoketest)\n");
    pc.local.play("forest");
    pc.run(3000);
    check(pc.phase() == Phase::Idle && pc.engine->status().remoteRevision == 1, "uploaded revision 1", pc.status());

    std::printf("\n4. Device link: a second device signs in anonymously and shows a code\n");
    Device sw{http, config, "Smoke-Switch"};
    std::string code;
    sw.backend->startPairing("Smoke-Switch", "smoke", 10 * 60 * 1000, [&](firebase::AuthResult r, std::string c) {
        auth = r;
        code = c;
    });
    check(auth.error == RemoteError::None && code.size() == 8, "link code published", code.empty() ? auth.message : code);
    {
        // An anonymous device cannot touch the owner's saves before it is linked.
        RemoteError denied = RemoteError::None;
        sw.backend->getHead("GZ2-01-smoketest", [&](RemoteError e, std::optional<RemoteHead>, std::string) { denied = e; });
        check(denied == RemoteError::Auth, "an unlinked device has no owner to sync with");
    }
    pc.backend->approvePairing(code, [&](firebase::AuthResult r) { auth = r; });
    check(auth.error == RemoteError::None, "owner approves the code", auth.message);
    firebase::PairingStatus pairing;
    sw.backend->pollPairing([&](firebase::PairingStatus s) { pairing = s; });
    check(pairing.approved, "device sees the approval", "label " + pairing.ownerLabel);
    sw.backend->confirmPairing([&](firebase::AuthResult r) { auth = r; });
    check(auth.error == RemoteError::None && sw.backend->accountId() == owner, "device confirms and syncs as the owner",
        auth.message);
    sw.engine->onAccountChanged(sw.now);
    sw.run(3000);
    check(sw.local.save && sw.local.save->content == "forest", "linked device downloaded the PC's save", sw.status());

    std::printf("\n5. Back and forth, fragments, conflict\n");
    sw.local.padTo = 1300 * 1024;  // 3 fragments
    sw.local.play("lakebed");
    sw.engine->onSaveWritten(sw.now);
    sw.run(8000);
    check(sw.engine->status().remoteRevision == 2, "linked device uploaded 1.3 MB in 3 fragments", sw.status());
    pc.engine->onSessionStart(pc.now);
    pc.run(3000);
    check(pc.local.save && pc.local.save->content == "lakebed", "PC downloaded it", pc.status());
    pc.local.play("pc-offline");
    sw.local.play("switch-offline");
    sw.engine->syncNow(sw.now);
    sw.run(3000);
    pc.engine->syncNow(pc.now);
    pc.run(3000);
    check(pc.phase() == Phase::Conflict && pc.engine->status().conflict.kind == ConflictKind::Diverged,
        "two devices that diverged get a conflict, nothing overwritten", pc.status());
    pc.engine->resolve(Choice::KeepLocal, pc.now);
    pc.run(3000);
    check(pc.phase() == Phase::Idle && pc.engine->status().remoteRevision == 4, "keep this device's copy -> revision 4",
        pc.status());

    std::printf("\n6. Revoke the linked device\n");
    std::vector<firebase::LinkedDevice> devices;
    pc.backend->listDevices([&](RemoteError, std::vector<firebase::LinkedDevice> list, std::string) { devices = list; });
    check(devices.size() >= 1, "owner lists linked devices", std::to_string(devices.size()) + " device(s)");
    auth = {RemoteError::Invalid, "the test device was not in the list"};
    for (const auto& device : devices) {
        if (device.uid == sw.backend->session()->uid) {
            pc.backend->revokeDevice(device.uid, [&](firebase::AuthResult r) { auth = r; });
        }
    }
    check(auth.error == RemoteError::None, "device unlinked", auth.message);
    sw.local.play("after-revoke");
    sw.engine->syncNow(sw.now);
    sw.run(3000);
    check(sw.phase() == Phase::Error, "the unlinked device is refused at once", sw.status());

    std::printf("\n7. Clean up\n");
    pc.engine->deleteRemote(pc.now);
    pc.run(3000);
    std::optional<RemoteHead> left;
    error = RemoteError::Invalid;
    pc.backend->getHead("GZ2-01-smoketest", [&](RemoteError e, std::optional<RemoteHead> h, std::string) {
        error = e;
        left = h;
    });
    check(error == RemoteError::None && !left, "test cloud copies deleted", pc.status());
    sw.backend->signOut();
    pc.backend->signOut();

    std::printf("\n%d OK, %d FAIL, %d HTTP requests. Nothing was stored on this PC.\n", g_pass, g_fail, http.requests);
    std::printf("The test device's anonymous user remains in Authentication > Users (safe to delete).\n");
    return g_fail == 0 ? 0 : 1;
}
