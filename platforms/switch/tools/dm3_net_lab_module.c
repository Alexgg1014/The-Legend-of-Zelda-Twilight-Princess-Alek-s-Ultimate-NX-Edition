// aleks2 (DM3): the network lab module. A Linux-built library (aarch64-linux-gnu, glibc headers) that does
// with the C library what catalog mods do -- Crests of Courage's local-IP trick and TCP, MFB/libjuice's UDP,
// poll, fcntl(O_NONBLOCK), ioctl, getaddrinfo/getnameinfo, getifaddrs and its interrupt pipe -- plus what other
// network code commonly reaches for (eventfd, epoll, select, socket options, errno on failures).
//
//   int dm3_net_lab_run(char* report, size_t size, int flags, const char* host, int port, const char* name);
//
// Writes one line per check ("PASS x", "FAIL x: why", "INFO x"), returns the number of FAILs.
//   flags & 1  shimmed: also check what only the console layer does (fork fails with ENOSYS; uname says Horizon
//              when flags & 2). A native glibc run leaves these out, so both runs must otherwise agree.
//   host/port  optional: an HTTP server to GET / from (a PC on the LAN, or the test's own server).
//   name       optional: a DNS name to resolve (needs a resolver that reaches the internet).
//
// Built by test-dm3.sh for the host (qemu) runs and copied to the card for the console self-test
// (sdmc:/switch/dusklight/net-selftest/net_lab.so; src/dusk/switch_net_selftest.cpp).
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

// The entry points the loader requires of any module (never called by the tests: they call dm3_net_lab_run).
__attribute__((visibility("default"))) int mod_meta = 0;
__attribute__((visibility("default"))) void* mod_ctx = 0;
__attribute__((visibility("default"))) int mod_initialize(void) { return 0; }
__attribute__((visibility("default"))) int mod_update(void) { return 0; }
__attribute__((visibility("default"))) int mod_shutdown(void) { return 0; }

static char* g_out;
static size_t g_size, g_len;
static int g_fail;

static void say(const char* fmt, ...) {
    if (g_len + 1 >= g_size) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_out + g_len, g_size - g_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_len += (size_t)n < g_size - g_len ? (size_t)n : g_size - g_len - 1;
}

static void check(int ok, const char* what, const char* fmt, ...) {
    char why[200] = "";
    if (!ok && fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(why, sizeof why, fmt, ap);
        va_end(ap);
    }
    if (ok) {
        say("PASS %s\n", what);
    } else {
        g_fail++;
        say("FAIL %s: %s (errno %d)\n", what, why, errno);
    }
}

