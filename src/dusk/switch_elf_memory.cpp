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
// Nothing is ever unmapped. A module that is turned off stays mapped until the game exits:
// it may still be on the stack below the current frame, or have callbacks registered.
#ifdef __SWITCH__
#include "dusk/switch_elf_memory.hpp"

#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace dusk::switchelf {
namespace {

constexpr uint64_t kPage = 0x1000;

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
    if (envGetOwnProcessHandle() == INVALID_HANDLE) {
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
    const Handle self = envGetOwnProcessHandle();
    armDCacheFlush(m_heap, m_size);
    Result rc = svcMapProcessCodeMemory(self, m_code, reinterpret_cast<uint64_t>(m_heap), m_size);
    if (R_FAILED(rc)) {
        error = "svcMapProcessCodeMemory failed: " + hex(rc);
        return false;
    }
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
#endif
