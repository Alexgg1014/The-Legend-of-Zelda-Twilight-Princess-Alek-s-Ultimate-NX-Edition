// aleks2 (DM3): see glibc_net.hpp.
#include "dusk/mods/net_buffers.hpp"
#include "glibc_net.hpp"

#include "glibc_abi.hpp"
#include "dusk/gai_text.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/utsname.h>
#endif

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace dusk::mods::elf::glibc::net {
namespace {

constexpr long long kNotDefined = -999999;
constexpr bool kBsd = offsetof(struct sockaddr, sa_family) == 1;

// glibc values the shims write or test directly (abi-glibc.json; the table has them too).
constexpr int G_AF_INET = 2;
constexpr int G_AF_INET6 = 10;
constexpr int G_SOCK_TYPE_MASK = 0xf;
constexpr int G_SOCK_NONBLOCK = 0x800;
constexpr int G_SOCK_CLOEXEC = 0x80000;
constexpr int G_O_NONBLOCK = 0x800;
constexpr int G_SOL_SOCKET = 1;
constexpr int G_SO_ERROR = 4;
constexpr int G_SO_TYPE = 3;
constexpr int G_EAI_SYSTEM = -11;
constexpr int G_EAI_FAMILY = -6;
constexpr int G_EAI_MEMORY = -10;
constexpr int G_FIONBIO = 0x5421;
constexpr int G_FIONREAD = 0x541B;
constexpr int G_EFD_SEMAPHORE = 1;
constexpr int G_EFD_NONBLOCK = 0x800;
constexpr int G_EPOLLET = static_cast<int>(1u << 31);
constexpr int G_EPOLLONESHOT = 1 << 30;
constexpr int G_EPOLLRDHUP = 0x2000;
constexpr int G_EPOLL_CTL_ADD = 1, G_EPOLL_CTL_DEL = 2, G_EPOLL_CTL_MOD = 3;
constexpr int G_IFF_UP = 0x1, G_IFF_BROADCAST = 0x2, G_IFF_LOOPBACK = 0x8, G_IFF_RUNNING = 0x40,
              G_IFF_MULTICAST = 0x1000;

// glibc errno values the shims set directly.
constexpr int G_ENOSYS = 38, G_ECHILD = 10, G_EINVAL = 22, G_EBADF = 9, G_ENOTTY = 25, G_EFAULT = 14,
              G_ENOPROTOOPT = 92, G_EOPNOTSUPP = 95, G_EAFNOSUPPORT = 97, G_ENOMEM = 12, G_EEXIST = 17,
              G_ENOENT = 2, G_EAGAIN = 11, G_EMFILE = 24;

Platform s_platform;

void log_line(int level, const std::string& line) {
    if (s_platform.log != nullptr) {
        s_platform.log(level, line);
    }
}

// A failed native call: errno as the module expects it.
void set_errno_from_native() {
    errno = errno_to_glibc(errno);
}

int fail(int glibcErrno) {
    errno = glibcErrno;
    return -1;
}

// --- the generated table, by group ---

struct Pair {
    int glibc;
    int native;
};

struct NetMaps {
    std::unordered_map<std::string_view, std::vector<Pair>> groups;
    std::unordered_map<std::string_view, std::vector<std::string_view>> glibcOnly;  // names with no native value
    std::unordered_map<std::string_view, int> glibcByName;

    NetMaps() {
        for (const auto& e : table()) {
            const std::string_view group = e.group;
            if (group == "errno" || group == "clock" || group == "oflag" || group == "fcntl") {
                continue;
            }
            glibcByName.emplace(e.name, static_cast<int>(e.glibc));
            if (e.native == kNotDefined) {
                glibcOnly[group].push_back(e.name);
            } else {
                groups[group].push_back({static_cast<int>(e.glibc), static_cast<int>(e.native)});
            }
        }
        // Bit groups: widest glibc masks first (none overlap today; kept as glibc_abi's oflags do).
        for (const char* g : {"msg", "ai", "ni", "poll", "iff"}) {
            auto& v = groups[g];
            std::stable_sort(v.begin(), v.end(), [](Pair a, Pair b) {
                return std::popcount(static_cast<unsigned>(a.glibc)) > std::popcount(static_cast<unsigned>(b.glibc));
            });
        }
    }

    const std::vector<Pair>& group(std::string_view g) const {
        static const std::vector<Pair> empty;
        const auto it = groups.find(g);
        return it == groups.end() ? empty : it->second;
    }

    bool glibc_only(std::string_view g, int glibcValue) const {
        const auto it = glibcOnly.find(g);
        if (it == glibcOnly.end()) {
            return false;
        }
        for (const auto name : it->second) {
            const auto v = glibcByName.find(name);
            if (v != glibcByName.end() && v->second == glibcValue) {
                return true;
            }
        }
        return false;
    }
};

const NetMaps& maps() {
    static const NetMaps s_maps;
    return s_maps;
}

int value_from_glibc(std::string_view group, int glibc, int fallback) {
    for (const auto& p : maps().group(group)) {
        if (p.glibc == glibc) {
            return p.native;
        }
    }
    return fallback;
}

int value_to_glibc(std::string_view group, int native, int fallback) {
    for (const auto& p : maps().group(group)) {
        if (p.native == native) {
            return p.glibc;
        }
    }
    return fallback;
}

size_t family_size(int nativeFamily) {
    if (nativeFamily == AF_INET) {
        return sizeof(sockaddr_in);
    }
#ifdef AF_INET6
    if (nativeFamily == AF_INET6) {
        return sizeof(sockaddr_in6);
    }
#endif
    return 0;
}

// --- emulated descriptors and the module socket registry ---

// V1-rc3: File = a descriptor a module opened itself (open, fcntl F_DUPFD). close() only closes descriptors of
// these kinds: anything else belongs to the game (SQLite caches, the log, Dawn) and a module closing it --
// a double close, a stale number kept after close -- would take the game's file away. Linux answers a close of
// a descriptor that is not open with EBADF; the game's own descriptors are treated as not the module's.
enum class Kind : uint8_t { Socket, PipeEnd, EventFd, Epoll, File };

struct EpollWatch {
    uint32_t events;  // glibc EPOLL* bits
    uint64_t data;
};

struct Registry {
    std::mutex mutex;
    std::unordered_map<int, Kind> kinds;
    std::unordered_map<int, std::map<int, EpollWatch>> epolls;
    std::unordered_map<int, bool> eventfdSemaphore;
    bool warnedEdgeTriggered = false;
    bool warnedIpv6 = false;
    std::atomic<uint64_t> lanRebinds{0};
    std::atomic<uint64_t> getaddrinfoCalls{0};
    std::atomic<uint64_t> refusedCloses{0};
};

Registry& registry() {
    static Registry* s_registry = new Registry;  // never destroyed: modules are never unloaded
    return *s_registry;
}

void track(int fd, Kind kind) {
    if (fd < 0) {
        return;
    }
    auto& r = registry();
    std::lock_guard lock{r.mutex};
    r.kinds[fd] = kind;
}

[[maybe_unused]] Kind kind_of(int fd) {
    auto& r = registry();
    std::lock_guard lock{r.mutex};
    const auto it = r.kinds.find(fd);
    return it == r.kinds.end() ? Kind::Socket : it->second;
}

bool is_emulated(int fd, Kind kind) {
    auto& r = registry();
    std::lock_guard lock{r.mutex};
    const auto it = r.kinds.find(fd);
    return it != r.kinds.end() && it->second == kind;
}

void untrack(int fd) {
    auto& r = registry();
    std::lock_guard lock{r.mutex};
    r.kinds.erase(fd);
    r.epolls.erase(fd);
    r.eventfdSemaphore.erase(fd);
    for (auto& [_, set] : r.epolls) {
        set.erase(fd);  // epoll drops a closed descriptor from every set, as Linux does
    }
}

bool set_nonblocking(int fd, bool on) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
}

sockaddr_in loopback_any_port() {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = 0;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if constexpr (kBsd) {
        reinterpret_cast<uint8_t*>(&a)[0] = sizeof(sockaddr_in);
    }
    return a;
}

// A connected pair over loopback: TCP (stream semantics, EOF on close) or, if the stack refuses TCP loopback,
// two UDP sockets connected to each other. Native calls only. Returns false with a native errno.
bool loopback_pair(int out[2], bool stream) {
    if (stream) {
        const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener >= 0) {
            sockaddr_in a = loopback_any_port();
            socklen_t len = sizeof(a);
            if (bind(listener, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 && listen(listener, 1) == 0 &&
                getsockname(listener, reinterpret_cast<sockaddr*>(&a), &len) == 0)
            {
                const int writer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (writer >= 0 && connect(writer, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
                    const int reader = accept(listener, nullptr, nullptr);
                    if (reader >= 0) {
                        close(listener);
                        const int one = 1;
                        setsockopt(writer, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                        setsockopt(reader, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                        out[0] = reader;
                        out[1] = writer;
                        return true;
                    }
                }
                if (writer >= 0) {
                    const int saved = errno;
                    close(writer);
                    errno = saved;
                }
            }
            const int saved = errno;
            close(listener);
            errno = saved;
        }
    }
    const int a = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const int b = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (a < 0 || b < 0) {
        const int saved = errno;
        if (a >= 0) close(a);
        if (b >= 0) close(b);
        errno = saved;
        return false;
    }
    sockaddr_in aa = loopback_any_port(), ba = loopback_any_port();
    socklen_t al = sizeof(aa), bl = sizeof(ba);
    if (bind(a, reinterpret_cast<sockaddr*>(&aa), sizeof(aa)) != 0 ||
        bind(b, reinterpret_cast<sockaddr*>(&ba), sizeof(ba)) != 0 ||
        getsockname(a, reinterpret_cast<sockaddr*>(&aa), &al) != 0 ||
        getsockname(b, reinterpret_cast<sockaddr*>(&ba), &bl) != 0 ||
        connect(a, reinterpret_cast<sockaddr*>(&ba), sizeof(ba)) != 0 ||
        connect(b, reinterpret_cast<sockaddr*>(&aa), sizeof(aa)) != 0)
    {
        const int saved = errno;
        close(a);
        close(b);
        errno = saved;
        return false;
    }
    out[0] = a;
    out[1] = b;
    return true;
}

// --- glibc structures the shims build or read ---

struct GAddrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    uint32_t ai_addrlen;
    void* ai_addr;
    char* ai_canonname;
    GAddrinfo* ai_next;
};
static_assert(sizeof(GAddrinfo) == 48 && offsetof(GAddrinfo, ai_addr) == 24 && offsetof(GAddrinfo, ai_canonname) == 32 &&
              offsetof(GAddrinfo, ai_next) == 40);

struct GIovec {
    void* base;
    size_t len;
};

struct GMsghdr {
    void* msg_name;
    uint32_t msg_namelen;
    GIovec* msg_iov;
    size_t msg_iovlen;
    void* msg_control;
    size_t msg_controllen;
    int msg_flags;
};
static_assert(sizeof(GMsghdr) == 56 && offsetof(GMsghdr, msg_iov) == 16 && offsetof(GMsghdr, msg_controllen) == 40 &&
              offsetof(GMsghdr, msg_flags) == 48);

struct GIfaddrs {
    GIfaddrs* ifa_next;
    char* ifa_name;
    unsigned ifa_flags;
    void* ifa_addr;
    void* ifa_netmask;
    void* ifa_ifu;  // ifu_broadaddr (glibc names it with a macro)
    void* ifa_data;
};
static_assert(sizeof(GIfaddrs) == 56 && offsetof(GIfaddrs, ifa_flags) == 16 && offsetof(GIfaddrs, ifa_addr) == 24 &&
              offsetof(GIfaddrs, ifa_ifu) == 40 && offsetof(GIfaddrs, ifa_data) == 48);

struct GUtsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};
static_assert(sizeof(GUtsname) == 390);

struct GEpollEvent {
    uint32_t events;
    uint64_t data;  // epoll_data_t; aarch64 does not pack the struct (x86-64 does)
};
static_assert(sizeof(GEpollEvent) == 16 && offsetof(GEpollEvent, data) == 8);

struct GSockaddrIn {
    uint16_t family;
    uint16_t port;
    uint32_t addr;
    uint8_t zero[8];
};
static_assert(sizeof(GSockaddrIn) == 16);

int socket_type(int fd) {
    int type = 0;
    socklen_t len = sizeof(type);
    return getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0 ? type : -1;
}

}  // namespace

// --- translation ---

void set_platform(const Platform& platform) {
    s_platform = platform;
}

int af_from_glibc(int glibc) {
    return value_from_glibc("af", glibc, glibc);
}

int af_to_glibc(int native) {
    return value_to_glibc("af", native, native);
}

int eai_to_glibc(int native) {
    if (native == 0) {
        return 0;
    }
    return value_to_glibc("eai", native, G_EAI_SYSTEM);
}

int bits_from_glibc(const char* group, int glibc, int* dropped) {
    int native = 0;
    int rest = glibc;
    for (const auto& p : maps().group(group)) {
        if (p.glibc != 0 && (rest & p.glibc) == p.glibc) {
            native |= p.native;
            rest &= ~p.glibc;
        }
    }
    if (dropped != nullptr) {
        *dropped = rest;
    }
    return native;
}

int bits_to_glibc(const char* group, int native) {
    int glibc = 0;
    int rest = native;
    for (const auto& p : maps().group(group)) {
        if (p.native != 0 && (rest & p.native) == p.native) {
            glibc |= p.glibc;
            rest &= ~p.native;
        }
    }
    return glibc;
}

Option option_from_glibc(int level, int name) {
    const auto& m = maps();
    std::string_view group;
    int nativeLevel = -1;
    if (level == G_SOL_SOCKET) {
        group = "so";
        nativeLevel = SOL_SOCKET;
    } else if (level == IPPROTO_IP) {
        group = "ipopt";
        nativeLevel = IPPROTO_IP;
    } else if (level == IPPROTO_IPV6) {
        group = "ipv6opt";
        nativeLevel = IPPROTO_IPV6;
    } else if (level == IPPROTO_TCP) {
        group = "tcpopt";
        nativeLevel = IPPROTO_TCP;
    } else {
        return {-1, -1};
    }
    for (const auto& p : m.group(group)) {
        if (p.glibc == name) {
            return {nativeLevel, p.native};
        }
    }
    // SO_RCVBUFFORCE/SO_SNDBUFFORCE: the unprivileged form is what can be done here.
    if (group == "so" && name == 33) {
        return {SOL_SOCKET, SO_RCVBUF};
    }
    if (group == "so" && name == 32) {
        return {SOL_SOCKET, SO_SNDBUF};
    }
    // Linux-only knobs that only tune behaviour: accepted, nothing done (glibc names in the table with no native
    // value). Anything else is an option this stack does not have.
    if (m.glibc_only(group, name)) {
        return {-2, -2};
    }
    return {-1, -1};
}

template <bool Bsd>
size_t sockaddr_from_glibc(const void* glibc, size_t length, void* out, size_t outSize) {
    if (glibc == nullptr || length < 2 || length > outSize) {
        return 0;
    }
    std::memcpy(out, glibc, length);
    uint16_t family = 0;
    std::memcpy(&family, glibc, 2);
    const int native = af_from_glibc(family);
    size_t nativeLength = length;
    const size_t familySize = family_size(native);
    if (familySize != 0) {
        if (length < familySize) {
            return 0;  // Linux: EINVAL for a short sockaddr_in/in6
        }
        nativeLength = familySize;  // Linux accepts a longer buffer (sizeof sockaddr_storage); BSD wants sa_len
    }
    auto* bytes = static_cast<uint8_t*>(out);
    if constexpr (Bsd) {
        bytes[0] = static_cast<uint8_t>(nativeLength);
        bytes[1] = static_cast<uint8_t>(native);
    } else {
        const uint16_t f = static_cast<uint16_t>(native);
        std::memcpy(bytes, &f, 2);
    }
    return nativeLength;
}

template <bool Bsd>
void sockaddr_to_glibc(const void* native, size_t nativeLength, void* glibc, uint32_t* glibcLength) {
    if (glibc == nullptr || glibcLength == nullptr) {
        return;
    }
    uint8_t tmp[sizeof(sockaddr_storage)]{};
    nativeLength = std::min(nativeLength, sizeof(tmp));
    std::memcpy(tmp, native, nativeLength);
    int family = 0;
    if constexpr (Bsd) {
        family = nativeLength >= 2 ? tmp[1] : 0;
    } else {
        uint16_t f = 0;
        std::memcpy(&f, tmp, std::min<size_t>(2, nativeLength));
        family = f;
    }
    // Horizon may report sa_len rather than the length it wrote; the family decides what the module gets.
    const size_t familySize = family_size(family);
    if (familySize != 0 && nativeLength < familySize) {
        nativeLength = familySize;
    }
    const uint16_t g = static_cast<uint16_t>(af_to_glibc(family));
    std::memcpy(tmp, &g, 2);
    std::memcpy(glibc, tmp, std::min<size_t>(*glibcLength, nativeLength));
    *glibcLength = static_cast<uint32_t>(nativeLength);
}

template size_t sockaddr_from_glibc<true>(const void*, size_t, void*, size_t);
template size_t sockaddr_from_glibc<false>(const void*, size_t, void*, size_t);
template void sockaddr_to_glibc<true>(const void*, size_t, void*, uint32_t*);
template void sockaddr_to_glibc<false>(const void*, size_t, void*, uint32_t*);

size_t shutdown_module_sockets() {
    std::vector<int> fds;
    {
        auto& r = registry();
        std::lock_guard lock{r.mutex};
        for (const auto& [fd, kind] : r.kinds) {
            if (kind == Kind::Socket) {
                fds.push_back(fd);
            }
        }
    }
    for (const int fd : fds) {
        shutdown(fd, SHUT_RDWR);
    }
    return fds.size();
}

size_t close_module_sockets() {
    std::vector<int> fds;
    {
        auto& r = registry();
        std::lock_guard lock{r.mutex};
        for (const auto& [fd, kind] : r.kinds) {
            if (kind == Kind::Socket) {
                fds.push_back(fd);
            }
        }
    }
    for (const int fd : fds) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
        untrack(fd);
    }
    return fds.size();
}