static struct sockaddr_in loopback(uint16_t port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

static void udp_tests(void) {
    int a = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, IPPROTO_UDP);
    int b = socket(AF_INET, SOCK_DGRAM, 0);
    check(a >= 0 && b >= 0, "udp: socket (with SOCK_NONBLOCK)", "%d %d", a, b);
    struct sockaddr_in addr = loopback(0);
    check(bind(a, (struct sockaddr*)&addr, sizeof addr) == 0, "udp: bind 127.0.0.1:0", "bind");
    struct sockaddr_storage st;
    socklen_t len = sizeof st;
    memset(&st, 0xab, sizeof st);
    check(getsockname(a, (struct sockaddr*)&st, &len) == 0 && len == sizeof(struct sockaddr_in) && st.ss_family == AF_INET &&
              ((struct sockaddr_in*)&st)->sin_port != 0,
        "udp: getsockname gives a glibc sockaddr_in (family at 0, 16 bytes)", "len %u family %u", (unsigned)len,
        (unsigned)st.ss_family);
    struct sockaddr_in bound = *(struct sockaddr_in*)&st;
    char buf[64];
    check(recv(a, buf, sizeof buf, 0) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK),
        "udp: empty non-blocking recv fails with EAGAIN", "");
    const char* msg = "dm3-udp";
    check(sendto(b, msg, 7, MSG_NOSIGNAL, (struct sockaddr*)&bound, sizeof bound) == 7, "udp: sendto loopback", "");
    struct pollfd p = {a, POLLIN, 0};
    check(poll(&p, 1, 2000) == 1 && (p.revents & POLLIN), "udp: poll sees POLLIN", "revents %#x", p.revents);
    int avail = -1;
    check(ioctl(a, FIONREAD, &avail) == 0 && avail >= 7, "udp: ioctl FIONREAD", "avail %d", avail);
    struct sockaddr_in from;
    socklen_t fromLen = sizeof from;
    memset(&from, 0, sizeof from);
    ssize_t n = recvfrom(a, buf, sizeof buf, 0, (struct sockaddr*)&from, &fromLen);
    check(n == 7 && memcmp(buf, msg, 7) == 0 && fromLen == sizeof from && from.sin_family == AF_INET &&
              from.sin_addr.s_addr == htonl(INADDR_LOOPBACK),
        "udp: recvfrom data and a glibc source address", "n %zd len %u family %u", n, (unsigned)fromLen, from.sin_family);
    // A short buffer for the address: truncated, real length reported (Linux semantics).
    sendto(b, msg, 7, 0, (struct sockaddr*)&bound, sizeof bound);
    poll(&p, 1, 2000);
    unsigned char tiny[4] = {0, 0, 0, 0};
    socklen_t tinyLen = sizeof tiny;
    n = recvfrom(a, buf, sizeof buf, 0, (struct sockaddr*)tiny, &tinyLen);
    check(n == 7 && tinyLen == sizeof(struct sockaddr_in) && tiny[0] == AF_INET && tiny[1] == 0,
        "udp: recvfrom into a short address buffer truncates and reports 16", "len %u bytes %02x %02x", (unsigned)tinyLen,
        tiny[0], tiny[1]);
    // MSG_PEEK then MSG_DONTWAIT.
    sendto(b, msg, 7, 0, (struct sockaddr*)&bound, sizeof bound);
    poll(&p, 1, 2000);
    check(recv(a, buf, sizeof buf, MSG_PEEK) == 7 && recv(a, buf, sizeof buf, MSG_DONTWAIT) == 7 &&
              recv(a, buf, sizeof buf, MSG_DONTWAIT) == -1 && errno == EAGAIN,
        "udp: MSG_PEEK and MSG_DONTWAIT", "");
    // Coop's local-IP trick: connect a UDP socket to a public address, read the local address.
    int probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in dns;
    memset(&dns, 0, sizeof dns);
    dns.sin_family = AF_INET;
    dns.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &dns.sin_addr);
    if (connect(probe, (struct sockaddr*)&dns, sizeof dns) == 0) {
        struct sockaddr_in local;
        socklen_t ll = sizeof local;
        getsockname(probe, (struct sockaddr*)&local, &ll);
        char text[32];
        inet_ntop(AF_INET, &local.sin_addr, text, sizeof text);
        say("INFO udp: local address by connect-to-8.8.8.8: %s\n", text);
    } else {
        say("INFO udp: connect to 8.8.8.8:53 failed (errno %d): no route (offline?)\n", errno);
    }
    close(probe);
    // SSDP: a multicast datagram (Coop's UPnP discovery). The route may not exist here; reported, not judged.
    struct sockaddr_in ssdp;
    memset(&ssdp, 0, sizeof ssdp);
    ssdp.sin_family = AF_INET;
    ssdp.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &ssdp.sin_addr);
    unsigned char ttl = 2;
    setsockopt(b, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    const char* search = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: ssdp:all\r\n\r\n";
    n = sendto(b, search, strlen(search), 0, (struct sockaddr*)&ssdp, sizeof ssdp);
    say("INFO udp: SSDP M-SEARCH to 239.255.255.250:1900 -> %zd (errno %d)\n", n, n < 0 ? errno : 0);
    close(a);
    close(b);
}

