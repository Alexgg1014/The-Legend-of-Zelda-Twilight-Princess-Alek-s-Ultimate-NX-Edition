// aleks2 (DM3): borealis::ws on Horizon -- the transport under borealis' own WebSocket layer.
//
// borealis keeps the connection state, queues, deadlines and UTF-8 checks in extern/borealis/src/ws.cpp and
// asks a backend Transport (ws_internal.hpp) to move messages. Its backends are libcurl >= 7.86, WinHTTP,
// NSURLSession and Android; devkitPro's libcurl is 7.69.1 with no WebSocket. This is the Horizon backend:
//   - TCP: a BSD socket, non-blocking connect bounded by Options::connectTimeout.
//   - TLS (wss://): the console's ssl service, as devkitPro's libcurl uses it for https: the system CA store,
//     SNI and host-name verification by the service (default verify options), TLS 1.0-1.3 as the firmware
//     offers. The socket stays ours (SslOptionType_DoNotCloseSocket), so abort can shut it down to unblock a
//     read or a handshake in progress.
//   - The protocol (handshake, framing, mask, fragmentation, ping/pong, close): ws_rfc6455.cpp.
//   - Sleep: sockets do not survive it; on resume every connection closes with Error::Network.
// One worker thread per connection, which waits for input in short slices (20 ms) so queued sends and a
// close request are picked up without a wakeup descriptor.
//
// Off Switch (the host tests) the same file builds without TLS: ws:// only.
#include "ws_internal.hpp"
#include "ws_rfc6455.hpp"

#include "dusk/gai_text.hpp"


#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>

#include <fmt/format.h>

#include "dusk/switch_net.h"
#endif

// The game log on Switch; nothing in the host tests (which build without borealis' logger).
#ifdef __SWITCH__
#include "borealis/log.hpp"
namespace {
constexpr borealis::Log Log{"aleks2::ws"};
}
#define WS_LOG_INFO(...) ::Log.info(__VA_ARGS__)
#define WS_LOG_WARN(...) ::Log.warn(__VA_ARGS__)
#define WS_LOG_ERROR(...) ::Log.error(__VA_ARGS__)
#else
#define WS_LOG_INFO(...) ((void)0)
#define WS_LOG_WARN(...) ((void)0)
#define WS_LOG_ERROR(...) ((void)0)
#endif

namespace borealis::ws::detail {
namespace {

using Clock = std::chrono::steady_clock;
namespace rfc = aleks2::ws;

void random_bytes(void* out, size_t size) {
#ifdef __SWITCH__
    csrngGetRandomBytes(out, size);
#else
    static std::mutex mutex;
    static std::random_device device;
    std::lock_guard lock{mutex};
    auto* p = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < size; i++) {
        p[i] = static_cast<uint8_t>(device());
    }
#endif
}

// A byte stream over a connected socket: plain, or TLS on Switch.
class Stream {
public:
    virtual ~Stream() = default;
    // True when a read would return data (or end of stream / an error) without blocking long.
    virtual bool wait_readable(int milliseconds) = 0;
    // > 0 bytes; 0 end of stream; -1 error (message set); -2 nothing yet.
    virtual long read(void* buffer, size_t size, std::string& message) = 0;
    virtual bool write_all(const void* data, size_t size, std::string& message, const std::atomic<bool>& stop) = 0;
};

class PlainStream final : public Stream {
public:
    explicit PlainStream(int fd) : m_fd{fd} {}

    bool wait_readable(int milliseconds) override {
        pollfd p{m_fd, POLLIN, 0};
        return ::poll(&p, 1, milliseconds) > 0;
    }

    long read(void* buffer, size_t size, std::string& message) override {
        const ssize_t n = ::recv(m_fd, buffer, size, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return -2;
            }
            message = std::strerror(errno);
            return -1;
        }
        return static_cast<long>(n);
    }

