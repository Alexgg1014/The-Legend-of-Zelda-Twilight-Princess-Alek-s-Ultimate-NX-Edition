// aleks2 (H37): see image_symbols.hpp.
#include "image_symbols.hpp"

#include <cstring>

namespace dusk::mods::elf {
namespace {

constexpr int64_t kDtNull = 0;
constexpr int64_t kDtHash = 4;
constexpr int64_t kDtStrtab = 5;
constexpr int64_t kDtSymtab = 6;
constexpr int64_t kDtGnuHash = 0x6ffffef5;
constexpr size_t kSymSize = 24;
constexpr uint16_t kShnUndef = 0;
constexpr uint8_t kBindGlobal = 1;
constexpr uint8_t kBindWeak = 2;
constexpr uint8_t kTypeNoType = 0;
constexpr uint8_t kTypeObject = 1;
constexpr uint8_t kTypeFunc = 2;

struct Sym {
    uint32_t name;
    uint8_t info;
    uint8_t other;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
};
static_assert(sizeof(Sym) == kSymSize);

Sym sym_at(const uint8_t* symtab, uint32_t index) {
    Sym s;
    std::memcpy(&s, symtab + size_t{index} * kSymSize, sizeof s);
    return s;
}

uint32_t gnu_hash(std::string_view name) {
    uint32_t h = 5381;
    for (const char c : name) {
        h = h * 33 + static_cast<uint8_t>(c);
    }
    return h;
}

uint32_t sysv_hash(std::string_view name) {
    uint32_t h = 0;
    for (const char c : name) {
        h = (h << 4) + static_cast<uint8_t>(c);
        const uint32_t g = h & 0xf0000000u;
        if (g != 0) {
            h ^= g >> 24;
        }
        h &= ~g;
    }
    return h;
}

// The symbol count, which neither hash table states for DT_GNU_HASH: one past the highest
// index any chain reaches.
size_t gnu_symbol_count(const uint32_t* table) {
    const uint32_t nbuckets = table[0];
    const uint32_t symoffset = table[1];
    const uint32_t bloomSize = table[2];
    const auto* buckets = table + 4 + size_t{bloomSize} * 2;  // 64-bit bloom words
    const auto* chains = buckets + nbuckets;
    uint32_t last = 0;
    for (uint32_t b = 0; b < nbuckets; ++b) {
        last = buckets[b] > last ? buckets[b] : last;
    }
    if (last < symoffset) {
        return symoffset;
    }
    while ((chains[last - symoffset] & 1u) == 0) {
        ++last;
    }
    return size_t{last} + 1;
}

}  // namespace

std::optional<ImageSymbols> ImageSymbols::from_dynamic(uintptr_t base, const void* dynamic, std::string& error) {
    ImageSymbols out;
    out.m_base = base;
    const auto* dyn = static_cast<const uint8_t*>(dynamic);
    for (size_t i = 0;; ++i) {
        int64_t tag = 0;
        uint64_t value = 0;
        std::memcpy(&tag, dyn + i * 16, 8);
        std::memcpy(&value, dyn + i * 16 + 8, 8);
        if (tag == kDtNull) {
            break;
        }
        const auto* at = reinterpret_cast<const uint8_t*>(base + value);
        switch (tag) {
        case kDtSymtab:
            out.m_symtab = at;
            break;
        case kDtStrtab:
            out.m_strtab = reinterpret_cast<const char*>(at);
            break;
        case kDtGnuHash:
            out.m_gnuHash = reinterpret_cast<const uint32_t*>(at);
            break;
        case kDtHash:
            out.m_sysvHash = reinterpret_cast<const uint32_t*>(at);
            break;
        default:
            break;
        }
    }
    if (out.m_symtab == nullptr || out.m_strtab == nullptr) {
        error = "the image has no dynamic symbol table";
        return std::nullopt;
    }
    if (out.m_gnuHash != nullptr) {
        if (out.m_gnuHash[0] == 0 || out.m_gnuHash[2] == 0) {
            error = "the image's DT_GNU_HASH is empty";
            return std::nullopt;
        }
        out.m_count = gnu_symbol_count(out.m_gnuHash);
    } else if (out.m_sysvHash != nullptr) {
        out.m_count = out.m_sysvHash[1];
    } else {
        error = "the image has neither DT_GNU_HASH nor DT_HASH";
        return std::nullopt;
    }
    return out;
}

bool ImageSymbols::defined_export(uint32_t index, std::string_view name) const {
    const Sym s = sym_at(m_symtab, index);
    const uint8_t bind = s.info >> 4;
    const uint8_t type = s.info & 0xf;
    if (s.shndx == kShnUndef || (bind != kBindGlobal && bind != kBindWeak) ||
        (type != kTypeFunc && type != kTypeObject && type != kTypeNoType))
    {
        return false;
    }
    const char* symName = m_strtab + s.name;
    return name.size() == std::strlen(symName) && std::memcmp(symName, name.data(), name.size()) == 0;
}

uintptr_t ImageSymbols::find(std::string_view name) const {
    if (m_gnuHash != nullptr) {
        const uint32_t nbuckets = m_gnuHash[0];
        const uint32_t symoffset = m_gnuHash[1];
        const uint32_t bloomSize = m_gnuHash[2];
        const uint32_t bloomShift = m_gnuHash[3];
        uint64_t word = 0;
        const uint32_t h = gnu_hash(name);
        std::memcpy(&word, m_gnuHash + 4 + size_t{(h / 64) % bloomSize} * 2, 8);
        const uint64_t mask = (uint64_t{1} << (h % 64)) | (uint64_t{1} << ((h >> bloomShift) % 64));
        if ((word & mask) != mask) {
            return 0;
        }
        const auto* buckets = m_gnuHash + 4 + size_t{bloomSize} * 2;
        const auto* chains = buckets + nbuckets;
        uint32_t index = buckets[h % nbuckets];
        if (index < symoffset) {
            return 0;
        }
        for (;; ++index) {
            const uint32_t chain = chains[index - symoffset];
            if ((chain | 1u) == (h | 1u) && defined_export(index, name)) {
                return m_base + sym_at(m_symtab, index).value;
            }
            if (chain & 1u) {
                return 0;
            }
        }
    }
    const uint32_t nbucket = m_sysvHash[0];
    const uint32_t* buckets = m_sysvHash + 2;
    const uint32_t* chains = buckets + nbucket;
    for (uint32_t index = buckets[sysv_hash(name) % nbucket]; index != 0; index = chains[index]) {
        if (defined_export(index, name)) {
            return m_base + sym_at(m_symtab, index).value;
        }
    }
    return 0;
}

std::optional<std::pair<std::string_view, uintptr_t>> ImageSymbols::entry(size_t index) const {
    if (index == 0 || index >= m_count) {
        return std::nullopt;
    }
    const Sym s = sym_at(m_symtab, static_cast<uint32_t>(index));
    const std::string_view name = m_strtab + s.name;
    if (!defined_export(static_cast<uint32_t>(index), name)) {
        return std::nullopt;
    }
    return std::pair{name, m_base + s.value};
}

}  // namespace dusk::mods::elf
