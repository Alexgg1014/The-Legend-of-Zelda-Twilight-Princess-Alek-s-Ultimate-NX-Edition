// H38: which package of one mod id the loader runs, and which updates it refuses (src/dusk/mods/loader/package_choice.*).
// The fixtures are real: a zip and a directory bundle, built by the test script with the same files, are classified
// from their real entry names, and both must come out alike.
#include "dusk/mods/loader/package_choice.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace dusk::mods;
namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                      \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

// "2.10.0" > "2.9.0": numeric dotted components, as borealis' parse_version orders them.
int compare_versions(std::string_view lhs, std::string_view rhs) {
    const auto parse = [](std::string_view v) {
        std::vector<int> parts;
        size_t start = 0;
        while (start <= v.size()) {
            const auto dot = v.find('.', start);
            const auto end = dot == std::string_view::npos ? v.size() : dot;
            parts.push_back(std::atoi(std::string{v.substr(start, end - start)}.c_str()));
            if (dot == std::string_view::npos) {
                break;
            }
            start = dot + 1;
        }
        return parts;
    };
    const auto a = parse(lhs);
    const auto b = parse(rhs);
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        const int x = i < a.size() ? a[i] : 0;
        const int y = i < b.size() ? b[i] : 0;
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return 0;
}

const PackageLibraries kSwitch{.switchArm64 = true};
const PackageLibraries kLinux{.linuxAarch64 = true};
const PackageLibraries kBoth{.switchArm64 = true, .linuxAarch64 = true};
const PackageLibraries kOther{.other = true};
const PackageLibraries kNone{};

struct Pkg {
    std::string version;
    uint32_t dir;  // 0 = the user's folder, 1 = the packaged one
    std::string path;
    PackageLibraries libs;
};

// Index into `list` of the package that loads, or -1.
int pick(const std::vector<Pkg>& list, bool preferSwitch) {
    std::vector<fs::path> paths;
    paths.reserve(list.size());
    for (const auto& p : list) {
        paths.emplace_back(p.path);
    }
    std::vector<PackageRank> ranks;
    for (size_t i = 0; i < list.size(); ++i) {
        ranks.push_back({.version = list[i].version, .searchDirIndex = list[i].dir, .path = &paths[i],
            .libraries = list[i].libs});
    }
    const size_t index = choose_package(ranks, preferSwitch, &compare_versions);
    return index == k_noPackage ? -1 : static_cast<int>(index);
}

// The central directory of a zip: the entry names, as the archive reader would list them.
std::vector<std::string> zip_names(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto u16 = [&](size_t o) { return static_cast<size_t>(data[o] | (data[o + 1] << 8)); };
    const auto u32 = [&](size_t o) { return u16(o) | (u16(o + 2) << 16); };
    std::vector<std::string> names;
    if (data.size() < 22) {
        return names;
    }
    size_t eocd = data.size() - 22;
    while (eocd > 0 && u32(eocd) != 0x06054b50) {
        --eocd;
    }
    if (u32(eocd) != 0x06054b50) {
        return names;
    }
    const size_t count = u16(eocd + 10);
    size_t offset = u32(eocd + 16);
    for (size_t i = 0; i < count && offset + 46 <= data.size() && u32(offset) == 0x02014b50; ++i) {
        const size_t nameLength = u16(offset + 28), extra = u16(offset + 30), comment = u16(offset + 32);
        names.emplace_back(reinterpret_cast<const char*>(&data[offset + 46]), nameLength);
        offset += 46 + nameLength + extra + comment;
    }
    return names;
}

// What ModBundleDisk::getFileNames returns: regular files, relative to the root, '/' separated.
std::vector<std::string> directory_names(const fs::path& root) {
    std::vector<std::string> names;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            names.push_back(entry.path().lexically_relative(root).generic_string());
        }
    }
    return names;
}

PackageLibraries classify(const std::vector<std::string>& names) {
    return classify_package_libraries(std::span<const std::string>{names});
}