    bool write_all(const void* data, size_t size, std::string& message, const std::atomic<bool>& stop) override {
        const auto* p = static_cast<const char*>(data);
        while (size > 0) {
            if (stop.load()) {
                message = "Canceled";
                return false;
            }
            const ssize_t n = ::send(m_fd, p, size, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    pollfd w{m_fd, POLLOUT, 0};
                    ::poll(&w, 1, 100);
                    continue;
                }
                message = std::strerror(errno);
                return false;
            }
            p += n;
            size -= static_cast<size_t>(n);
        }
        return true;
    }

private:
    int m_fd;
};

#ifdef __SWITCH__
// The ssl service: one context for the process, one connection per WebSocket.
SslContext* ssl_context(std::string& message) {
    static std::once_flag once;
    static SslContext context{};
    static Result result = 0;
    std::call_once(once, [] {
        result = sslInitialize(3);  // reference-counted in libnx; libcurl initializes the same service
        if (R_SUCCEEDED(result)) {
            result = sslCreateContext(&context, SslVersion_Auto);
        }
    });
    if (R_FAILED(result)) {
        message = "TLS service unavailable (0x" + fmt::format("{:x}", result) + ")";
        return nullptr;
    }
    return &context;
}

class TlsStream final : public Stream {
public:
    ~TlsStream() override {
        if (m_open) {
            sslConnectionClose(&m_connection);
        }
    }

    // fd stays owned by the caller (DoNotCloseSocket).
    bool start(int fd, const std::string& host, std::string& message) {
        SslContext* context = ssl_context(message);
        if (context == nullptr) {
            return false;
        }
        Result r = sslContextCreateConnection(context, &m_connection);
        if (R_FAILED(r)) {
            message = fmt::format("TLS connection could not be created (0x{:x})", r);
            return false;
        }
        m_open = true;
        // The ssl service runs the socket in its own blocking I/O: hand it a blocking descriptor.
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        }
        r = sslConnectionSetOption(&m_connection, SslOptionType_DoNotCloseSocket, true);
        if (R_SUCCEEDED(r) && socketSslConnectionSetSocketDescriptor(&m_connection, fd) < 0 && errno != ENOENT) {
            message = std::string{"TLS could not take the socket: "} + std::strerror(errno);
            return false;
        }
        if (R_SUCCEEDED(r)) {
            r = sslConnectionSetHostName(&m_connection, host.c_str(), static_cast<u32>(host.size() + 1));
        }
        if (R_SUCCEEDED(r)) {
            r = sslConnectionSetIoMode(&m_connection, SslIoMode_Blocking);
        }
        if (R_FAILED(r)) {
            message = fmt::format("TLS setup failed (0x{:x})", r);
            return false;
        }
        u32 certSize = 0, certCount = 0;
        r = sslConnectionDoHandshake(&m_connection, &certSize, &certCount, nullptr, 0);
        if (R_FAILED(r)) {
            const Result verify = sslConnectionGetVerifyCertError(&m_connection);
            message = fmt::format("TLS handshake with {} failed (0x{:x}, certificate check 0x{:x})", host, r, verify);
            return false;
        }
        m_fd = fd;
        return true;
    }

    bool wait_readable(int milliseconds) override {
        s32 pending = 0;
        if (R_SUCCEEDED(sslConnectionPending(&m_connection, &pending)) && pending > 0) {
            return true;
        }
        if (m_servicePoll) {
            u32 events = 0;
            const Result r = sslConnectionPoll(&m_connection, SslPollEvent_Read, &events,
                static_cast<u32>(milliseconds));
            if (R_SUCCEEDED(r)) {
                return (events & (SslPollEvent_Read | SslPollEvent_Except)) != 0;
            }
            // Not every firmware answers Poll the same way: fall back to the socket itself (TLS records
            // arriving make it readable; a read then blocks until the record is whole).
            WS_LOG_WARN("ssl Poll failed (0x{:x}); waiting on the socket instead", r);
            m_servicePoll = false;
        }
        pollfd p{m_fd, POLLIN, 0};
        return ::poll(&p, 1, milliseconds) > 0;
    }

    long read(void* buffer, size_t size, std::string& message) override {
        u32 got = 0;
        const Result r = sslConnectionRead(&m_connection, buffer, static_cast<u32>(std::min<size_t>(size, 0x10000)), &got);
        if (R_FAILED(r)) {
            message = fmt::format("TLS read failed (0x{:x})", r);
            return -1;
        }
        return static_cast<long>(got);  // 0: the peer closed the TLS session
    }

    bool write_all(const void* data, size_t size, std::string& message, const std::atomic<bool>& stop) override {
        const auto* p = static_cast<const uint8_t*>(data);
        while (size > 0) {
            if (stop.load()) {
                message = "Canceled";
                return false;
            }
            u32 sent = 0;
            const Result r = sslConnectionWrite(&m_connection, p, static_cast<u32>(std::min<size_t>(size, 0x10000)), &sent);
            if (R_FAILED(r) || sent == 0) {
                message = fmt::format("TLS write failed (0x{:x})", r);
                return false;
            }
            p += sent;
            size -= sent;
        }
        return true;
    }