Counters counters() {
    auto& r = registry();
    std::lock_guard lock{r.mutex};
    Counters c;
    c.sockets = r.kinds.size();
    for (const auto& [_, kind] : r.kinds) {
        c.emulated += kind != Kind::Socket;
    }
    c.lanRebinds = r.lanRebinds.load();
    c.getaddrinfoCalls = r.getaddrinfoCalls.load();
    return c;
}

}  // namespace dusk::mods::elf::glibc::net

using namespace dusk::mods::elf::glibc;
using namespace dusk::mods::elf::glibc::net;

namespace {

// In and out of the native layout for this build.
size_t addr_in(const void* glibc, uint32_t length, sockaddr_storage& out) {
    return sockaddr_from_glibc<kBsd>(glibc, length, &out, sizeof(out));
}

void addr_out(const sockaddr_storage& native, socklen_t nativeLength, void* glibc, uint32_t* glibcLength) {
    sockaddr_to_glibc<kBsd>(&native, nativeLength, glibc, glibcLength);
}

int msg_flags_in(int glibc) {
    return bits_from_glibc("msg", glibc);
}

int poll_events_in(int glibc) {
    return bits_from_glibc("poll", glibc);
}

int poll_events_out(int native) {
    return bits_to_glibc("poll", native);
}

int apply_socket_flags(int fd, int glibcType) {
    if ((glibcType & G_SOCK_NONBLOCK) != 0 && !set_nonblocking(fd, true)) {
        const int saved = errno;
        close(fd);
        errno = saved;
        set_errno_from_native();
        return -1;
    }
    return fd;  // SOCK_CLOEXEC: there is no exec
}

// One wait on a pollfd array with glibc events, written back with glibc revents.
int poll_glibc(struct pollfd* fds, unsigned long count, int timeoutMs) {
    std::vector<short> saved(count);
    for (unsigned long i = 0; i < count; i++) {
        saved[i] = fds[i].events;
        fds[i].events = static_cast<short>(poll_events_in(static_cast<unsigned short>(fds[i].events)));
        fds[i].revents = 0;
    }
    const int r = poll(fds, static_cast<nfds_t>(count), timeoutMs);
    const int nativeErrno = errno;
    for (unsigned long i = 0; i < count; i++) {
        fds[i].events = saved[i];
        fds[i].revents = static_cast<short>(poll_events_out(static_cast<unsigned short>(fds[i].revents)));
    }
    if (r < 0) {
        errno = errno_to_glibc(nativeErrno);
    }
    return r;
}

int timeval_ms(const struct timeval* tv) {
    if (tv == nullptr) {
        return -1;
    }
    const long long ms = static_cast<long long>(tv->tv_sec) * 1000 + (tv->tv_usec + 999) / 1000;
    return static_cast<int>(std::clamp<long long>(ms, 0, 0x7fffffff));
}

int timespec_ms(const struct timespec* ts) {
    if (ts == nullptr) {
        return -1;
    }
    const long long ms = static_cast<long long>(ts->tv_sec) * 1000 + (ts->tv_nsec + 999999) / 1000000;
    return static_cast<int>(std::clamp<long long>(ms, 0, 0x7fffffff));
}

// glibc fd_set: 1024 bits in unsigned longs.
constexpr int kGlibcFdSetSize = 1024;
using GFdSet = unsigned long[kGlibcFdSetSize / (8 * sizeof(unsigned long))];

bool fd_isset(const unsigned long* set, int fd) {
    return set != nullptr && ((set[fd / 64] >> (fd % 64)) & 1) != 0;
}

void fd_set_bit(unsigned long* set, int fd) {
    set[fd / 64] |= 1ul << (fd % 64);
}

int select_on_poll(int nfds, unsigned long* r, unsigned long* w, unsigned long* e, int timeoutMs) {
    if (nfds < 0 || nfds > kGlibcFdSetSize) {
        return fail(G_EINVAL);
    }
    std::vector<struct pollfd> fds;
    for (int fd = 0; fd < nfds; fd++) {
        short ev = 0;
        if (fd_isset(r, fd)) ev |= POLLIN;
        if (fd_isset(w, fd)) ev |= POLLOUT;
        if (fd_isset(e, fd)) ev |= POLLPRI;
        if (ev != 0) {
            fds.push_back({fd, ev, 0});
        }
    }
    const int n = poll(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMs);
    if (n < 0) {
        set_errno_from_native();
        return -1;
    }
    for (unsigned long* set : {r, w, e}) {
        if (set != nullptr) {
            std::memset(set, 0, sizeof(GFdSet));
        }
    }
    int count = 0;
    for (const auto& p : fds) {
        if ((p.revents & POLLNVAL) != 0) {
            return fail(G_EBADF);
        }
        if (r != nullptr && (p.events & POLLIN) && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
            fd_set_bit(r, p.fd);
            count++;
        }
        if (w != nullptr && (p.events & POLLOUT) && (p.revents & (POLLOUT | POLLERR))) {
            fd_set_bit(w, p.fd);
            count++;
        }
        if (e != nullptr && (p.events & POLLPRI) && (p.revents & POLLPRI)) {
            fd_set_bit(e, p.fd);
            count++;
        }
    }
    return count;
}

GAddrinfo* glibc_addrinfo_node(const addrinfo& n) {
    const size_t canon = n.ai_canonname != nullptr ? std::strlen(n.ai_canonname) + 1 : 0;
    auto* block = static_cast<uint8_t*>(std::calloc(1, sizeof(GAddrinfo) + sizeof(sockaddr_storage) + canon));
    if (block == nullptr) {
        return nullptr;
    }
    auto* g = reinterpret_cast<GAddrinfo*>(block);
    g->ai_flags = bits_to_glibc("ai", n.ai_flags);
    g->ai_family = af_to_glibc(n.ai_family);
    g->ai_socktype = n.ai_socktype;
    g->ai_protocol = n.ai_protocol;
    uint32_t length = sizeof(sockaddr_storage);
    if (n.ai_addr != nullptr && n.ai_addrlen > 0) {
        sockaddr_storage tmp{};
        std::memcpy(&tmp, n.ai_addr, std::min<size_t>(n.ai_addrlen, sizeof(tmp)));
        addr_out(tmp, n.ai_addrlen, block + sizeof(GAddrinfo), &length);
        g->ai_addr = block + sizeof(GAddrinfo);
        g->ai_addrlen = length;
    }
    if (canon != 0) {
        g->ai_canonname = reinterpret_cast<char*>(block + sizeof(GAddrinfo) + sizeof(sockaddr_storage));
        std::memcpy(g->ai_canonname, n.ai_canonname, canon);
    }
    return g;
}

// V1-rc11: the first kTraceCalls socket calls a module makes, with the glibc values it passed and the native ones
// the BSD service got (a tester's bsdsocket aborted twice, 2162-0002; the translation is one suspect). Log only.
constexpr int kTraceCalls = 16;
std::atomic<int> s_traced{0};

void trace_call(const char* what, int fd, long g1, long g2, long n1, long n2, int result) {
    if (s_traced.fetch_add(1, std::memory_order_relaxed) >= kTraceCalls) {
        return;
    }
    char line[200];
    std::snprintf(line, sizeof line, "net trace: %s fd %d glibc (%ld, %ld) -> native (%ld, %ld) = %d", what, fd, g1,
        g2, n1, n2, result);
    log_line(0, line);
}

void free_glibc_addrinfo(GAddrinfo* list) {
    while (list != nullptr) {
        GAddrinfo* next = list->ai_next;
        std::free(list);
        list = next;
    }
}

}  // namespace

