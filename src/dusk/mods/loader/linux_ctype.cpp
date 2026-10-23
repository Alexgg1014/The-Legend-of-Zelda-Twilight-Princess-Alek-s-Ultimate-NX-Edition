// aleks2 (DM2-b): std::ctype<char> with glibc's mask table, for linux-aarch64 mods.
//
// ctype_base::mask is 2 bytes with glibc's bits in a Linux build (upper 0x100, lower 0x200, alpha 0x400,
// digit 0x800...) and 1 byte with newlib's here. ctype<char>::is() and scan_is() are inline: a Linux module
// reads the facet's _M_table with its own halfword loads and bits. Against the game's facet -- a newlib byte
// table -- std::isalpha(c, loc) and std::regex's \d, \w and [[:xdigit:]] answer wrongly (iostream_lab, in the
// loader-only lab mode: those three checks failed, all the streams passed). Randomizer 1.0.5's regex scanner
// does exactly that read.
//
// So a Linux module gets a facet of its own: a ctype<char> whose table is glibc's (the one __ctype_b_loc hands
// out, src/dusk/switch_mod_libc.cpp, indexed 0..255 as ctype<char>::is indexes it), under an id of its own
// (its import of ctype<char>::id binds there; linux_ctype_shims(), Linux modules only).
//
// Sol's review (respuesta-sol-dm2b.md, points 3, 4 and 9), checked: GCC 13's use_facet/__try_use_facet takes a
// standard facet with a static_cast of facets[id], NO bounds check -- a locale without this facet, read at
// this id, is read out of bounds. Randomizer imports locale::classic() (25 call sites) and locale(const char*),
// so it is not enough to put the facet in the global locale. Every locale a Linux module can reach holds it:
//   * the global locale (prepare_linux_runtime(): once, explicitly, before the first Linux module's imports
//     are bound or any of its code runs -- not as a side effect of resolving a name), and so every locale the
//     game's library default-constructs afterwards (a stream's, through ios_base);
//   * the standard streams' locales, imbued again then (they were made before);
//   * locale::classic() and the named-locale constructors, as a Linux module calls them: shims that give the
//     same locale plus this facet;
//   * use_facet/has_facet<ctype<char>> called out of line: this facet.
// The game's own facet, its id and its lookups are unchanged; nothing in the game constructs, compares or
// names locales (measured: no std::locale use in the game's sources). The global locale's name becomes "*".
//
// Out-of-line members: the game's ctype<char>'s (do_widen, do_narrow) except do_toupper/do_tolower, which
// devkitA64 implements through is() over _M_table (glibc's masks here): the C library's mapping instead.
#include "dusk/mods/loader/elf_runtime_policy.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <locale>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <typeinfo>
#include <vector>

extern "C" const unsigned short** __ctype_b_loc();
// linux_fstream.cpp, renamed: where _M_file sits in the glibc-layout basic_filebuf, and its type_info.
extern "C" size_t aleks2lx_filebuf_file_offset();
extern "C" const char aleks2lx_ifstream_typeinfo __asm__("aleks2lx__ZTISt14basic_ifstreamIcSt11char_traitsIcEE");

