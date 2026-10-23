// aleks2 (H37): code memory for src/dusk/mods/loader/elf_loader.cpp on Horizon.
//
// What Horizon's own loader does with an NRO, done for a mod: the image is staged and
// relocated in ordinary heap memory, then svcMapProcessCodeMemory (0x77) maps that heap at a
// free address in the code region and svcSetProcessMemoryPermission (0x73) gives each page
// range its final permission (RX code, R read-only data and RELRO, RW data). The staging
// heap becomes inaccessible once mapped, so nothing writes code after it is executable, and
// no page is ever writable and executable at once.
//
// Both calls need the launcher to permit the syscalls and a handle to our own process --
// the same requirement the hook backend already has (switch_jit.cpp, probe_self_patch).
// When either is missing, reserve() says so and the mod is refused with that message.
//
// Nothing is unmapped while the game runs. A module that is turned off stays mapped until the game exits:
// it may still be on the stack below the current frame, or have callbacks registered.
// V1-rc9: at exit every mapping is undone (aleks2_release_mod_images, from userAppExit): returning to hbloader
// does not end the process, and hbloader got back a heap with pages still locked as the source of code aliases --
// it then stopped with InvalidCurrentMemory (2001-0106, rc8 console, three times).
#ifdef __SWITCH__
#include "dusk/switch_elf_memory.hpp"

#include <switch.h>

#include "own_process_handle.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace dusk::switchelf {
namespace {

constexpr uint64_t kPage = 0x1000;

// V1-rc9: every image mapped, for the release at exit.
struct Mapping {
    uint64_t code;
    uint64_t heap;
    uint64_t size;
};
constexpr int kMaxMappings = 256;
Mapping s_mappings[kMaxMappings];
int s_mappingCount = 0;

uint32_t horizon_perm(uint32_t elfPerm) {
    uint32_t p = 0;
    if (elfPerm & mods::elf::kPermR) p |= Perm_R;
    if (elfPerm & mods::elf::kPermW) p |= Perm_W;
    if (elfPerm & mods::elf::kPermX) p |= Perm_X;
    return p;
}

std::string hex(uint32_t rc) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%x", rc);
    return buf;
}

}  // namespace

std::optional<std::string> unavailable_reason() {
    if (!envIsSyscallHinted(0x77)) {
        return "the launcher does not permit svcMapProcessCodeMemory";
    }
    if (!envIsSyscallHinted(0x73)) {
        return "the launcher does not permit svcSetProcessMemoryPermission";
    }
    // DM1-c: hbloader's handle, or (an emulator starting the NRO itself) one from the IPC fallback.
    if (aleks2_own_process_handle() == INVALID_HANDLE) {
        return "no handle to our own process (start the game as a title takeover)";
    }
    return std::nullopt;
}

bool CodeMemory::reserve(uint64_t size, uint64_t align, uint8_t*& staging, uint64_t& runtimeBase,
    std::string& error) {
    if (const auto why = unavailable_reason()) {
        error = *why;
        return false;
    }
    if (size == 0 || (size % kPage) != 0) {
        error = "image size is not whole pages";
        return false;
    }
    m_heap = std::aligned_alloc(std::max<uint64_t>(align, kPage), size);
    if (m_heap == nullptr) {
        error = "out of memory for the staging copy";
        return false;
    }
    virtmemLock();
    void* code = virtmemFindCodeMemory(size, kPage);
    m_reservation = code != nullptr ? virtmemAddReservation(code, size) : nullptr;
    virtmemUnlock();
    if (code == nullptr || m_reservation == nullptr) {
        std::free(m_heap);
        m_heap = nullptr;
        error = "no free code address range of that size";
        return false;
    }
    m_code = reinterpret_cast<uint64_t>(code);
    m_size = size;
    staging = static_cast<uint8_t*>(m_heap);
    runtimeBase = m_code;
    return true;
}

bool CodeMemory::publish(const std::vector<mods::elf::Protection>& ranges, std::string& error) {
    const Handle self = aleks2_own_process_handle();
    armDCacheFlush(m_heap, m_size);
    if (s_mappingCount >= kMaxMappings) {
        error = "too many mod images mapped";
        return false;
    }
    Result rc = svcMapProcessCodeMemory(self, m_code, reinterpret_cast<uint64_t>(m_heap), m_size);
    if (R_FAILED(rc)) {
        error = "svcMapProcessCodeMemory failed: " + hex(rc);
        return false;
    }
    s_mappings[s_mappingCount++] = {m_code, reinterpret_cast<uint64_t>(m_heap), m_size};
    // From here the heap copy belongs to the mapping: never freed, never written.
    for (const auto& r : ranges) {
        rc = svcSetProcessMemoryPermission(self, m_code + r.offset, r.size, horizon_perm(r.perm));
        if (R_FAILED(rc)) {
            error = "svcSetProcessMemoryPermission failed: " + hex(rc);
            return false;
        }
    }
    for (const auto& r : ranges) {
        if (r.perm & mods::elf::kPermX) {
            armICacheInvalidate(reinterpret_cast<void*>(m_code + r.offset), r.size);
        }
    }
    return true;
}

}  // namespace dusk::switchelf

// V1-rc9: undoes every image mapping (any permission: the kernel only checks the alias state). Returns how many
// failed; *released counts the others, *lastRc is the last failure's result.
extern "C" int aleks2_release_mod_images(int* released, uint32_t* lastRc) {
    using namespace dusk::switchelf;
    const Handle self = aleks2_own_process_handle();
    int failures = 0;
    for (int i = s_mappingCount - 1; i >= 0; --i) {
        const Mapping& m = s_mappings[i];
        const Result rc = svcUnmapProcessCodeMemory(self, m.code, m.heap, m.size);
        if (R_FAILED(rc)) {
            ++failures;
            *lastRc = rc;
        } else {
            ++*released;
        }
    }
    s_mappingCount = 0;
    return failures;
}
#endif