extern "C" {

// --- sockets ---

int aleks2_glibc_socket(int domain, int type, int protocol) {
    const int fd = socket(af_from_glibc(domain), type & G_SOCK_TYPE_MASK, protocol);
    trace_call("socket(domain, type)", fd, domain, type, af_from_glibc(domain), type & G_SOCK_TYPE_MASK, fd);
    if (fd < 0) {
        set_errno_from_native();
        return -1;
    }
    if (apply_socket_flags(fd, type) < 0) {
        return -1;
    }
    track(fd, Kind::Socket);
    return fd;
}

int aleks2_glibc_socketpair(int domain, int type, int protocol, int sv[2]) {
    (void)protocol;
    // AF_UNIX (Linux's usual socketpair) does not exist on Horizon; a loopback pair behaves the same for bytes.
    if (domain != 1 /* AF_UNIX */ && domain != G_AF_INET) {
        return fail(G_EAFNOSUPPORT);
    }
    const int base = type & G_SOCK_TYPE_MASK;
    int fds[2];
    if (!loopback_pair(fds, base == SOCK_STREAM)) {
        set_errno_from_native();
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        if ((type & G_SOCK_NONBLOCK) != 0) {
            set_nonblocking(fds[i], true);
        }
        track(fds[i], Kind::PipeEnd);
        sv[i] = fds[i];
    }
    return 0;
}

int aleks2_glibc_bind(int fd, const void* address, uint32_t length) {
    sockaddr_storage n{};
    const size_t nlen = addr_in(address, length, n);
    if (nlen == 0) {
        return fail(G_EINVAL);
    }
    // DM3 LAN option: a TCP socket on 127.0.0.1 (a mod's local server: Item Tracker, OBS overlays) listens on
    // every interface instead, so that a PC or phone on the same network can reach it. Off by default.
    if (n.ss_family == AF_INET && s_platform.lanListenAllowed != nullptr && s_platform.lanListenAllowed()) {
        auto* in = reinterpret_cast<sockaddr_in*>(&n);
        if (in->sin_addr.s_addr == htonl(INADDR_LOOPBACK) && socket_type(fd) == SOCK_STREAM) {
            in->sin_addr.s_addr = htonl(INADDR_ANY);
            registry().lanRebinds++;
            log_line(1, "net: a mod's TCP socket on 127.0.0.1:" + std::to_string(ntohs(in->sin_port)) +
                            " listens on the local network instead (Settings: allow mods to listen on the LAN)");
        }
    }
    const int r = bind(fd, reinterpret_cast<sockaddr*>(&n), static_cast<socklen_t>(nlen));
    trace_call("bind(family, length)", fd, length >= 2 ? static_cast<const uint8_t*>(address)[0] : -1, length,
        n.ss_family, static_cast<long>(nlen), r);
    if (r != 0) {
        set_errno_from_native();
    }
    return r;
}

int aleks2_glibc_connect(int fd, const void* address, uint32_t length) {
    sockaddr_storage n{};
    const size_t nlen = addr_in(address, length, n);
    if (nlen == 0) {
        return fail(G_EINVAL);
    }
    const int r = connect(fd, reinterpret_cast<sockaddr*>(&n), static_cast<socklen_t>(nlen));
    if (r != 0) {
        set_errno_from_native();
    }
    return r;
}

int aleks2_glibc_listen(int fd, int backlog) {
    const int r = listen(fd, backlog);
    if (r != 0) {
        set_errno_from_native();
    }
    return r;
}

int aleks2_glibc_accept4(int fd, void* address, uint32_t* length, int flags) {
    sockaddr_storage n{};
    socklen_t nlen = sizeof(n);
    const int c = accept(fd, reinterpret_cast<sockaddr*>(&n), &nlen);
    if (c < 0) {
        set_errno_from_native();
        return -1;
    }
    addr_out(n, nlen, address, length);
    if (apply_socket_flags(c, flags) < 0) {
        return -1;
    }
    track(c, Kind::Socket);
    return c;
}

int aleks2_glibc_accept(int fd, void* address, uint32_t* length) {
    return aleks2_glibc_accept4(fd, address, length, 0);
}

int aleks2_glibc_getsockname(int fd, void* address, uint32_t* length) {
    sockaddr_storage n{};
    socklen_t nlen = sizeof(n);
    const int r = getsockname(fd, reinterpret_cast<sockaddr*>(&n), &nlen);
    if (r != 0) {
        set_errno_from_native();
        return r;
    }
    addr_out(n, nlen, address, length);
    return 0;
}

int aleks2_glibc_getpeername(int fd, void* address, uint32_t* length) {
    sockaddr_storage n{};
    socklen_t nlen = sizeof(n);
    const int r = getpeername(fd, reinterpret_cast<sockaddr*>(&n), &nlen);
    if (r != 0) {
        set_errno_from_native();
        return r;
    }
    addr_out(n, nlen, address, length);
    return 0;
}

int aleks2_glibc_shutdown(int fd, int how) {
    const int r = shutdown(fd, how);  // SHUT_RD/WR/RDWR are 0/1/2 in both
    if (r != 0) {
        set_errno_from_native();
    }
    return r;
}

ssize_t aleks2_glibc_send(int fd, const void* data, size_t size, int flags) {
    const ssize_t r = send(fd, data, size, msg_flags_in(flags));
    if (r < 0) {
        set_errno_from_native();
    }
    return r;
}

ssize_t aleks2_glibc_sendto(int fd, const void* data, size_t size, int flags, const void* address, uint32_t length) {
    sockaddr_storage n{};
    size_t nlen = 0;
    if (address != nullptr) {
        nlen = addr_in(address, length, n);
        if (nlen == 0) {
            return fail(G_EINVAL);
        }
    }
    const ssize_t r = sendto(fd, data, size, msg_flags_in(flags), address != nullptr ? reinterpret_cast<sockaddr*>(&n) : nullptr,
        static_cast<socklen_t>(nlen));
    if (r < 0) {
        set_errno_from_native();
    }
    return r;
}

ssize_t aleks2_glibc_recv(int fd, void* data, size_t size, int flags) {
    const ssize_t r = recv(fd, data, size, msg_flags_in(flags));
    if (r < 0) {
        set_errno_from_native();
    }
    return r;
}

ssize_t aleks2_glibc_recvfrom(int fd, void* data, size_t size, int flags, void* address, uint32_t* length) {
    sockaddr_storage n{};
    socklen_t nlen = sizeof(n);
    const ssize_t r = recvfrom(fd, data, size, msg_flags_in(flags), reinterpret_cast<sockaddr*>(&n), &nlen);
    if (r < 0) {
        set_errno_from_native();
        return r;
    }
    if (address != nullptr && length != nullptr) {
        if (nlen == 0) {
            *length = 0;  // a connected socket's datagram may come without a source
        } else {
            addr_out(n, nlen, address, length);
        }
    }
    return r;
}

ssize_t aleks2_glibc_sendmsg(int fd, const GMsghdr* m, int flags) {
    if (m == nullptr) {
        return fail(G_EFAULT);
    }
    if (m->msg_control != nullptr && m->msg_controllen != 0) {
        return fail(G_EOPNOTSUPP);  // cmsghdr differs (size_t vs socklen_t length); nothing a mod needs today
    }
    sockaddr_storage n{};
    size_t nlen = 0;
    if (m->msg_name != nullptr) {
        nlen = addr_in(m->msg_name, m->msg_namelen, n);
        if (nlen == 0) {
            return fail(G_EINVAL);
        }
    }
    msghdr nm{};
    nm.msg_name = m->msg_name != nullptr ? &n : nullptr;
    nm.msg_namelen = static_cast<socklen_t>(nlen);
    nm.msg_iov = reinterpret_cast<iovec*>(m->msg_iov);  // struct iovec is {void*, size_t} in both
    nm.msg_iovlen = static_cast<decltype(nm.msg_iovlen)>(m->msg_iovlen);
    const ssize_t r = sendmsg(fd, &nm, msg_flags_in(flags));
    if (r < 0) {
        set_errno_from_native();
    }
    return r;
}

ssize_t aleks2_glibc_recvmsg(int fd, GMsghdr* m, int flags) {
    if (m == nullptr) {
        return fail(G_EFAULT);
    }
    sockaddr_storage n{};
    msghdr nm{};
    nm.msg_name = m->msg_name != nullptr ? &n : nullptr;
    nm.msg_namelen = m->msg_name != nullptr ? sizeof(n) : 0;
    nm.msg_iov = reinterpret_cast<iovec*>(m->msg_iov);
    nm.msg_iovlen = static_cast<decltype(nm.msg_iovlen)>(m->msg_iovlen);
    const ssize_t r = recvmsg(fd, &nm, msg_flags_in(flags));
    if (r < 0) {
        set_errno_from_native();
        return r;
    }
    if (m->msg_name != nullptr) {
        if (nm.msg_namelen == 0) {
            m->msg_namelen = 0;
        } else {
            addr_out(n, nm.msg_namelen, m->msg_name, &m->msg_namelen);
        }
    }
    m->msg_controllen = 0;  // no ancillary data is delivered
    m->msg_flags = bits_to_glibc("msg", nm.msg_flags);
    return r;
}

int aleks2_glibc_setsockopt(int fd, int level, int name, const void* value, uint32_t length) {
    const Option o = option_from_glibc(level, name);
    if (o.level == -2) {
        return 0;  // a Linux tuning knob: accepted, nothing to tune here
    }
    if (o.level == -1) {
        return fail(G_ENOPROTOOPT);
    }
#ifdef __SWITCH__
    // V1-rc5: buffer sizes come out of one pool shared by every socket of the process (net_buffers.hpp).
    if (o.level == SOL_SOCKET && (o.name == SO_RCVBUF || o.name == SO_SNDBUF) && value != nullptr &&
        length >= sizeof(int))
    {
        int size = 0;
        std::memcpy(&size, value, sizeof size);
        size = dusk::mods::net_buffers::clamp_mod_socket_buffer(size);
        const int r = setsockopt(fd, o.level, o.name, &size, sizeof size);
        if (r != 0) {
            set_errno_from_native();
        }
        return r;
    }
#endif
    const int r = setsockopt(fd, o.level, o.name, value, static_cast<socklen_t>(length));
    trace_call("setsockopt(level, name)", fd, level, name, o.level, o.name, r);
    if (r != 0) {
        set_errno_from_native();
    }
    return r;
}

int aleks2_glibc_getsockopt(int fd, int level, int name, void* value, uint32_t* length) {
    const Option o = option_from_glibc(level, name);
    if (o.level == -2) {
        if (value != nullptr && length != nullptr && *length >= sizeof(int)) {
            std::memset(value, 0, sizeof(int));
            *length = sizeof(int);
        }
        return 0;
    }
    if (o.level == -1) {
        return fail(G_ENOPROTOOPT);
    }
    socklen_t nlen = length != nullptr ? *length : 0;
    const int r = getsockopt(fd, o.level, o.name, value, &nlen);
    if (r != 0) {
        set_errno_from_native();
        return r;
    }
    if (length != nullptr) {
        *length = nlen;
    }
    if (level == G_SOL_SOCKET && value != nullptr && nlen >= sizeof(int)) {
        int v = 0;
        std::memcpy(&v, value, sizeof(int));
        if (name == G_SO_ERROR) {
            v = errno_to_glibc(v);
        } else if (name == G_SO_TYPE) {
            v = value_to_glibc("sock", v, v);
        } else {
            return 0;
        }
        std::memcpy(value, &v, sizeof(int));
    }
    return 0;
}

int aleks2_glibc_ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    if (request == G_FIONBIO) {
        if (arg == nullptr) {
            return fail(G_EFAULT);
        }
        int on = 0;
        std::memcpy(&on, arg, sizeof(int));
        if (!set_nonblocking(fd, on != 0)) {
            set_errno_from_native();
            return -1;
        }
        return 0;
    }
    if (request == G_FIONREAD) {
        const int r = ioctl(fd, FIONREAD, arg);
        if (r < 0) {
            set_errno_from_native();
        }
        return r;
    }
    return fail(G_ENOTTY);  // terminal and interface ioctls: nothing here answers them
}

