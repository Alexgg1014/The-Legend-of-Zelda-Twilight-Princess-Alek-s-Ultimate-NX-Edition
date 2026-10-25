// aleks2 (1.0.2): manifest::resolve's fallback for names that differ only in [abi:...] tags (src/dusk/mods/abi_tags.hpp).
// Twilight HD HUD hooks "dusk::ui::midna_icon_source"; the manifest has it as "...midna_icon_source[abi:cxx11]".
#include "dusk/mods/abi_tags.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace at = dusk::mods::abi_tags;

struct Sym {
    const char* name;
    uint64_t rva;
};

static const std::vector<Sym> kTable{
    {"dusk::ui::midna_icon_source[abi:cxx11]", 0x100},
    {"daAlink_c::execute", 0x200},
    {"name_of[abi:cxx11]", 0x300},   // two functions whose names differ only in the tag: ambiguous
    {"name_of[abi:other]", 0x310},
    {"alias[abi:cxx11]", 0x400},     // two names of one function: still a single match
    {"alias[abi:cxx11][abi:x]", 0x400},
    {"untagged_only", 0x500},
    {nullptr, 0x600},                // a name outside the string table
};

static int64_t find(const char* name) {
    return at::find_ignoring_tags(name, static_cast<uint32_t>(kTable.size()),
        [](uint32_t i) { return kTable[i].name; }, [](uint32_t i) { return kTable[i].rva; });
}

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    g_fail += !ok;
}

int main() {
    check(at::strip("dusk::ui::midna_icon_source[abi:cxx11]") == "dusk::ui::midna_icon_source", "strip removes the tag");
    check(at::strip("f[abi:cxx11][abi:x]::g[abi:y]") == "f::g", "strip removes every tag");
    check(at::strip("broken[abi:cxx11") == "broken[abi:cxx11", "an unterminated tag is left alone");
    check(find("dusk::ui::midna_icon_source") == 0, "midna_icon_source resolves without the tag");
    check(find("dusk::ui::midna_icon_source[abi:cxx11]") == 0, "and with it");
    check(find("dusk::ui::midna_icon_source[abi:other]") == 0, "and with another tag");
    check(find("name_of") == at::kAmbiguous, "two functions that differ only in the tag are ambiguous");
    check(find("alias") == 4, "two names of one function are one match");
    check(find("untagged_only[abi:cxx11]") == 6, "a tagged request finds an untagged name");
    check(find("untagged_only") == at::kNone, "an untagged request does not rescan untagged names");
    check(find("missing") == at::kNone, "a missing name stays missing");
    std::printf("abi-tags: %s\n", g_fail ? "FAIL" : "ok");
    return g_fail ? 1 : 0;
}
