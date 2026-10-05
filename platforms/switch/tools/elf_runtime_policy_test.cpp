// H37-c6: the libstdc++ ABI refusal of src/dusk/mods/loader/elf_runtime_policy.cpp, by package ABI.
#include "dusk/mods/loader/elf_runtime_policy.hpp"

#include <cstdio>
#include <fstream>
#include <string>

using namespace dusk::mods::elf;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main(int argc, char** argv) {
    // What H37-c5's Randomizer imported (Itanium names as in its .dynsym), and the rest of the family.
    const char* unsafe[] = {
        "_ZNSt8ios_baseC2Ev", "_ZNSt8ios_baseD2Ev", "_ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E",
        "_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1Ev", "_ZTVSt14basic_ofstreamIcSt11char_traitsIcEE",
        "_ZTTSt14basic_ofstreamIcSt11char_traitsIcEE", "_ZNSt14basic_ifstreamIcSt11char_traitsIcEED1Ev",
        "_ZNSt13basic_fstreamIcSt11char_traitsIcEEC1Ev", "_ZNSt7__cxx1119basic_stringstreamIcSt11char_traitsIcESaIcEEC1Ev",
        "_ZNSt7__cxx1118basic_ostringstreamIcSt11char_traitsIcESaIcEEC1Ev", "_ZNSt7__cxx1118basic_istringstreamIcSt11char_traitsIcESaIcEED1Ev",
    };
    // Cosmetics' six locale imports (ran on the console): std::locale alone is allowed, for Linux modules too.
    const char* locale_only[] = {
        "_ZNKSt6locale2id5_M_idEv", "_ZNSt6locale5facetD2Ev", "_ZNSt6localeC1ERKS_", "_ZNSt6localeC1Ev",
        "_ZNSt6localeD1Ev", "_ZTINSt6locale5facetE", "_ZNSt6localeaSERKS_", "_ZNSt6locale7classicEv",
    };
    for (const char* name : locale_only) {
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