int aleks2_glibc_poll(struct pollfd* fds, unsigned long count, int timeoutMs) {
    return poll_glibc(fds, count, timeoutMs);
}

int aleks2_glibc_ppoll(struct pollfd* fds, unsigned long count, const struct timespec* timeout, const void*) {
    return poll_glibc(fds, count, timespec_ms(timeout));  // no signals here: the mask has nothing to block
}

int aleks2_glibc_select(int nfds, unsigned long* r, unsigned long* w, unsigned long* e, struct timeval* timeout) {
    return select_on_poll(nfds, r, w, e, timeval_ms(timeout));
}

int aleks2_glibc_pselect(int nfds, unsigned long* r, unsigned long* w, unsigned long* e, const struct timespec* timeout,
    const void*) {
    return select_on_poll(nfds, r, w, e, timespec_ms(timeout));
}

// --- names and addresses ---

int aleks2_glibc_getaddrinfo(const char* node, const char* service, const GAddrinfo* hints, GAddrinfo** result) {
    if (result == nullptr) {
        return G_EAI_SYSTEM;
    }
    *result = nullptr;
    registry().getaddrinfoCalls++;
    addrinfo h{};
    if (hints != nullptr) {
        h.ai_flags = bits_from_glibc("ai", hints->ai_flags & ~(8 | 16));  // AI_V4MAPPED, AI_ALL: IPv4 only here
        h.ai_socktype = hints->ai_socktype & G_SOCK_TYPE_MASK;
        h.ai_protocol = hints->ai_protocol;
        if (hints->ai_family == G_AF_INET6) {
            auto& r = registry();
            if (!r.warnedIpv6) {
                r.warnedIpv6 = true;
                log_line(1, "net: a mod asked getaddrinfo for IPv6; this console's resolver answers IPv4 only");
            }
        } else if (hints->ai_family != 0 && hints->ai_family != G_AF_INET) {
            return G_EAI_FAMILY;
        }
    }
    h.ai_family = AF_INET;
    addrinfo* list = nullptr;
    const int r = getaddrinfo(node, service, &h, &list);
    if (r != 0) {
        const int nativeErrno = errno;
        const int g = eai_to_glibc(r);
        if (g == G_EAI_SYSTEM) {
            errno = errno_to_glibc(nativeErrno);
        }
        return g;
    }
    GAddrinfo* head = nullptr;
    GAddrinfo** tail = &head;
    for (const addrinfo* n = list; n != nullptr; n = n->ai_next) {
        GAddrinfo* g = glibc_addrinfo_node(*n);
        if (g == nullptr) {
            free_glibc_addrinfo(head);
            freeaddrinfo(list);
            return G_EAI_MEMORY;
        }
        *tail = g;
        tail = &g->ai_next;
    }
    freeaddrinfo(list);
    *result = head;
    return 0;
}