static void tcp_tests(void) {
    int l = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    check(setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0, "tcp: SO_REUSEADDR", "");
    struct sockaddr_in addr = loopback(0);
    check(bind(l, (struct sockaddr*)&addr, sizeof addr) == 0 && listen(l, 4) == 0, "tcp: bind+listen 127.0.0.1", "");
    socklen_t len = sizeof addr;
    getsockname(l, (struct sockaddr*)&addr, &len);
    int type = -1;
    socklen_t tl = sizeof type;
    check(getsockopt(l, SOL_SOCKET, SO_TYPE, &type, &tl) == 0 && type == SOCK_STREAM, "tcp: SO_TYPE is SOCK_STREAM",
        "type %d", type);
    int c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int flags = fcntl(c, F_GETFL, 0);
    check(fcntl(c, F_SETFL, flags | O_NONBLOCK) == 0 && (fcntl(c, F_GETFL, 0) & O_NONBLOCK), "tcp: fcntl O_NONBLOCK", "");
    check(setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) == 0, "tcp: TCP_NODELAY", "");
    int r = connect(c, (struct sockaddr*)&addr, sizeof addr);
    check(r == 0 || errno == EINPROGRESS, "tcp: non-blocking connect is 0 or EINPROGRESS", "r %d", r);
    struct pollfd p = {c, POLLOUT, 0};
    check(poll(&p, 1, 2000) == 1 && (p.revents & POLLOUT), "tcp: poll POLLOUT when connected", "revents %#x", p.revents);
    int soError = -1;
    socklen_t el = sizeof soError;
    check(getsockopt(c, SOL_SOCKET, SO_ERROR, &soError, &el) == 0 && soError == 0, "tcp: SO_ERROR is 0", "%d", soError);
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    int s = accept4(l, (struct sockaddr*)&peer, &pl, SOCK_NONBLOCK);
    check(s >= 0 && pl == sizeof peer && peer.sin_family == AF_INET && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK),
        "tcp: accept4 with a glibc peer address", "s %d len %u family %u", s, (unsigned)pl, peer.sin_family);
    char buf[64];
    check(recv(s, buf, sizeof buf, 0) == -1 && errno == EAGAIN, "tcp: accept4 SOCK_NONBLOCK applied", "");
    check(send(c, "hello", 5, MSG_NOSIGNAL) == 5, "tcp: send MSG_NOSIGNAL", "");
    struct pollfd ps = {s, POLLIN, 0};
    poll(&ps, 1, 2000);
    check(recv(s, buf, sizeof buf, 0) == 5 && memcmp(buf, "hello", 5) == 0, "tcp: recv", "");
    struct sockaddr_in name;
    socklen_t nl = sizeof name;
    check(getpeername(c, (struct sockaddr*)&name, &nl) == 0 && name.sin_port == addr.sin_port, "tcp: getpeername", "");
    check(shutdown(c, SHUT_WR) == 0, "tcp: shutdown SHUT_WR", "");
    poll(&ps, 1, 2000);
    check(recv(s, buf, sizeof buf, 0) == 0, "tcp: peer sees end of stream", "");
    close(c);
    close(s);
    // A refused connection: errno ECONNREFUSED as glibc numbers it.
    int d = socket(AF_INET, SOCK_STREAM, 0);
    close(l);  // the port is free now
    errno = 0;
    r = connect(d, (struct sockaddr*)&addr, sizeof addr);
    check(r == -1 && errno == ECONNREFUSED, "tcp: connect to a closed port fails ECONNREFUSED (111)", "r %d", r);
    close(d);
    // Options: hints Linux has and others do not are accepted.
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    int size = 65536, prio = 6, got = 0;
    socklen_t gl = sizeof got;
    check(setsockopt(u, SOL_SOCKET, SO_RCVBUF, &size, sizeof size) == 0 &&
              getsockopt(u, SOL_SOCKET, SO_RCVBUF, &got, &gl) == 0 && got > 0,
        "opt: SO_RCVBUF set/get", "got %d", got);
    check(setsockopt(u, SOL_SOCKET, SO_PRIORITY, &prio, sizeof prio) == 0, "opt: SO_PRIORITY accepted", "");
    check(setsockopt(u, SOL_SOCKET, SO_BROADCAST, &one, sizeof one) == 0, "opt: SO_BROADCAST", "");
    struct timeval tv = {1, 0};
    check(setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0, "opt: SO_RCVTIMEO", "");
    check(setsockopt(u, SOL_SOCKET, 0x7fff, &one, sizeof one) == -1 && errno == ENOPROTOOPT,
        "opt: an unknown option fails ENOPROTOOPT (92)", "");
    int on = 1;
    check(ioctl(u, FIONBIO, &on) == 0 && (fcntl(u, F_GETFL, 0) & O_NONBLOCK), "ioctl: FIONBIO sets O_NONBLOCK", "");
    close(u);
    errno = 0;
    int six = socket(AF_INET6, SOCK_DGRAM, 0);
    check(six >= 0 || errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT, "socket: AF_INET6 works or fails with a glibc errno", "");
    if (six >= 0) close(six);
}