void classification(const fs::path& fixtures) {
    // The two real bundles of each shape: a zip and a directory with the same files.
    struct Shape {
        const char* name;
        PackageLibraries expected;
    };
    const Shape shapes[] = {
        {"switch_only", kSwitch}, {"linux_only", kLinux}, {"both", kBoth},       {"other_only", kOther},
        {"assets_only", kNone},   {"misplaced", kNone},   {"windows_dll", kOther},
    };
    for (const auto& shape : shapes) {
        const auto zip = classify(zip_names(fixtures / (std::string{shape.name} + ".dusk")));
        const auto dir = classify(directory_names(fixtures / shape.name));
        if (!(zip == shape.expected) || !(dir == shape.expected)) {
            std::fprintf(stderr, "shape %s: zip(sw=%d lx=%d ot=%d) dir(sw=%d lx=%d ot=%d)\n", shape.name,
                zip.switchArm64, zip.linuxAarch64, zip.other, dir.switchArm64, dir.linuxAarch64, dir.other);
        }
        CHECK(zip == shape.expected);
        CHECK(dir == shape.expected);
        CHECK(zip == dir);
        CHECK(zip.native() == shape.expected.native());
    }
    CHECK(!zip_names(fixtures / "switch_only.dusk").empty());
    CHECK(!directory_names(fixtures / "switch_only").empty());
    // Entries that are not a library at lib/<platform>/mod.so|dll.
    CHECK(classify({"lib/", "lib/switch-arm64/", "mod.json"}) == kNone);
    CHECK(classify({"lib/switch-arm64/mod.so.bak"}) == kNone);
    CHECK(classify({"lib/switch-arm64/sub/mod.so"}) == kNone);
    CHECK(classify({"lib//mod.so"}) == kNone);
    CHECK(classify({"lib/mod.so"}) == kNone);
    CHECK(classify({"data/lib/switch-arm64/mod.so"}) == kNone);
    CHECK(classify({"lib/switch-arm64/other.so"}) == kNone);
}

void selection() {
    // 1. the packaged Switch 2.5.0 against a user's Linux 2.6.0
    {
        const std::vector<Pkg> l{
            {"2.6.0", 0, "/mods/twilit.dusk", kLinux}, {"2.5.0", 1, "/bundled/twilit.dusk", kSwitch}};
        CHECK(pick(l, true) == 1);
        CHECK(pick(l, false) == 0);  // without the preference: the plain highest version, as before
    }
    // 2. Switch against Switch: the higher one
    {
        const std::vector<Pkg> l{{"2.5.0", 1, "/bundled/a.dusk", kSwitch}, {"2.6.0", 0, "/mods/a.dusk", kSwitch}};
        CHECK(pick(l, true) == 1);
        CHECK(pick({{"2.10.0", 0, "/m/a.dusk", kSwitch}, {"2.9.0", 1, "/b/a.dusk", kSwitch}}, true) == 0);
    }
    // 3. only Linux: the higher version still wins
    {
        const std::vector<Pkg> l{{"2.5.0", 1, "/bundled/a.dusk", kLinux}, {"2.6.0", 0, "/mods/a.dusk", kLinux}};
        CHECK(pick(l, true) == 1);
        const std::vector<Pkg> m{{"2.6.0", 0, "/mods/a.dusk", kLinux}, {"2.5.0", 1, "/bundled/a.dusk", kLinux}};
        CHECK(pick(m, true) == 0);
    }
    // A package with both libraries is a Switch copy; code for another platform only is never a Switch copy.
    {
        const std::vector<Pkg> l{{"3.0.0", 0, "/m/a.dusk", kLinux}, {"2.0.0", 1, "/b/a.dusk", kBoth}};
        CHECK(pick(l, true) == 1);
        const std::vector<Pkg> o{{"3.0.0", 0, "/m/a.dusk", kOther}, {"2.0.0", 1, "/b/a.dusk", kLinux}};
        CHECK(pick(o, true) == 0);  // no Switch copy: the version decides
        const std::vector<Pkg> s{{"3.0.0", 0, "/m/a.dusk", kOther}, {"2.0.0", 1, "/b/a.dusk", kSwitch}};
        CHECK(pick(s, true) == 1);
    }
    // 4. no native code at all: versions and ties as before, never demoted
    {
        const std::vector<Pkg> l{{"1.0.0", 1, "/b/a.dusk", kNone}, {"1.1.0", 0, "/m/a.dusk", kNone}};
        CHECK(pick(l, true) == 1);
        CHECK(pick(l, false) == 1);
        const std::vector<Pkg> tie{{"1.0.0", 1, "/b/a.dusk", kNone}, {"1.0.0", 0, "/m/a.dusk", kNone}};
        CHECK(pick(tie, true) == 1);  // equal versions: the lower search directory
        const std::vector<Pkg> path{{"1.0.0", 0, "/m/b.dusk", kNone}, {"1.0.0", 0, "/m/a.dusk", kNone}};
        CHECK(pick(path, true) == 1);  // then the lower path
        // a script package is not demoted by a Switch copy of the same id, and a newer one still wins
        const std::vector<Pkg> mixed{{"2.0.0", 0, "/m/a.dusk", kNone}, {"1.0.0", 1, "/b/a.dusk", kSwitch}};
        CHECK(pick(mixed, true) == 0);
    }
    // ties among Switch copies
    {
        const std::vector<Pkg> tie{{"2.5.0", 1, "/b/a.dusk", kSwitch}, {"2.5.0", 0, "/m/a.dusk", kSwitch}};
        CHECK(pick(tie, true) == 1);
        const std::vector<Pkg> mixedTie{{"2.5.0", 0, "/m/a.dusk", kLinux}, {"2.5.0", 1, "/b/a.dusk", kSwitch}};
        CHECK(pick(mixedTie, true) == 1);  // Linux in the lower directory still gives way to the Switch copy
    }
    CHECK(pick({}, true) == -1);
    CHECK(pick({{"1.0.0", 0, "/m/a.dusk", kLinux}}, true) == 0);
}