void aleks2_glibc_freeaddrinfo(GAddrinfo* list) {
    free_glibc_addrinfo(list);  // every list a Linux module holds was built by the shim above
}

const char* aleks2_glibc_gai_strerror(int code) {
    return dusk::gai_text(value_from_glibc("eai", code, code));  // not gai_strerror: see gai_text.hpp
}

int aleks2_glibc_getnameinfo(const void* address, uint32_t length, char* host, uint32_t hostLength, char* service,
    uint32_t serviceLength, int flags) {
    sockaddr_storage n{};
    const size_t nlen = addr_in(address, length, n);
    if (nlen == 0) {
        return G_EAI_FAMILY;
    }
    const int r = getnameinfo(reinterpret_cast<sockaddr*>(&n), static_cast<socklen_t>(nlen), host, hostLength, service,
        serviceLength, bits_from_glibc("ni", flags));
    if (r != 0) {
        const int nativeErrno = errno;
        const int g = eai_to_glibc(r);
        if (g == G_EAI_SYSTEM) {
            errno = errno_to_glibc(nativeErrno);
        }
        return g;
    }
    return 0;
}

int aleks2_glibc_inet_pton(int family, const char* text, void* out) {
    const int r = inet_pton(af_from_glibc(family), text, out);
    if (r < 0) {
        errno = G_EAFNOSUPPORT;
    }
    return r;
}

