#pragma once
// aleks2 (H37): the exported symbols of an ELF image already in memory, looked up by name
// through its own hash tables.
//
// On Switch this is the game's import surface for native mods: the NRO keeps its .dynsym
// (only the names the build's --dynamic-list exports: platforms/switch/cmake/ModAbiExports.cmake)
// and its .dynamic in loaded memory, so the running image can answer "where is X" exactly,
// with no file to go stale. It finds nothing the link did not export, which is the allowlist.
//
// Platform-neutral and pure: it reads the image at `base` and does not care how it got there
// (libnx's crt0 for the NRO, elf_loader for a test library).
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dusk::mods::elf {

class ImageSymbols {
public:
    // `base` is the runtime address of virtual address 0 of the image; `dynamic` points at its
    // PT_DYNAMIC array (unrelocated d_ptr values, as the linker wrote them). Needs DT_SYMTAB,
    // DT_STRTAB and DT_GNU_HASH or DT_HASH.
    static std::optional<ImageSymbols> from_dynamic(uintptr_t base, const void* dynamic, std::string& error);

    // Runtime address of a defined GLOBAL or WEAK function or object named `name`, or 0.
    uintptr_t find(std::string_view name) const;

    // Number of entries in the dynamic symbol table (including the null entry).
    size_t size() const { return m_count; }
    // Name and runtime address of entry `index` if it is a defined export, for enumeration.
    std::optional<std::pair<std::string_view, uintptr_t>> entry(size_t index) const;

private:
    ImageSymbols() = default;
    bool defined_export(uint32_t index, std::string_view name) const;

    uintptr_t m_base = 0;
    const uint8_t* m_symtab = nullptr;
    const char* m_strtab = nullptr;
    const uint32_t* m_gnuHash = nullptr;
    const uint32_t* m_sysvHash = nullptr;
    size_t m_count = 0;
};

}  // namespace dusk::mods::elf