private:
    SslConnection m_connection{};
    bool m_open = false;
    bool m_servicePoll = true;
    int m_fd = -1;
};
#endif

class SwitchTransport;

// Every live transport, for resume from sleep.
struct Live {
    std::mutex mutex;
    std::unordered_set<SwitchTransport*> transports;
    bool listening = false;
};

Live& live() {
    static Live* s = new Live;
    return *s;
}

class SwitchTransport final : public Transport {
public:
    SwitchTransport() {
        auto& l = live();
        std::lock_guard lock{l.mutex};
        l.transports.insert(this);
#ifdef __SWITCH__
        if (!l.listening) {
            l.listening = true;
            dusk::switchnet::add_resume_listener([] {
                auto& lv = live();
                std::lock_guard lk{lv.mutex};
                for (auto* t : lv.transports) {
                    t->interrupt("The console resumed from sleep; the connection was lost");
                }
            });
        }
#endif
    }

    ~SwitchTransport() override {
        abort();
        auto& l = live();
        std::lock_guard lock{l.mutex};
        l.transports.erase(this);
    }

    void start(const Options& value, std::shared_ptr<EventSink> eventSink) override {
        options = value;
        sink = std::move(eventSink);
        try {
            worker = std::thread{[this] { run_guarded(); }};
        } catch (const std::exception& error) {
            sink->closed(Error::Network, error.what(), 0, 0, {});
        }
    }

    bool send(MessageKind kind, std::string data) override {
        std::lock_guard lock{mutex};
        if (stopping || closeRequested) {
            return false;
        }
        outgoing.push_back({kind, std::move(data)});
        return true;
    }

    void close(uint16_t code, std::string reason) override {
        std::lock_guard lock{mutex};
        if (stopping || closeRequested) {
            return;
        }
        closeRequested = true;
        closeCode = code;
        closeReason = std::move(reason);
    }

    void abort() noexcept override {
        interrupt({});
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
            worker.join();
        }
        close_socket();
    }

    // From another thread: stop the worker, unblocking any read, write or handshake in progress.
    void interrupt(std::string why) noexcept {
        std::lock_guard lock{mutex};
        if (!why.empty() && failure.empty()) {
            failure = std::move(why);
        }
        stopping = true;
        stopFlag = true;
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
        }
    }

