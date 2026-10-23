#pragma once

// Base64 (RFC 4648, standard alphabet with padding: Firestore bytesValue) and URL/form encoding.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace crosssave {

std::string base64_encode(std::span<const uint8_t> bytes);
// Strict: rejects characters outside the alphabet, bad padding and non-canonical trailing bits.
std::optional<std::vector<uint8_t>> base64_decode(std::string_view text);

// Percent-encodes everything except RFC 3986 unreserved characters.
std::string url_encode(std::string_view text);

}  // namespace crosssave
