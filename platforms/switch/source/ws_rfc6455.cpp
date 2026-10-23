// aleks2 (DM3): see ws_rfc6455.hpp.
#include "ws_rfc6455.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace aleks2::ws {
namespace {

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// A comma-separated header value containing `token` (case-insensitive), as Connection: keep-alive, Upgrade.
bool has_token(std::string_view value, std::string_view token) {
    while (!value.empty()) {
        const size_t comma = value.find(',');
        if (iequals(trim(value.substr(0, comma)), token)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        value.remove_prefix(comma + 1);
    }
    return false;
}

uint32_t rol(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

}  // namespace

std::optional<Url> parse_url(std::string_view url) {
    Url out;
    const size_t sep = url.find("://");
    if (sep == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view scheme = url.substr(0, sep);
    if (iequals(scheme, "wss")) {
        out.secure = true;
        out.port = 443;
    } else if (iequals(scheme, "ws")) {
        out.port = 80;
    } else {
        return std::nullopt;
    }
    std::string_view rest = url.substr(sep + 3);
    const size_t pathStart = rest.find_first_of("/?#");
    std::string_view authority = rest.substr(0, pathStart);
    std::string_view resource = pathStart == std::string_view::npos ? std::string_view{} : rest.substr(pathStart);
    if (const size_t hash = resource.find('#'); hash != std::string_view::npos) {
        resource = resource.substr(0, hash);
    }
    if (const size_t at = authority.rfind('@'); at != std::string_view::npos) {
        authority.remove_prefix(at + 1);  // userinfo is not sent
    }
    std::string_view host = authority;
    std::string_view port;
    if (!authority.empty() && authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                return std::nullopt;
            }
            port = authority.substr(close + 2);
        }
    } else if (const size_t colon = authority.rfind(':'); colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    if (host.empty()) {
        return std::nullopt;
    }
    if (!port.empty()) {
        unsigned value = 0;
        for (const char c : port) {
            if (c < '0' || c > '9' || (value = value * 10 + static_cast<unsigned>(c - '0')) > 65535) {
                return std::nullopt;
            }
        }
        if (value == 0) {
            return std::nullopt;
        }
        out.port = static_cast<uint16_t>(value);
    }
    out.host = std::string{host};
    out.resource = resource.empty() ? "/" : (resource.front() == '?' ? "/" + std::string{resource} : std::string{resource});
    return out;
}

std::string base64(const uint8_t* data, size_t size) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t b = (uint32_t{data[i]} << 16) | (i + 1 < size ? uint32_t{data[i + 1]} << 8 : 0) |
                           (i + 2 < size ? uint32_t{data[i + 2]} : 0);
        out += kTable[(b >> 18) & 63];
        out += kTable[(b >> 12) & 63];
        out += i + 1 < size ? kTable[(b >> 6) & 63] : '=';
        out += i + 2 < size ? kTable[b & 63] : '=';
    }
    return out;
}

