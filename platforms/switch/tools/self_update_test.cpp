// Host test for src/dusk/self_update_core.cpp (test-self-update.sh).
#include "dusk/self_update_core.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace dusk::self_update;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

static void write(const fs::path& p, const std::string& text) { std::ofstream(p, std::ios::binary) << text; }
static std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
static borealis::update::Asset asset(std::string name, std::string url, std::string digest) {
    return {.name = std::move(name), .browserDownloadUrl = std::move(url), .digest = std::move(digest)};
}

int main(int argc, char** argv) {
    // --- versions ----------------------------------------------------------------------------------------------------
    CHECK(is_newer("v1.0.0", "v1.0.0-rc.8"));  // the release after the candidates
    CHECK(is_newer("v1.0.0-rc.9", "v1.0.0-rc.8"));
    CHECK(is_newer("v1.0.0-rc.10", "v1.0.0-rc.9"));  // numeric, not text
    CHECK(!is_newer("v1.0.0-rc.8", "v1.0.0-rc.8"));
    CHECK(!is_newer("v1.0.0-beta.2", "v1.0.0-rc.8"));  // the public beta.2 is older than this build
    CHECK(!is_newer("v1.0.0-rc.7", "v1.0.0-rc.8"));
    CHECK(is_newer("v1.0.1", "v1.0.0"));
    CHECK(is_newer("1.1.0", "v1.0.9"));
    CHECK(!is_newer("not-a-version", "v1.0.0"));
    CHECK(!is_newer("v2.0.0", "UNKNOWN-VERSION"));  // a build that cannot name itself is never offered one

    // --- the NRO asset -----------------------------------------------------------------------------------------------
    const std::string hex(64, 'a');
    {
        borealis::update::Release r;
        r.assets = {asset("Alek-Ultimate-NX-v1.0.0.zip", "https://x/zip", "sha256:" + std::string(64, 'b')),
            asset("dusklight.NRO", "https://github.com/o/r/releases/download/v1.0.0/dusklight.nro",
                "sha256:" + std::string(64, 'A'))};
        std::string why;
        const auto a = pick_nro_asset(r, why);
        CHECK(a && a->name == "dusklight.NRO" && a->sha256 == hex);  // the digest lowercased
    }
    {
        borealis::update::Release r;  // the beta.2 shape: only a zip, installed by hand
        r.assets = {asset("Alek-Ultimate-NX-v1.0.0-beta.2.zip", "https://x/zip", "sha256:" + hex),
            asset("Alek-Ultimate-NX-v1.0.0-beta.2.zip.sha256", "https://x/sha", "sha256:" + hex)};
        std::string why;
        CHECK(!pick_nro_asset(r, why) && why.find("no .nro") != std::string::npos);
    }
    {
        borealis::update::Release r;
        std::string why;
        r.assets = {asset("dusklight.nro", "https://x/nro", "")};
        CHECK(!pick_nro_asset(r, why) && why.find("SHA-256") != std::string::npos);
        r.assets = {asset("dusklight.nro", "https://x/nro", "sha256:" + std::string(63, 'a') + "z")};
        CHECK(!pick_nro_asset(r, why));
        r.assets = {asset("dusklight.nro", "http://x/nro", "sha256:" + hex)};
        CHECK(!pick_nro_asset(r, why));
        r.assets = {asset("a.nro", "https://x/a", "sha256:" + hex), asset("b.nro", "https://x/b", "sha256:" + hex)};
        CHECK(!pick_nro_asset(r, why) && why.find("more than one") != std::string::npos);
    }

    // --- paths -------------------------------------------------------------------------------------------------------
    CHECK(replaceable_nro_path("sdmc:/switch/dusklight/dusklight.nro"));
    CHECK(replaceable_nro_path("sdmc:/switch/Dusklight.NRO"));
    CHECK(!replaceable_nro_path("romfs:/dusklight.nro"));
    CHECK(!replaceable_nro_path("sdmc:/switch/dusklight"));
    CHECK(!replaceable_nro_path("sdmc:/switch/../x.nro"));
    CHECK(!replaceable_nro_path(""));
    CHECK(part_path("sdmc:/switch/d.nro").string() == "sdmc:/switch/d.nro.part");
    CHECK(backup_path("sdmc:/switch/d.nro").string() == "sdmc:/switch/d.nro.bak");

    // --- once a day --------------------------------------------------------------------------------------------------
    CHECK(auto_check_due(0, 1000));
    CHECK(!auto_check_due(1000, 1000 + 60));
    CHECK(auto_check_due(1000, 1000 + 24 * 60));
    CHECK(auto_check_due(5000, 1000));  // the clock went back

    // --- replace with a backup ---------------------------------------------------------------------------------------
    const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "dusk-self-update-fs";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path nro = dir / "dusklight.nro";
    write(nro, "old");
    write(part_path(nro), "new");
    write(backup_path(nro), "older");  // a .bak left by an earlier update is dropped
    CHECK(replace_with_backup(nro, part_path(nro)).empty());
    CHECK(read(nro) == "new" && read(backup_path(nro)) == "old" && !fs::exists(part_path(nro)));
    // no download: nothing moves
    CHECK(!replace_with_backup(nro, part_path(nro)).empty());
    CHECK(read(nro) == "new" && read(backup_path(nro)) == "old");
    // a download that is not a file: refused before anything moves
    {
        const fs::path other = dir / "other.nro";
        write(other, "cur");
        fs::create_directories(part_path(other) / "x");
        CHECK(!replace_with_backup(other, part_path(other)).empty());
        CHECK(read(other) == "cur" && !fs::exists(backup_path(other)));
    }
    // the running NRO cannot be moved aside (it is missing here): an error, the download stays for the next exit
    {
        const fs::path gone = dir / "gone.nro";
        write(part_path(gone), "next");
        CHECK(!replace_with_backup(gone, part_path(gone)).empty());
        CHECK(fs::exists(part_path(gone)) && !fs::exists(gone));
    }
    // a second update in a row
    write(part_path(nro), "newer");
    CHECK(replace_with_backup(nro, part_path(nro)).empty());
    CHECK(read(nro) == "newer" && read(backup_path(nro)) == "new");
    fs::remove_all(dir);

    if (g_failures == 0) {
        std::puts("self_update_core: all checks passed");
    }
    return g_failures == 0 ? 0 : 1;
}
