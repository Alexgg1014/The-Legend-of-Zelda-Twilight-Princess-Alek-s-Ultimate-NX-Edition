#pragma once
// aleks2 (DM3): the socket API a Linux-built module calls, translated to the C library the game runs
// (newlib/libnx on Switch). It extends the DM2-d glibc ABI layer (glibc_abi.hpp) and reads the same generated
// table (glibc_abi_table.inc): address families, socket types and flags, SOL_SOCKET/SO_*, IP/IPV6/TCP options,
// MSG_*, AI_*/NI_*/EAI_*, FIONBIO/FIONREAD, POLL*, IFF_*, all measured with aarch64-linux-gnu and devkitA64.
//
// What differs and how it is handled (research/dm3/layouts.md, platforms/switch/tools/glibc-abi/abi-table.md):
//   sockaddr   glibc: u16 family at 0. BSD (libnx): u8 sa_len at 0, u8 family at 1. Converted on every call
//              that passes one in or out; the caller's *addrlen is honoured the Linux way (truncate, report
//              the real length).
//   addrinfo   ai_addr and ai_canonname swap places. getaddrinfo builds a glibc-layout list (one block per
//              entry) that the module's freeaddrinfo frees. Horizon's resolver is IPv4: AF_UNSPEC and AF_INET6
//              are asked as AF_INET.
//   msghdr     size_t msg_iovlen/msg_controllen in glibc, int/socklen_t in BSD. Ancillary data is not carried
//              (cmsghdr differs too): sendmsg with control data fails EOPNOTSUPP, recvmsg returns none.
//   fd_set     1024 bits in glibc, 64 in newlib: select/pselect are implemented on poll.
//   constants  translated by table; a glibc option with no native counterpart is either a hint that is
//              accepted and ignored (SO_PRIORITY, IP_MTU_DISCOVER...) or refused with ENOPROTOOPT.
//   errno      every shim leaves a glibc value (the module reads newlib's errno through __errno_location).
//
// Emulated where Horizon has nothing: pipe/pipe2 and socketpair (a TCP loopback pair, UDP if TCP loopback is
// refused), eventfd (a UDP socket connected to itself), epoll (level-triggered, on poll), getifaddrs (loopback
// plus what the platform reports), uname (the platform's description). fork/execvp/waitpid fail the way glibc
// does when it cannot (ENOSYS, ECHILD): the one catalog caller (MFB's xdg-open) has a failure path.
#include "elf_runtime_policy.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dusk::mods::elf::glibc::net {

// One IPv4 interface for getifaddrs; addresses in network byte order.
struct Interface {
    std::string name;
    uint32_t address = 0;
    uint32_t netmask = 0;
    bool loopback = false;
};

// What only the platform knows. Every hook is optional; the defaults are the C library's own answers
// (on a glibc host, which is what the tests use).
struct Platform {
    // DM3 LAN option: may a module's TCP socket bound to 127.0.0.1 be bound to every interface instead?
    bool (*lanListenAllowed)() = nullptr;
    // The interfaces besides loopback (Switch: nifm's current IP configuration).
    std::vector<Interface> (*interfaces)() = nullptr;
    // uname: sysname, release, version, machine (each up to 64 characters; nodename is gethostname()).
    void (*describe)(std::string& sysname, std::string& release, std::string& version, std::string& machine) = nullptr;
    // One line for the game log (level 0 info, 1 warning).
    void (*log)(int level, const std::string& line) = nullptr;
};
void set_platform(const Platform& platform);

// --- translation, exposed for the tests ---
int af_from_glibc(int glibc);
int af_to_glibc(int native);
int eai_to_glibc(int native);
// Bit-flag groups ("msg", "ai", "ni", "poll", "iff"): glibc bits to native and back. Bits with no
// counterpart are dropped (reported through `dropped` when not null).
int bits_from_glibc(const char* group, int glibc, int* dropped = nullptr);
int bits_to_glibc(const char* group, int native);
// Socket option (level, name): {native level, native name}; {-1, -1} unknown; {-2, -2} a hint to ignore.
struct Option {
    int level;
    int name;
};
Option option_from_glibc(int level, int name);

// sockaddr conversion with an explicit layout, so the BSD path is testable on a glibc host.
// in: glibc bytes -> the native layout (Bsd: sa_len, family byte). Returns the native length, 0 if invalid.
template <bool Bsd>
size_t sockaddr_from_glibc(const void* glibc, size_t length, void* out, size_t outSize);
// out: native bytes (nativeLength) -> glibc layout into the caller's buffer of *glibcLength bytes; *glibcLength
// becomes the full length, as Linux reports it.
template <bool Bsd>
void sockaddr_to_glibc(const void* native, size_t nativeLength, void* glibc, uint32_t* glibcLength);

// --- the shims (Shim::linuxOnly), named as glibc names them ---
std::vector<Shim> shims();

// --- lifecycle ---
// shutdown(SHUT_RDWR) on every socket a module opened through these shims, so that its loop sees an error and
// treats it as a disconnect (resume from sleep). Returns how many.
size_t shutdown_module_sockets();
// V1-rc11: at exit, after the mods are unloaded: shutdown and close every socket a module still holds (a Linux mod
// that never closed its own, rc9's co-op session left 4), so none is open when the BSD service is closed. Returns
// how many.
size_t close_module_sockets();

struct Counters {
    size_t sockets = 0;   // open sockets a module created (pipes and eventfds included)
    size_t emulated = 0;  // of those: pipe ends, eventfds, epoll sets
    uint64_t lanRebinds = 0;
    uint64_t getaddrinfoCalls = 0;
};
Counters counters();

}  // namespace dusk::mods::elf::glibc::net
