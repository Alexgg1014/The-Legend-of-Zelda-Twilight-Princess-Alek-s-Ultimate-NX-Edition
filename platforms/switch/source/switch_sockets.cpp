// aleks2 (V1-rc5): socket(), connect() and close() for the whole program, wrapped at link time
// (-Wl,--wrap=..., platforms/switch/CMakeLists.txt), only to count. Every socket the game, libcurl, borealis::net
// or a mod opens goes through here: how many are open, how many were made and closed, and how many attempts
// failed with ENOBUFS (the per-process socket buffer pool is full). The session heartbeat prints them.
//
// Why: on the rc4 console the mod browser and the update check failed with "Couldn't connect to server" while
// the co-op WebSocket stayed connected. A socket leak was suspected; the logs pointed instead at the buffer
// pool (a mod UDP socket asked for 1 MiB + 1 MiB of a ~2.2 MiB pool, then "No buffer space available"). The
// counters tell the two apart on the console: a leak keeps `open` growing, a full pool shows in `enobufs`.
//
// Nothing changes for the caller: the real call runs and its result and errno come back untouched. Files are
// never counted (close() of an fd not made by socket() is passed straight through). Built for the host too
// (platforms/switch/tools/test-netpool.sh).
#include <sys/socket.h>

#include <atomic>
#include <cerrno>
#include <cstdint>

extern "C" {
int __real_socket(int domain, int type, int protocol);
int __real_connect(int fd, const struct sockaddr* address, socklen_t length);
int __real_close(int fd);

struct aleks2_socket_counters {
    uint32_t open;
    uint32_t created;
    uint32_t closed;
    uint32_t enobufs;
    uint32_t connectFailed;
    int lastErrno;  // of the last failed socket()/connect() (EINPROGRESS and EAGAIN are not failures)
};
void aleks2_get_socket_counters(struct aleks2_socket_counters* out);
// Optional, for the host test: told when a counted socket is closed.
__attribute__((weak)) void aleks2_socket_closed(int fd);
}

namespace {

constexpr int kMaxTracked = 1024;  // newlib's fds are small; anything above is passed through uncounted
std::atomic<uint8_t> s_isSocket[kMaxTracked];
std::atomic<uint32_t> s_open{0}, s_created{0}, s_closed{0}, s_enobufs{0}, s_connectFailed{0};
std::atomic<int> s_lastErrno{0};

void note_failure(int error) {
    s_lastErrno.store(error, std::memory_order_relaxed);
    if (error == ENOBUFS) {
        s_enobufs.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

extern "C" int __wrap_socket(int domain, int type, int protocol) {
    const int fd = __real_socket(domain, type, protocol);
    if (fd < 0) {
        const int error = errno;
        note_failure(error);
        errno = error;
        return fd;
    }
    s_created.fetch_add(1, std::memory_order_relaxed);
    if (fd < kMaxTracked && s_isSocket[fd].exchange(1, std::memory_order_relaxed) == 0) {
        s_open.fetch_add(1, std::memory_order_relaxed);
    }
    return fd;
}

extern "C" int __wrap_connect(int fd, const struct sockaddr* address, socklen_t length) {
    const int r = __real_connect(fd, address, length);
    if (r != 0) {
        const int error = errno;
        if (error != EINPROGRESS && error != EAGAIN && error != EALREADY) {
            s_connectFailed.fetch_add(1, std::memory_order_relaxed);
            note_failure(error);
        }
        errno = error;
    }
    return r;
}

extern "C" int __wrap_close(int fd) {
    const bool counted = fd >= 0 && fd < kMaxTracked && s_isSocket[fd].exchange(0, std::memory_order_relaxed) != 0;
    if (counted) {
        // Before the real close: once it returns the number may already be another socket's.
        s_open.fetch_sub(1, std::memory_order_relaxed);
        s_closed.fetch_add(1, std::memory_order_relaxed);
        if (aleks2_socket_closed != nullptr) {
            aleks2_socket_closed(fd);
        }
    }
    return __real_close(fd);
}

// V1-rc11: whether this process ever made a socket (switch_startup.cpp, aleks2_relaunch_on_exit).
extern "C" uint32_t aleks2_sockets_created(void) {
    return s_created.load(std::memory_order_relaxed);
}

extern "C" void aleks2_get_socket_counters(struct aleks2_socket_counters* out) {
    if (out == nullptr) {
        return;
    }
    out->open = s_open.load(std::memory_order_relaxed);
    out->created = s_created.load(std::memory_order_relaxed);
    out->closed = s_closed.load(std::memory_order_relaxed);
    out->enobufs = s_enobufs.load(std::memory_order_relaxed);
    out->connectFailed = s_connectFailed.load(std::memory_order_relaxed);
    out->lastErrno = s_lastErrno.load(std::memory_order_relaxed);
}