const char* aleks2_glibc_inet_ntop(int family, const void* address, char* out, uint32_t size) {
    const char* r = inet_ntop(af_from_glibc(family), address, out, static_cast<socklen_t>(size));
    if (r == nullptr) {
        set_errno_from_native();
    }
    return r;
}

int aleks2_glibc_getifaddrs(GIfaddrs** out) {
    if (out == nullptr) {
        return fail(G_EFAULT);
    }
    *out = nullptr;
    std::vector<Interface> list;
    list.push_back({"lo", htonl(INADDR_LOOPBACK), htonl(0xff000000u), true});
    if (s_platform.interfaces != nullptr) {
        for (auto& i : s_platform.interfaces()) {
            if (!i.loopback) {
                list.push_back(std::move(i));
            }
        }
    }
#if defined(__GLIBC__)
    else {
        ifaddrs* native = nullptr;
        if (getifaddrs(&native) == 0) {
            for (const ifaddrs* i = native; i != nullptr; i = i->ifa_next) {
                if (i->ifa_addr != nullptr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_LOOPBACK) == 0) {
                    const auto* a = reinterpret_cast<const sockaddr_in*>(i->ifa_addr);
                    const auto* m = reinterpret_cast<const sockaddr_in*>(i->ifa_netmask);
                    list.push_back({i->ifa_name, a->sin_addr.s_addr, m != nullptr ? m->sin_addr.s_addr : 0, false});
                }
            }
            freeifaddrs(native);
        }
    }
#endif
    // One block: the nodes, then three glibc sockaddr_in per node, then the names.
    size_t names = 0;
    for (const auto& i : list) {
        names += i.name.size() + 1;
    }
    const size_t nodes = list.size();
    auto* block = static_cast<uint8_t*>(std::calloc(1, nodes * (sizeof(GIfaddrs) + 3 * sizeof(GSockaddrIn)) + names));
    if (block == nullptr) {
        return fail(G_ENOMEM);
    }
    auto* node = reinterpret_cast<GIfaddrs*>(block);
    auto* addrs = reinterpret_cast<GSockaddrIn*>(block + nodes * sizeof(GIfaddrs));
    auto* name = reinterpret_cast<char*>(block + nodes * (sizeof(GIfaddrs) + 3 * sizeof(GSockaddrIn)));
    for (size_t k = 0; k < nodes; k++) {
        const auto& i = list[k];
        GIfaddrs& g = node[k];
        g.ifa_next = k + 1 < nodes ? &node[k + 1] : nullptr;
        std::memcpy(name, i.name.c_str(), i.name.size() + 1);
        g.ifa_name = name;
        name += i.name.size() + 1;
        g.ifa_flags = G_IFF_UP | G_IFF_RUNNING | (i.loopback ? G_IFF_LOOPBACK : (G_IFF_BROADCAST | G_IFF_MULTICAST));
        GSockaddrIn* a = &addrs[3 * k];
        a[0] = {G_AF_INET, 0, i.address, {}};
        a[1] = {G_AF_INET, 0, i.netmask, {}};
        a[2] = {G_AF_INET, 0, i.loopback ? i.address : (i.address | ~i.netmask), {}};
        g.ifa_addr = &a[0];
        g.ifa_netmask = &a[1];
        g.ifa_ifu = &a[2];
    }
    *out = node;
    return 0;
}

void aleks2_glibc_freeifaddrs(GIfaddrs* list) {
    std::free(list);  // the head is the block
}

int aleks2_glibc_uname(GUtsname* out) {
    if (out == nullptr) {
        return fail(G_EFAULT);
    }
    std::memset(out, 0, sizeof(*out));
    std::string sysname = "Horizon", release, version, machine = "aarch64";
#if defined(__GLIBC__)
    if (s_platform.describe == nullptr) {
        struct utsname u{};
        if (uname(&u) == 0) {
            sysname = u.sysname;
            release = u.release;
            version = u.version;
            machine = u.machine;
        }
    }
#endif
    if (s_platform.describe != nullptr) {
        s_platform.describe(sysname, release, version, machine);
    }
    const auto put = [](char* field, const std::string& s) { std::snprintf(field, 65, "%s", s.c_str()); };
    put(out->sysname, sysname);
    if (gethostname(out->nodename, sizeof(out->nodename) - 1) != 0 || out->nodename[0] == '\0') {
        put(out->nodename, "switch");
    }
    put(out->release, release);
    put(out->version, version);
    put(out->machine, machine);
    put(out->domainname, "(none)");
    return 0;
}

// --- descriptors: read/write/close with glibc errno, and the emulated kinds ---

