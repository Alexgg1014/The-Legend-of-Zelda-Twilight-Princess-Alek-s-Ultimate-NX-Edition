#pragma once
// aleks2 (H37): Horizon code memory for the native mod loader. See switch_elf_memory.cpp.
#include "dusk/mods/loader/elf_loader.hpp"

#include <optional>
#include <string>
#include <vector>

struct VirtmemReservation;

namespace dusk::switchelf {

// Why native mods cannot be mapped on this launch, or nullopt if they can.
std::optional<std::string> unavailable_reason();

class CodeMemory final : public mods::elf::MemoryBackend {
public:
    bool reserve(uint64_t size, uint64_t align, uint8_t*& staging, uint64_t& runtimeBase,
        std::string& error) override;
    bool publish(const std::vector<mods::elf::Protection>& ranges, std::string& error) override;

private:
    void* m_heap = nullptr;
    VirtmemReservation* m_reservation = nullptr;
    uint64_t m_code = 0;
    uint64_t m_size = 0;
};

}  // namespace dusk::switchelf