static void descriptor_tests(void) {
    int fds[2];
    check(pipe2(fds, O_NONBLOCK) == 0, "pipe2 O_NONBLOCK", "");
    char b[16];
    check(read(fds[0], b, sizeof b) == -1 && errno == EAGAIN, "pipe: empty read EAGAIN", "");
    check(write(fds[1], "x", 1) == 1, "pipe: write", "");
    struct pollfd p = {fds[0], POLLIN, 0};
    check(poll(&p, 1, 2000) == 1, "pipe: poll POLLIN", "");
    check(read(fds[0], b, sizeof b) == 1 && b[0] == 'x', "pipe: read", "");
    close(fds[1]);
    poll(&p, 1, 2000);
    check(read(fds[0], b, sizeof b) == 0, "pipe: end of file after the write end closes", "");
    close(fds[0]);
    int blocking[2];
    check(pipe(blocking) == 0 && fcntl(blocking[0], F_SETFL, O_NONBLOCK) == 0, "pipe + fcntl(F_SETFL, O_NONBLOCK)", "");
    close(blocking[0]);
    close(blocking[1]);

    int e = eventfd(0, EFD_NONBLOCK);
    uint64_t v = 2;
    check(e >= 0 && write(e, &v, 8) == 8, "eventfd: write 2", "");
    v = 3;
    write(e, &v, 8);
    struct pollfd pe = {e, POLLIN, 0};
    poll(&pe, 1, 2000);
    v = 0;
    check(read(e, &v, 8) == 8 && v == 5, "eventfd: read sums to 5", "v %llu", (unsigned long long)v);
    check(read(e, &v, 8) == -1 && errno == EAGAIN, "eventfd: empty read EAGAIN", "");

    int ep = epoll_create1(0);
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = loopback(0);
    socklen_t al = sizeof a;
    bind(u, (struct sockaddr*)&a, sizeof a);
    getsockname(u, (struct sockaddr*)&a, &al);
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    ev.data.u64 = 0x1122334455667788ull;
    check(ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, u, &ev) == 0, "epoll: create + add", "");
    check(epoll_ctl(ep, EPOLL_CTL_ADD, u, &ev) == -1 && errno == EEXIST, "epoll: add twice EEXIST", "");
    struct epoll_event out[4];
    check(epoll_wait(ep, out, 4, 0) == 0, "epoll: nothing ready", "");
    int w = socket(AF_INET, SOCK_DGRAM, 0);
    sendto(w, "e", 1, 0, (struct sockaddr*)&a, sizeof a);
    int n = epoll_wait(ep, out, 4, 2000);
    check(n == 1 && (out[0].events & EPOLLIN) && out[0].data.u64 == 0x1122334455667788ull,
        "epoll: wait reports EPOLLIN with the data", "n %d", n);
    check(epoll_ctl(ep, EPOLL_CTL_DEL, u, NULL) == 0 && epoll_wait(ep, out, 4, 0) == 0, "epoll: del", "");

    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(u, &rs);
    struct timeval tv = {2, 0};
    check(select(u + 1, &rs, NULL, NULL, &tv) == 1 && FD_ISSET(u, &rs), "select: readable (glibc fd_set)", "");
    char c;
    recv(u, &c, 1, 0);
    FD_ZERO(&rs);
    FD_SET(u, &rs);
    tv.tv_sec = 0;
    tv.tv_usec = 1000;
    check(select(u + 1, &rs, NULL, NULL, &tv) == 0, "select: times out when empty", "");
    close(w);
    close(u);
    close(ep);
    close(e);
}

