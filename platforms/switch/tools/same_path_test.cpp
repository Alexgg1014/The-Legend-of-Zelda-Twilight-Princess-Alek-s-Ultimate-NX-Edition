// aleks2 (V1): host test for src/dusk/mods/loader/same_path.hpp -- the Switch's name comparison that replaces
// std::filesystem::equivalent (st_ino is 0 there, so every file on the SD card was "the same file").
#include "dusk/mods/loader/same_path.hpp"

#include <cstdio>
#include <fstream>

namespace fs = std::filesystem;
using dusk::mods::same_path;
using dusk::mods::same_path_by_name;

static int g_failures = 0;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                                          \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

static void touch(const fs::path& p) { std::ofstream(p) << "x"; }

int main(int argc, char** argv) {
    const fs::path root = (argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "dusk-same-path-test");
    fs::remove_all(root);
    fs::create_directories(root / "mods");
    // The rc14 case: Dawnlight installed as mods/dev.bezide.dawnlight.dusk next to other mods; installing 5.0.0 (the
    // same id) aims at the same file. Before, on the Switch, every other installed mod compared equal to it.
    const fs::path dawnlight = root / "mods" / "dev.bezide.dawnlight.dusk";
    const fs::path glider = root / "mods" / "dev.bezide.dawnlight_custom_model-1.3.0.dusk";
    const fs::path cosmetics = root / "mods" / "dev.twilitrealm.cosmetics.dusk";
    touch(dawnlight);
    touch(glider);
    touch(cosmetics);

    CHECK(!same_path_by_name(glider, dawnlight));
    CHECK(!same_path_by_name(cosmetics, dawnlight));
    CHECK(same_path_by_name(dawnlight, dawnlight));
    // Spellings of the same name: case (FAT), dot segments, a trailing separator on a directory.
    CHECK(same_path_by_name(root / "mods" / "DEV.Bezide.Dawnlight.DUSK", dawnlight));
    CHECK(same_path_by_name(root / "mods" / "." / "dev.bezide.dawnlight.dusk", dawnlight));
    CHECK(same_path_by_name(root / "mods" / ".." / "mods" / "dev.bezide.dawnlight.dusk", dawnlight));
    CHECK(same_path_by_name(root / "mods" / "", root / "mods"));
    CHECK(!same_path_by_name(root / "mods", root / "mods" / ".staging"));

    // The wrapper: both must exist (as for equivalent()), and the error is left clear.
    std::error_code error;
    CHECK(same_path(dawnlight, dawnlight, error) && !error);
    CHECK(!same_path(glider, dawnlight, error));
    CHECK(!same_path(root / "mods" / "missing.dusk", dawnlight, error) && !error);
    // A first install: the destination does not exist yet.
    CHECK(!same_path(glider, root / "mods" / "new.mod.dusk", error) && !error);

    fs::remove_all(root);
    if (g_failures != 0) {
        std::fprintf(stderr, "same_path_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("same_path_test: all passed\n");
    return 0;
}
