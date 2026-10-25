// H37-c6: the libstdc++ ABI refusal of src/dusk/mods/loader/elf_runtime_policy.cpp, by package ABI.
#include "dusk/mods/loader/elf_runtime_policy.hpp"

#include <dirent.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <string>
#include <vector>

using namespace dusk::mods::elf;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main(int argc, char** argv) {
    // What H37-c5's Randomizer imported (Itanium names as in its .dynsym), and the rest of the family.
    // DM2-b: only basic_filebuf and the file streams differ in layout; these stay refused without a glibc-layout
    // instance in the shim table.
    const char* unsafe[] = {
        "_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1Ev", "_ZTVSt14basic_ofstreamIcSt11char_traitsIcEE",
        "_ZTTSt14basic_ofstreamIcSt11char_traitsIcEE", "_ZNSt14basic_ifstreamIcSt11char_traitsIcEED1Ev",
        "_ZNSt13basic_fstreamIcSt11char_traitsIcEEC1Ev", "_ZNSt13basic_filebufIcSt11char_traitsIcEEC1Ev",
    };
    // Cosmetics' six locale imports (ran on the console): std::locale alone is allowed, for Linux modules too.
    const char* locale_only[] = {
        "_ZNKSt6locale2id5_M_idEv", "_ZNSt6locale5facetD2Ev", "_ZNSt6localeC1ERKS_", "_ZNSt6localeC1Ev",
        "_ZNSt6localeD1Ev", "_ZTINSt6locale5facetE", "_ZNSt6localeaSERKS_", "_ZNSt6locale7classicEv",
    };
    for (const char* name : locale_only) {
        CHECK(!denied(name));
    }
    // DM2-b: laid out alike in glibc/libstdc++ 13 and newlib/devkitA64 (measured): allowed for Linux modules.
    const char* identical[] = {
        "_ZNSt8ios_baseC2Ev", "_ZNSt8ios_baseD2Ev", "_ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E",
        "_ZTVSt9basic_iosIcSt11char_traitsIcEE", "_ZNSt7__cxx1119basic_stringstreamIcSt11char_traitsIcESaIcEEC1Ev",
        "_ZNSt7__cxx1118basic_ostringstreamIcSt11char_traitsIcESaIcEEC1Ev",
        "_ZNSt7__cxx1118basic_istringstreamIcSt11char_traitsIcESaIcEED1Ev",
    };
    for (const char* name : identical) {
        CHECK(!denied(name));
    }
    for (const char* name : unsafe) {
        const auto why = denied(name);
        CHECK(why.has_value() && why->find("libstdc++ ABI not safe") != std::string_view::npos &&
              why->find("switch-arm64") != std::string_view::npos);
    }
    // Not the family: ios_base::Init, plain library calls, C names, and the existing refusals unchanged.
    CHECK(!denied("_ZNSt8ios_base4InitC1Ev") && !denied("_ZNSt8ios_base4InitD1Ev"));
    CHECK(!denied("_ZNSolsEi") && !denied("_ZSt4cout") && !denied("memcpy") && !denied("printf") &&
          !denied("_Znwm") && !denied("_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE9_M_appendEPKcm"));
    CHECK(!denied("ios_base") && !denied("locale"));  // C names are not Itanium names
    CHECK(denied("_dl_find_object").has_value() && denied("fork").has_value());

    // By package ABI: a Linux module's import is refused, a switch-arm64 module's is resolved.
    const GameLookup game = [](std::string_view) -> std::optional<uint64_t> { return 0x1234; };
    std::string why;
    const auto linux_ = make_resolver(PackageAbi::Linux, game);
    const auto switch_ = make_resolver(PackageAbi::Switch, game);
    for (const char* name : unsafe) {
        const auto a = linux_({name, "GLIBCXX_3.4", true, false}, why);
        CHECK(a && a->source == Source::Denied && why.find("switch-arm64") != std::string::npos);
        const auto b = linux_({name, "", true, false}, why);
        CHECK(b && b->source == Source::Denied);
        const auto c = switch_({name, "", true, false}, why);
        CHECK(c && c->source == Source::Runtime && c->address == 0x1234);
    }
    for (const char* name : locale_only) {
        const auto l = linux_({name, "GLIBCXX_3.4", true, false}, why);
        CHECK(l && l->source == Source::Runtime);
    }
    const auto d = linux_({"_ZNSt8ios_base4InitC1Ev", "GLIBCXX_3.4", true, false}, why);
    CHECK(d && d->source == Source::Runtime);
    const auto e = linux_({"_Znwm", "GLIBCXX_3.4", true, false}, why);
    CHECK(e && e->source == Source::Runtime);  // a Linux module without the family keeps its behaviour
    for (const char* name : identical) {
        const auto i = linux_({name, "GLIBCXX_3.4", true, false}, why);
        CHECK(i && i->source == Source::Runtime && i->address == 0x1234);
    }
    {
        // DM2-b: glibc's __mbstate_t is newlib's _mbstate_t; a Linux module's fpos/codecvt names bind under the game's
        // spelling, a switch-arm64 module's are taken as they are.
        CHECK(newlib_spelling("_ZNSo5seekpESt4fposI11__mbstate_tE") == "_ZNSo5seekpESt4fposI10_mbstate_tE");
        CHECK(newlib_spelling("_ZNSt7codecvtIcc11__mbstate_tE2idE") == "_ZNSt7codecvtIcc10_mbstate_tE2idE");
        CHECK(newlib_spelling("_Znwm") == "_Znwm");
        const GameLookup newlibOnly = [](std::string_view n) -> std::optional<uint64_t> {
            return n == "_ZNSo5seekpESt4fposI10_mbstate_tE" ? std::optional<uint64_t>{0x9abc} : std::nullopt;
        };
        const auto lx = make_resolver(PackageAbi::Linux, newlibOnly);
        const auto sw = make_resolver(PackageAbi::Switch, newlibOnly);
        const auto l = lx({"_ZNSo5seekpESt4fposI11__mbstate_tE", "GLIBCXX_3.4", true, false}, why);
        CHECK(l && l->source == Source::Runtime && l->address == 0x9abc && why.find("_mbstate_t") != std::string::npos);
        CHECK(!sw({"_ZNSo5seekpESt4fposI11__mbstate_tE", "", true, false}, why));
    }
    {
        // DM2-b: a glibc-layout instance in the shim table (linux_stream_shims()) wins over the refusal, for a
        // Linux module; a switch-arm64 module still binds the game's own.
        const char* ctor = "_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1Ev";
        std::vector<Shim> table = {{ctor, 0x5678, "glibc-layout std::basic_filebuf/fstream (DM2-b)", true}};
        const auto lx = make_resolver(PackageAbi::Linux, game, table);
        const auto sw = make_resolver(PackageAbi::Switch, game, table);
        const auto l = lx({ctor, "GLIBCXX_3.4", true, false}, why);
        CHECK(l && l->source == Source::Shim && l->address == 0x5678 && why.find("DM2-b") != std::string::npos);
        const auto w = sw({ctor, "", true, false}, why);
        CHECK(w && w->source == Source::Runtime && w->address == 0x1234);
        const auto other = lx({"_ZNSt14basic_ifstreamIwSt11char_traitsIwEEC1Ev", "GLIBCXX_3.4", true, false}, why);
        CHECK(other && other->source == Source::Denied);  // not in the table: still refused
    }

    // DM1-c: readdir/closedir are shims (glibc's struct dirent) for a Linux module only; a switch-arm64 module
    // binds the game's own, newlib-layout ones. opendir is the C library's either way.
    for (const char* name : {"readdir", "closedir"}) {
        const auto l = linux_({name, "GLIBC_2.17", true, false}, why);
        CHECK(l && l->source == Source::Shim && why.find("glibc") != std::string::npos);
        const auto s = switch_({name, "", true, false}, why);
        CHECK(s && s->source == Source::Runtime && s->address == 0x1234);
    }
    const auto o = linux_({"opendir", "GLIBC_2.17", true, false}, why);
    CHECK(o && o->source == Source::Runtime);
    {
        // The shim itself, through the address the resolver hands out: glibc's offsets, the whole name.
        const auto r = linux_({"readdir", "GLIBC_2.17", true, false}, why);
        const auto c = linux_({"closedir", "GLIBC_2.17", true, false}, why);
        const std::string dir = std::string(argc > 2 ? argv[2] : "/tmp") + "/dm1c-readdir";
        const std::string file = dir + "/" + std::string(200, 'n') + ".txt";  // a long name, copied whole
        CHECK(std::system(("rm -rf '" + dir + "' && mkdir -p '" + dir + "' && : > '" + file + "'").c_str()) == 0);
        auto* readdirShim = reinterpret_cast<const unsigned char* (*)(DIR*)>(static_cast<uintptr_t>(r->address));
        auto* closedirShim = reinterpret_cast<int (*)(DIR*)>(static_cast<uintptr_t>(c->address));
        DIR* d = opendir(dir.c_str());
        CHECK(d != nullptr);
        int files = 0;
        for (const unsigned char* e; d != nullptr && (e = readdirShim(d)) != nullptr;) {
            const char* name = reinterpret_cast<const char*>(e + 19);  // glibc aarch64: d_name at 19
            unsigned short reclen;
            std::memcpy(&reclen, e + 16, sizeof reclen);
            CHECK(static_cast<size_t>(reclen) == 19 + std::strlen(name) + 1);
            if (std::string(name) == std::string(200, 'n') + ".txt") {
                CHECK(e[18] == DT_REG || e[18] == DT_UNKNOWN);
                ++files;
            }
        }
        CHECK(files == 1);
        CHECK(d == nullptr || closedirShim(d) == 0);
        CHECK(std::system(("rm -rf '" + dir + "'").c_str()) == 0);
    }

    {
        // 1.0.1: std::filesystem::equivalent goes to the path-comparing shim (Horizon has no inode numbers), for
        // both package ABIs and both overloads.
        namespace fs = std::filesystem;
        const char* ecName = "_ZNSt10filesystem10equivalentERKNS_7__cxx114pathES3_RSt10error_code";
        const auto l = linux_({ecName, "GLIBCXX_3.4.26", true, false}, why);
        const auto s = switch_({ecName, "", true, false}, why);
        const auto t = linux_({"_ZNSt10filesystem10equivalentERKNS_7__cxx114pathES3_", "GLIBCXX_3.4.26", true, false}, why);
        CHECK(l && l->source == Source::Shim && s && s->source == Source::Shim && t && t->source == Source::Shim);
        using EquivalentEc = bool (*)(const fs::path&, const fs::path&, std::error_code&);
        auto* equivalent = reinterpret_cast<EquivalentEc>(static_cast<uintptr_t>(l->address));
        const std::string dir = std::string(argc > 2 ? argv[2] : "/tmp") + "/fs-equivalent";
        CHECK(std::system(("rm -rf '" + dir + "' && mkdir -p '" + dir + "/sub' && : > '" + dir + "/a.txt' && : > '" + dir +
                           "/b.txt'").c_str()) == 0);
        std::error_code ec;
        CHECK(equivalent(dir + "/a.txt", dir + "/sub/../a.txt", ec) && !ec);  // one file, two spellings
        CHECK(!equivalent(dir + "/a.txt", dir + "/b.txt", ec) && !ec);          // two files: not inode 0 == inode 0
        CHECK(!equivalent(dir + "/a.txt", dir + "/missing.txt", ec) && !ec);    // only one exists: false
        CHECK(!equivalent(dir + "/x.txt", dir + "/y.txt", ec) && ec);           // neither: an error
        CHECK(std::system(("rm -rf '" + dir + "'").c_str()) == 0);
    }

    // The shared table (abi_policy_names.txt): the same verdicts elf_audit.py gives.
    if (argc > 1) {
        std::ifstream table(argv[1]);
        std::string verdict, name;
        int rows = 0;
        for (std::string line; std::getline(table, line);) {
            if (line.empty() || line[0] == '#') continue;
            verdict = line.substr(0, line.find(' '));
            name = line.substr(line.find(' ') + 1);
            CHECK(denied(name).has_value() == (verdict == "refused"));
            ++rows;
        }
        CHECK(rows >= 25);
    }
    std::printf("runtime policy (libstdc++ ABI): %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
