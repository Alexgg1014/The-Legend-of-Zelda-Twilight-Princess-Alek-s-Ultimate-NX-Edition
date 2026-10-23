// aleks2 (V1-rc1, rc3): getaddrinfo, freeaddrinfo and getnameinfo for the whole program, wrapped at link time
// (-Wl,--wrap=..., platforms/switch/CMakeLists.txt). Every caller goes through here: borealis::net, the
// WebSocket transport, libcurl and the glibc layer for Linux-built mods (which first turns glibc flags into
// these newlib ones).
//
// Why: measured on the console with DM3-c1 (Crests of Courage 1.10.0 joining a PC). borealis::net formats
// the source of every received datagram with getnameinfo(NI_NUMERICHOST | NI_NUMERICSERV), and Horizon's
// resolver answered with the PC's short hostname ("udp://LAPTOP-DMU3QHRL:27716"), i.e. it did a reverse
// lookup. Every such call blocked the calling thread (the 1.6 s and 5 s [HITCH] frames), and sending back to
// that name needed a forward lookup that failed ("udp send ... refused", "host did not answer hello").
// Which flag values sfdnsres really expects cannot be checked off the console, so the numeric work is not
// left to it: numeric answers are built here (inet_ntop / inet_pton, the port in decimal), with no IPC.
//
// A real lookup (a name, or a reverse lookup that was asked for) goes to the system on the caller's thread.
// rc1 ran it on a helper std::thread with a bounded wait; on the console that helper ended in std::terminate
// -> abort -> __appExit, which unmounts sdmc while the other threads still use it (the rc1/rc2 crashes in
// _read_r/_write_r with devoptab_list[sdmc] == NULL). The callers that resolve names already do it off the
// game thread (borealis::net's resolver executor, the WebSocket thread, libcurl), so no helper is needed.
//
// Horizon's resolver is IPv4 only, so AF_INET is the only family answered locally; anything else goes
// through unchanged. Built for the host too (platforms/switch/tools/test-netdb.sh), where sockaddr_in has
// no sin_len.
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_set>

extern "C" {
int __real_getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** res);
void __real_freeaddrinfo(addrinfo* res);
int __real_getnameinfo(const sockaddr* sa, socklen_t salen, char* host, socklen_t hostlen, char* serv,
    socklen_t servlen, int flags);
int __wrap_getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** res);
void __wrap_freeaddrinfo(addrinfo* res);
int __wrap_getnameinfo(const sockaddr* sa, socklen_t salen, char* host, socklen_t hostlen, char* serv,
    socklen_t servlen, int flags);
}

namespace {

std::mutex g_localMutex;
std::unordered_set<addrinfo*> g_local;  // heads built here; freed here

bool parse_port(const char* service, uint16_t* port) {
    if (service == nullptr || *service == '\0') {
        *port = 0;
        return true;
    }
    unsigned long value = 0;
    for (const char* c = service; *c != '\0'; ++c) {
        if (*c < '0' || *c > '9') {
            return false;
        }
        value = value * 10 + static_cast<unsigned long>(*c - '0');
        if (value > 65535) {
            return false;
        }
    }
    *port = static_cast<uint16_t>(value);
    return true;
}

// One addrinfo, its sockaddr_in and the canonical name in one block.
addrinfo* make_entry(in_addr address, uint16_t port, int socktype, int protocol, int flags, const char* canon) {
    const size_t canonLen = canon != nullptr ? std::strlen(canon) + 1 : 0;
    auto* block = static_cast<unsigned char*>(std::calloc(1, sizeof(addrinfo) + sizeof(sockaddr_in) + canonLen));
    if (block == nullptr) {
        return nullptr;
    }
    auto* ai = reinterpret_cast<addrinfo*>(block);
    auto* sin = reinterpret_cast<sockaddr_in*>(block + sizeof(addrinfo));
#ifdef __SWITCH__
    sin->sin_len = sizeof(sockaddr_in);
#endif
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    sin->sin_addr = address;
    ai->ai_flags = flags;
    ai->ai_family = AF_INET;
    ai->ai_socktype = socktype;
    ai->ai_protocol = protocol;
    ai->ai_addrlen = sizeof(sockaddr_in);
    ai->ai_addr = reinterpret_cast<sockaddr*>(sin);
    if (canonLen != 0) {
        ai->ai_canonname = reinterpret_cast<char*>(block + sizeof(addrinfo) + sizeof(sockaddr_in));
        std::memcpy(ai->ai_canonname, canon, canonLen);
    }
    return ai;
}

void free_local_chain(addrinfo* head) {
    while (head != nullptr) {
        addrinfo* next = head->ai_next;
        std::free(head);  // one block per entry (make_entry)
        head = next;
    }
}

// Numeric host (or none) and numeric service: answered without the resolver. Returns -1 when it does not apply.
int local_getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** res) {
    const int family = hints != nullptr ? hints->ai_family : AF_UNSPEC;
    if (family != AF_UNSPEC && family != AF_INET) {
        return -1;
    }
    const int flags = hints != nullptr ? hints->ai_flags : 0;
    in_addr address{};
    if (node == nullptr) {
        address.s_addr = htonl((flags & AI_PASSIVE) != 0 ? INADDR_ANY : INADDR_LOOPBACK);
    } else if (inet_pton(AF_INET, node, &address) != 1) {
        return -1;
    }
    uint16_t port = 0;
    if (!parse_port(service, &port)) {
        return -1;
    }
    const int socktype = hints != nullptr ? hints->ai_socktype : 0;
    const int protocol = hints != nullptr ? hints->ai_protocol : 0;
    const char* canon = (flags & AI_CANONNAME) != 0 && node != nullptr ? node : nullptr;
    addrinfo* head = nullptr;
    addrinfo** tail = &head;
    auto add = [&](int type, int proto) -> bool {
        addrinfo* entry = make_entry(address, port, type, proto, flags, head == nullptr ? canon : nullptr);
        if (entry == nullptr) {
            return false;
        }
        *tail = entry;
        tail = &entry->ai_next;
        return true;
    };
    bool ok = true;
    if (socktype == 0) {
        ok = (protocol == 0 || protocol == IPPROTO_TCP ? add(SOCK_STREAM, IPPROTO_TCP) : true) &&
             (protocol == 0 || protocol == IPPROTO_UDP ? add(SOCK_DGRAM, IPPROTO_UDP) : true);
    } else {
        int proto = protocol;
        if (proto == 0) {
            proto = socktype == SOCK_STREAM ? IPPROTO_TCP : socktype == SOCK_DGRAM ? IPPROTO_UDP : 0;
        }
        ok = add(socktype, proto);
    }
    if (!ok || head == nullptr) {
        free_local_chain(head);
        return head == nullptr && ok ? EAI_SOCKTYPE : EAI_MEMORY;
    }
    {
        std::lock_guard lock(g_localMutex);
        g_local.insert(head);
    }
    *res = head;
    return 0;
}

}  // namespace