int aleks2_glibc_pipe2(int fds[2], int flags) {
    if (fds == nullptr) {
        return fail(G_EFAULT);
    }
    int pair[2];
    if (!loopback_pair(pair, true)) {
        set_errno_from_native();
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        if ((flags & G_O_NONBLOCK) != 0) {
            set_nonblocking(pair[i], true);
        }
        track(pair[i], Kind::PipeEnd);
        fds[i] = pair[i];
    }
    return 0;
}

int aleks2_glibc_pipe(int fds[2]) {
    return aleks2_glibc_pipe2(fds, 0);
}

int aleks2_glibc_eventfd(unsigned initval, int flags) {
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        set_errno_from_native();
        return -1;
    }
    sockaddr_in a = loopback_any_port();
    socklen_t len = sizeof(a);
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 ||
        getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0 ||
        connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
    {
        const int saved = errno;
        close(fd);
        errno = errno_to_glibc(saved);
        return -1;
    }
    if (initval != 0) {
        const uint64_t v = initval;
        send(fd, &v, sizeof(v), 0);
    }
    if ((flags & G_EFD_NONBLOCK) != 0) {
        set_nonblocking(fd, true);
    }
    track(fd, Kind::EventFd);
    auto& r = registry();
    std::lock_guard lock{r.mutex};
    r.eventfdSemaphore[fd] = (flags & G_EFD_SEMAPHORE) != 0;
    return fd;
}

namespace {

ssize_t eventfd_read(int fd, void* buffer, size_t size) {
    if (size < sizeof(uint64_t)) {
        return fail(G_EINVAL);
    }
    bool semaphore = false;
    {
        auto& r = registry();
        std::lock_guard lock{r.mutex};
        semaphore = r.eventfdSemaphore[fd];
    }
    uint64_t value = 0;
    ssize_t n = recv(fd, &value, sizeof(value), 0);  // blocks unless EFD_NONBLOCK, as eventfd does
    if (n < 0) {
        set_errno_from_native();
        return -1;
    }
    if (semaphore) {
        if (value > 1) {
            const uint64_t rest = value - 1;
            send(fd, &rest, sizeof(rest), 0);
        }
        value = 1;
    } else {
        uint64_t more = 0;
        while (recv(fd, &more, sizeof(more), MSG_DONTWAIT) == sizeof(more)) {
            value += more;
        }
    }
    std::memcpy(buffer, &value, sizeof(value));
    return sizeof(value);
}

ssize_t eventfd_write(int fd, const void* buffer, size_t size) {
    if (size < sizeof(uint64_t)) {
        return fail(G_EINVAL);
    }
    uint64_t value = 0;
    std::memcpy(&value, buffer, sizeof(value));
    if (value == ~0ull) {
        return fail(G_EINVAL);
    }
    if (value != 0 && send(fd, &value, sizeof(value), 0) < 0) {
        set_errno_from_native();
        return -1;
    }
    return sizeof(value);
}

}  // namespace

ssize_t aleks2_glibc_read(int fd, void* buffer, size_t size) {
    if (is_emulated(fd, Kind::EventFd)) {
        return eventfd_read(fd, buffer, size);
    }
    const ssize_t r = read(fd, buffer, size);
    if (r < 0) {
        set_errno_from_native();
    }
    return r;
}

ssize_t aleks2_glibc_write(int fd, const void* buffer, size_t size) {
    if (is_emulated(fd, Kind::EventFd)) {
        return eventfd_write(fd, buffer, size);
    }
    const ssize_t r = write(fd, buffer, size);
    if (r < 0) {
        set_errno_from_native();
    }
    return r;
}

int aleks2_glibc_close(int fd) {
    bool owned = false;
    {
        auto& r = registry();
        std::lock_guard lock{r.mutex};
        owned = r.kinds.count(fd) != 0;
    }
    if (!owned) {
        // V1-rc3: not a descriptor this layer gave the module (see Kind). Refused, logged the first few times.
        const uint64_t n = registry().refusedCloses.fetch_add(1) + 1;
        if (n <= 8) {
            log_line(1, ("net: a mod tried to close descriptor " + std::to_string(fd) +
                         ", which it did not open here; refused (EBADF), the game keeps it")
                            .c_str());
        }
        errno = errno_to_glibc(EBADF);
        return -1;
    }
    untrack(fd);
    const int r = close(fd);
    if (r != 0) {
        set_errno_from_native();
    }
    return r;
}

// V1-rc3: descriptors a module opens through the file layer (glibc_abi.cpp: open, fcntl F_DUPFD).
void aleks2_glibc_note_module_fd(int fd) {
    track(fd, Kind::File);
}

int aleks2_glibc_eventfd_read(int fd, uint64_t* value) {
    return aleks2_glibc_read(fd, value, sizeof(*value)) == sizeof(*value) ? 0 : -1;
}

int aleks2_glibc_eventfd_write(int fd, uint64_t value) {
    return aleks2_glibc_write(fd, &value, sizeof(value)) == sizeof(value) ? 0 : -1;
}

// --- epoll on poll (level-triggered) ---

int aleks2_glibc_epoll_create1(int) {
    // The set needs a descriptor number of its own that nothing else will be given: a UDP socket that is never
    // used for traffic.
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        set_errno_from_native();
        return -1;
    }
    track(fd, Kind::Epoll);
    auto& r = registry();
    std::lock_guard lock{r.mutex};
    r.epolls[fd];
    return fd;
}

int aleks2_glibc_epoll_create(int size) {
    if (size <= 0) {
        return fail(G_EINVAL);
    }
    return aleks2_glibc_epoll_create1(0);
}

int aleks2_glibc_epoll_ctl(int epfd, int op, int fd, const GEpollEvent* event) {
    auto& r = registry();
    std::lock_guard lock{r.mutex};
    const auto set = r.epolls.find(epfd);
    if (set == r.epolls.end()) {
        return fail(G_EBADF);
    }
    if (op == G_EPOLL_CTL_DEL) {
        return set->second.erase(fd) != 0 ? 0 : fail(G_ENOENT);
    }
    if (event == nullptr) {
        return fail(G_EFAULT);
    }
    if ((event->events & G_EPOLLET) != 0 && !r.warnedEdgeTriggered) {
        r.warnedEdgeTriggered = true;
        log_line(1, "net: a mod asked epoll for edge-triggered events; they are level-triggered here");
    }
    const bool present = set->second.count(fd) != 0;
    if (op == G_EPOLL_CTL_ADD) {
        if (present) {
            return fail(G_EEXIST);
        }
    } else if (op == G_EPOLL_CTL_MOD) {
        if (!present) {
            return fail(G_ENOENT);
        }
    } else {
        return fail(G_EINVAL);
    }
    set->second[fd] = {event->events, event->data};
    return 0;
}

int aleks2_glibc_epoll_wait(int epfd, GEpollEvent* events, int max, int timeoutMs) {
    if (events == nullptr || max <= 0) {
        return fail(G_EINVAL);
    }
    std::vector<struct pollfd> fds;
    std::vector<EpollWatch> watches;
    {
        auto& r = registry();
        std::lock_guard lock{r.mutex};
        const auto set = r.epolls.find(epfd);
        if (set == r.epolls.end()) {
            return fail(G_EBADF);
        }
        for (const auto& [fd, w] : set->second) {
            if ((w.events & ~(G_EPOLLET | G_EPOLLONESHOT)) == 0) {
                continue;  // disarmed (EPOLLONESHOT after an event, or nothing asked)
            }
            fds.push_back({fd, static_cast<short>(poll_events_in(w.events & 0xffff)), 0});
            watches.push_back(w);
        }
    }
    const int n = poll(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMs);
    if (n < 0) {
        set_errno_from_native();
        return -1;
    }
    int count = 0;
    std::vector<int> fired;
    for (size_t i = 0; i < fds.size() && count < max; i++) {
        if (fds[i].revents == 0) {
            continue;
        }
        uint32_t ev = static_cast<uint32_t>(poll_events_out(static_cast<unsigned short>(fds[i].revents)));
        if ((fds[i].revents & POLLHUP) != 0 && (watches[i].events & G_EPOLLRDHUP) != 0) {
            ev |= G_EPOLLRDHUP;
        }
        events[count].events = ev;
        events[count].data = watches[i].data;
        count++;
        if ((watches[i].events & G_EPOLLONESHOT) != 0) {
            fired.push_back(fds[i].fd);
        }
    }
    if (!fired.empty()) {
        auto& r = registry();
        std::lock_guard lock{r.mutex};
        const auto set = r.epolls.find(epfd);
        if (set != r.epolls.end()) {
            for (const int fd : fired) {
                const auto it = set->second.find(fd);
                if (it != set->second.end()) {
                    it->second.events &= G_EPOLLET | G_EPOLLONESHOT;
                }
            }
        }
    }
    return count;
}

