// H37-c5: the Randomizer's data-directory lookup against a stub host (the real src/paths.cpp and
// src/data_dir.hpp of the patched Randomizer, with a stub session.hpp).
#include "data_dir.hpp"
#include "paths.hpp"

#include "session.hpp"  // the stub

#include <cstdio>
#include <string>

namespace randomizer::session { ServiceManager svc_mng; }

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static int g_dataDirCalls, g_failCalls;
static ModResult g_result = MOD_OK;
static const char* g_path = nullptr;
static std::string g_failMessage;

static ModResult dataDir(ModContext*, const char** out) {
    ++g_dataDirCalls;
    *out = g_result == MOD_OK ? g_path : nullptr;
    return g_result;
}
static void fail(ModContext*, ModResult, const char* message) { ++g_failCalls; g_failMessage = message; }

static void useHost(ModResult result, const char* path) {
    static HostService host{};
    host.data_dir = dataDir;
    host.fail = fail;
    randomizer::session::svc_mng.host = &host;
    g_result = result; g_path = path; g_dataDirCalls = g_failCalls = 0; g_failMessage.clear();
}

namespace rp = randomizer::paths;

int main() {
    // 1. A good path: all five resolve under it, the host is asked once, fail() never.
    // (the lookup is cached per process; the failure cases use the header function directly)
    useHost(MOD_OK, "sdmc:/switch/dusklight/mod_data/dev.twilitrealm.randomizer");
    const auto base = rp::GetRandomizerPath();
    CHECK(base && base->generic_string() == "sdmc:/switch/dusklight/mod_data/dev.twilitrealm.randomizer");
    CHECK(rp::GetRandomizerSettingsPath()->generic_string().ends_with("/dev.twilitrealm.randomizer/settings.yaml"));
    CHECK(rp::GetRandomizerPreferencesPath()->generic_string().ends_with("/preferences.yaml"));
    CHECK(rp::GetRandomizerPresetsPath()->generic_string().ends_with("/presets"));
    CHECK(rp::GetRandomizerSeedsPath()->generic_string().ends_with("/seeds"));
    CHECK(g_dataDirCalls == 1 && g_failCalls == 0);
    CHECK(base->generic_string().find("sdmc:", 1) == std::string::npos);  // one mount prefix

    // 2. data_dir refuses: nullopt, outPath stays null, nothing built from it.
    useHost(MOD_ERROR, "ignored");
    ModContext* ctx = nullptr;
    CHECK(!rp::ResolveDataDir(randomizer::session::svc_mng.host, ctx));
    useHost(MOD_OK, nullptr);                               // OK but no path
    CHECK(!rp::ResolveDataDir(randomizer::session::svc_mng.host, ctx));
    useHost(MOD_OK, "");                                    // OK but empty: would mean the cwd
    CHECK(!rp::ResolveDataDir(randomizer::session::svc_mng.host, ctx));
    CHECK(!rp::ResolveDataDir(nullptr, ctx));
    HostService none{};
    CHECK(!rp::ResolveDataDir(&none, ctx));
    CHECK(g_failCalls == 0);  // the lookup itself never calls fail: the caller decides, once

    std::printf("randomizer paths (success): %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
