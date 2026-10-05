// H37-c5: the failing lookup through the real GetRandomizerPath*(): fail() exactly once, nullopt for all five,
// and no filesystem access afterwards. A separate process from the success test (the lookup is cached).
#include "paths.hpp"

#include "session.hpp"

#include <cstdio>
#include <string>

namespace randomizer::session { ServiceManager svc_mng; }
static int failures = 0, dataDirCalls = 0, failCalls = 0;
static std::string message;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static ModResult dataDir(ModContext*, const char** out) { ++dataDirCalls; *out = nullptr; return MOD_ERROR; }
static void fail(ModContext*, ModResult, const char* m) { ++failCalls; message = m; }

int main() {
    static HostService host{};
    host.data_dir = dataDir;
    host.fail = fail;
    randomizer::session::svc_mng.host = &host;
    namespace rp = randomizer::paths;
    for (int i = 0; i < 3; ++i) {
        CHECK(!rp::GetRandomizerPath());
        CHECK(!rp::GetRandomizerSettingsPath());
        CHECK(!rp::GetRandomizerPreferencesPath());
        CHECK(!rp::GetRandomizerPresetsPath());
        CHECK(!rp::GetRandomizerSeedsPath());
    }
    CHECK(failCalls == 1);
    CHECK(dataDirCalls == 1);
    CHECK(message == "Failed to get data directory");
    std::printf("randomizer paths (failure): %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