int aleks2_glibc_epoll_pwait(int epfd, GEpollEvent* events, int max, int timeoutMs, const void*) {
    return aleks2_glibc_epoll_wait(epfd, events, max, timeoutMs);
}

// --- processes: none here ---

int aleks2_glibc_fork() {
    return fail(G_ENOSYS);
}

int aleks2_glibc_execvp(const char*, char* const[]) {
    return fail(G_ENOSYS);
}

int aleks2_glibc_execv(const char*, char* const[]) {
    return fail(G_ENOSYS);
}

int aleks2_glibc_execve(const char*, char* const[], char* const[]) {
    return fail(G_ENOSYS);
}

int aleks2_glibc_waitpid(int, int*, int) {
    return fail(G_ECHILD);
}

int aleks2_glibc_posix_spawn(int*, const char*, const void*, const void*, char* const[], char* const[]) {
    return G_ENOSYS;  // the error is the return value
}

int aleks2_glibc_system(const char* command) {
    if (command == nullptr) {
        return 0;  // no shell
    }
    return fail(G_ENOSYS);
}

}  // extern "C"

namespace dusk::mods::elf::glibc::net {

template <class F>
static uint64_t addr(F* f) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(f));
}

std::vector<Shim> shims() {
    constexpr const char* kAddr = "glibc sockaddr and flags, glibc errno (DM3 sockets layer)";
    constexpr const char* kErr = "glibc errno (DM3 sockets layer)";
    constexpr const char* kEmu = "emulated over loopback sockets (DM3: there is no pipe/eventfd/epoll here)";
    constexpr const char* kProc = "fails as glibc does without processes (DM3)";
    return {
        {"socket", addr(&aleks2_glibc_socket), "glibc AF_*/SOCK_* values, glibc errno (DM3)", true},
        {"socketpair", addr(&aleks2_glibc_socketpair), kEmu, true},
        {"bind", addr(&aleks2_glibc_bind), kAddr, true},
        {"connect", addr(&aleks2_glibc_connect), kAddr, true},
        {"listen", addr(&aleks2_glibc_listen), kErr, true},
        {"accept", addr(&aleks2_glibc_accept), kAddr, true},
        {"accept4", addr(&aleks2_glibc_accept4), kAddr, true},
        {"getsockname", addr(&aleks2_glibc_getsockname), kAddr, true},
        {"getpeername", addr(&aleks2_glibc_getpeername), kAddr, true},
        {"shutdown", addr(&aleks2_glibc_shutdown), kErr, true},
        {"send", addr(&aleks2_glibc_send), kAddr, true},
        {"sendto", addr(&aleks2_glibc_sendto), kAddr, true},
        {"recv", addr(&aleks2_glibc_recv), kAddr, true},
        {"recvfrom", addr(&aleks2_glibc_recvfrom), kAddr, true},
        {"sendmsg", addr(&aleks2_glibc_sendmsg), "glibc msghdr, no ancillary data (DM3)", true},
        {"recvmsg", addr(&aleks2_glibc_recvmsg), "glibc msghdr, no ancillary data (DM3)", true},
        {"setsockopt", addr(&aleks2_glibc_setsockopt), "glibc SOL_*/SO_*/IP_*/TCP_* values (DM3)", true},
        {"getsockopt", addr(&aleks2_glibc_getsockopt), "glibc SOL_*/SO_*/IP_*/TCP_* values (DM3)", true},
        {"ioctl", addr(&aleks2_glibc_ioctl), "glibc FIONBIO/FIONREAD (DM3)", true},
        {"poll", addr(&aleks2_glibc_poll), "glibc POLL* bits (DM3)", true},
        {"__poll", addr(&aleks2_glibc_poll), "glibc POLL* bits (DM3)", true},
        {"ppoll", addr(&aleks2_glibc_ppoll), "glibc POLL* bits (DM3)", true},
        {"select", addr(&aleks2_glibc_select), "glibc 1024-bit fd_set, on poll (DM3)", true},
        {"pselect", addr(&aleks2_glibc_pselect), "glibc 1024-bit fd_set, on poll (DM3)", true},
        {"getaddrinfo", addr(&aleks2_glibc_getaddrinfo), "glibc addrinfo list, IPv4 (DM3)", true},
        {"freeaddrinfo", addr(&aleks2_glibc_freeaddrinfo), "glibc addrinfo list (DM3)", true},
        {"gai_strerror", addr(&aleks2_glibc_gai_strerror), "glibc EAI_* values (DM3)", true},
        {"getnameinfo", addr(&aleks2_glibc_getnameinfo), "glibc sockaddr and NI_* flags (DM3)", true},
        {"inet_pton", addr(&aleks2_glibc_inet_pton), "glibc AF_* values (DM3)", true},
        {"inet_ntop", addr(&aleks2_glibc_inet_ntop), "glibc AF_* values (DM3)", true},
        {"getifaddrs", addr(&aleks2_glibc_getifaddrs), "glibc ifaddrs: loopback and the console's IPv4 (DM3)", true},
        {"freeifaddrs", addr(&aleks2_glibc_freeifaddrs), "glibc ifaddrs (DM3)", true},
        {"uname", addr(&aleks2_glibc_uname), "glibc utsname describing Horizon (DM3)", true},
        {"pipe", addr(&aleks2_glibc_pipe), kEmu, true},
        {"pipe2", addr(&aleks2_glibc_pipe2), kEmu, true},
        {"eventfd", addr(&aleks2_glibc_eventfd), kEmu, true},
        {"eventfd_read", addr(&aleks2_glibc_eventfd_read), kEmu, true},
        {"eventfd_write", addr(&aleks2_glibc_eventfd_write), kEmu, true},
        {"epoll_create", addr(&aleks2_glibc_epoll_create), kEmu, true},
        {"epoll_create1", addr(&aleks2_glibc_epoll_create1), kEmu, true},
        {"epoll_ctl", addr(&aleks2_glibc_epoll_ctl), kEmu, true},
        {"epoll_wait", addr(&aleks2_glibc_epoll_wait), kEmu, true},
        {"epoll_pwait", addr(&aleks2_glibc_epoll_pwait), kEmu, true},
        {"read", addr(&aleks2_glibc_read), kErr, true},
        {"write", addr(&aleks2_glibc_write), kErr, true},
        {"close", addr(&aleks2_glibc_close), kErr, true},
        {"fork", addr(&aleks2_glibc_fork), kProc, true},
        {"vfork", addr(&aleks2_glibc_fork), kProc, true},
        {"execvp", addr(&aleks2_glibc_execvp), kProc, true},
        {"execv", addr(&aleks2_glibc_execv), kProc, true},
        {"execve", addr(&aleks2_glibc_execve), kProc, true},
        {"waitpid", addr(&aleks2_glibc_waitpid), kProc, true},
        {"posix_spawn", addr(&aleks2_glibc_posix_spawn), kProc, true},
        {"posix_spawnp", addr(&aleks2_glibc_posix_spawn), kProc, true},
        {"system", addr(&aleks2_glibc_system), kProc, true},
    };
}

}  // namespace dusk::mods::elf::glibc::net
