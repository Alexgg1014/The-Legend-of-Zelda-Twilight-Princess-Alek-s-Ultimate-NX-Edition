// H36: links the two generated tables of a static-mod build -- switch_builtin_mods_table.cpp
// (StaticModSet.cmake) and switch_builtin_symbols.cpp (static_mod_link.py) -- and checks them
// through switch_builtin_mods.hpp, with the lookup the game uses. Run by test-static-mods.sh,
// which generates both tables from the real mod folders and sealed objects, and writes
// static_mods_table_defs.cpp with a stand-in for every per-mod export the table names.
//
//   static_mods_table_test <expected id>...
#include "dusk/switch_builtin_mods.hpp"

#include <cstdio>
#include <cstring>
#include <string_view>

using dusk::mods::loader::BuiltinSymbol;
using dusk::mods::loader::detail::find_builtin_symbol;
using dusk::mods::loader::detail::kBuiltinMods;
using dusk::mods::loader::detail::kBuiltinSymbols;

// Defined here under the link names the symbol table refers to, so those references
// resolve; the others it holds stay unresolved and must read as null.
extern "C" const char h36_present_vtable[16] __asm__("_ZTV10COutFont_c");
const char h36_present_vtable[16] = {1};
extern "C" void h36_present_function() __asm__("fixture_c_function");
void h36_present_function() {}

namespace {
int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)
}  // namespace

int main(int argc, char** argv) {
    // The built-in mod table: one row per mod in the configured order, every entry point
    // set, and the null row that ends it.
    int rows = 0;
    while (kBuiltinMods[rows].id != nullptr) {
        const auto& mod = kBuiltinMods[rows];
        CHECK(mod.meta != nullptr && mod.context != nullptr && mod.initialize != nullptr &&
              mod.update != nullptr && mod.shutdown != nullptr);
        if (rows + 1 < argc) {
            CHECK(std::string_view{mod.id} == argv[rows + 1]);
        }
        ++rows;
    }
    CHECK(rows == argc - 1);
    CHECK(kBuiltinMods[rows].meta == nullptr && kBuiltinMods[rows].initialize == nullptr);

    // The symbol table: what this link has resolves to it, what it does not is null, and
    // the kind (code or data) travels with each name.
    const BuiltinSymbol* vtable = find_builtin_symbol(kBuiltinSymbols, "_ZTV10COutFont_c");
    CHECK(vtable != nullptr && vtable->address == h36_present_vtable && !vtable->code);
    const BuiltinSymbol* function = find_builtin_symbol(kBuiltinSymbols, "fixture_c_function");
    CHECK(function != nullptr && function->code &&
          function->address == reinterpret_cast<const void*>(&h36_present_function));
    const BuiltinSymbol* absentVtable = find_builtin_symbol(kBuiltinSymbols, "_ZTV15dMeterHakusha_c");
    CHECK(absentVtable != nullptr && absentVtable->address == nullptr && !absentVtable->code);
    const BuiltinSymbol* absentByName =
        find_builtin_symbol(kBuiltinSymbols, "dMenu_DmapBg_c::buttonIconScreenInit");
    CHECK(absentByName != nullptr && absentByName->address == nullptr && absentByName->code);
    // A display name is looked up as the mod spells it, never by its link name.
    CHECK(find_builtin_symbol(kBuiltinSymbols, "_ZN14dMenu_DmapBg_c20buttonIconScreenInitEv") == nullptr);
    CHECK(find_builtin_symbol(kBuiltinSymbols, "fixture_c::unmapped") == nullptr);
    CHECK(find_builtin_symbol(kBuiltinSymbols, "") == nullptr);
    CHECK(find_builtin_symbol(nullptr, "fixture_c_function") == nullptr);
    int names = 0;
    while (kBuiltinSymbols[names].name != nullptr) {
        ++names;
    }
    CHECK(names == 4);  // two vtables, one mapped display name, one C name; deduplicated

    if (g_failures != 0) {
        std::fprintf(stderr, "static_mods_table_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("static_mods_table_test: %d built-in mod(s), %d name(s), all checks passed\n", rows,
        names);
    return 0;
}
