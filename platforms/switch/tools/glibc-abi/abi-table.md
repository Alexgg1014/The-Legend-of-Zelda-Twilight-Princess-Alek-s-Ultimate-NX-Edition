| Nombre | glibc | newlib | |
|---|---|---|---|
| `EPERM` | 1 | 1 |  |
| `ENOENT` | 2 | 2 |  |
| `ESRCH` | 3 | 3 |  |
| `EINTR` | 4 | 4 |  |
| `EIO` | 5 | 5 |  |
| `ENXIO` | 6 | 6 |  |
| `E2BIG` | 7 | 7 |  |
| `ENOEXEC` | 8 | 8 |  |
| `EBADF` | 9 | 9 |  |
| `ECHILD` | 10 | 10 |  |
| `EAGAIN` | 11 | 11 |  |
| `ENOMEM` | 12 | 12 |  |
| `EACCES` | 13 | 13 |  |
| `EFAULT` | 14 | 14 |  |
| `ENOTBLK` | 15 | — | solo glibc |
| `EBUSY` | 16 | 16 |  |
| `EEXIST` | 17 | 17 |  |
| `EXDEV` | 18 | 18 |  |
| `ENODEV` | 19 | 19 |  |
| `ENOTDIR` | 20 | 20 |  |
| `EISDIR` | 21 | 21 |  |
| `EINVAL` | 22 | 22 |  |
| `ENFILE` | 23 | 23 |  |
| `EMFILE` | 24 | 24 |  |
| `ENOTTY` | 25 | 25 |  |
| `ETXTBSY` | 26 | 26 |  |
| `EFBIG` | 27 | 27 |  |
| `ENOSPC` | 28 | 28 |  |
| `ESPIPE` | 29 | 29 |  |
| `EROFS` | 30 | 30 |  |
| `EMLINK` | 31 | 31 |  |
| `EPIPE` | 32 | 32 |  |
| `EDOM` | 33 | 33 |  |
| `ERANGE` | 34 | 34 |  |
| `EDEADLK` | 35 | 45 | **difiere** |
| `ENAMETOOLONG` | 36 | 91 | **difiere** |
| `ENOLCK` | 37 | 46 | **difiere** |
| `ENOSYS` | 38 | 88 | **difiere** |
| `ENOTEMPTY` | 39 | 90 | **difiere** |
| `ELOOP` | 40 | 92 | **difiere** |
| `ENOMSG` | 42 | 35 | **difiere** |
| `EIDRM` | 43 | 36 | **difiere** |
| `ECHRNG` | 44 | — | solo glibc |
| `EL2NSYNC` | 45 | — | solo glibc |
| `EL3HLT` | 46 | — | solo glibc |
| `EL3RST` | 47 | — | solo glibc |
| `ELNRNG` | 48 | — | solo glibc |
| `EUNATCH` | 49 | — | solo glibc |
| `ENOCSI` | 50 | — | solo glibc |
| `EL2HLT` | 51 | — | solo glibc |
| `EBADE` | 52 | — | solo glibc |
| `EBADR` | 53 | — | solo glibc |
| `EXFULL` | 54 | — | solo glibc |
| `ENOANO` | 55 | — | solo glibc |
| `EBADRQC` | 56 | — | solo glibc |
| `EBADSLT` | 57 | — | solo glibc |
| `EBFONT` | 59 | — | solo glibc |
| `ENOSTR` | 60 | 60 |  |
| `ENODATA` | 61 | 61 |  |
| `ETIME` | 62 | 62 |  |
| `ENOSR` | 63 | 63 |  |
| `ENONET` | 64 | — | solo glibc |
| `ENOPKG` | 65 | — | solo glibc |
| `EREMOTE` | 66 | — | solo glibc |
| `ENOLINK` | 67 | 67 |  |
| `EADV` | 68 | — | solo glibc |
| `ESRMNT` | 69 | — | solo glibc |
| `ECOMM` | 70 | — | solo glibc |
| `EPROTO` | 71 | 71 |  |
| `EMULTIHOP` | 72 | 74 | **difiere** |
| `EDOTDOT` | 73 | — | solo glibc |
| `EBADMSG` | 74 | 77 | **difiere** |
| `EOVERFLOW` | 75 | 139 | **difiere** |
| `ENOTUNIQ` | 76 | — | solo glibc |
| `EBADFD` | 77 | — | solo glibc |
| `EREMCHG` | 78 | — | solo glibc |
| `ELIBACC` | 79 | — | solo glibc |
| `ELIBBAD` | 80 | — | solo glibc |
| `ELIBSCN` | 81 | — | solo glibc |
| `ELIBMAX` | 82 | — | solo glibc |
| `ELIBEXEC` | 83 | — | solo glibc |
| `EILSEQ` | 84 | 138 | **difiere** |
| `ERESTART` | 85 | — | solo glibc |
| `ESTRPIPE` | 86 | — | solo glibc |
| `EUSERS` | 87 | — | solo glibc |
| `ENOTSOCK` | 88 | 108 | **difiere** |
| `EDESTADDRREQ` | 89 | 121 | **difiere** |
| `EMSGSIZE` | 90 | 122 | **difiere** |
| `EPROTOTYPE` | 91 | 107 | **difiere** |
| `ENOPROTOOPT` | 92 | 109 | **difiere** |
| `EPROTONOSUPPORT` | 93 | 123 | **difiere** |
| `ESOCKTNOSUPPORT` | 94 | — | solo glibc |
| `EOPNOTSUPP` | 95 | 95 |  |
| `EPFNOSUPPORT` | 96 | 96 |  |
| `EAFNOSUPPORT` | 97 | 106 | **difiere** |
| `EADDRINUSE` | 98 | 112 | **difiere** |
| `EADDRNOTAVAIL` | 99 | 125 | **difiere** |
| `ENETDOWN` | 100 | 115 | **difiere** |
| `ENETUNREACH` | 101 | 114 | **difiere** |
| `ENETRESET` | 102 | 126 | **difiere** |
| `ECONNABORTED` | 103 | 113 | **difiere** |
| `ECONNRESET` | 104 | 104 |  |
| `ENOBUFS` | 105 | 105 |  |
| `EISCONN` | 106 | 127 | **difiere** |
| `ENOTCONN` | 107 | 128 | **difiere** |
| `ESHUTDOWN` | 108 | — | solo glibc |
| `ETOOMANYREFS` | 109 | 129 | **difiere** |
| `ETIMEDOUT` | 110 | 116 | **difiere** |
| `ECONNREFUSED` | 111 | 111 |  |
| `EHOSTDOWN` | 112 | 117 | **difiere** |
| `EHOSTUNREACH` | 113 | 118 | **difiere** |
| `EALREADY` | 114 | 120 | **difiere** |
| `EINPROGRESS` | 115 | 119 | **difiere** |
| `ESTALE` | 116 | 133 | **difiere** |
| `EUCLEAN` | 117 | — | solo glibc |
| `ENOTNAM` | 118 | — | solo glibc |
| `ENAVAIL` | 119 | — | solo glibc |
| `EISNAM` | 120 | — | solo glibc |
| `EREMOTEIO` | 121 | — | solo glibc |
| `EDQUOT` | 122 | 132 | **difiere** |
| `ENOMEDIUM` | 123 | — | solo glibc |
| `EMEDIUMTYPE` | 124 | — | solo glibc |
| `ECANCELED` | 125 | 140 | **difiere** |
| `ENOKEY` | 126 | — | solo glibc |
| `EKEYEXPIRED` | 127 | — | solo glibc |
| `EKEYREVOKED` | 128 | — | solo glibc |
| `EKEYREJECTED` | 129 | — | solo glibc |
| `EOWNERDEAD` | 130 | 142 | **difiere** |
| `ENOTRECOVERABLE` | 131 | 141 | **difiere** |
| `ERFKILL` | 132 | — | solo glibc |
| `EHWPOISON` | 133 | — | solo glibc |
| `ENOTSUP` | 95 | 134 | **difiere** |
| `EWOULDBLOCK` | 11 | 11 |  |
| `EDEADLOCK` | 35 | — | solo glibc |
| `CLOCK_REALTIME` | 0 | 1 | **difiere** |
| `CLOCK_MONOTONIC` | 1 | 4 | **difiere** |
| `CLOCK_PROCESS_CPUTIME_ID` | 2 | — | solo glibc |
| `CLOCK_THREAD_CPUTIME_ID` | 3 | — | solo glibc |
| `CLOCK_MONOTONIC_RAW` | 4 | 5 | **difiere** |
| `CLOCK_REALTIME_COARSE` | 5 | 0 | **difiere** |
| `CLOCK_MONOTONIC_COARSE` | 6 | 6 |  |
| `CLOCK_BOOTTIME` | 7 | 7 |  |
| `CLOCK_REALTIME_ALARM` | 8 | 8 |  |
| `CLOCK_BOOTTIME_ALARM` | 9 | 9 |  |
| `CLOCK_TAI` | 11 | 11 |  |
| `O_RDONLY` | 0 | 0 |  |
| `O_WRONLY` | 1 | 1 |  |
| `O_RDWR` | 2 | 2 |  |
| `O_ACCMODE` | 3 | 3 |  |
| `O_CREAT` | 64 | 512 | **difiere** |
| `O_EXCL` | 128 | 2048 | **difiere** |
| `O_NOCTTY` | 256 | 32768 | **difiere** |
| `O_TRUNC` | 512 | 1024 | **difiere** |
| `O_APPEND` | 1024 | 8 | **difiere** |
| `O_NONBLOCK` | 2048 | 16384 | **difiere** |
| `O_DSYNC` | 4096 | 8192 | **difiere** |
| `O_SYNC` | 1052672 | 8192 | **difiere** |
| `O_RSYNC` | 1052672 | 8192 | **difiere** |
| `O_DIRECTORY` | 16384 | 2097152 | **difiere** |
| `O_NOFOLLOW` | 32768 | 1048576 | **difiere** |
| `O_CLOEXEC` | 524288 | 262144 | **difiere** |
| `O_DIRECT` | 65536 | 524288 | **difiere** |
| `O_LARGEFILE` | 0 | — | solo glibc |
| `O_NOATIME` | 262144 | — | solo glibc |
| `O_PATH` | 2097152 | — | solo glibc |
| `O_TMPFILE` | 4210688 | — | solo glibc |
| `O_ASYNC` | 8192 | — | solo glibc |
| `F_DUPFD` | 0 | 0 |  |
| `F_GETFD` | 1 | 1 |  |
| `F_SETFD` | 2 | 2 |  |
| `F_GETFL` | 3 | 3 |  |
| `F_SETFL` | 4 | 4 |  |
| `F_GETLK` | 5 | 7 | **difiere** |
| `F_SETLK` | 6 | 8 | **difiere** |
| `F_SETLKW` | 7 | 9 | **difiere** |
| `F_SETOWN` | 8 | 6 | **difiere** |
| `F_GETOWN` | 9 | 5 | **difiere** |
| `F_DUPFD_CLOEXEC` | 1030 | 14 | **difiere** |
| `FD_CLOEXEC` | 1 | 1 |  |
| `AT_FDCWD` | -100 | -2 | **difiere** |
| `AT_REMOVEDIR` | 512 | 8 | **difiere** |
| `AT_SYMLINK_NOFOLLOW` | 256 | 2 | **difiere** |
| `sizeof(struct tm)` | 56 | 36 | **difiere** |
| `offsetof(tm, tm_sec)` | 0 | 0 |  |
| `offsetof(tm, tm_min)` | 4 | 4 |  |
| `offsetof(tm, tm_hour)` | 8 | 8 |  |
| `offsetof(tm, tm_mday)` | 12 | 12 |  |
| `offsetof(tm, tm_mon)` | 16 | 16 |  |
| `offsetof(tm, tm_year)` | 20 | 20 |  |
| `offsetof(tm, tm_wday)` | 24 | 24 |  |
| `offsetof(tm, tm_yday)` | 28 | 28 |  |
| `offsetof(tm, tm_isdst)` | 32 | 32 |  |
| `offsetof(tm, tm_gmtoff)` | 40 | — | solo glibc |
| `offsetof(tm, tm_zone)` | 48 | — | solo glibc |
| `AF_UNSPEC` | 0 | 0 |  |
| `AF_UNIX` | 1 | 1 |  |
| `AF_INET` | 2 | 2 |  |
| `AF_INET6` | 10 | 28 | **difiere** |
| `SOCK_STREAM` | 1 | 1 |  |
| `SOCK_DGRAM` | 2 | 2 |  |
| `SOCK_RAW` | 3 | 3 |  |
| `SOCK_SEQPACKET` | 5 | 5 |  |
| `SOCK_NONBLOCK` | 2048 | 536870912 | **difiere** |
| `SOCK_CLOEXEC` | 524288 | 268435456 | **difiere** |
| `SOL_SOCKET` | 1 | 65535 | **difiere** |
| `IPPROTO_IP` | 0 | 0 |  |
| `IPPROTO_ICMP` | 1 | 1 |  |
| `IPPROTO_TCP` | 6 | 6 |  |
| `IPPROTO_UDP` | 17 | 17 |  |
| `IPPROTO_IPV6` | 41 | 41 |  |
| `SO_DEBUG` | 1 | 1 |  |
| `SO_REUSEADDR` | 2 | 4 | **difiere** |
| `SO_TYPE` | 3 | 4104 | **difiere** |
| `SO_ERROR` | 4 | 4103 | **difiere** |
| `SO_DONTROUTE` | 5 | 16 | **difiere** |
| `SO_BROADCAST` | 6 | 32 | **difiere** |
| `SO_SNDBUF` | 7 | 4097 | **difiere** |
| `SO_RCVBUF` | 8 | 4098 | **difiere** |
| `SO_KEEPALIVE` | 9 | 8 | **difiere** |
| `SO_OOBINLINE` | 10 | 256 | **difiere** |
| `SO_LINGER` | 13 | 128 | **difiere** |
| `SO_REUSEPORT` | 15 | 512 | **difiere** |
| `SO_RCVLOWAT` | 18 | 4100 | **difiere** |
| `SO_SNDLOWAT` | 19 | 4099 | **difiere** |
| `SO_RCVTIMEO` | 20 | 4102 | **difiere** |
| `SO_SNDTIMEO` | 21 | 4101 | **difiere** |
| `SO_ACCEPTCONN` | 30 | 2 | **difiere** |
| `SO_PRIORITY` | 12 | — | solo glibc |
| `SO_BINDTODEVICE` | 25 | — | solo glibc |
| `SO_TIMESTAMP` | 29 | 1024 | **difiere** |
| `SO_NO_CHECK` | 11 | — | solo glibc |
| `SO_PASSCRED` | 16 | — | solo glibc |
| `SO_PEERCRED` | 17 | — | solo glibc |
| `SO_RCVBUFFORCE` | 33 | — | solo glibc |
| `SO_SNDBUFFORCE` | 32 | — | solo glibc |
| `SO_BUSY_POLL` | 46 | — | solo glibc |
| `SO_INCOMING_CPU` | 49 | — | solo glibc |
| `SO_ZEROCOPY` | 60 | — | solo glibc |
| `IP_TOS` | 1 | 3 | **difiere** |
| `IP_TTL` | 2 | 4 | **difiere** |
| `IP_HDRINCL` | 3 | 2 | **difiere** |
| `IP_OPTIONS` | 4 | 1 | **difiere** |
| `IP_MULTICAST_IF` | 32 | 9 | **difiere** |
| `IP_MULTICAST_TTL` | 33 | 10 | **difiere** |
| `IP_MULTICAST_LOOP` | 34 | 11 | **difiere** |
| `IP_ADD_MEMBERSHIP` | 35 | 12 | **difiere** |
| `IP_DROP_MEMBERSHIP` | 36 | 13 | **difiere** |
| `IP_PKTINFO` | 8 | — | solo glibc |
| `IP_RECVERR` | 11 | — | solo glibc |
| `IP_MTU_DISCOVER` | 10 | — | solo glibc |
| `IP_RECVTOS` | 13 | 68 | **difiere** |
| `IP_RECVTTL` | 12 | 65 | **difiere** |
| `IP_FREEBIND` | 15 | — | solo glibc |
| `IPV6_V6ONLY` | 26 | 27 | **difiere** |
| `IPV6_UNICAST_HOPS` | 16 | 4 | **difiere** |
| `IPV6_MULTICAST_IF` | 17 | 9 | **difiere** |
| `IPV6_MULTICAST_HOPS` | 18 | 10 | **difiere** |
| `IPV6_MULTICAST_LOOP` | 19 | 11 | **difiere** |
| `IPV6_JOIN_GROUP` | 20 | 12 | **difiere** |
| `IPV6_LEAVE_GROUP` | 21 | 13 | **difiere** |
| `IPV6_RECVPKTINFO` | 49 | — | solo glibc |
| `IPV6_TCLASS` | 67 | — | solo glibc |
| `TCP_NODELAY` | 1 | 1 |  |
| `TCP_MAXSEG` | 2 | 2 |  |
| `TCP_KEEPIDLE` | 4 | 256 | **difiere** |
| `TCP_KEEPINTVL` | 5 | 512 | **difiere** |
| `TCP_KEEPCNT` | 6 | 1024 | **difiere** |
| `TCP_QUICKACK` | 12 | — | solo glibc |
| `TCP_USER_TIMEOUT` | 18 | — | solo glibc |
| `TCP_CORK` | 3 | — | solo glibc |
| `MSG_OOB` | 1 | 1 |  |
| `MSG_PEEK` | 2 | 2 |  |
| `MSG_DONTROUTE` | 4 | 4 |  |
| `MSG_CTRUNC` | 8 | 32 | **difiere** |
| `MSG_TRUNC` | 32 | 16 | **difiere** |
| `MSG_DONTWAIT` | 64 | 128 | **difiere** |
| `MSG_EOR` | 128 | 8 | **difiere** |
| `MSG_WAITALL` | 256 | 64 | **difiere** |
| `MSG_NOSIGNAL` | 16384 | 131072 | **difiere** |
| `MSG_CONFIRM` | 2048 | — | solo glibc |
| `MSG_MORE` | 32768 | — | solo glibc |
| `MSG_ERRQUEUE` | 8192 | — | solo glibc |
| `MSG_CMSG_CLOEXEC` | 1073741824 | 262144 | **difiere** |
| `SHUT_RD` | 0 | 0 |  |
| `SHUT_WR` | 1 | 1 |  |
| `SHUT_RDWR` | 2 | 2 |  |
| `AI_PASSIVE` | 1 | 1 |  |
| `AI_CANONNAME` | 2 | 2 |  |
| `AI_NUMERICHOST` | 4 | 4 |  |
| `AI_V4MAPPED` | 8 | 2048 | **difiere** |
| `AI_ALL` | 16 | 256 | **difiere** |
| `AI_ADDRCONFIG` | 32 | 1024 | **difiere** |
| `AI_NUMERICSERV` | 1024 | 8 | **difiere** |
| `NI_NUMERICHOST` | 1 | 2 | **difiere** |
| `NI_NUMERICSERV` | 2 | 8 | **difiere** |
| `NI_NOFQDN` | 4 | 1 | **difiere** |
| `NI_NAMEREQD` | 8 | 4 | **difiere** |
| `NI_DGRAM` | 16 | 16 |  |
| `NI_MAXHOST` | 1025 | 1025 |  |
| `NI_MAXSERV` | 32 | 32 |  |
| `EAI_BADFLAGS` | -1 | 3 | **difiere** |
| `EAI_NONAME` | -2 | 8 | **difiere** |
| `EAI_AGAIN` | -3 | 2 | **difiere** |
| `EAI_FAIL` | -4 | 4 | **difiere** |
| `EAI_FAMILY` | -6 | 5 | **difiere** |
| `EAI_SOCKTYPE` | -7 | 10 | **difiere** |
| `EAI_SERVICE` | -8 | 9 | **difiere** |
| `EAI_MEMORY` | -10 | 6 | **difiere** |
| `EAI_SYSTEM` | -11 | 11 | **difiere** |
| `EAI_OVERFLOW` | -12 | 14 | **difiere** |
| `EAI_NODATA` | -5 | 7 | **difiere** |
| `EAI_ADDRFAMILY` | -9 | 1 | **difiere** |
| `FIONBIO` | 21537 | -2147195266 | **difiere** |
| `FIONREAD` | 21531 | 1074030207 | **difiere** |
| `FIOASYNC` | 21586 | -2147195267 | **difiere** |
| `SIOCGIFADDR` | 35093 | -1071617759 | **difiere** |
| `SIOCGIFCONF` | 35090 | -1072666332 | **difiere** |
| `POLLIN` | 1 | 1 |  |
| `POLLPRI` | 2 | 2 |  |
| `POLLOUT` | 4 | 4 |  |
| `POLLERR` | 8 | 8 |  |
| `POLLHUP` | 16 | 16 |  |
| `POLLNVAL` | 32 | 32 |  |
| `POLLRDNORM` | 64 | 64 |  |
| `POLLRDBAND` | 128 | 128 |  |
| `POLLWRNORM` | 256 | 4 | **difiere** |
| `POLLWRBAND` | 512 | 256 | **difiere** |
| `IFF_UP` | 1 | 1 |  |
| `IFF_BROADCAST` | 2 | 2 |  |
| `IFF_LOOPBACK` | 8 | 8 |  |
| `IFF_POINTOPOINT` | 16 | 16 |  |
| `IFF_RUNNING` | 64 | 64 |  |
| `IFF_MULTICAST` | 4096 | 32768 | **difiere** |
| `sizeof(struct sockaddr)` | 16 | 16 |  |
| `offsetof(sockaddr, sa_family)` | 0 | 1 | **difiere** |
| `sizeof(sa_family_t)` | 2 | 1 | **difiere** |
| `sizeof(struct sockaddr_in)` | 16 | 16 |  |
| `offsetof(sockaddr_in, sin_port)` | 2 | 2 |  |
| `offsetof(sockaddr_in, sin_addr)` | 4 | 4 |  |
| `sizeof(struct sockaddr_in6)` | 28 | 28 |  |
| `offsetof(sockaddr_in6, sin6_port)` | 2 | 2 |  |
| `offsetof(sockaddr_in6, sin6_flowinfo)` | 4 | 4 |  |
| `offsetof(sockaddr_in6, sin6_addr)` | 8 | 8 |  |
| `offsetof(sockaddr_in6, sin6_scope_id)` | 24 | 24 |  |
| `sizeof(struct sockaddr_storage)` | 128 | 128 |  |
| `sizeof(socklen_t)` | 4 | 4 |  |
| `sizeof(struct addrinfo)` | 48 | 48 |  |
| `offsetof(addrinfo, ai_addrlen)` | 16 | 16 |  |
| `offsetof(addrinfo, ai_addr)` | 24 | 32 | **difiere** |
| `offsetof(addrinfo, ai_canonname)` | 32 | 24 | **difiere** |
| `offsetof(addrinfo, ai_next)` | 40 | 40 |  |
| `sizeof(struct msghdr)` | 56 | 48 | **difiere** |
| `offsetof(msghdr, msg_namelen)` | 8 | 8 |  |
| `offsetof(msghdr, msg_iov)` | 16 | 16 |  |
| `offsetof(msghdr, msg_iovlen)` | 24 | 24 |  |
| `offsetof(msghdr, msg_control)` | 32 | 32 |  |
| `offsetof(msghdr, msg_controllen)` | 40 | 40 |  |
| `offsetof(msghdr, msg_flags)` | 48 | 44 | **difiere** |
| `sizeof(struct pollfd)` | 8 | 8 |  |
| `sizeof(nfds_t)` | 8 | 4 | **difiere** |
| `sizeof(fd_set)` | 128 | 8 | **difiere** |
| `FD_SETSIZE` | 1024 | 64 | **difiere** |
| `sizeof(struct linger)` | 8 | 8 |  |
| `sizeof(struct ip_mreq)` | 8 | 8 |  |
| `sizeof(struct timeval)` | 16 | 16 |  |
| `sizeof(struct hostent)` | 32 | 32 |  |
| `offsetof(hostent, h_addrtype)` | 16 | 16 |  |
| `offsetof(hostent, h_addr_list)` | 24 | 24 |  |
| `sizeof(struct ifaddrs)` | 56 | — | solo glibc |
| `offsetof(ifaddrs, ifa_name)` | 8 | — | solo glibc |
| `offsetof(ifaddrs, ifa_flags)` | 16 | — | solo glibc |
| `offsetof(ifaddrs, ifa_addr)` | 24 | — | solo glibc |
| `offsetof(ifaddrs, ifa_netmask)` | 32 | — | solo glibc |
| `offsetof(ifaddrs, ifa_ifu)` | 40 | — | solo glibc |
| `offsetof(ifaddrs, ifa_data)` | 48 | — | solo glibc |
| `sizeof(struct utsname)` | 390 | — | solo glibc |
| `sizeof(struct epoll_event)` | 16 | — | solo glibc |
| `offsetof(epoll_event, data)` | 8 | — | solo glibc |
| `EPOLLIN` | 1 | — | solo glibc |
| `EPOLLOUT` | 4 | — | solo glibc |
| `EPOLLERR` | 8 | — | solo glibc |
| `EPOLLHUP` | 16 | — | solo glibc |
| `EPOLLRDHUP` | 8192 | — | solo glibc |
| `EPOLLPRI` | 2 | — | solo glibc |
| `EPOLLET` | 2147483648 | — | solo glibc |
| `EPOLLONESHOT` | 1073741824 | — | solo glibc |
| `EPOLL_CTL_ADD` | 1 | — | solo glibc |
| `EPOLL_CTL_DEL` | 2 | — | solo glibc |
| `EPOLL_CTL_MOD` | 3 | — | solo glibc |
| `EPOLL_CLOEXEC` | 524288 | — | solo glibc |
| `EFD_SEMAPHORE` | 1 | — | solo glibc |
| `EFD_NONBLOCK` | 2048 | — | solo glibc |
| `EFD_CLOEXEC` | 524288 | — | solo glibc |

258 de 393 difieren o faltan en uno de los dos.
