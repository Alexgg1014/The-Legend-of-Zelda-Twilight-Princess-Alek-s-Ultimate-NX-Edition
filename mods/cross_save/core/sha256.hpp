#pragma once

// SHA-256 (FIPS 180-4). Self-contained so the mod's import surface stays plain libc/libstdc++ on
// every platform the same package ships to (PC, Android, Switch through the linux-aarch64 bridge).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace crosssave {

class Sha256 {
public:
    Sha256();
    void update(std::span<const uint8_t> bytes);
    void update(std::string_view text);
    std::array<uint8_t, 32> finish();
    std::string finish_hex();

private:
    void block(const uint8_t* data);

    std::array<uint32_t, 8> m_state{};
    std::array<uint8_t, 64> m_buffer{};
    size_t m_buffered = 0;
    uint64_t m_length = 0;
};

std::string sha256_hex(std::span<const uint8_t> bytes);
std::string to_hex(std::span<const uint8_t> bytes);

}  // namespace crosssave
