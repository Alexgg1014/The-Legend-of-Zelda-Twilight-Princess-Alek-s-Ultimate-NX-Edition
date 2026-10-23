// DM3: the Switch WebSocket transport off the console. ws_rfc6455.cpp unit checks (RFC 6455's own handshake
// example, SHA-1/base64 vectors, URL parsing, frame round trips and refusals) and ws_transport_switch.cpp built
// for the host (plain ws:// over BSD sockets; TLS is the console's) against dm3_ws_server.py: open, echo of text
// and of binary at each length encoding, a fragmented message with a ping inside, client close, server close, a
// refused upgrade, a wrong Sec-WebSocket-Accept, a connection refused, abort while connected.
//
//   ws_transport_test PORT       (dm3_ws_server.py PORT already listening on 127.0.0.1)
#include "ws_internal.hpp"
#include "ws_rfc6455.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

using namespace borealis::ws;
using namespace borealis::ws::detail;
namespace rfc = aleks2::ws;

namespace {
int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

std::string hex(const std::string& s) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out += d[c >> 4];
        out += d[c & 15];
    }
    return out;
}

void protocol_checks() {
    std::printf("== RFC 6455 pieces\n");
    CHECK(rfc::accept_for("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");  // RFC 6455 section 1.3
    CHECK(hex(rfc::sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex(rfc::sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hex(rfc::sha1(std::string(1000, 'a'))) == "291e9a6c66994949b57ba5e650361e98fc36b1ba");
    const uint8_t b[] = {'f', 'o', 'o', 'b', 'a', 'r'};
    CHECK(rfc::base64(b, 6) == "Zm9vYmFy" && rfc::base64(b, 5) == "Zm9vYmE=" && rfc::base64(b, 4) == "Zm9vYg==");
    auto u = rfc::parse_url("wss://rooms.example.workers.dev/lobby?x=1#frag");
    CHECK(u && u->secure && u->port == 443 && u->host == "rooms.example.workers.dev" && u->resource == "/lobby?x=1");
    u = rfc::parse_url("ws://192.168.1.5:8080");
    CHECK(u && !u->secure && u->port == 8080 && u->resource == "/");
    u = rfc::parse_url("ws://[::1]:9/a");
    CHECK(u && u->host == "::1" && u->port == 9 && u->resource == "/a");
    CHECK(!rfc::parse_url("http://x/") && !rfc::parse_url("ws://:80/") && !rfc::parse_url("ws://h:99999/"));
    const std::string req = rfc::handshake_request(*rfc::parse_url("ws://h:81/p"), "KEY", {"a", "b"}, {{"X-T", "1"}});
    CHECK(req.find("GET /p HTTP/1.1\r\nHost: h:81\r\n") == 0 && req.find("Sec-WebSocket-Protocol: a, b\r\n") != std::string::npos &&
          req.find("X-T: 1\r\n") != std::string::npos && req.size() > 4 && req.substr(req.size() - 4) == "\r\n\r\n");
    const auto ok = rfc::check_response("HTTP/1.1 101 Switching Protocols\r\nUpgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\n"
                                        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n",
        "dGhlIHNhbXBsZSBub25jZQ==");
    CHECK(ok.status == 101 && ok.error.empty());
    CHECK(!rfc::check_response("HTTP/1.1 101 x\r\nUpgrade: websocket\r\nConnection: upgrade\r\nSec-WebSocket-Accept: nope\r\n\r\n", "k").error.empty());
    CHECK(rfc::check_response("HTTP/1.1 403 Forbidden\r\n\r\n", "k").status == 403);
    // Frames: every length encoding round-trips through a server-style (unmasked) copy.
    const uint8_t mask[4] = {1, 2, 3, 4};
    for (size_t n : {size_t{0}, size_t{125}, size_t{126}, size_t{65535}, size_t{65536}, size_t{200000}}) {
        std::string payload(n, 'q');
        std::string masked = rfc::encode_frame(rfc::Opcode::Binary, payload, mask);
        // Unmask into a server frame: clear the mask bit and drop the key.
        const size_t header = n < 126 ? 2 : n <= 0xffff ? 4 : 10;
        std::string server = masked.substr(0, header);
        server[1] = static_cast<char>(server[1] & 0x7f);
        for (size_t i = 0; i < n; i++) server += static_cast<char>(masked[header + 4 + i] ^ mask[i & 3]);
        rfc::Decoder d{1 << 20};
        d.feed(server.data(), server.size() / 2);  // in two pieces
        CHECK(!d.next());
        d.feed(server.data() + server.size() / 2, server.size() - server.size() / 2);
        auto f = d.next();
        CHECK(f && f->fin && f->opcode == rfc::Opcode::Binary && f->payload == payload);
    }
    rfc::Decoder masked{1024};
    const std::string clientFrame = rfc::encode_frame(rfc::Opcode::Text, "x", mask);
    masked.feed(clientFrame.data(), clientFrame.size());
    CHECK(!masked.next() && !masked.error().empty());  // a server must not mask
    rfc::Decoder control{1024};
    const char bigPing[] = {static_cast<char>(0x89), 126, 0, static_cast<char>(200)};
    control.feed(bigPing, 4);
    CHECK(!control.next() && !control.error().empty());
    rfc::Decoder limit{10};
    const char tooBig[] = {static_cast<char>(0x82), 20};
    limit.feed(tooBig, 2);
    CHECK(!limit.next() && limit.error().find("message limit") != std::string::npos);
    rfc::Assembler as;
    CHECK(!as.add({false, rfc::Opcode::Text, "ab"}) && as.error().empty());
    auto m = as.add({true, rfc::Opcode::Continuation, "cd"});
    CHECK(m && m->text && m->data == "abcd");
    rfc::Assembler bad;
    CHECK(!bad.add({true, rfc::Opcode::Continuation, "x"}) && !bad.error().empty());
    std::printf("  accept key, SHA-1, base64, URLs, request/response, frames at 7/16/64-bit lengths, refusals\n");
}

struct Sink final : EventSink {
    std::mutex mutex;
    std::condition_variable cv;
    bool open = false, isClosed = false;
    std::string protocol;
    std::deque<std::pair<MessageKind, std::string>> messages;
    Error error = Error::None;
    std::string errorMessage;
    int status = 0;
    uint16_t code = 0;
    std::string reason;
    size_t sent = 0;

    void opened(std::string p, std::vector<borealis::http::Header>) override {
        std::lock_guard l{mutex};
        open = true;
        protocol = std::move(p);
        cv.notify_all();
    }
    void message_(MessageKind k, std::string d) {
        std::lock_guard l{mutex};
        messages.emplace_back(k, std::move(d));
        cv.notify_all();
    }
    void message(MessageKind k, std::string d) override { message_(k, std::move(d)); }
    void send_complete(size_t bytes, Error, std::string) override {
        std::lock_guard l{mutex};
        sent += bytes;
    }
    void closed(Error e, std::string m, int s, uint16_t c, std::string r, std::vector<borealis::http::Header>) override {
        std::lock_guard l{mutex};
        isClosed = true;
        error = e;
        errorMessage = std::move(m);
        status = s;
        code = c;
        reason = std::move(r);
        cv.notify_all();
    }
    template <class P>
    bool wait(P pred, int ms = 5000) {
        std::unique_lock l{mutex};
        return cv.wait_for(l, std::chrono::milliseconds(ms), pred);
    }
};

Options opts(int port, const char* path) {
    Options o;
    o.url = "ws://127.0.0.1:" + std::to_string(port) + path;
    o.allowPlaintext = true;
    o.connectTimeout = std::chrono::milliseconds(3000);
    o.maxMessageBytes = 1 << 20;
    o.protocols = {"dm3"};
    return o;
}

void transport_checks(int port) {
    std::printf("== transport against dm3_ws_server.py on port %d\n", port);
    CHECK(backend_available());
    {
        auto sink = std::make_shared<Sink>();
        auto t = make_transport();
        t->start(opts(port, "/echo"), sink);
        CHECK(sink->wait([&] { return sink->open || sink->isClosed; }) && sink->open && sink->protocol == "dm3");
        CHECK(t->send(MessageKind::Text, "hola"));
        CHECK(sink->wait([&] { return !sink->messages.empty(); }));
        if (!sink->messages.empty()) {
            CHECK(sink->messages.front().first == MessageKind::Text && sink->messages.front().second == "hola");
            sink->messages.pop_front();
        }
        for (size_t n : {size_t{125}, size_t{300}, size_t{70000}}) {
            std::string data(n, '\0');
            for (size_t i = 0; i < n; i++) data[i] = static_cast<char>(i * 7);
            CHECK(t->send(MessageKind::Binary, data));
            CHECK(sink->wait([&] { return !sink->messages.empty(); }));
            if (!sink->messages.empty()) {
                CHECK(sink->messages.front().first == MessageKind::Binary && sink->messages.front().second == data);
                sink->messages.pop_front();
            }
        }
        CHECK(t->send(MessageKind::Text, "frag"));
        CHECK(sink->wait([&] { return !sink->messages.empty(); }));
        if (!sink->messages.empty()) {
            CHECK(sink->messages.front().second == "frag-ment");  // two fragments with a ping between them
            sink->messages.pop_front();
        }
        t->close(1000, "done");
        CHECK(sink->wait([&] { return sink->isClosed; }) && sink->error == Error::None && sink->code == 1000);
        CHECK(sink->sent == 4 + 125 + 300 + 70000 + 4);
        t->abort();
        std::printf("  open with subprotocol, text and binary echo (7/16/64-bit lengths), fragmented message + ping, "
                    "client close 1000\n");
    }
    {
        auto sink = std::make_shared<Sink>();
        auto t = make_transport();
        t->start(opts(port, "/echo"), sink);
        CHECK(sink->wait([&] { return sink->open; }));
        t->send(MessageKind::Text, "close-me");
        CHECK(sink->wait([&] { return sink->isClosed; }) && sink->error == Error::None && sink->code == 4001 && sink->reason == "bye");
        t->abort();
        std::printf("  server-initiated close 4001 \"bye\"\n");
    }
    {
        auto sink = std::make_shared<Sink>();
        auto t = make_transport();
        t->start(opts(port, "/deny"), sink);
        CHECK(sink->wait([&] { return sink->isClosed; }) && sink->error == Error::Handshake && sink->status == 403);
        t->abort();
        auto sink2 = std::make_shared<Sink>();
        auto t2 = make_transport();
        t2->start(opts(port, "/badkey"), sink2);
        CHECK(sink2->wait([&] { return sink2->isClosed; }) && sink2->error == Error::Handshake && !sink2->open);
        t2->abort();
        std::printf("  refused upgrade (403), wrong Sec-WebSocket-Accept\n");
    }
    {
        auto sink = std::make_shared<Sink>();
        auto t = make_transport();
        t->start(opts(1, "/"), sink);  // nothing listens on port 1
        CHECK(sink->wait([&] { return sink->isClosed; }) && sink->error == Error::Network);
        t->abort();
        auto sink2 = std::make_shared<Sink>();
        auto t2 = make_transport();
        t2->start(opts(port, "/echo"), sink2);
        CHECK(sink2->wait([&] { return sink2->open; }));
        const auto start = std::chrono::steady_clock::now();
        t2->abort();  // must return promptly, with the worker joined
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
        std::printf("  connection refused, abort while open\n");
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    protocol_checks();
    if (argc >= 2) {
        transport_checks(std::atoi(argv[1]));
    }
    if (g_failures) {
        std::fprintf(stderr, "ws_transport_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ws_transport_test: all checks passed\n");
    return 0;
}
