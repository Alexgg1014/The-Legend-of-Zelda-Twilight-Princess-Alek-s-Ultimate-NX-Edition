"""DM2-d: the glibc-vs-newlib values the general ABI layer translates (errno, clock ids, open/fcntl flags,
struct tm), measured with both toolchains. DM3 adds the sockets layer (address families, socket types and
flags, SOL_SOCKET/SO_*, IP/IPV6/TCP options, MSG_*, AI_*/NI_*/EAI_*, FIONBIO/FIONREAD, POLL*, IFF_*) and the
layouts it converts (sockaddr, addrinfo, msghdr, fd_set, ifaddrs, utsname, epoll_event). Same method as research/dm3/layout_probe.py: one int64 per fact
in .data; -999999 means "not defined by this C library".

usage: python -I abi_probe.py gen OUT.c
       python -I abi_probe.py decode OBJDUMP_TXT OUT.json
       python -I abi_probe.py table GLIBC.json NEWLIB.json OUT.inc OUT.md   (C++ table for the shims + report)
"""
import json
import re
import sys

# Every errno name Linux/glibc defines on aarch64 (asm-generic/errno-base.h, asm-generic/errno.h, glibc aliases).
ERRNO = """EPERM ENOENT ESRCH EINTR EIO ENXIO E2BIG ENOEXEC EBADF ECHILD EAGAIN ENOMEM EACCES EFAULT ENOTBLK EBUSY
EEXIST EXDEV ENODEV ENOTDIR EISDIR EINVAL ENFILE EMFILE ENOTTY ETXTBSY EFBIG ENOSPC ESPIPE EROFS EMLINK EPIPE EDOM
ERANGE EDEADLK ENAMETOOLONG ENOLCK ENOSYS ENOTEMPTY ELOOP ENOMSG EIDRM ECHRNG EL2NSYNC EL3HLT EL3RST ELNRNG EUNATCH
ENOCSI EL2HLT EBADE EBADR EXFULL ENOANO EBADRQC EBADSLT EBFONT ENOSTR ENODATA ETIME ENOSR ENONET ENOPKG EREMOTE
ENOLINK EADV ESRMNT ECOMM EPROTO EMULTIHOP EDOTDOT EBADMSG EOVERFLOW ENOTUNIQ EBADFD EREMCHG ELIBACC ELIBBAD ELIBSCN
ELIBMAX ELIBEXEC EILSEQ ERESTART ESTRPIPE EUSERS ENOTSOCK EDESTADDRREQ EMSGSIZE EPROTOTYPE ENOPROTOOPT EPROTONOSUPPORT
ESOCKTNOSUPPORT EOPNOTSUPP EPFNOSUPPORT EAFNOSUPPORT EADDRINUSE EADDRNOTAVAIL ENETDOWN ENETUNREACH ENETRESET
ECONNABORTED ECONNRESET ENOBUFS EISCONN ENOTCONN ESHUTDOWN ETOOMANYREFS ETIMEDOUT ECONNREFUSED EHOSTDOWN EHOSTUNREACH
EALREADY EINPROGRESS ESTALE EUCLEAN ENOTNAM ENAVAIL EISNAM EREMOTEIO EDQUOT ENOMEDIUM EMEDIUMTYPE ECANCELED ENOKEY
EKEYEXPIRED EKEYREVOKED EKEYREJECTED EOWNERDEAD ENOTRECOVERABLE ERFKILL EHWPOISON ENOTSUP EWOULDBLOCK EDEADLOCK""".split()
CLOCKS = """CLOCK_REALTIME CLOCK_MONOTONIC CLOCK_PROCESS_CPUTIME_ID CLOCK_THREAD_CPUTIME_ID CLOCK_MONOTONIC_RAW
CLOCK_REALTIME_COARSE CLOCK_MONOTONIC_COARSE CLOCK_BOOTTIME CLOCK_REALTIME_ALARM CLOCK_BOOTTIME_ALARM CLOCK_TAI""".split()
OFLAGS = """O_RDONLY O_WRONLY O_RDWR O_ACCMODE O_CREAT O_EXCL O_NOCTTY O_TRUNC O_APPEND O_NONBLOCK O_DSYNC O_SYNC O_RSYNC
O_DIRECTORY O_NOFOLLOW O_CLOEXEC O_DIRECT O_LARGEFILE O_NOATIME O_PATH O_TMPFILE O_ASYNC""".split()
FCMDS = """F_DUPFD F_GETFD F_SETFD F_GETFL F_SETFL F_GETLK F_SETLK F_SETLKW F_SETOWN F_GETOWN F_DUPFD_CLOEXEC FD_CLOEXEC
AT_FDCWD AT_REMOVEDIR AT_SYMLINK_NOFOLLOW""".split()
# DM3: sockets. Every name a Linux module can pass to the socket API, by group (the group decides the map).
NET_GROUPS = {
    "af": "AF_UNSPEC AF_UNIX AF_INET AF_INET6".split(),
    "sock": "SOCK_STREAM SOCK_DGRAM SOCK_RAW SOCK_SEQPACKET SOCK_NONBLOCK SOCK_CLOEXEC".split(),
    "level": "SOL_SOCKET IPPROTO_IP IPPROTO_ICMP IPPROTO_TCP IPPROTO_UDP IPPROTO_IPV6".split(),
    "so": """SO_DEBUG SO_REUSEADDR SO_TYPE SO_ERROR SO_DONTROUTE SO_BROADCAST SO_SNDBUF SO_RCVBUF SO_KEEPALIVE
SO_OOBINLINE SO_LINGER SO_REUSEPORT SO_RCVLOWAT SO_SNDLOWAT SO_RCVTIMEO SO_SNDTIMEO SO_ACCEPTCONN SO_PRIORITY
SO_BINDTODEVICE SO_TIMESTAMP SO_NO_CHECK SO_PASSCRED SO_PEERCRED SO_RCVBUFFORCE SO_SNDBUFFORCE SO_BUSY_POLL
SO_INCOMING_CPU SO_ZEROCOPY""".split(),
    "ipopt": """IP_TOS IP_TTL IP_HDRINCL IP_OPTIONS IP_MULTICAST_IF IP_MULTICAST_TTL IP_MULTICAST_LOOP IP_ADD_MEMBERSHIP
IP_DROP_MEMBERSHIP IP_PKTINFO IP_RECVERR IP_MTU_DISCOVER IP_RECVTOS IP_RECVTTL IP_FREEBIND""".split(),
    "ipv6opt": """IPV6_V6ONLY IPV6_UNICAST_HOPS IPV6_MULTICAST_IF IPV6_MULTICAST_HOPS IPV6_MULTICAST_LOOP IPV6_JOIN_GROUP
IPV6_LEAVE_GROUP IPV6_RECVPKTINFO IPV6_TCLASS""".split(),
    "tcpopt": "TCP_NODELAY TCP_MAXSEG TCP_KEEPIDLE TCP_KEEPINTVL TCP_KEEPCNT TCP_QUICKACK TCP_USER_TIMEOUT TCP_CORK".split(),
    "msg": """MSG_OOB MSG_PEEK MSG_DONTROUTE MSG_CTRUNC MSG_TRUNC MSG_DONTWAIT MSG_EOR MSG_WAITALL MSG_NOSIGNAL
MSG_CONFIRM MSG_MORE MSG_ERRQUEUE MSG_CMSG_CLOEXEC""".split(),
    "shut": "SHUT_RD SHUT_WR SHUT_RDWR".split(),
    "ai": "AI_PASSIVE AI_CANONNAME AI_NUMERICHOST AI_V4MAPPED AI_ALL AI_ADDRCONFIG AI_NUMERICSERV".split(),
    "ni": "NI_NUMERICHOST NI_NUMERICSERV NI_NOFQDN NI_NAMEREQD NI_DGRAM NI_MAXHOST NI_MAXSERV".split(),
    "eai": """EAI_BADFLAGS EAI_NONAME EAI_AGAIN EAI_FAIL EAI_FAMILY EAI_SOCKTYPE EAI_SERVICE EAI_MEMORY EAI_SYSTEM
EAI_OVERFLOW EAI_NODATA EAI_ADDRFAMILY""".split(),
    "ioctl": "FIONBIO FIONREAD FIOASYNC SIOCGIFADDR SIOCGIFCONF".split(),
    "poll": "POLLIN POLLPRI POLLOUT POLLERR POLLHUP POLLNVAL POLLRDNORM POLLRDBAND POLLWRNORM POLLWRBAND".split(),
    "iff": "IFF_UP IFF_BROADCAST IFF_LOOPBACK IFF_POINTOPOINT IFF_RUNNING IFF_MULTICAST".split(),
}
NET = [n for names in NET_GROUPS.values() for n in names]
# Layouts the shims convert. glibc-only facts (ifaddrs, epoll_event, utsname) read -999999 on newlib.
NET_LAYOUT = [
    ("sizeof(struct sockaddr)", "sizeof(struct sockaddr)"),
    ("offsetof(sockaddr, sa_family)", "offsetof(struct sockaddr, sa_family)"),
    ("sizeof(sa_family_t)", "sizeof(sa_family_t)"),
    ("sizeof(struct sockaddr_in)", "sizeof(struct sockaddr_in)"),
    ("offsetof(sockaddr_in, sin_port)", "offsetof(struct sockaddr_in, sin_port)"),
    ("offsetof(sockaddr_in, sin_addr)", "offsetof(struct sockaddr_in, sin_addr)"),
    ("sizeof(struct sockaddr_in6)", "sizeof(struct sockaddr_in6)"),
    ("offsetof(sockaddr_in6, sin6_port)", "offsetof(struct sockaddr_in6, sin6_port)"),
    ("offsetof(sockaddr_in6, sin6_flowinfo)", "offsetof(struct sockaddr_in6, sin6_flowinfo)"),
    ("offsetof(sockaddr_in6, sin6_addr)", "offsetof(struct sockaddr_in6, sin6_addr)"),
    ("offsetof(sockaddr_in6, sin6_scope_id)", "offsetof(struct sockaddr_in6, sin6_scope_id)"),
    ("sizeof(struct sockaddr_storage)", "sizeof(struct sockaddr_storage)"),
    ("sizeof(socklen_t)", "sizeof(socklen_t)"),
    ("sizeof(struct addrinfo)", "sizeof(struct addrinfo)"),
    ("offsetof(addrinfo, ai_addrlen)", "offsetof(struct addrinfo, ai_addrlen)"),
    ("offsetof(addrinfo, ai_addr)", "offsetof(struct addrinfo, ai_addr)"),
    ("offsetof(addrinfo, ai_canonname)", "offsetof(struct addrinfo, ai_canonname)"),
    ("offsetof(addrinfo, ai_next)", "offsetof(struct addrinfo, ai_next)"),
    ("sizeof(struct msghdr)", "sizeof(struct msghdr)"),
    ("offsetof(msghdr, msg_namelen)", "offsetof(struct msghdr, msg_namelen)"),
    ("offsetof(msghdr, msg_iov)", "offsetof(struct msghdr, msg_iov)"),
    ("offsetof(msghdr, msg_iovlen)", "offsetof(struct msghdr, msg_iovlen)"),
    ("offsetof(msghdr, msg_control)", "offsetof(struct msghdr, msg_control)"),
    ("offsetof(msghdr, msg_controllen)", "offsetof(struct msghdr, msg_controllen)"),
    ("offsetof(msghdr, msg_flags)", "offsetof(struct msghdr, msg_flags)"),
    ("sizeof(struct pollfd)", "sizeof(struct pollfd)"),
    ("sizeof(nfds_t)", "sizeof(nfds_t)"),
    ("sizeof(fd_set)", "sizeof(fd_set)"),
    ("FD_SETSIZE", "FD_SETSIZE"),
    ("sizeof(struct linger)", "sizeof(struct linger)"),
    ("sizeof(struct ip_mreq)", "sizeof(struct ip_mreq)"),
    ("sizeof(struct timeval)", "sizeof(struct timeval)"),
    ("sizeof(struct hostent)", "sizeof(struct hostent)"),
    ("offsetof(hostent, h_addrtype)", "offsetof(struct hostent, h_addrtype)"),
    ("offsetof(hostent, h_addr_list)", "offsetof(struct hostent, h_addr_list)"),
]
NET_LAYOUT_GLIBC_ONLY = [
    ("sizeof(struct ifaddrs)", "sizeof(struct ifaddrs)"),
    ("offsetof(ifaddrs, ifa_name)", "offsetof(struct ifaddrs, ifa_name)"),
    ("offsetof(ifaddrs, ifa_flags)", "offsetof(struct ifaddrs, ifa_flags)"),
    ("offsetof(ifaddrs, ifa_addr)", "offsetof(struct ifaddrs, ifa_addr)"),
    ("offsetof(ifaddrs, ifa_netmask)", "offsetof(struct ifaddrs, ifa_netmask)"),
    ("offsetof(ifaddrs, ifa_ifu)", "offsetof(struct ifaddrs, ifa_ifu)"),
    ("offsetof(ifaddrs, ifa_data)", "offsetof(struct ifaddrs, ifa_data)"),
    ("sizeof(struct utsname)", "sizeof(struct utsname)"),
    ("sizeof(struct epoll_event)", "sizeof(struct epoll_event)"),
    ("offsetof(epoll_event, data)", "offsetof(struct epoll_event, data)"),
    ("EPOLLIN", "EPOLLIN"), ("EPOLLOUT", "EPOLLOUT"), ("EPOLLERR", "EPOLLERR"), ("EPOLLHUP", "EPOLLHUP"),
    ("EPOLLRDHUP", "EPOLLRDHUP"), ("EPOLLPRI", "EPOLLPRI"), ("EPOLLET", "EPOLLET"), ("EPOLLONESHOT", "EPOLLONESHOT"),
    ("EPOLL_CTL_ADD", "EPOLL_CTL_ADD"), ("EPOLL_CTL_DEL", "EPOLL_CTL_DEL"), ("EPOLL_CTL_MOD", "EPOLL_CTL_MOD"),
    ("EPOLL_CLOEXEC", "EPOLL_CLOEXEC"), ("EFD_SEMAPHORE", "EFD_SEMAPHORE"), ("EFD_NONBLOCK", "EFD_NONBLOCK"),
    ("EFD_CLOEXEC", "EFD_CLOEXEC"),
]

