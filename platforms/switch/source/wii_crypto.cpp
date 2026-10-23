// aleks2 (rc17, Wii discs): see wii_crypto.hpp. Key files only; no key in this program.
#include "switch/wii_crypto.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

namespace dusk::sw::disc {
namespace {

constexpr std::size_t kBootMiiKeysSize = 0x400;
constexpr std::size_t kBootMiiCommonKeyOffset = 0x114;  // OTP at 0x100, the common key 0x14 into it

bool read_file(const std::string& path, std::vector<std::uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    out.clear();
    std::uint8_t buf[0x1000];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && out.size() < 0x10000) {
        out.insert(out.end(), buf, buf + n);
    }
    std::fclose(f);
    return true;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_hex_key(const std::vector<std::uint8_t>& text, std::uint8_t key[16]) {
    std::string digits;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = static_cast<char>(text[i]);
        if (c == '0' && i + 1 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
            ++i;  // a 0x prefix
            continue;
        }
        if (hex_value(c) >= 0) {
            digits.push_back(c);
        } else if (!std::isspace(static_cast<unsigned char>(c)) && c != ':' && c != '-') {
            return false;
        }
    }
    if (digits.size() != 32) {
        return false;
    }
    for (int i = 0; i < 16; ++i) {
        key[i] = static_cast<std::uint8_t>(hex_value(digits[2 * i]) * 16 + hex_value(digits[2 * i + 1]));
    }
    return true;
}

}  // namespace

bool load_wii_common_key(const std::string& dir, std::uint8_t key[16], std::string& source, std::string& problem) {
    problem.clear();
    for (const char* name : kWiiKeyFiles) {
        const std::string path = dir + "/" + name;
        std::vector<std::uint8_t> data;
        if (!read_file(path, data)) {
            continue;
        }
        const std::string file(name);
        bool ok = false;
        if (file == "wii_common_key.bin") {
            ok = data.size() == 16;
            if (ok) {
                std::copy(data.begin(), data.end(), key);
            }
        } else if (file == "wii_common_key.txt") {
            ok = parse_hex_key(data, key);
        } else {  // keys.bin
            ok = data.size() == kBootMiiKeysSize;
            if (ok) {
                std::copy(data.begin() + kBootMiiCommonKeyOffset, data.begin() + kBootMiiCommonKeyOffset + 16, key);
            }
        }
        if (ok) {
            source = path;
            return true;
        }
        if (problem.empty()) {
            problem = path + " is not a Wii common key file (expected 16 bytes, 32 hex digits, or BootMii's 1 KiB "
                             "keys.bin)";
        }
    }
    return false;
}

}  // namespace dusk::sw::disc