static void name_tests(const char* name) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    int r = getaddrinfo("127.0.0.1", "8080", &hints, &res);
    check(r == 0 && res && res->ai_family == AF_INET && res->ai_addrlen == sizeof(struct sockaddr_in) && res->ai_addr &&
              ((struct sockaddr_in*)res->ai_addr)->sin_family == AF_INET &&
              ((struct sockaddr_in*)res->ai_addr)->sin_port == htons(8080),
        "getaddrinfo: numeric, glibc addrinfo layout", "r %d", r);
    if (r == 0) {
        char host[NI_MAXHOST], serv[NI_MAXSERV];
        int g = getnameinfo(res->ai_addr, res->ai_addrlen, host, sizeof host, serv, sizeof serv, NI_NUMERICHOST | NI_NUMERICSERV);
        check(g == 0 && strcmp(host, "127.0.0.1") == 0 && strcmp(serv, "8080") == 0, "getnameinfo: numeric", "g %d", g);
        freeaddrinfo(res);
    }
    res = NULL;
    hints.ai_flags = 0;
    hints.ai_socktype = SOCK_DGRAM;
    r = getaddrinfo("localhost", "53", &hints, &res);
    int v4 = 0;
    for (struct addrinfo* a = res; a; a = a->ai_next) v4 += a->ai_family == AF_INET;
    check(r == 0 && v4 >= 1, "getaddrinfo: localhost has an IPv4 address", "r %d (%s)", r, r ? gai_strerror(r) : "");
    if (res) freeaddrinfo(res);
    res = NULL;
    hints.ai_flags = AI_NUMERICHOST;
    r = getaddrinfo("not-a-number", NULL, &hints, &res);
    check(r == EAI_NONAME, "getaddrinfo: AI_NUMERICHOST with a name is EAI_NONAME (-2)", "r %d", r);
    check(gai_strerror(EAI_NONAME) != NULL && gai_strerror(EAI_NONAME)[0] != 0, "gai_strerror", "");
    char text[INET6_ADDRSTRLEN];
    struct in_addr in4;
    check(inet_pton(AF_INET, "192.168.1.20", &in4) == 1 && inet_ntop(AF_INET, &in4, text, sizeof text) &&
              strcmp(text, "192.168.1.20") == 0,
        "inet_pton/inet_ntop AF_INET", "");
    struct in6_addr in6;
    int p6 = inet_pton(AF_INET6, "::1", &in6);
    check(p6 == 1 || p6 == -1, "inet_pton AF_INET6 answers (1, or -1 EAFNOSUPPORT)", "%d", p6);
    if (name && *name) {
        res = NULL;
        hints.ai_flags = 0;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        r = getaddrinfo(name, "443", &hints, &res);
        check(r == 0 && res && res->ai_family == AF_INET, "getaddrinfo: a DNS name (AF_UNSPEC answered IPv4)", "%s: r %d (%s)",
            name, r, r ? gai_strerror(r) : "");
        if (res) freeaddrinfo(res);
    }
    struct ifaddrs* ifs = NULL;
    int lo = 0, others = 0;
    check(getifaddrs(&ifs) == 0, "getifaddrs", "");
    for (struct ifaddrs* i = ifs; i; i = i->ifa_next) {
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET) {
            char t[32];
            inet_ntop(AF_INET, &((struct sockaddr_in*)i->ifa_addr)->sin_addr, t, sizeof t);
            if (i->ifa_flags & IFF_LOOPBACK) {
                lo += strcmp(t, "127.0.0.1") == 0;
            } else {
                others++;
                say("INFO getifaddrs: %s %s%s\n", i->ifa_name, t, (i->ifa_flags & IFF_UP) ? " up" : "");
            }
        }
    }
    check(lo == 1, "getifaddrs: loopback 127.0.0.1 with IFF_LOOPBACK", "lo %d", lo);
    if (ifs) freeifaddrs(ifs);
    say("INFO getifaddrs: %d non-loopback IPv4 interface(s)\n", others);
}