TM = [("sizeof(struct tm)", "sizeof(struct tm)")] + [
    (f"offsetof(tm, {f})", f"offsetof(struct tm, {f})")
    for f in ("tm_sec", "tm_min", "tm_hour", "tm_mday", "tm_mon", "tm_year", "tm_wday", "tm_yday", "tm_isdst")]
TM_GLIBC_ONLY = [("offsetof(tm, tm_gmtoff)", "offsetof(struct tm, tm_gmtoff)"),
                 ("offsetof(tm, tm_zone)", "offsetof(struct tm, tm_zone)")]

FACTS = ([(n, n) for n in ERRNO] + [(n, n) for n in CLOCKS] + [(n, n) for n in OFLAGS] + [(n, n) for n in FCMDS]
         + TM + TM_GLIBC_ONLY + [(n, n) for n in NET] + NET_LAYOUT + NET_LAYOUT_GLIBC_ONLY)
_LAYOUT_NAMES = {n for n, _ in NET_LAYOUT} - {"FD_SETSIZE"}
_GLIBC_ONLY = {n for n, _ in NET_LAYOUT_GLIBC_ONLY if n.startswith(("sizeof", "offsetof"))}


def guard(name, expr):
    if name in _LAYOUT_NAMES:
        return f"(int64_t)({expr})"
    if name in _GLIBC_ONLY:
        return f"\n#ifdef __GLIBC__\n    (int64_t)({expr})\n#else\n    -999999\n#endif\n   "
    if name.startswith(("sizeof", "offsetof(tm, tm_s", "offsetof(tm, tm_m", "offsetof(tm, tm_h", "offsetof(tm, tm_y",
                        "offsetof(tm, tm_w", "offsetof(tm, tm_i")):
        return f"(int64_t)({expr})"
    if name in ("offsetof(tm, tm_gmtoff)", "offsetof(tm, tm_zone)"):
        return f"\n#ifdef __GLIBC__\n    (int64_t)({expr})\n#else\n    -999999\n#endif\n   "
    return f"\n#ifdef {name}\n    (int64_t)({name})\n#else\n    -999999\n#endif\n   "


