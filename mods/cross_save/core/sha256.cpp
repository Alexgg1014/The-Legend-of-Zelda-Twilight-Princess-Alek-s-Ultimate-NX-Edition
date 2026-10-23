#include "sha256.hpp"

#include <algorithm>
#include <cstring>

namespace crosssave {
namespace {

constexpr std::array<uint32_t, 64> kRound{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr uint32_t rotr(uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

}  // namespace

Sha256::Sha256()
    : m_state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
          0x5be0cd19} {}

void Sha256::block(const uint8_t* data) {
    std::array<uint32_t, 64> w{};
    for (size_t i = 0; i < 16; ++i) {
        w[i] = (uint32_t{data[i * 4]} << 24) | (uint32_t{data[i * 4 + 1]} << 16) |
               (uint32_t{data[i * 4 + 2]} << 8) | uint32_t{data[i * 4 + 3]};
    }
    for (size_t i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
    uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
    for (size_t i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t choice = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + choice + kRound[i] + w[i];
        const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256::update(std::span<const uint8_t> bytes) {
    m_length += bytes.size();
    size_t offset = 0;
    if (m_buffered != 0) {
        const size_t take = std::min(bytes.size(), m_buffer.size() - m_buffered);
        std::memcpy(m_buffer.data() + m_buffered, bytes.data(), take);
        m_buffered += take;
        offset = take;
        if (m_buffered < m_buffer.size()) {
            return;
        }
        block(m_buffer.data());
        m_buffered = 0;
    }
    for (; offset + 64 <= bytes.size(); offset += 64) {
        block(bytes.data() + offset);
    }
    if (offset < bytes.size()) {
        m_buffered = bytes.size() - offset;
        std::memcpy(m_buffer.data(), bytes.data() + offset, m_buffered);
    }
}

void Sha256::update(std::string_view text) {
    update(std::span{reinterpret_cast<const uint8_t*>(text.data()), text.size()});
}

std::array<uint8_t, 32> Sha256::finish() {
    const uint64_t bits = m_length * 8;
    std::array<uint8_t, 72> padding{};
    padding[0] = 0x80;
    const size_t padLength = (m_buffered < 56 ? 56 : 120) - m_buffered;
    std::array<uint8_t, 8> length{};
    for (size_t i = 0; i < 8; ++i) {
        length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    }
    update(std::span{padding.data(), padLength});
    update(length);
    std::array<uint8_t, 32> digest{};
    for (size_t i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<uint8_t>(m_state[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(m_state[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(m_state[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(m_state[i]);
    }
    *this = Sha256{};
    return digest;
}

std::string Sha256::finish_hex() {
    const auto digest = finish();
    return to_hex(digest);
}

std::string to_hex(std::span<const uint8_t> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const uint8_t value : bytes) {
        text.push_back(kDigits[value >> 4]);
        text.push_back(kDigits[value & 15]);
    }
    return text;
}

std::string sha256_hex(std::span<const uint8_t> bytes) {
    Sha256 hasher;
    hasher.update(bytes);
    return hasher.finish_hex();
}

}  // namespace crosssave
