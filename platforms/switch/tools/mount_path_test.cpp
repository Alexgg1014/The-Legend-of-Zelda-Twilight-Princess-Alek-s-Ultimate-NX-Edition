// H37-c5: libnx mount-qualified paths ("sdmc:/...") in the mod services. Host test of src/dusk/mount_path.hpp.
#include "dusk/mount_path.hpp"

#include <cstdio>
#include <string>

namespace fs = std::filesystem;
namespace mp = dusk::mount_path;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { const std::string x = (a), y = (b); if (x != y) { \
    std::printf("FAIL %s:%d: '%s' != '%s'\n", __FILE__, __LINE__, x.c_str(), y.c_str()); ++failures; } } while (0)

int main() {
    const fs::path config = "sdmc:/switch/dusklight";
    const std::string id = "dev.twilitrealm.randomizer";
    const std::string want = "sdmc:/switch/dusklight/mod_data/dev.twilitrealm.randomizer";
    const fs::path cwd = "sdmc:/switch/dusklight-h37-c4";  // where H37-c4 was launched from

    // What libnx's std::filesystem::absolute() did: cwd / path, because "sdmc:/x" has no leading '/'.
    CHECK(!fs::path(want).is_absolute());
    CHECK_EQ((cwd / (config / "mod_data" / id)).string(),
             "sdmc:/switch/dusklight-h37-c4/sdmc:/switch/dusklight/mod_data/dev.twilitrealm.randomizer");

    std::error_code ec;
    CHECK_EQ(mp::persistent_data_dir(config, id, ec).string(), want);
    CHECK(!ec);
    CHECK_EQ(mp::absolute_from(config / "mod_data" / id, cwd).string(), want);  // cwd ignored
    CHECK_EQ(mp::absolute_from(config / "mod_data" / id, "/somewhere/else").string(), want);
    CHECK(mp::absolute_from(want, cwd).string().find("sdmc:", 1) == std::string::npos);  // one prefix only
    CHECK_EQ(mp::absolute(want).string(), want);
    CHECK_EQ(mp::absolute("sdmc:/switch/./dusklight/x/../mod_data").string(), "sdmc:/switch/dusklight/mod_data");

    // Ordinary host paths keep the standard behaviour.
    CHECK_EQ(mp::absolute_from("a/b", "/work").string(), "/work/a/b");
    CHECK_EQ(mp::absolute_from("/abs/b", "/work").string(), "/abs/b");
    CHECK(!mp::is_mount_qualified("a:b") && !mp::is_mount_qualified("/a:/b") && !mp::is_mount_qualified(":/b") &&
          !mp::is_mount_qualified("dir/a:/b") && mp::is_mount_qualified("romfs:/x"));
    CHECK(mp::is_absolute("sdmc:/x") && mp::is_absolute("/x") && !mp::is_absolute("x/y") && !mp::is_absolute("sdmc:x"));
    CHECK_EQ(mp::persistent_data_dir("/home/u/.config/dusklight", id, ec).string(),
             "/home/u/.config/dusklight/mod_data/" + id);

    // HTTP containment: scratch and persistent data only, no staging, no escape, no other mount.
    const fs::path scratch = "sdmc:/switch/dusklight/cache/mods/dev.x/data";
    const fs::path data = want;
    auto ok = [&](const char* p) { return mp::validate_download_path(p, scratch, data).has_value(); };
    CHECK(ok("sdmc:/switch/dusklight/mod_data/dev.twilitrealm.randomizer/seeds/a.json"));
    CHECK(ok("sdmc:/switch/dusklight/cache/mods/dev.x/data/out.bin"));
    CHECK(ok("sdmc:/switch/dusklight/mod_data/dev.twilitrealm.randomizer/./x/../y"));
    CHECK_EQ(mp::validate_download_path("sdmc:/switch/dusklight/mod_data/dev.twilitrealm.randomizer/./x/../y", scratch,
                                        data)->string(),
             want + "/y");
    CHECK(!ok("sdmc:/switch/dusklight/mod_data/dev.twilitrealm.randomizer"));                       // the root itself
    CHECK(!ok("sdmc:/switch/dusklight/mod_data/dev.twilitrealm.randomizer/../dev.other/a"));       // escape by ..
    CHECK(!ok("sdmc:/switch/dusklight/mod_data/dev.twilitrealm.randomizer/../../../../etc/x"));
    CHECK(!ok("sdmc:/switch/dusklight/cache/mods/dev.x/data/downloads/0123.part"));               // staging
    CHECK(!ok("sdmc:/switch/dusklight/cache/mods/dev.x/data/downloads"));
    CHECK(!ok("sdmc:/switch/dusklight/cache/mods/dev.x/data/sub/../downloads/a"));
    CHECK(!ok("romfs:/switch/dusklight/mod_data/dev.twilitrealm.randomizer/a"));                  // another mount
    CHECK(!ok("save:/switch/dusklight/cache/mods/dev.x/data/a"));
    CHECK(!ok("sdmc:/switch/dusklight/mod_data/dev.twilitrealm.randomizer-evil/a"));              // sibling prefix
    CHECK(!ok("relative/path"));                                                                   // not absolute
    CHECK(!ok("sdmc:/switch/dusklight-h37-c4/sdmc:/switch/dusklight/mod_data/dev.twilitrealm.randomizer/a"));
    // The same on a host path.
    auto hostOk = [&](const char* p) {
        return mp::validate_download_path(p, "/cache/dev.x/data", "/home/u/mod_data/dev.x").has_value();
    };
    CHECK(hostOk("/home/u/mod_data/dev.x/a") && hostOk("/cache/dev.x/data/a"));
    CHECK(!hostOk("/cache/dev.x/data/downloads/a") && !hostOk("/home/u/mod_data/dev.x/../y") && !hostOk("rel"));

    std::printf("mount paths: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