def gen(path):
    body = """#define _GNU_SOURCE 1
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <net/if.h>
#ifdef __GLIBC__
#include <ifaddrs.h>
#include <sys/utsname.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#endif
__attribute__((used)) int64_t abi_probe[] = {
"""
    body += "".join(f"    {guard(n, e)},\n" for n, e in FACTS)
    body += "};\n"
    open(path, "w").write(body)


def decode(objdump_txt, out):
    vals = bytearray()
    in_data = False
    for line in open(objdump_txt):
        if line.startswith("Contents of section"):
            in_data = ".data" in line
            continue
        if in_data:
            m = re.match(r"\s*[0-9a-f]+ ((?:[0-9a-f]{2,8} ?){1,4})", line)
            if m:
                vals += bytes.fromhex(m.group(1).replace(" ", ""))
    values = [int.from_bytes(vals[8 * i:8 * i + 8], "little", signed=True) for i in range(len(FACTS))]
    json.dump(dict(zip([f for f, _ in FACTS], values)), open(out, "w"), indent=1)


def table(gj, nj, inc, md):
    g, n = json.load(open(gj)), json.load(open(nj))
    NA = -999999
    lines = ["// Generated by platforms/switch/tools/glibc-abi/abi_probe.py from values measured with aarch64-linux-gnu (glibc 2.36)",
             "// and devkitA64 (newlib/libnx). {group, name, glibc value, native value}: the native value is the one",
             "// of the C library this file is compiled against (kNotDefined if it has no such name); the comment",
             "// is what devkitA64's newlib measured. Do not edit by hand: regenerate (platforms/switch/tools/glibc-abi/abi_probe.py), then run test-dm2d.sh and check_newlib_table.py.", ""]
    for group, names in (("errno", ERRNO), ("clock", CLOCKS), ("oflag", OFLAGS), ("fcntl", FCMDS),
                         *NET_GROUPS.items()):
        for name in names:
            if g[name] == NA:
                continue  # nothing a glibc module could pass
            lines += [f"{{\"{group}\", \"{name}\", {g[name]},  // newlib: {'n/a' if n[name] == NA else n[name]}",
                      f"#ifdef {name}", f"    {name}", "#else", "    kNotDefined", "#endif", "},"]
    open(inc, "w").write("\n".join(lines) + "\n")
    rep = ["| Nombre | glibc | newlib | |", "|---|---|---|---|"]
    diff = 0
    for name, _ in FACTS:
        gv, nv = g[name], n[name]
        tag = "" if gv == nv else ("solo glibc" if nv == NA else ("solo newlib" if gv == NA else "**difiere**"))
        diff += gv != nv
        rep.append(f"| `{name}` | {'—' if gv == NA else gv} | {'—' if nv == NA else nv} | {tag} |")
    rep.append(f"\n{diff} de {len(FACTS)} difieren o faltan en uno de los dos.")
    open(md, "w", encoding="utf-8").write("\n".join(rep) + "\n")
    print(f"{diff} of {len(FACTS)} differ")


if __name__ == "__main__":
    {"gen": lambda: gen(sys.argv[2]), "decode": lambda: decode(sys.argv[2], sys.argv[3]),
     "table": lambda: table(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])}[sys.argv[1]]()