private:
    struct Outgoing {
        MessageKind kind;
        std::string data;
    };

    bool should_stop() {
        std::lock_guard lock{mutex};
        return stopping;
    }

    void fail(Error error, std::string message) {
        std::string why;
        {
            std::lock_guard lock{mutex};
            why = failure;
        }
        if (!why.empty()) {
            sink->closed(Error::Network, why, 0, 0, {});  // resume from sleep
            return;
        }
        if (!should_stop()) {
            sink->closed(error, std::move(message), 0, 0, {});
        }
    }

    void close_socket() noexcept {
        std::lock_guard lock{mutex};
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    void run_guarded() noexcept try { run(); } catch (const std::exception& exception) {
        WS_LOG_ERROR("worker: {}", exception.what());
        fail(Error::Network, exception.what());
    } catch (...) {
        fail(Error::Network, "WebSocket worker failed");
    }

    bool connect_tcp(const rfc::Url& url, std::string& message, Error& error) {
#ifdef __SWITCH__
        if (dusk::switchnet::link() == dusk::switchnet::Link::Offline) {
            message = "No network connection (the console reports none)";
            error = Error::Network;
            return false;
        }
#endif
        addrinfo hints{};
        hints.ai_family = AF_INET;  // Horizon's resolver is IPv4
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* list = nullptr;
        const std::string port = std::to_string(url.port);
        const int r = getaddrinfo(url.host.c_str(), port.c_str(), &hints, &list);
        if (r != 0 || list == nullptr) {
            message = "Could not resolve " + url.host + ": " + dusk::gai_text(r);
            error = Error::Network;
            return false;
        }
        const auto deadline = Clock::now() + options.connectTimeout;
        for (addrinfo* a = list; a != nullptr; a = a->ai_next) {
            const int s = ::socket(a->ai_family, SOCK_STREAM, IPPROTO_TCP);
            if (s < 0) {
                message = std::strerror(errno);
                continue;
            }
            {
                std::lock_guard lock{mutex};
                fd = s;
                if (stopping) {
                    break;
                }
            }
            const int flags = fcntl(s, F_GETFL, 0);
            fcntl(s, F_SETFL, flags | O_NONBLOCK);
            const int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            if (::connect(s, a->ai_addr, a->ai_addrlen) == 0) {
                freeaddrinfo(list);
                return true;
            }
            if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN) {
                while (Clock::now() < deadline && !should_stop()) {
                    pollfd p{s, POLLOUT, 0};
                    if (::poll(&p, 1, 100) > 0) {
                        int soError = 0;
                        socklen_t len = sizeof(soError);
                        getsockopt(s, SOL_SOCKET, SO_ERROR, &soError, &len);
                        if (soError == 0) {
                            freeaddrinfo(list);
                            return true;
                        }
                        errno = soError;
                        break;
                    }
                }
                if (Clock::now() >= deadline) {
                    errno = ETIMEDOUT;
                }
            }
            message = "Could not connect to " + url.host + ": " + std::strerror(errno);
            close_socket();
        }
        freeaddrinfo(list);
        error = Error::Network;
        return false;
    }

    void run() {
        const auto url = rfc::parse_url(options.url);
        if (!url) {
            fail(Error::InvalidUrl, "Invalid WebSocket URL");
            return;
        }
        Error error = Error::Network;
        std::string message;
        if (!connect_tcp(*url, message, error)) {
            fail(error, message);
            return;
        }
        int socketFd;
        {
            std::lock_guard lock{mutex};
            socketFd = fd;
        }
        std::unique_ptr<Stream> stream;
        if (url->secure) {
#ifdef __SWITCH__
            auto tls = std::make_unique<TlsStream>();
            if (!tls->start(socketFd, url->host, message)) {
                fail(Error::Network, message);
                return;
            }
            stream = std::move(tls);
#else
            fail(Error::UnsupportedScheme, "wss:// needs the console's TLS service; this build has none");
            return;
#endif
        } else {
            stream = std::make_unique<PlainStream>(socketFd);
        }

        // Opening handshake.
        uint8_t keyBytes[16];
        random_bytes(keyBytes, sizeof keyBytes);
        const std::string key = rfc::base64(keyBytes, sizeof keyBytes);
        std::vector<std::pair<std::string, std::string>> headers;
        for (const auto& h : options.headers) {
            headers.emplace_back(h.name, h.value);
        }
        const std::string request = rfc::handshake_request(*url, key, options.protocols, headers);
        if (!stream->write_all(request.data(), request.size(), message, stopFlag)) {
            fail(Error::Network, "WebSocket handshake could not be sent: " + message);
            return;
        }
        std::string head;
        std::string leftover;
        const auto deadline = Clock::now() + options.connectTimeout;
        char buffer[16 * 1024];
        while (true) {
            if (should_stop()) {
                fail(Error::Canceled, "Connection canceled");
                return;
            }
            if (Clock::now() >= deadline) {
                fail(Error::Timeout, "WebSocket handshake timed out");
                return;
            }
            if (!stream->wait_readable(50)) {
                continue;
            }
            const long n = stream->read(buffer, sizeof buffer, message);
            if (n == -2) {
                continue;
            }
            if (n <= 0) {
                fail(Error::Network, n == 0 ? "Connection closed during the WebSocket handshake" : message);
                return;
            }
            head.append(buffer, static_cast<size_t>(n));
            const size_t end = head.find("\r\n\r\n");
            if (end != std::string::npos) {
                leftover = head.substr(end + 4);
                head.resize(end + 4);
                break;
            }
            if (head.size() > 64 * 1024) {
                fail(Error::Handshake, "WebSocket handshake response too large");
                return;
            }
        }
        auto response = rfc::check_response(head, key);
        std::vector<http::Header> responseHeaders;
        for (auto& [name, value] : response.headers) {
            responseHeaders.push_back({std::move(name), std::move(value)});
        }
        if (!response.error.empty()) {
            if (!should_stop()) {
                sink->closed(Error::Handshake, response.error, response.status, 0, {}, std::move(responseHeaders));
            }
            return;
        }
        WS_LOG_INFO("connected to {}:{}{} ({})", url->host, url->port, url->resource.substr(0, url->resource.find('?')),
            url->secure ? "TLS by the console" : "plain");
        sink->opened(response.protocol, std::move(responseHeaders));

        rfc::Decoder decoder{options.maxMessageBytes + 1};
        if (!leftover.empty()) {
            decoder.feed(leftover.data(), leftover.size());
        }
        io_loop(*stream, decoder);
    }

    bool send_frame(Stream& stream, rfc::Opcode op, std::string_view payload) {
        uint8_t mask[4];
        random_bytes(mask, sizeof mask);
        const std::string frame = rfc::encode_frame(op, payload, mask);
        std::string message;
        if (!stream.write_all(frame.data(), frame.size(), message, stopFlag)) {
            fail(Error::Network, message);
            return false;
        }
        return true;
    }

    void io_loop(Stream& stream, rfc::Decoder& decoder) {
        rfc::Assembler assembler;
        auto nextPing = options.keepaliveInterval.count() != 0 ? Clock::now() + options.keepaliveInterval
                                                              : Clock::time_point::max();
        bool closeSent = false;
        bool peerClosed = false;
        uint16_t peerCode = 0;
        std::string peerReason;
        char buffer[16 * 1024];
        while (true) {
            if (should_stop()) {
                fail(Error::Canceled, "Connection canceled");
                return;
            }
            // Whole frames already buffered.
            while (auto frame = decoder.next()) {
                switch (frame->opcode) {
                case rfc::Opcode::Ping:
                    if (!closeSent && !send_frame(stream, rfc::Opcode::Pong, frame->payload)) {
                        return;
                    }
                    break;
                case rfc::Opcode::Pong:
                    break;
                case rfc::Opcode::Close: {
                    if (frame->payload.size() >= 2) {
                        peerCode = static_cast<uint16_t>((static_cast<uint8_t>(frame->payload[0]) << 8) |
                                                         static_cast<uint8_t>(frame->payload[1]));
                        peerReason = frame->payload.substr(2);
                    }
                    if (closeSent) {
                        sink->closed(Error::None, {}, 0, peerCode, std::move(peerReason));
                        return;
                    }
                    // Echo it, as RFC 6455 5.5.1 asks, then report.
                    peerClosed = true;
                    send_frame(stream, rfc::Opcode::Close, frame->payload.substr(0, 2));
                    sink->closed(Error::None, {}, 0, peerCode, std::move(peerReason));
                    return;
                }
                default: {
                    auto m = assembler.add(std::move(*frame));
                    if (!assembler.error().empty()) {
                        send_frame(stream, rfc::Opcode::Close, std::string{"\x03\xea", 2});
                        fail(Error::Protocol, assembler.error());
                        return;
                    }
                    if (m) {
                        sink->message(m->text ? MessageKind::Text : MessageKind::Binary, std::move(m->data));
                    } else if (assembler.pending() > options.maxMessageBytes) {
                        send_frame(stream, rfc::Opcode::Close, std::string{"\x03\xf1", 2});  // 1009
                        fail(Error::TooLarge, "Message limit exceeded");
                        return;
                    }
                    break;
                }
                }
            }
            if (!decoder.error().empty()) {
                const bool tooLarge = decoder.error().find("message limit") != std::string::npos;
                send_frame(stream, rfc::Opcode::Close, std::string{tooLarge ? "\x03\xf1" : "\x03\xea", 2});
                fail(tooLarge ? Error::TooLarge : Error::Protocol, tooLarge ? "Message limit exceeded" : decoder.error());
                return;
            }
            // Outgoing messages, then a requested close.
            for (;;) {
                Outgoing next;
                {
                    std::lock_guard lock{mutex};
                    if (outgoing.empty()) {
                        break;
                    }
                    next = std::move(outgoing.front());
                    outgoing.pop_front();
                }
                const size_t bytes = next.data.size();
                if (!send_frame(stream, next.kind == MessageKind::Text ? rfc::Opcode::Text : rfc::Opcode::Binary, next.data)) {
                    return;
                }
                sink->send_complete(bytes, Error::None, {});
            }
            bool wantClose = false;
            uint16_t code = 1000;
            std::string reason;
            {
                std::lock_guard lock{mutex};
                wantClose = closeRequested && !closeSent;
                code = closeCode;
                reason = closeReason;
            }
            if (wantClose && !peerClosed) {
                std::string payload;
                payload += static_cast<char>(code >> 8);
                payload += static_cast<char>(code & 0xff);
                payload += reason;
                if (!send_frame(stream, rfc::Opcode::Close, payload)) {
                    return;
                }
                closeSent = true;
            }
            if (Clock::now() >= nextPing && !closeSent) {
                if (!send_frame(stream, rfc::Opcode::Ping, {})) {
                    return;
                }
                nextPing = Clock::now() + options.keepaliveInterval;
            }
            if (!stream.wait_readable(20)) {
                continue;
            }
            std::string message;
            const long n = stream.read(buffer, sizeof buffer, message);
            if (n == -2) {
                continue;
            }
            if (n == 0) {
                fail(Error::Network, "WebSocket peer closed without a close frame");
                return;
            }
            if (n < 0) {
                fail(Error::Network, message);
                return;
            }
            decoder.feed(buffer, static_cast<size_t>(n));
        }
    }

    Options options;
    std::shared_ptr<EventSink> sink;
    std::mutex mutex;
    std::deque<Outgoing> outgoing;
    std::thread worker;
    std::string closeReason;
    std::string failure;
    uint16_t closeCode = 1000;
    int fd = -1;
    bool stopping = false;
    bool closeRequested = false;
    std::atomic<bool> stopFlag{false};
};

}  // namespace

std::unique_ptr<Transport> make_transport() {
    return std::make_unique<SwitchTransport>();
}

bool backend_available() noexcept {
#ifdef __SWITCH__
    return dusk::switchnet::available();
#else
    return true;
#endif
}

}  // namespace borealis::ws::detail
