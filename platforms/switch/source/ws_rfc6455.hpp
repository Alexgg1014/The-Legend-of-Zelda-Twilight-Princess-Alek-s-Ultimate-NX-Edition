#pragma once
// aleks2 (DM3): the WebSocket protocol (RFC 6455) as a client speaks it, with no I/O: the opening handshake
// (request, response check, Sec-WebSocket-Accept), frame encoding with the client mask, and an incremental
// frame decoder. ws_transport_switch.cpp moves the bytes (BSD socket, Horizon's ssl service for wss://);
// platforms/switch/tools/test-ws-rfc6455.sh runs this against a real server on the host.
//
// Why not wslay (devkitPro portlib) or curl >= 7.86: wslay is not in the installed portlibs and would only do
// the framing, which is the small part; curl's WebSocket needs a newer libcurl than devkitPro ships (7.69.1).
// TLS is the console's own ssl service (the one libcurl already uses for https), so the system CA store
// applies and no TLS library or certificate bundle ships with the NRO.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aleks2::ws {

// "ws"/"wss", host (brackets removed), port, resource ("/path?query").
struct Url {
    bool secure = false;
    std::string host;
    uint16_t port = 0;
    std::string resource;
};
std::optional<Url> parse_url(std::string_view url);

std::string base64(const uint8_t* data, size_t size);
std::string sha1(std::string_view data);  // 20 raw bytes
// base64(sha1(key + GUID))
std::string accept_for(std::string_view key);

// The GET request. key = base64 of 16 random bytes.
std::string handshake_request(const Url& url, std::string_view key, const std::vector<std::string>& protocols,
    const std::vector<std::pair<std::string, std::string>>& headers);

struct HandshakeResponse {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string protocol;  // Sec-WebSocket-Protocol, if any
    std::string error;     // why it is not a valid upgrade (empty = valid)
};
// Parses the response head (everything up to and including the blank line) and checks it against the key.
HandshakeResponse check_response(std::string_view head, std::string_view key);

enum class Opcode : uint8_t { Continuation = 0, Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10 };

// One frame from the client: FIN set, masked with `mask`.
std::string encode_frame(Opcode opcode, std::string_view payload, const uint8_t mask[4], bool fin = true);

struct Frame {
    bool fin = true;
    Opcode opcode = Opcode::Binary;
    std::string payload;
};

// Feed bytes as they arrive; take whole frames. A server frame must not be masked; a control frame must be
// FIN and at most 125 bytes; reserved bits must be clear (no extension is negotiated).
class Decoder {
public:
    explicit Decoder(size_t maxFrameBytes) : m_max{maxFrameBytes} {}
    void feed(const void* data, size_t size);
    // A frame if one is complete; nullopt otherwise. After error() is set, always nullopt.
    std::optional<Frame> next();
    const std::string& error() const { return m_error; }

private:
    std::string m_buffer;
    size_t m_offset = 0;
    size_t m_max;
    std::string m_error;
};

// Message reassembly over frames (text/binary + continuations). Returns a whole message when FIN arrives.
class Assembler {
public:
    struct Message {
        bool text = false;
        std::string data;
    };
    // nullopt = need more; sets error() on a protocol violation.
    std::optional<Message> add(Frame&& frame);
    const std::string& error() const { return m_error; }
    size_t pending() const { return m_data.size(); }

private:
    bool m_open = false;
    bool m_text = false;
    std::string m_data;
    std::string m_error;
};

}  // namespace aleks2::ws