extern "C" int __wrap_getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** res) {
    if (res == nullptr || (node == nullptr && service == nullptr)) {
        return EAI_NONAME;
    }
    const int local = local_getaddrinfo(node, service, hints, res);
    if (local >= 0) {
        return local;
    }
    return __real_getaddrinfo(node, service, hints, res);
}

extern "C" void __wrap_freeaddrinfo(addrinfo* res) {
    if (res == nullptr) {
        return;
    }
    {
        std::lock_guard lock(g_localMutex);
        if (g_local.erase(res) != 0) {
            free_local_chain(res);
            return;
        }
    }
    __real_freeaddrinfo(res);
}

extern "C" int __wrap_getnameinfo(const sockaddr* sa, socklen_t salen, char* host, socklen_t hostlen, char* serv,
    socklen_t servlen, int flags) {
    if (sa == nullptr || sa->sa_family != AF_INET || salen < static_cast<socklen_t>(sizeof(sockaddr_in))) {
        return __real_getnameinfo(sa, salen, host, hostlen, serv, servlen, flags);
    }
    sockaddr_in address{};
    std::memcpy(&address, sa, sizeof(address));
    if (serv != nullptr && servlen != 0) {
        // Always the number: Horizon has no services database worth a round trip.
        const int n = std::snprintf(serv, servlen, "%u", static_cast<unsigned>(ntohs(address.sin_port)));
        if (n < 0 || static_cast<socklen_t>(n) >= servlen) {
            return EAI_OVERFLOW;
        }
    }
    if (host == nullptr || hostlen == 0) {
        return 0;
    }
    char numeric[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &address.sin_addr, numeric, sizeof(numeric)) == nullptr) {
        return EAI_FAIL;
    }
    if ((flags & NI_NUMERICHOST) == 0) {
        // A reverse lookup was really asked for: the system resolver, with flags 0 (whatever meaning sfdnsres
        // gives the other bits, 0 is "a name if there is one").
        char name[NI_MAXHOST]{};
        if (__real_getnameinfo(reinterpret_cast<const sockaddr*>(&address), sizeof(address), name, sizeof(name),
                nullptr, 0, 0) == 0 &&
            name[0] != '\0' && std::strcmp(name, numeric) != 0)
        {
            if (std::strlen(name) >= hostlen) {
                return EAI_OVERFLOW;
            }
            std::strcpy(host, name);
            return 0;
        }
        if ((flags & NI_NAMEREQD) != 0) {
            return EAI_NONAME;
        }
    }
    if (std::strlen(numeric) >= hostlen) {
        return EAI_OVERFLOW;
    }
    std::strcpy(host, numeric);
    return 0;
}
