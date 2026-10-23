// aleks2 (V1-rc3): host test of source/switch_netdb.cpp, linked the way the NRO links it (-Wl,--wrap=...).
//
//   1. Numeric answers are built locally and freed by the wrapper; names go to the system resolver.
//   2. The rc1/rc2 console crash scenario: while one thread reads a SQLite file, others open, use and close
//      UDP and TCP sockets (resolving through the wrapper, formatting peers with getnameinfo) and resolve real
//      names. Afterwards the SQLite descriptor must still be the same file, every query must have matched, and
//      no descriptor may be left over. Run under ASan/UBSan by test-netdb.sh.
#include <arpa/inet.h>
#include <dirent.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

static int open_fds() {
    int n = 0;
    if (DIR* d = opendir("/proc/self/fd")) {
        while (readdir(d) != nullptr) {
            ++n;
        }
        closedir(d);
    }
    return n;
}

static void numeric_answers() {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    CHECK(getaddrinfo("192.168.1.240", "27716", &hints, &res) == 0 && res != nullptr);
    if (res != nullptr) {
        CHECK(res->ai_family == AF_INET && res->ai_socktype == SOCK_DGRAM && res->ai_protocol == IPPROTO_UDP);
        const auto* sin = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
        CHECK(ntohs(sin->sin_port) == 27716 && res->ai_next == nullptr);
        char host[NI_MAXHOST], serv[NI_MAXSERV];
        CHECK(getnameinfo(res->ai_addr, res->ai_addrlen, host, sizeof host, serv, sizeof serv,
                  NI_NUMERICHOST | NI_NUMERICSERV) == 0);
        CHECK(std::strcmp(host, "192.168.1.240") == 0 && std::strcmp(serv, "27716") == 0);
        freeaddrinfo(res);
    }
    hints = {};
    hints.ai_flags = AI_PASSIVE;
    res = nullptr;
    CHECK(getaddrinfo(nullptr, "0", &hints, &res) == 0 && res != nullptr && res->ai_next != nullptr);  // TCP + UDP
    if (res != nullptr) {
        CHECK(reinterpret_cast<const sockaddr_in*>(res->ai_addr)->sin_addr.s_addr == htonl(INADDR_ANY));
        freeaddrinfo(res);
    }
    res = nullptr;
    CHECK(getaddrinfo("localhost", "80", nullptr, &res) == 0 && res != nullptr);  // a name: the system resolver
    if (res != nullptr) {
        freeaddrinfo(res);
    }
    res = nullptr;
    // Non-numeric service: not answered locally, so whatever the system resolver says (the container may have
    // no services database: then EAI_SERVICE/EAI_NONAME).
    const int named = getaddrinfo("1.2.3.4", "http", nullptr, &res);
    CHECK(named == 0 || named == EAI_SERVICE || named == EAI_NONAME);
    if (named == 0 && res != nullptr) {
        freeaddrinfo(res);
    }
    hints = {};
    hints.ai_family = AF_INET6;
    res = nullptr;
    CHECK(getaddrinfo("::1", "1", &hints, &res) == 0);  // not IPv4: passed through
    if (res != nullptr) {
        freeaddrinfo(res);
    }
}

static void udp_round(std::atomic<int>& rounds) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo("127.0.0.1", "0", &hints, &res) != 0 || res == nullptr) {
        ++g_failures;
        return;
    }
    const int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    CHECK(s >= 0);
    CHECK(bind(s, res->ai_addr, res->ai_addrlen) == 0);
    freeaddrinfo(res);
    sockaddr_in self{};
    socklen_t len = sizeof self;
    getsockname(s, reinterpret_cast<sockaddr*>(&self), &len);
    const char ping[] = "hello";
    CHECK(sendto(s, ping, sizeof ping, 0, reinterpret_cast<sockaddr*>(&self), len) == sizeof ping);
    char buf[16];
    sockaddr_in from{};
    socklen_t fromLen = sizeof from;
    CHECK(recvfrom(s, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fromLen) == sizeof ping);
    char host[NI_MAXHOST], serv[NI_MAXSERV];
    CHECK(getnameinfo(reinterpret_cast<sockaddr*>(&from), fromLen, host, sizeof host, serv, sizeof serv,
              NI_NUMERICHOST | NI_NUMERICSERV) == 0 &&
          std::strcmp(host, "127.0.0.1") == 0);
    CHECK(close(s) == 0);
    ++rounds;
}