namespace dusk::mods::elf {
namespace {

class LinuxCtype final : public std::ctype<char> {
public:
    static std::locale::id id;
    // refs 1: no locale ever deletes it (it is a function-local static). del false: the table is glibc's own.
    explicit LinuxCtype(const mask* table) : std::ctype<char>(table, false, 1) {}

protected:
    char do_toupper(char c) const override {
        return static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    }
    const char* do_toupper(char* lo, const char* hi) const override {
        for (; lo < hi; ++lo) {
            *lo = do_toupper(*lo);
        }
        return hi;
    }
    char do_tolower(char c) const override {
        return static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    }
    const char* do_tolower(char* lo, const char* hi) const override {
        for (; lo < hi; ++lo) {
            *lo = do_tolower(*lo);
        }
        return hi;
    }
};
std::locale::id LinuxCtype::id;

const unsigned short* glibc_table() {
    return *__ctype_b_loc();
}

LinuxCtype& facet() {
    static LinuxCtype instance(reinterpret_cast<const std::ctype_base::mask*>(glibc_table()));
    return instance;
}

std::locale with_facet(const std::locale& base) {
    return std::locale(base, &facet());
}

const std::locale& linux_classic() {
    static const std::locale classic = with_facet(std::locale::classic());
    return classic;
}

std::once_flag s_prepared;
std::string s_check;

// What can be checked from inside the game, with the game's own library: the result goes to the log.
std::string self_check() {
    int pass = 0, fail = 0;
    std::string failed;
    const auto check = [&](bool ok, const char* what) {
        ok ? ++pass : ++fail;
        if (!ok) {
            failed += failed.empty() ? "" : ", ";
            failed += what;
        }
    };
    // Layout: the glibc-layout filebuf puts _M_file at 112 (64-byte streambuf + 48-byte lock); the game's own is
    // 216 bytes in all.
    check(aleks2lx_filebuf_file_offset() == 112, "glibc filebuf _M_file at 112");
    check(sizeof(std::basic_filebuf<char>) == 216, "native filebuf 216 bytes");
    // RTTI across layouts (sol, point 1): the renamed type_info's name is marked '*' (linux_streams_rename.py), so
    // the game's type_info::operator== -- pointer first, then the name unless it starts with '*' -- never
    // equates it with the native std::ifstream, in either direction.
    const auto& linuxIfstream = reinterpret_cast<const std::type_info&>(aleks2lx_ifstream_typeinfo);
    // type_info is {vptr, __name}; name() hides a leading '*', so read the field.
    const char* rawName = *reinterpret_cast<const char* const*>(reinterpret_cast<const char*>(&linuxIfstream) + 8);
    check(rawName[0] == '*', "renamed RTTI name marked '*'");
    check(!(linuxIfstream == typeid(std::ifstream)) && !(typeid(std::ifstream) == linuxIfstream),
          "type_info differs from native both ways");
    check(linuxIfstream == linuxIfstream, "type_info equals itself");
    // ctype: the facet is in the global locale, classic() as a Linux module sees it, and a locale the game's
    // library default-constructs (a stream's); and its table is glibc's for all 256 bytes, as C's ctype says.
    check(std::has_facet<LinuxCtype>(std::locale()), "facet in the global locale");
    check(std::has_facet<LinuxCtype>(linux_classic()), "facet in classic() for Linux modules");
    std::ostringstream stream;
    check(std::has_facet<LinuxCtype>(stream.getloc()), "facet in a new stream's locale");
    check(std::has_facet<LinuxCtype>(std::cout.getloc()), "facet in std::cout's locale");
    check(&std::use_facet<std::ctype<char>>(std::locale()) != static_cast<const std::ctype<char>*>(&facet()),
          "the game's own ctype<char> is unchanged");
    int mismatches = 0;
    const unsigned short* table = glibc_table();
    for (int c = 0; c < 256; ++c) {
        const unsigned short m = table[c];
        mismatches += ((m & 0x0400) != 0) != (std::isalpha(c) != 0);   // _ISalpha
        mismatches += ((m & 0x0800) != 0) != (std::isdigit(c) != 0);   // _ISdigit
        mismatches += ((m & 0x2000) != 0) != (std::isspace(c) != 0);   // _ISspace
        mismatches += ((m & 0x1000) != 0) != (std::isxdigit(c) != 0);  // _ISxdigit
        mismatches += ((m & 0x0100) != 0) != (std::isupper(c) != 0);   // _ISupper
        mismatches += ((m & 0x0004) != 0) != (std::ispunct(c) != 0);   // _ISpunct
    }
    check(mismatches == 0, "glibc table = C ctype for 256 bytes");
    check(facet().toupper('q') == 'Q' && facet().tolower('Q') == 'q', "facet toupper/tolower");
    char line[400];
    std::snprintf(line, sizeof line, "LINUX STREAMS SELFCHECK: %s (%d passed, %d failed)%s%s", fail ? "FAIL" : "PASS",
                  pass, fail, fail ? " -- " : "", failed.c_str());
    return line;
}

}  // namespace

std::string prepare_linux_runtime() {
    std::call_once(s_prepared, [] {
        std::locale::global(with_facet(std::locale()));
        // Made before: imbue them again so their locales hold the facet too.
        const std::locale global;
        std::cout.imbue(global);
        std::cerr.imbue(global);
        std::clog.imbue(global);
        std::cin.imbue(global);
        s_check = self_check();
    });
    return s_check;
}

}  // namespace dusk::mods::elf