void installs() {
    const PackageLibraries bundledSwitch[] = {kSwitch};
    const PackageLibraries bundledLinux[] = {kLinux};
    const PackageLibraries none[] = {kNone};
    // 5. Linux over a Switch copy is refused, and says so
    CHECK(refuses_non_switch_update(kLinux, bundledSwitch, true));
    CHECK(refuses_non_switch_update(kOther, bundledSwitch, true));
    CHECK(k_nonSwitchUpdateMessage ==
          "This update has no switch-arm64 library; the compatible Switch version was kept.");
    // a Switch update, or a package with both libraries, goes in
    CHECK(!refuses_non_switch_update(kSwitch, bundledSwitch, true));
    CHECK(!refuses_non_switch_update(kBoth, bundledSwitch, true));
    // 6. no Switch copy yet: a Linux package may be installed and the ELF policy / Dry Run decide
    CHECK(!refuses_non_switch_update(kLinux, bundledLinux, true));
    CHECK(!refuses_non_switch_update(kLinux, {}, true));
    CHECK(!refuses_non_switch_update(kLinux, none, true));
    // packages without native code are never refused
    CHECK(!refuses_non_switch_update(kNone, bundledSwitch, true));
    // other builds do not change
    CHECK(!refuses_non_switch_update(kLinux, bundledSwitch, false));
}

// 7. removing the user's package gives back the right bundled copy
void uninstall() {
    std::vector<Pkg> l{{"2.6.0", 0, "/mods/a.dusk", kSwitch}, {"2.5.0", 1, "/bundled/a.dusk", kSwitch}};
    CHECK(pick(l, true) == 0);
    l.erase(l.begin());
    CHECK(pick(l, true) == 0 && l[0].path == "/bundled/a.dusk");
    // a user's Linux package left behind by an earlier install is not what comes back over a Switch copy
    std::vector<Pkg> m{{"2.6.0", 0, "/mods/a.dusk", kLinux}, {"2.5.0", 1, "/bundled/a.dusk", kSwitch},
        {"2.4.0", 1, "/bundled/old.dusk", kSwitch}};
    CHECK(pick(m, true) == 1);
    m.erase(m.begin());
    CHECK(pick(m, true) == 0 && m[0].version == "2.5.0");
    // the last package removed: nothing to load
    m.clear();
    CHECK(pick(m, true) == -1);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: package_preference_test FIXTURE_DIR\n");
        return 2;
    }
    classification(argv[1]);
    selection();
    installs();
    uninstall();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("switch package preference: Switch copy kept over a newer Linux one, Switch vs Switch, Linux only, "
              "non-native, refused/allowed installs, uninstall, zip == directory: ok");
    return 0;
}