static void http_test(const char* host, int port) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    int r = getaddrinfo(host, ps, &hints, &res);
    check(r == 0 && res, "http: resolve the server", "%s r %d", host, r);
    if (r != 0) return;
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    struct timeval tv = {5, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    check(connect(s, res->ai_addr, res->ai_addrlen) == 0, "http: connect", "%s:%d", host, port);
    freeaddrinfo(res);
    char req[256];
    int n = snprintf(req, sizeof req, "GET / HTTP/1.0\r\nHost: %s\r\nUser-Agent: dm3-net-lab\r\n\r\n", host);
    check(send(s, req, n, MSG_NOSIGNAL) == n, "http: send request", "");
    char resp[512];
    size_t got = 0;
    for (;;) {
        ssize_t k = recv(s, resp + got, sizeof resp - 1 - got, 0);
        if (k <= 0) break;
        got += (size_t)k;
        if (got >= sizeof resp - 1) break;
    }
    resp[got] = 0;
    check(got > 12 && strncmp(resp, "HTTP/1.", 7) == 0, "http: a response", "got %zu", got);
    char* eol = strstr(resp, "\r\n");
    if (eol) *eol = 0;
    say("INFO http: %s\n", resp);
    close(s);
}

static void console_only(int flags) {
    errno = 0;
    pid_t pid = fork();
    check(pid == -1 && errno == ENOSYS, "fork: -1 with ENOSYS (38): no processes here", "pid %d", (int)pid);
    char* argv[] = {"xdg-open", "https://example.invalid", NULL};
    errno = 0;
    check(execvp("xdg-open", argv) == -1 && errno == ENOSYS, "execvp: -1 with ENOSYS", "");
    int st = 0;
    check(waitpid(1, &st, 0) == -1 && errno == ECHILD, "waitpid: -1 with ECHILD (10)", "");
    struct utsname u;
    check(uname(&u) == 0 && strcmp(u.machine, "aarch64") == 0, "uname: machine aarch64", "%s", u.machine);
    if (flags & 2) {
        check(strcmp(u.sysname, "Horizon") == 0, "uname: sysname Horizon", "%s", u.sysname);
    }
    say("INFO uname: %s %s %s %s %s\n", u.sysname, u.nodename, u.release, u.version, u.machine);
}

__attribute__((visibility("default"))) int dm3_net_lab_run(char* report, size_t size, int flags, const char* host, int port,
    const char* name) {
    g_out = report;
    g_size = size;
    g_len = 0;
    g_fail = 0;
    if (size) report[0] = 0;
    udp_tests();
    tcp_tests();
    descriptor_tests();
    name_tests(name);
    if (host && *host && port > 0) http_test(host, port);
    if (flags & 1) console_only(flags);
    say("DM3 NET LAB: %d failure(s)\n", g_fail);
    return g_fail;
}
