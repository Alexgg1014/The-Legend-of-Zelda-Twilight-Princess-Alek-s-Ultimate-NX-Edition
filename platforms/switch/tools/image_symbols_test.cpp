// H37: host test for src/dusk/mods/loader/image_symbols.cpp, the lookup the Switch build uses to
// resolve a native mod's imports against the running NRO's own .dynsym.
//
//   image_symbols_test ELF [ELF...]
//
// Each ELF (the synthetic games test-symbol-manifest.sh links with GNU, SysV and both hash
// styles, and real mod libraries) is laid out in memory by this test -- PT_LOAD by PT_LOAD,
// nothing shared with the loader -- and then:
//   * every dynamic symbol read straight from the section headers (not through the hash
//     tables) is looked up by name: a defined GLOBAL/WEAK function or object must come back at
//     base + st_value, anything else (undefined, local) must not be found;
//   * names that are not in the table, and near misses of names that are, are not found;
//   * the table enumerates exactly the defined exports.
#include "dusk/mods/loader/image_symbols.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using dusk::mods::elf::ImageSymbols;

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

namespace {

template <class T>
T get(const std::vector<uint8_t>& b, size_t at) {
    T v;
    std::memcpy(&v, b.data() + at, sizeof v);
    return v;
}

struct DynSym {
    std::string name;
    uint64_t value;
    uint8_t bind, type;
    uint16_t shndx;
};

void check_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> file{std::istreambuf_iterator<char>(in), {}};
    if (file.size() < 64) {
        std::fprintf(stderr, "%s: unreadable\n", path);
        ++g_failures;
        return;
    }
    // Lay the image out: every PT_LOAD at its vaddr, BSS zero.
    const auto phoff = get<uint64_t>(file, 32);
    const auto phnum = get<uint16_t>(file, 56);
    uint64_t end = 0, dynamicVaddr = 0;
    for (uint16_t i = 0; i < phnum; ++i) {
        const size_t at = phoff + 56ull * i;
        const auto type = get<uint32_t>(file, at);
        if (type == 1) end = std::max(end, get<uint64_t>(file, at + 16) + get<uint64_t>(file, at + 40));
        if (type == 2) dynamicVaddr = get<uint64_t>(file, at + 16);
    }
    std::vector<uint8_t> image(end, 0);
    for (uint16_t i = 0; i < phnum; ++i) {
        const size_t at = phoff + 56ull * i;
        if (get<uint32_t>(file, at) != 1) continue;
        std::memcpy(image.data() + get<uint64_t>(file, at + 16), file.data() + get<uint64_t>(file, at + 8),
            get<uint64_t>(file, at + 32));
    }
    const auto base = reinterpret_cast<uintptr_t>(image.data());

    // The reference: .dynsym through the section headers.
    std::vector<DynSym> reference;
    const auto shoff = get<uint64_t>(file, 40);
    const auto shnum = get<uint16_t>(file, 60);
    for (uint16_t i = 0; i < shnum; ++i) {
        const size_t sh = shoff + 64ull * i;
        if (get<uint32_t>(file, sh + 4) != 11) continue;  // SHT_DYNSYM
        const size_t strOff = get<uint64_t>(file, shoff + 64ull * get<uint32_t>(file, sh + 40) + 24);
        const auto symOff = get<uint64_t>(file, sh + 24);
        const auto count = get<uint64_t>(file, sh + 32) / 24;
        for (uint64_t k = 1; k < count; ++k) {
            const size_t s = symOff + 24 * k;
            reference.push_back({reinterpret_cast<const char*>(file.data() + strOff + get<uint32_t>(file, s)),
                get<uint64_t>(file, s + 8), static_cast<uint8_t>(file[s + 4] >> 4),
                static_cast<uint8_t>(file[s + 4] & 0xf), get<uint16_t>(file, s + 6)});
        }
    }

    std::string error;
    const auto table = ImageSymbols::from_dynamic(base, image.data() + dynamicVaddr, error);
    CHECK(table.has_value());
    if (!table) {
        std::fprintf(stderr, "%s: %s\n", path, error.c_str());
        return;
    }
    CHECK(table->size() == reference.size() + 1);
    const auto shouldFind = [](const DynSym& s) {
        return s.shndx != 0 && (s.bind == 1 || s.bind == 2) && s.type <= 2;
    };
    std::set<std::string> exported;  // all of them first: near misses may be exports too
    for (const auto& s : reference) {
        if (shouldFind(s)) exported.insert(s.name);
    }
    int found = 0, absent = 0;
    for (const auto& s : reference) {
        const uintptr_t got = table->find(s.name);
        if (shouldFind(s)) {
            CHECK(got == base + s.value);
            ++found;
        } else if (!exported.count(s.name)) {
            CHECK(got == 0);
            ++absent;
        }
        // Near misses: the same name one character longer or shorter.
        CHECK(table->find(s.name + "x") == 0 || exported.count(s.name + "x"));
        if (s.name.size() > 1) {
            const std::string shorter = s.name.substr(0, s.name.size() - 1);
            CHECK(table->find(shorter) == 0 || exported.count(shorter));
        }
    }
    CHECK(table->find("") == 0);
    CHECK(table->find("certainly_not_a_symbol_in_this_image") == 0);
    std::set<std::string> enumerated;
    for (size_t i = 0; i < table->size(); ++i) {
        if (const auto e = table->entry(i)) enumerated.insert(std::string(e->first));
    }
    CHECK(enumerated == exported);
    std::printf("%-40s %4zu dynamic symbols: %d exports found at their addresses, %d others not found\n",
        path, reference.size(), found, absent);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s ELF [ELF...]\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; ++i) check_file(argv[i]);
    if (g_failures) {
        std::fprintf(stderr, "image_symbols_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("image_symbols_test: all checks passed\n");
    return 0;
}
