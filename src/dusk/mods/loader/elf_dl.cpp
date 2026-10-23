// aleks2 (DM2-d): see elf_dl.hpp.
#include "elf_dl.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <mutex>

namespace dusk::mods::elf::dl {
namespace {

// glibc's struct dl_phdr_info on LP64, field for field.
struct GlibcDlPhdrInfo {
    uint64_t dlpi_addr;
    const char* dlpi_name;
    const void* dlpi_phdr;
    uint16_t dlpi_phnum;
    unsigned long long dlpi_adds;
    unsigned long long dlpi_subs;
    size_t dlpi_tls_modid;
    void* dlpi_tls_data;
};
static_assert(sizeof(GlibcDlPhdrInfo) == 64 && offsetof(GlibcDlPhdrInfo, dlpi_phdr) == 16 &&
              offsetof(GlibcDlPhdrInfo, dlpi_phnum) == 24 && offsetof(GlibcDlPhdrInfo, dlpi_adds) == 32 &&
              offsetof(GlibcDlPhdrInfo, dlpi_tls_modid) == 48 && offsetof(GlibcDlPhdrInfo, dlpi_tls_data) == 56);

std::mutex s_mutex;
std::deque<Object> s_objects;  // append-only: a snapshot's pointers stay valid
const Object* s_main = nullptr;
std::deque<Object> s_mains;    // every main ever set, so a snapshot of an older one stays valid
unsigned long long s_adds = 0;
void* (*s_tlsBlock)(uint64_t) = nullptr;

}  // namespace

void set_main(Object object) {
    std::lock_guard lock(s_mutex);
    s_mains.push_back(std::move(object));
    s_main = &s_mains.back();
    ++s_adds;
}

void add(Object object) {
    std::lock_guard lock(s_mutex);
    s_objects.push_back(std::move(object));
    ++s_adds;
}

void set_tls_block_lookup(void* (*lookup)(uint64_t module)) {
    std::lock_guard lock(s_mutex);
    s_tlsBlock = lookup;
}

size_t object_count() {
    std::lock_guard lock(s_mutex);
    return s_objects.size() + (s_main != nullptr ? 1 : 0);
}

std::vector<Shim> shims() {
    return {{"dl_iterate_phdr", static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&aleks2_dl_iterate_phdr)),
                "the game and every mapped module, glibc dl_phdr_info layout (DM2-d)", true},
        {"_dl_find_object", static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&aleks2_dl_find_object)),
            "the object holding a pc and its PT_GNU_EH_FRAME, glibc dl_find_object layout (DM2-d)", true}};
}

}  // namespace dusk::mods::elf::dl

using namespace dusk::mods::elf::dl;

namespace {
// glibc 2.35+ <dlfcn.h> struct dl_find_object on LP64.
struct GlibcDlFindObject {
    unsigned long long dlfo_flags;
    void* dlfo_map_start;
    void* dlfo_map_end;
    void* dlfo_link_map;
    void* dlfo_eh_frame;
    unsigned long long reserved[7];
};
static_assert(sizeof(GlibcDlFindObject) == 96 && offsetof(GlibcDlFindObject, dlfo_eh_frame) == 32);

struct Phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
static_assert(sizeof(Phdr) == 56);
}  // namespace

extern "C" int aleks2_dl_find_object(void* pc, void* result) {
    const auto at = reinterpret_cast<uint64_t>(pc);
    std::vector<const Object*> walk;
    {
        std::lock_guard lock(s_mutex);
        if (s_main != nullptr) {
            walk.push_back(s_main);
        }
        for (const auto& o : s_objects) {
            walk.push_back(&o);
        }
    }
    for (const Object* o : walk) {
        const auto* phdrs = reinterpret_cast<const Phdr*>(o->phdrTable.data());
        uint64_t lo = ~0ull, hi = 0, ehFrame = 0;
        bool holds = false;
        for (uint16_t i = 0; i < o->phnum; ++i) {
            const Phdr& p = phdrs[i];
            if (p.type == 1) {  // PT_LOAD
                const uint64_t start = o->bias + p.vaddr, end = start + p.memsz;
                lo = std::min(lo, start);
                hi = std::max(hi, end);
                holds |= at >= start && at < end;
            } else if (p.type == 0x6474E550) {  // PT_GNU_EH_FRAME
                ehFrame = o->bias + p.vaddr;
            }
        }
        if (holds) {
            auto* r = static_cast<GlibcDlFindObject*>(result);
            *r = {};
            r->dlfo_map_start = reinterpret_cast<void*>(static_cast<uintptr_t>(lo));
            r->dlfo_map_end = reinterpret_cast<void*>(static_cast<uintptr_t>(hi));
            r->dlfo_eh_frame = reinterpret_cast<void*>(static_cast<uintptr_t>(ehFrame));
            return 0;
        }
    }
    return -1;
}

// The callback runs without the lock (glibc holds dl_load_write_lock; here nothing can be removed and an
// object added meanwhile is simply not in this walk), so it may itself load or iterate.
extern "C" int aleks2_dl_iterate_phdr(int (*callback)(void* info, size_t size, void* data), void* data) {
    if (callback == nullptr) {
        return 0;
    }
    std::vector<const Object*> walk;
    unsigned long long adds = 0;
    void* (*tlsBlock)(uint64_t) = nullptr;
    {
        std::lock_guard lock(s_mutex);
        if (s_main != nullptr) {
            walk.push_back(s_main);
        }
        for (const auto& o : s_objects) {
            walk.push_back(&o);
        }
        adds = s_adds;
        tlsBlock = s_tlsBlock;
    }
    for (const Object* o : walk) {
        GlibcDlPhdrInfo info{};
        info.dlpi_addr = o->bias;
        info.dlpi_name = o->name.c_str();
        info.dlpi_phdr = o->phdrTable.data();
        info.dlpi_phnum = o->phnum;
        info.dlpi_adds = adds;
        info.dlpi_subs = 0;
        info.dlpi_tls_modid = o->tlsModule;
        info.dlpi_tls_data = o->tlsModule != 0 && tlsBlock != nullptr ? tlsBlock(o->tlsModule) : nullptr;
        if (const int r = callback(&info, sizeof info, data); r != 0) {
            return r;
        }
    }
    return 0;
}