std::string sha1(std::string_view data) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m{data};
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    m += static_cast<char>(0x80);
    while (m.size() % 64 != 56) {
        m += '\0';
    }
    for (int i = 7; i >= 0; i--) {
        m += static_cast<char>((bits >> (i * 8)) & 0xff);
    }
    for (size_t chunk = 0; chunk < m.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            const auto* p = reinterpret_cast<const uint8_t*>(m.data() + chunk + 4 * i);
            w[i] = (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::string out(20, '\0');
    for (int i = 0; i < 5; i++) {
        out[4 * i] = static_cast<char>(h[i] >> 24);
        out[4 * i + 1] = static_cast<char>(h[i] >> 16);
        out[4 * i + 2] = static_cast<char>(h[i] >> 8);
        out[4 * i + 3] = static_cast<char>(h[i]);
    }
    return out;
}

std::string accept_for(std::string_view key) {
    const std::string digest = sha1(std::string{key} + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return base64(reinterpret_cast<const uint8_t*>(digest.data()), digest.size());
}

std::string handshake_request(const Url& url, std::string_view key, const std::vector<std::string>& protocols,
    const std::vector<std::pair<std::string, std::string>>& headers) {
    std::string host = url.host.find(':') != std::string::npos ? "[" + url.host + "]" : url.host;
    if (url.port != (url.secure ? 443 : 80)) {
        host += ":" + std::to_string(url.port);
    }
    std::string r = "GET " + url.resource + " HTTP/1.1\r\nHost: " + host +
                    "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + std::string{key} +
                    "\r\nSec-WebSocket-Version: 13\r\n";
    if (!protocols.empty()) {
        r += "Sec-WebSocket-Protocol: ";
        for (size_t i = 0; i < protocols.size(); i++) {
            r += (i != 0 ? ", " : "") + protocols[i];
        }
        r += "\r\n";
    }
    bool userAgent = false;
    for (const auto& [name, value] : headers) {
        userAgent = userAgent || iequals(name, "User-Agent");
        r += name + ": " + value + "\r\n";
    }
    if (!userAgent) {
        r += "User-Agent: Dusklight-Switch\r\n";
    }
    return r + "\r\n";
}

HandshakeResponse check_response(std::string_view head, std::string_view key) {
    HandshakeResponse out;
    const size_t lineEnd = head.find("\r\n");
    const std::string_view status = head.substr(0, lineEnd);
    if (!status.starts_with("HTTP/1.1 ") && !status.starts_with("HTTP/1.0 ")) {
        out.error = "Not an HTTP response";
        return out;
    }
    if (status.size() < 12) {
        out.error = "Malformed status line";
        return out;
    }
    for (int i = 9; i < 12; i++) {
        if (status[i] < '0' || status[i] > '9') {
            out.error = "Malformed status line";
            return out;
        }
        out.status = out.status * 10 + (status[i] - '0');
    }
    std::string_view rest = lineEnd == std::string_view::npos ? std::string_view{} : head.substr(lineEnd + 2);
    while (!rest.empty()) {
        const size_t end = rest.find("\r\n");
        const std::string_view line = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 2);
        if (line.empty()) {
            break;
        }
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        out.headers.emplace_back(std::string{trim(line.substr(0, colon))}, std::string{trim(line.substr(colon + 1))});
    }
    if (out.status != 101) {
        out.error = "WebSocket upgrade was refused (HTTP " + std::to_string(out.status) + ")";
        return out;
    }
    bool upgrade = false, connection = false, accept = false;
    for (const auto& [name, value] : out.headers) {
        if (iequals(name, "Upgrade")) {
            upgrade = has_token(value, "websocket");
        } else if (iequals(name, "Connection")) {
            connection = has_token(value, "upgrade");
        } else if (iequals(name, "Sec-WebSocket-Accept")) {
            accept = value == accept_for(key);
        } else if (iequals(name, "Sec-WebSocket-Protocol")) {
            out.protocol = value;
        } else if (iequals(name, "Sec-WebSocket-Extensions") && !value.empty()) {
            out.error = "Server enabled an extension that was not offered";
            return out;
        }
    }
    if (!upgrade || !connection) {
        out.error = "Server did not upgrade the connection to WebSocket";
    } else if (!accept) {
        out.error = "Sec-WebSocket-Accept does not match the key";
    }
    return out;
}

std::string encode_frame(Opcode opcode, std::string_view payload, const uint8_t mask[4], bool fin) {
    std::string f;
    f.reserve(payload.size() + 14);
    f += static_cast<char>((fin ? 0x80 : 0) | static_cast<uint8_t>(opcode));
    const uint64_t n = payload.size();
    if (n < 126) {
        f += static_cast<char>(0x80 | n);
    } else if (n <= 0xffff) {
        f += static_cast<char>(0x80 | 126);
        f += static_cast<char>(n >> 8);
        f += static_cast<char>(n & 0xff);
    } else {
        f += static_cast<char>(0x80 | 127);
        for (int i = 7; i >= 0; i--) {
            f += static_cast<char>((n >> (8 * i)) & 0xff);
        }
    }
    f.append(reinterpret_cast<const char*>(mask), 4);
    const size_t start = f.size();
    f.append(payload);
    for (size_t i = 0; i < payload.size(); i++) {
        f[start + i] = static_cast<char>(f[start + i] ^ mask[i & 3]);
    }
    return f;
}

void Decoder::feed(const void* data, size_t size) {
    if (m_offset > 0 && m_offset == m_buffer.size()) {
        m_buffer.clear();
        m_offset = 0;
    } else if (m_offset > 64 * 1024) {
        m_buffer.erase(0, m_offset);
        m_offset = 0;
    }
    m_buffer.append(static_cast<const char*>(data), size);
}

std::optional<Frame> Decoder::next() {
    if (!m_error.empty()) {
        return std::nullopt;
    }
    const size_t available = m_buffer.size() - m_offset;
    if (available < 2) {
        return std::nullopt;
    }
    const auto* p = reinterpret_cast<const uint8_t*>(m_buffer.data() + m_offset);
    const bool fin = (p[0] & 0x80) != 0;
    const uint8_t opcode = p[0] & 0x0f;
    if ((p[0] & 0x70) != 0) {
        m_error = "Reserved bits set in a frame";
        return std::nullopt;
    }
    if ((p[1] & 0x80) != 0) {
        m_error = "Server sent a masked frame";
        return std::nullopt;
    }
    uint64_t length = p[1] & 0x7f;
    size_t header = 2;
    if (length == 126) {
        if (available < 4) return std::nullopt;
        length = (uint64_t{p[2]} << 8) | p[3];
        header = 4;
    } else if (length == 127) {
        if (available < 10) return std::nullopt;
        length = 0;
        for (int i = 0; i < 8; i++) {
            length = (length << 8) | p[2 + i];
        }
        if ((length >> 63) != 0) {
            m_error = "Frame length out of range";
            return std::nullopt;
        }
        header = 10;
    }
    const bool control = (opcode & 0x8) != 0;
    if (control && (!fin || length > 125)) {
        m_error = "Fragmented or oversized control frame";
        return std::nullopt;
    }
    if (opcode != 0 && opcode != 1 && opcode != 2 && opcode != 8 && opcode != 9 && opcode != 10) {
        m_error = "Unknown opcode";
        return std::nullopt;
    }
    if (length > m_max) {
        m_error = "Frame larger than the message limit";
        return std::nullopt;
    }
    if (available < header + length) {
        return std::nullopt;
    }
    Frame f;
    f.fin = fin;
    f.opcode = static_cast<Opcode>(opcode);
    f.payload.assign(m_buffer.data() + m_offset + header, static_cast<size_t>(length));
    m_offset += header + static_cast<size_t>(length);
    return f;
}

std::optional<Assembler::Message> Assembler::add(Frame&& frame) {
    if (frame.opcode == Opcode::Continuation) {
        if (!m_open) {
            m_error = "Continuation frame without a message";
            return std::nullopt;
        }
        m_data += frame.payload;
    } else {
        if (m_open) {
            m_error = "New message before the previous one finished";
            return std::nullopt;
        }
        m_open = true;
        m_text = frame.opcode == Opcode::Text;
        m_data = std::move(frame.payload);
    }
    if (!frame.fin) {
        return std::nullopt;
    }
    m_open = false;
    Message m{m_text, std::move(m_data)};
    m_data.clear();
    return m;
}

}  // namespace aleks2::ws
