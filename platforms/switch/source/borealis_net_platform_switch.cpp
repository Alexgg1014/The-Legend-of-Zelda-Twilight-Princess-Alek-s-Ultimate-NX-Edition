// aleks2 (DM3): borealis::net's platform layer for Horizon.
//
// borealis' POSIX layer (extern/borealis/src/net/platform_posix.cpp) almost fits: libnx offers BSD sockets
// with non-blocking mode, poll and getaddrinfo once socketInitialize has run (src/dusk/switch_net.cpp). Three
// things differ, and this file is that layer with them changed:
//   - available(): upstream answers false on __SWITCH__. Here it is "the sockets service is up".
//   - Wakeup: upstream uses eventfd (Linux) or pipe. Horizon has neither, and libnx's poll only accepts
//     socket descriptors, so the I/O thread's wakeup is a UDP socket on 127.0.0.1 connected to itself: wake()
//     sends one byte to itself, drain() reads until empty.
//   - SIGPIPE: there are no signals; MSG_NOSIGNAL exists in libnx and is passed as upstream does.
// Everything else is upstream's code, unchanged. Errors stay native (borealis maps them itself); the glibc
// translation is only for Linux-built modules (src/dusk/mods/loader/glibc_net.cpp).
#include "net/platform.hpp"

#include "dusk/gai_text.hpp"
#include "dusk/switch_net.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>
#include <utility>

namespace borealis::net::detail::platform {

bool available() noexcept {
    return dusk::switchnet::available();
}

bool initialize() noexcept {
    return true;
}

void cleanup() noexcept {}

int last_socket_error() noexcept {
    return errno;
}

bool would_block(int error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK;
}

bool connect_in_progress(int error) noexcept {
    return error == EINPROGRESS || would_block(error);
}

bool interrupted(int error) noexcept {
    return error == EINTR;
}

void close_socket(NativeSocket socket) noexcept {
    if (socket != InvalidSocket) {
        ::close(socket);
    }
}

bool set_nonblocking(NativeSocket socket) noexcept {
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::string socket_error_message(int error) {
    return std::strerror(error);
}

std::string resolve_error_message(int error) {
    return dusk::gai_text(error);  // not gai_strerror: its thread-local buffer grows every thread (gai_text.hpp)
}

Error map_socket_error(int error) noexcept {
    switch (error) {
    case ECONNREFUSED:
        return Error::Refused;
    case EHOSTUNREACH:
    case ENETUNREACH:
    case ENETDOWN:
        return Error::Unreachable;
    case ECONNRESET:
    case ECONNABORTED:
    case EPIPE:
        return Error::Reset;
    case EADDRINUSE:
        return Error::AddressInUse;
    case EACCES:
    case EPERM:
        return Error::Permission;
    case ETIMEDOUT:
        return Error::Timeout;
    default:
        return Error::Network;
    }
}

bool set_socket_option(NativeSocket socket, int level, int name, const void* value, size_t size) noexcept {
    return setsockopt(socket, level, name, value, static_cast<SockLength>(size)) == 0;
}

void configure_common_socket(NativeSocket) noexcept {}

void configure_listener_reuse(NativeSocket socket) noexcept {
    const int enabled = 1;
    set_socket_option(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
}

void shutdown_send(NativeSocket socket) noexcept {
    ::shutdown(socket, SHUT_WR);
}

void prepare_connect(NativeSocket, const sockaddr*) noexcept {}

int socket_error(NativeSocket socket) noexcept {
    int error = 0;
    SockLength length = sizeof(error);
    if (getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
        return last_socket_error();
    }
    return error;
}

std::ptrdiff_t send_bytes(NativeSocket socket, const void* data, size_t size) noexcept {
    return ::send(socket, data, size, MSG_NOSIGNAL);
}

std::ptrdiff_t receive_bytes(NativeSocket socket, void* data, size_t size) noexcept {
    return ::recv(socket, data, size, 0);
}

std::ptrdiff_t send_datagram(NativeSocket socket, const void* data, size_t size, const sockaddr* destination,
    SockLength destinationLength) noexcept {
    return ::sendto(socket, data, size, MSG_NOSIGNAL, destination, destinationLength);
}

std::ptrdiff_t receive_datagram(
    NativeSocket socket, void* data, size_t size, sockaddr* source, SockLength* sourceLength) noexcept {
    return ::recvfrom(socket, data, size, 0, source, sourceLength);
}

int poll(std::span<PollDescriptor> descriptors, std::chrono::milliseconds timeout) noexcept {
    return ::poll(descriptors.data(), descriptors.size(), static_cast<int>(timeout.count()));
}

bool Wakeup::open() noexcept {
    const int descriptor = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (descriptor < 0) {
        return false;
    }
    sockaddr_in address{};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    SockLength length = sizeof(address);
    if (::bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &length) != 0 ||
        ::connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        !set_nonblocking(descriptor))
    {
        ::close(descriptor);
        return false;
    }
    std::lock_guard lock{m_mutex};
    m_read = descriptor;
    m_write = descriptor;
    return true;
}

void Wakeup::close() noexcept {
    std::lock_guard lock{m_mutex};
    const NativeSocket reader = std::exchange(m_read, InvalidSocket);
    const NativeSocket writer = std::exchange(m_write, InvalidSocket);
    close_socket(reader);
    if (writer != reader) {
        close_socket(writer);
    }
}

void Wakeup::wake() noexcept {
    std::lock_guard lock{m_mutex};
    if (m_write == InvalidSocket) {
        return;
    }
    const char byte = 0;
    ::send(m_write, &byte, 1, MSG_DONTWAIT);
}

void Wakeup::drain() noexcept {
    std::lock_guard lock{m_mutex};
    if (m_read == InvalidSocket) {
        return;
    }
    std::array<char, 256> buffer{};
    while (::recv(m_read, buffer.data(), buffer.size(), MSG_DONTWAIT) > 0) {
    }
}

NativeSocket Wakeup::descriptor() const noexcept {
    std::lock_guard lock{m_mutex};
    return m_read;
}

}  // namespace borealis::net::detail::platform