static void tcp_round(std::atomic<int>& rounds) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo("127.0.0.1", "0", &hints, &res) != 0 || res == nullptr) {
        ++g_failures;
        return;
    }
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(bind(listener, res->ai_addr, res->ai_addrlen) == 0 && listen(listener, 1) == 0);
    freeaddrinfo(res);
    sockaddr_in where{};
    socklen_t len = sizeof where;
    getsockname(listener, reinterpret_cast<sockaddr*>(&where), &len);
    const int client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(client, reinterpret_cast<sockaddr*>(&where), len) == 0);
    const int server = accept(listener, nullptr, nullptr);
    CHECK(server >= 0);
    CHECK(send(client, "x", 1, 0) == 1);
    char c = 0;
    CHECK(recv(server, &c, 1, 0) == 1 && c == 'x');
    CHECK(close(server) == 0 && close(client) == 0 && close(listener) == 0);
    ++rounds;
}

static void concurrency() {
    const std::string path = "/tmp/netdb_test.db";
    std::remove(path.c_str());
    sqlite3* db = nullptr;
    CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    sqlite3_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v BLOB); BEGIN;", nullptr, nullptr, nullptr);
    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO t VALUES(?, zeroblob(4000))", -1, &ins, nullptr);
    for (int i = 0; i < 4000; ++i) {
        sqlite3_bind_int(ins, 1, i);
        sqlite3_step(ins);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    sqlite3_close(db);

    const int fdsBefore = open_fds();
    std::atomic<bool> stop{false};
    std::atomic<int> reads{0}, udp{0}, tcp{0}, names{0};
    std::thread reader([&] {
        sqlite3* r = nullptr;
        if (sqlite3_open_v2(path.c_str(), &r, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            ++g_failures;
            return;
        }
        sqlite3_exec(r, "PRAGMA cache_size=1;", nullptr, nullptr, nullptr);  // every query goes to the file
        sqlite3_stmt* q = nullptr;
        sqlite3_prepare_v2(r, "SELECT length(v) FROM t WHERE k = ?", -1, &q, nullptr);
        for (int i = 0; !stop.load(); i = (i + 37) % 4000) {
            sqlite3_bind_int(q, 1, i);
            const int rc = sqlite3_step(q);
            CHECK(rc == SQLITE_ROW && sqlite3_column_int(q, 0) == 4000);
            sqlite3_reset(q);
            ++reads;
        }
        sqlite3_finalize(q);
        CHECK(sqlite3_close(r) == SQLITE_OK);
    });
    std::vector<std::thread> sockets;
    for (int t = 0; t < 2; ++t) {
        sockets.emplace_back([&] {
            for (int i = 0; i < 400; ++i) {
                udp_round(udp);
                tcp_round(tcp);
            }
        });
    }
    std::thread resolver([&] {
        for (int i = 0; i < 200; ++i) {
            addrinfo* res = nullptr;
            if (getaddrinfo("localhost", "443", nullptr, &res) == 0) {
                freeaddrinfo(res);
                ++names;
            }
        }
    });
    for (auto& t : sockets) {
        t.join();
    }
    resolver.join();
    stop = true;
    reader.join();
    std::printf("reads %d, udp rounds %d, tcp rounds %d, name lookups %d\n", reads.load(), udp.load(), tcp.load(),
        names.load());
    CHECK(reads > 0 && udp == 800 && tcp == 800 && names == 200);
    CHECK(open_fds() == fdsBefore);
    std::remove(path.c_str());
}

int main() {
    numeric_answers();
    concurrency();
    if (g_failures != 0) {
        std::printf("netdb_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("netdb_test: all passed\n");
    return 0;
}
