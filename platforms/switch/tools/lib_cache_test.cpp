// aleks2 (V1-rc19): src/dusk/mods/loader/lib_cache.cpp -- the extracted mod libraries kept between boots. Walks a mod
// through what the console does: first boot (written), next boot (hit), a damaged cache, an update (new key), an
// uninstall and a half-written folder (pruned), a read failure (empty result, nothing left behind), a directory mod
// (no cache), a deleted cache (rebuilt).
#include "dusk/mods/loader/lib_cache.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using namespace dusk::mods;
static int s_failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++s_failures;
    }
}

static std::map<std::string, std::string> s_package;  // entry -> bytes: the package's contents
static void write_package(const fs::path& p, const std::string& content) {
    std::ofstream(p, std::ios::binary) << content;
}
static std::vector<uint8_t> read_entry(const std::string& entry) {
    const auto it = s_package.find(entry);
    if (it == s_package.end()) throw std::runtime_error("no such entry");
    return {it->second.begin(), it->second.end()};
}
static std::string file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
static std::vector<uint8_t> no_extract(const std::string&) {
    throw std::runtime_error("must not extract on a hit");
}
static std::vector<uint8_t> bad_zip(const std::string&) {
    throw std::runtime_error("bad zip");
}

int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "dusk-lib-cache-test";
    fs::remove_all(root);
    fs::create_directories(root / "mods");
    const fs::path cache = root / "mods" / ".libcache";
    const fs::path package = root / "mods" / "dev.test.mod.dusk";
    const std::string prefix = "lib/linux-aarch64/";
    const std::vector<std::string> entries = {"mod.json", "lib/linux-aarch64/mod.so", "lib/switch-arm64/mod.so"};
    s_package = {{"lib/linux-aarch64/mod.so", std::string(300000, 'A')}, {"lib/switch-arm64/mod.so", "other"}};
    write_package(package, "package v1");

    // First boot: written, only this platform's files, served.
    auto first = lib_cache::serve(cache, "dev.test.mod", package, prefix, entries, read_entry);
    check(!first.dir.empty() && first.note.find("written") != std::string::npos, "first boot writes the entry");
    check(file(first.dir / "mod.so") == s_package["lib/linux-aarch64/mod.so"], "the library is the package's");
    check(!fs::exists(first.dir / "lib"), "only the platform folder's files");
    check(lib_cache::served(first.dir), "served this session");
    std::printf("first: %s\n", first.note.c_str());

    // Next boot, same package: a hit, the same folder, nothing read from the package.
    auto second = lib_cache::serve(cache, "dev.test.mod", package, prefix, entries, no_extract);
    check(second.dir == first.dir && second.note.find("hit") != std::string::npos, "next boot is a hit");
    std::printf("second: %s\n", second.note.c_str());

    // A damaged cache file: never served, written again from the package.
    { std::ofstream(first.dir / "mod.so", std::ios::binary | std::ios::app) << "x"; }
    auto damaged = lib_cache::serve(cache, "dev.test.mod", package, prefix, entries, read_entry);
    check(damaged.dir == first.dir && damaged.note.find("written") != std::string::npos &&
              damaged.note.find("did not match") != std::string::npos,
        "a damaged entry is replaced, not served");
    check(file(damaged.dir / "mod.so") == s_package["lib/linux-aarch64/mod.so"], "replaced with the package's bytes");
    // A missing manifest: the same.
    fs::remove(first.dir / "manifest.txt");
    auto noManifest = lib_cache::serve(cache, "dev.test.mod", package, prefix, entries, read_entry);
    check(noManifest.note.find("written") != std::string::npos, "an entry without a manifest is written again");

    // An update: another package, another key, the update's library.
    s_package["lib/linux-aarch64/mod.so"] = std::string(200000, 'B');
    write_package(package, "package v2");
    auto updated = lib_cache::serve(cache, "dev.test.mod", package, prefix, entries, read_entry);
    check(!updated.dir.empty() && updated.dir != first.dir, "an update gets its own entry");
    check(file(updated.dir / "mod.so") == s_package["lib/linux-aarch64/mod.so"], "the update's library, not the old");

    // Prune keeps what was served and removes the rest: an uninstalled mod, a half-written folder.
    fs::create_directories(cache / "dev.uninstalled" / "0123456789abcdef0123456789abcdef");
    fs::create_directories(cache / "dev.test.mod" / "deadbeefdeadbeefdeadbeefdeadbeef.tmp");
    const size_t removed = lib_cache::prune(cache);
    check(removed == 2, "prune removes the uninstalled mod's entry and the half-written folder");
    check(!fs::exists(cache / "dev.uninstalled"), "an uninstalled mod's folder goes");
    check(fs::exists(updated.dir), "the served entry stays");

    // A read failure on a miss: empty result, no folder left behind.
    write_package(package, "package v3");
    auto failed = lib_cache::serve(cache, "dev.test.mod", package, prefix, entries, bad_zip);
    check(failed.dir.empty() && failed.note.find("extracting as before") != std::string::npos,
        "a read failure falls back to the normal path");
    for (const auto& e : fs::directory_iterator{cache / "dev.test.mod"}) {
        check(e.path().extension() != ".tmp", "no temporary folder left after a failure");
    }

    // A directory mod: no cache at all.
    fs::create_directories(root / "mods" / "dirmod");
    check(lib_cache::serve(cache, "dirmod", root / "mods" / "dirmod", prefix, entries, read_entry).dir.empty(),
        "a directory mod is not cached");
    // Deleting the cache is always safe: the next serve writes it again.
    fs::remove_all(cache);
    check(!lib_cache::serve(cache, "dev.test.mod", package, prefix, entries, read_entry).dir.empty(),
        "a deleted cache is rebuilt");

    fs::remove_all(root);
    if (s_failures == 0) std::printf("lib_cache_test: all checks passed\n");
    return s_failures == 0 ? 0 : 1;
}
