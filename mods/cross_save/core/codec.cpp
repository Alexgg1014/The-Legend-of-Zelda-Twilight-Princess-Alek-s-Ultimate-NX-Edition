#include "codec.hpp"

namespace crosssave {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decode_char(char value) {
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    if (value == '+') {
        return 62;
    }
    if (value == '/') {
        return 63;
    }
    return -1;
}

}  // namespace

std::string base64_encode(std::span<const uint8_t> bytes) {
    std::string text;
    text.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const uint32_t value = (uint32_t{bytes[i]} << 16) | (uint32_t{bytes[i + 1]} << 8) | bytes[i + 2];
        text.push_back(kAlphabet[(value >> 18) & 63]);
        text.push_back(kAlphabet[(value >> 12) & 63]);
        text.push_back(kAlphabet[(value >> 6) & 63]);
        text.push_back(kAlphabet[value & 63]);
    }
    const size_t rest = bytes.size() - i;
    if (rest == 1) {
        const uint32_t value = uint32_t{bytes[i]} << 16;
        text.push_back(kAlphabet[(value >> 18) & 63]);
        text.push_back(kAlphabet[(value >> 12) & 63]);
        text += "==";
    } else if (rest == 2) {
        const uint32_t value = (uint32_t{bytes[i]} << 16) | (uint32_t{bytes[i + 1]} << 8);
        text.push_back(kAlphabet[(value >> 18) & 63]);
        text.push_back(kAlphabet[(value >> 12) & 63]);
        text.push_back(kAlphabet[(value >> 6) & 63]);
        text.push_back('=');
    }
    return text;
}

std::optional<std::vector<uint8_t>> base64_decode(std::string_view text) {
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        const int a = decode_char(text[i]);
        const int b = decode_char(text[i + 1]);
        if (a < 0 || b < 0) {
            return std::nullopt;
        }
        if (last && text[i + 2] == '=' && text[i + 3] == '=') {
            if ((b & 15) != 0) {
                return std::nullopt;
            }
            bytes.push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
            break;
        }
        const int c = decode_char(text[i + 2]);
        if (c < 0) {
            return std::nullopt;
        }
        if (last && text[i + 3] == '=') {
            if ((c & 3) != 0) {
                return std::nullopt;
            }
            bytes.push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
            bytes.push_back(static_cast<uint8_t>(((b & 15) << 4) | (c >> 2)));
            break;
        }
        const int d = decode_char(text[i + 3]);
        if (d < 0) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<uint8_t>((a << 2) | (b >> 4)));
        bytes.push_back(static_cast<uint8_t>(((b & 15) << 4) | (c >> 2)));
        bytes.push_back(static_cast<uint8_t>(((c & 3) << 6) | d));
    }
    return bytes;
}

std::string url_encode(std::string_view text) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    for (const char character : text) {
        const auto value = static_cast<unsigned char>(character);
        if ((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
            (value >= '0' && value <= '9') || value == '-' || value == '_' || value == '.' ||
            value == '~')
        {
            out.push_back(character);
        } else {
            out.push_back('%');
            out.push_back(kDigits[value >> 4]);
            out.push_back(kDigits[value & 15]);
        }
    }
    return out;
}

}  // namespace crosssave
