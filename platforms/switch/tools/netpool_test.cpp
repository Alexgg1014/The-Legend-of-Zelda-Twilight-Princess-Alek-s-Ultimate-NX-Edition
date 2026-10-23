// aleks2 (V1-rc5): host test of the socket counters (source/switch_sockets.cpp, linked with the NRO's
// -Wl,--wrap=socket,connect,close) and of the mod socket buffer sizes (src/dusk/mods/net_buffers.hpp).
//
// 1. 200 HTTP requests in a row (socket, connect, request, response, close) to a local server while a UDP
//    socket stays open, as the browser and the update check run while the co-op holds its room socket: every
//    request succeeds, the counters return to "1 open" (the UDP socket), and /proc/self/fd to its baseline.
//    A file's close() is not counted.
// 2. The console's buffer pool (switch_net.cpp's config, sb_efficiency x (TCP max + UDP)), with libcurl's TCP
//    sockets at the TCP maximum: with rc4's 1 MiB + 1 MiB per mod UDP socket there is no room left for one more
//    TCP socket; with rc5's sizes there is room for the browser's two images, the catalog, the update check, a
//    download and the co-op's WebSocket at once.
// Linux does not reproduce Horizon's ENOBUFS (it caps SO_RCVBUF silently), so part 2 is the arithmetic, not a
// console run; the heartbeat's "net:" line is the console's check.
#include "dusk/mods/net_buffers.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

extern "C" {
struct aleks2_socket_counters {
    uint32_t open;
    uint32_t created;
    uint32_t closed;
    uint32_t enobufs;
    uint32_t connectFailed;
    int lastErrno;
};
void aleks2_get_socket_counters(struct aleks2_socket_counters* out);
}

namespace {

int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

int open_fds() {
    int count = 0;
    if (DIR* dir = opendir("/proc/self/fd")) {
        while (readdir(dir) != nullptr) {
            ++count;
        }
        closedir(dir);
    }
    return count;
}

aleks2_socket_counters counters() {
    aleks2_socket_counters c{};
    aleks2_get_socket_counters(&c);
    return c;
}

constexpr int kRequests = 200;

void serve(int listener, std::atomic<int>& served) {
    for (int i = 0; i < kRequests; ++i) {
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            return;
        }
        char buffer[512];
        (void)recv(client, buffer, sizeof buffer, 0);
        static constexpr char kResponse[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        (void)send(client, kResponse, sizeof kResponse - 1, 0);
        close(client);
        served.fetch_add(1);
    }
}

bool request(uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
    if (ok) {
        static constexpr char kRequest[] = "GET /catalog HTTP/1.1\r\nHost: localhost\r\n\r\n";
        ok = send(fd, kRequest, sizeof kRequest - 1, 0) == static_cast<ssize_t>(sizeof kRequest - 1);
        char buffer[256];
        std::string response;
        for (ssize_t n; ok && (n = recv(fd, buffer, sizeof buffer, 0)) > 0;) {
            response.append(buffer, static_cast<size_t>(n));
        }
        ok = ok && response.ends_with("ok");
    }
    close(fd);
    return ok;
}

void test_requests_with_udp_open() {
    const int fdsBefore = open_fds();
    const auto before = counters();

    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
    socklen_t length = sizeof address;
    CHECK(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    CHECK(listen(listener, 16) == 0);
    const uint16_t port = ntohs(address.sin_port);

    // The co-op's room socket, with the sizes the mod net service now asks for.
    const int udp = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(udp >= 0);
    const int receive = dusk::mods::net_buffers::kModUdpReceiveBytes;
    const int sendBytes = dusk::mods::net_buffers::kModUdpSendBytes;
    CHECK(setsockopt(udp, SOL_SOCKET, SO_RCVBUF, &receive, sizeof receive) == 0);
    CHECK(setsockopt(udp, SOL_SOCKET, SO_SNDBUF, &sendBytes, sizeof sendBytes) == 0);

    std::atomic<int> served{0};
    std::thread server{serve, listener, std::ref(served)};
    int ok = 0;
    for (int i = 0; i < kRequests; ++i) {
        ok += request(port) ? 1 : 0;
    }
    server.join();
    CHECK(ok == kRequests);
    CHECK(served.load() == kRequests);

    const auto during = counters();
    // listener + udp + 200 client sockets made here; accept()ed ones are not made by socket()
    CHECK(during.created - before.created == kRequests + 2);
    CHECK(during.closed - before.closed == kRequests);
    CHECK(during.open - before.open == 2);
    CHECK(during.connectFailed == before.connectFailed && during.enobufs == before.enobufs);

    // A file's close is not counted as a socket's.
    const int file = open("/proc/self/stat", O_RDONLY);
    CHECK(file >= 0);
    close(file);
    CHECK(counters().closed == during.closed);

    close(udp);
    close(listener);
    const auto after = counters();
    CHECK(after.open == before.open);
    CHECK(open_fds() == fdsBefore);

    // A refused connect is a failure, with its errno.
    const int probe = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0);
    close(probe);
    const auto refused = counters();
    CHECK(refused.connectFailed == after.connectFailed + 1 && refused.lastErrno == ECONNREFUSED);
    std::printf("netpool: %d requests with a UDP socket open; sockets created %u closed %u open %u\n", ok,
        refused.created, refused.closed, refused.open);
}

// switch_net.cpp's SocketInitConfig: 4 BSD sessions, tcp 32/64 KiB (max 256/256 KiB), udp 9/41 KiB,
// sb_efficiency 4 -> the "transfer memory 2252 KiB" of the console log.
constexpr int64_t kKiB = 1024;
constexpr int64_t kPool = 4 * (256 + 256 + 9 + 41) * kKiB;
constexpr int64_t kTcpSocket = (256 + 256) * kKiB;  // libcurl asks for nothing: a TCP socket can grow to its max
constexpr int64_t kWebSocket = kTcpSocket;

constexpr int64_t tcp_room(int64_t udpReceive, int64_t udpSend) {
    return (kPool - (udpReceive + udpSend) - kWebSocket) / kTcpSocket;
}

void test_pool_arithmetic() {
    using namespace dusk::mods::net_buffers;
    const int64_t rc4 = tcp_room(1024 * kKiB, 1024 * kKiB);
    const int64_t rc5 = tcp_room(kModUdpReceiveBytes, kModUdpSendBytes);
    std::printf("netpool: pool %lld KiB; TCP sockets left beside the co-op (UDP + WebSocket): rc4 %lld, rc5 %lld\n",
        static_cast<long long>(kPool / kKiB), static_cast<long long>(rc4), static_cast<long long>(rc5));
    CHECK(kPool == 2248 * kKiB);  // 2252 KiB once page-aligned on the console
    CHECK(rc4 < 1);
    CHECK(rc5 >= 2);  // two images, or one image and the catalog: the browser's worst case; more queue in libnx
    CHECK(clamp_mod_socket_buffer(4 * 1024 * 1024) == kModSocketBufferMax);
    CHECK(clamp_mod_socket_buffer(16 * 1024) == 16 * 1024);
}

}  // namespace

int main() {
    test_requests_with_udp_open();
    test_pool_arithmetic();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("netpool_test: all passed\n");
    return 0;
}