extern "C" {
// use_facet<ctype<char>>(loc) and has_facet<ctype<char>>(loc) called out of line by a Linux module.
const std::ctype<char>* aleks2lx_use_facet_ctype_char(const std::locale& loc) {
    if (std::has_facet<dusk::mods::elf::LinuxCtype>(loc)) {
        return &std::use_facet<dusk::mods::elf::LinuxCtype>(loc);
    }
    return &dusk::mods::elf::facet();
}
bool aleks2lx_has_facet_ctype_char(const std::locale&) {
    return true;
}
// locale::classic() and the named-locale constructors, as a Linux module calls them (member functions: this in x0).
const std::locale* aleks2lx_locale_classic() {
    return &dusk::mods::elf::linux_classic();
}
void aleks2lx_locale_ctor_cstr(std::locale* self, const char* name) {
    new (self) std::locale(dusk::mods::elf::with_facet(std::locale(name)));
}
void aleks2lx_locale_ctor_string(std::locale* self, const std::string& name) {
    new (self) std::locale(dusk::mods::elf::with_facet(std::locale(name)));
}
void aleks2lx_locale_ctor_cstr_cat(std::locale* self, const std::locale& base, const char* name, int category) {
    new (self) std::locale(dusk::mods::elf::with_facet(std::locale(base, name, category)));
}
}

namespace dusk::mods::elf {

std::vector<Shim> linux_ctype_shims() {
    constexpr std::string_view kReason = "ctype<char> with glibc's 2-byte mask table (DM2-b)";
    constexpr std::string_view kLocale = "a locale holding the glibc-table ctype<char> (DM2-b)";
    const auto at = [](const void* p) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)); };
    return {
        {"_ZNSt5ctypeIcE2idE", at(&LinuxCtype::id), kReason, true},
        {"_ZSt9use_facetISt5ctypeIcEERKT_RKSt6locale", at(reinterpret_cast<const void*>(&aleks2lx_use_facet_ctype_char)),
         kReason, true},
        {"_ZSt9has_facetISt5ctypeIcEEbRKSt6locale", at(reinterpret_cast<const void*>(&aleks2lx_has_facet_ctype_char)),
         kReason, true},
        // GCC 13's helper behind use_facet/has_facet, called out of line when it is not inlined (iostream_lab's
        // std::isdigit(c, loc) did: bound to the game's, it answered with the native facet). Same answer as use_facet.
        {"_ZSt15__try_use_facetISt5ctypeIcEEPKT_RKSt6locale",
         at(reinterpret_cast<const void*>(&aleks2lx_use_facet_ctype_char)), kReason, true},
        {"_ZNSt6locale7classicEv", at(reinterpret_cast<const void*>(&aleks2lx_locale_classic)), kLocale, true},
        {"_ZNSt6localeC1EPKc", at(reinterpret_cast<const void*>(&aleks2lx_locale_ctor_cstr)), kLocale, true},
        {"_ZNSt6localeC2EPKc", at(reinterpret_cast<const void*>(&aleks2lx_locale_ctor_cstr)), kLocale, true},
        {"_ZNSt6localeC1ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
         at(reinterpret_cast<const void*>(&aleks2lx_locale_ctor_string)), kLocale, true},
        {"_ZNSt6localeC1ERKS_PKci", at(reinterpret_cast<const void*>(&aleks2lx_locale_ctor_cstr_cat)), kLocale, true},
        {"_ZNSt6localeC2ERKS_PKci", at(reinterpret_cast<const void*>(&aleks2lx_locale_ctor_cstr_cat)), kLocale, true},
    };
}

}  // namespace dusk::mods::elf
