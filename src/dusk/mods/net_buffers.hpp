#pragma once

// aleks2 (V1-rc5): socket buffer sizes for mods on the Switch. All of the process's sockets share one buffer
// pool (switch_net.cpp: sb_efficiency 4 x (TCP 256+256 KiB + UDP 9+41 KiB) = ~2.2 MiB). The mod net service
// asked for 1 MiB receive + 1 MiB send on every UDP socket (Crests of Courage's GLOBAL room), which left too
// little for anything else: on the rc4 console the browser, the image downloads and the update check failed
// with "Couldn't connect to server" (ENOBUFS, "No buffer space available" in the co-op's own log) while the
// co-op's WebSocket, opened first, kept working. A game datagram is at most a few KiB; 64 KiB queues dozens.
namespace dusk::mods::net_buffers {

inline constexpr int kModUdpReceiveBytes = 64 * 1024;
inline constexpr int kModUdpSendBytes = 32 * 1024;
// The most a Linux-built mod's setsockopt(SO_RCVBUF/SO_SNDBUF) gets.
inline constexpr int kModSocketBufferMax = 64 * 1024;

constexpr int clamp_mod_socket_buffer(int requested) {
    return requested > kModSocketBufferMax ? kModSocketBufferMax : requested;
}

}  // namespace dusk::mods::net_buffers
