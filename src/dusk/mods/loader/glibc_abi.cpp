// aleks2 (DM2-d): see glibc_abi.hpp.
#include "glibc_abi.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <wchar.h>
// DM3: the sockets groups of the table (af, sock, level, so, ipopt, ipv6opt, tcpopt, msg, shut, ai, ni, eai,
// ioctl, poll, iff) read their native values from these.
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unordered_map>

namespace dusk::mods::elf::glibc {
namespace {

constexpr long long kNotDefined = -999999;

const Entry kTable[] = {
#include "glibc_abi_table.inc"
};

template <class F>
uint64_t addr(F* f) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(f));
}

struct Maps {
    std::unordered_map<int, int> errnoToGlibc, errnoFromGlibc;
    // open/fcntl flags: {glibc mask, native mask}, widest masks first (O_SYNC contains O_DSYNC's bit).
    std::vector<std::pair<int, int>> oflags;
    std::unordered_map<int, int> fcntlFromGlibc;

    Maps() {
        for (const auto& e : kTable) {
            const std::string_view group = e.group;
            if (group == "errno" && e.native != kNotDefined) {
                errnoFromGlibc.emplace(static_cast<int>(e.glibc), static_cast<int>(e.native));
                errnoToGlibc.emplace(static_cast<int>(e.native), static_cast<int>(e.glibc));
            } else if (group == "oflag" && e.glibc > 3 && e.native != kNotDefined) {
                // The access mode (O_RDONLY/O_WRONLY/O_RDWR/O_ACCMODE, 0..3) is the same value in both.
                oflags.emplace_back(static_cast<int>(e.glibc), static_cast<int>(e.native));
            } else if (group == "fcntl" && e.name[0] == 'F' && e.native != kNotDefined) {
                fcntlFromGlibc.emplace(static_cast<int>(e.glibc), static_cast<int>(e.native));
            }
        }
        std::stable_sort(oflags.begin(), oflags.end(), [](auto a, auto b) {
            return std::popcount(static_cast<unsigned>(a.first)) > std::popcount(static_cast<unsigned>(b.first));
        });
    }
};

const Maps& maps() {
    static const Maps s_maps;
    return s_maps;
}

// errno after a failed native call, as the module expects it.
void set_glibc_errno() {
    errno = errno_to_glibc(errno);
}

// The layout of a glibc struct tm on LP64: the nine ints, then tm_gmtoff and tm_zone.
struct GlibcTm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char* tm_zone;
};
static_assert(sizeof(GlibcTm) == 56 && offsetof(GlibcTm, tm_gmtoff) == 40 && offsetof(GlibcTm, tm_zone) == 48);

// Seconds east of UTC for `local`, from the broken-down local and UTC times of the same instant.
long gmtoff_of(const std::tm& local, const std::tm& utc) {
    long days = local.tm_yday - utc.tm_yday;
    if (local.tm_year != utc.tm_year) {
        days = local.tm_year > utc.tm_year ? 1 : -1;  // across New Year: a day either way
    }
    return ((days * 24 + (local.tm_hour - utc.tm_hour)) * 60 + (local.tm_min - utc.tm_min)) * 60 +
           (local.tm_sec - utc.tm_sec);
}

GlibcTm* to_glibc_tm(const std::tm& native, long gmtoff, const char* zone, GlibcTm* out) {
    static_assert(offsetof(std::tm, tm_isdst) == 32, "the nine ints are laid out as glibc lays them out");
    std::memcpy(out, &native, 9 * sizeof(int));
    out->tm_gmtoff = gmtoff;
    out->tm_zone = zone;
    return out;
}

}  // namespace

int errno_to_glibc(int native) {
    const auto& m = maps().errnoToGlibc;
    const auto it = m.find(native);
    return it == m.end() ? native : it->second;
}

int errno_from_glibc(int glibc) {
    const auto& m = maps().errnoFromGlibc;
    const auto it = m.find(glibc);
    return it == m.end() ? glibc : it->second;
}

int clock_from_glibc(int glibc) {
    // glibc: 0 REALTIME, 1 MONOTONIC, 2/3 CPU time, 4 MONOTONIC_RAW, 5 REALTIME_COARSE, 6 MONOTONIC_COARSE,
    // 7 BOOTTIME, 8 REALTIME_ALARM, 9 BOOTTIME_ALARM, 11 TAI. Every one goes to the native clock with the same
    // meaning a game can offer: wall time or a monotonic count (CPU time included -- there is no per-thread
    // CPU clock here, and a monotonic one is what such a caller measures with in practice).
    switch (glibc) {
    case 0: case 5: case 8: case 11: return CLOCK_REALTIME;
    case 1: case 2: case 3: case 4: case 6: case 7: case 9: return CLOCK_MONOTONIC;
    default: return -1;
    }
}

int oflags_from_glibc(int glibc) {
    int native = glibc & 3;
    int rest = glibc & ~3;
    for (const auto& [g, n] : maps().oflags) {
        if (g != 0 && (rest & g) == g) {
            native |= n;
            rest &= ~g;
        }
    }
    return native;  // bits with no native meaning (O_NOATIME, O_LARGEFILE...) are dropped
}

int oflags_to_glibc(int native) {
    int glibc = native & 3;
    int rest = native & ~3;
    for (const auto& [g, n] : maps().oflags) {
        if (n != 0 && (rest & n) == n) {
            glibc |= g;
            rest &= ~n;
        }
    }
    return glibc;
}

int fcntl_cmd_from_glibc(int glibc) {
    const auto& m = maps().fcntlFromGlibc;
    const auto it = m.find(glibc);
    return it == m.end() ? -1 : it->second;
}

std::vector<Entry> table() {
    return {std::begin(kTable), std::end(kTable)};
}

}  // namespace dusk::mods::elf::glibc

using namespace dusk::mods::elf::glibc;

// glibc_net.cpp (V1-rc3) defines it: close() only closes the module's own descriptors. Weak and empty here for
// the builds and tests that link the file layer without the sockets layer.
extern "C" __attribute__((weak)) void aleks2_glibc_note_module_fd(int) {}

extern "C" {

// --- errno producers: the native call, then a glibc value in errno if it failed ---

int aleks2_glibc_open(const char* path, int flags, ...) {
    unsigned mode = 0;
    if (flags & 0x40 /* glibc O_CREAT */ || (flags & 0x410000) == 0x410000 /* glibc O_TMPFILE */) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, unsigned);
        va_end(ap);
    }
    const int fd = open(path, oflags_from_glibc(flags), mode);
    if (fd < 0) {
        set_glibc_errno();
    } else {
        aleks2_glibc_note_module_fd(fd);
    }
    return fd;
}

int aleks2_glibc_fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    const long arg = va_arg(ap, long);
    va_end(ap);
    const int native = fcntl_cmd_from_glibc(cmd);
    if (native < 0) {
        errno = errno_to_glibc(EINVAL);
        return -1;
    }
    const long nativeArg = native == F_SETFL ? oflags_from_glibc(static_cast<int>(arg)) : arg;
    const int r = fcntl(fd, native, nativeArg);
    if (r < 0) {
        set_glibc_errno();
        return r;
    }
    if (native == F_DUPFD) {
        aleks2_glibc_note_module_fd(r);
    }
    return native == F_GETFL ? oflags_to_glibc(r) : r;
}

FILE* aleks2_glibc_fopen(const char* path, const char* mode) {
    FILE* f = fopen(path, mode);
    if (f == nullptr) {
        set_glibc_errno();
    }
    return f;
}

DIR* aleks2_glibc_opendir(const char* path) {
    DIR* d = opendir(path);
    if (d == nullptr) {
        set_glibc_errno();
    }
    return d;
}

int aleks2_glibc_remove(const char* path) {
    const int r = remove(path);
    if (r != 0) {
        set_glibc_errno();
    }
    return r;
}

int aleks2_glibc_rename(const char* from, const char* to) {
    const int r = rename(from, to);
    if (r != 0) {
        set_glibc_errno();
    }
    return r;
}

// The multibyte conversions fail with EILSEQ, 84 in glibc and 138 in newlib.
size_t aleks2_glibc_mbrtowc(wchar_t* pwc, const char* s, size_t n, mbstate_t* ps) {
    const size_t r = mbrtowc(pwc, s, n, ps);
    if (r == static_cast<size_t>(-1)) {
        set_glibc_errno();
    }
    return r;
}

int aleks2_glibc_mbtowc(wchar_t* pwc, const char* s, size_t n) {
    const int r = mbtowc(pwc, s, n);
    if (r < 0) {
        set_glibc_errno();
    }
    return r;
}

size_t aleks2_glibc_mbsrtowcs(wchar_t* dst, const char** src, size_t len, mbstate_t* ps) {
    const size_t r = mbsrtowcs(dst, src, len, ps);
    if (r == static_cast<size_t>(-1)) {
        set_glibc_errno();
    }
    return r;
}

size_t aleks2_glibc_mbsnrtowcs(wchar_t* dst, const char** src, size_t nms, size_t len, mbstate_t* ps) {
    const size_t r = mbsnrtowcs(dst, src, nms, len, ps);
    if (r == static_cast<size_t>(-1)) {
        set_glibc_errno();
    }
    return r;
}

size_t aleks2_glibc_wcrtomb(char* s, wchar_t wc, mbstate_t* ps) {
    const size_t r = wcrtomb(s, wc, ps);
    if (r == static_cast<size_t>(-1)) {
        set_glibc_errno();
    }
    return r;
}

size_t aleks2_glibc_wcsnrtombs(char* dst, const wchar_t** src, size_t nwc, size_t len, mbstate_t* ps) {
    const size_t r = wcsnrtombs(dst, src, nwc, len, ps);
    if (r == static_cast<size_t>(-1)) {
        set_glibc_errno();
    }
    return r;
}

// pthread functions return the error instead of setting errno.
int aleks2_glibc_pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* t) {
    return errno_to_glibc(pthread_cond_timedwait(c, m, t));
}

// --- errno consumers: the module hands over a glibc value ---

char* aleks2_glibc_strerror(int e) {
    return strerror(errno_from_glibc(e));
}

// glibc's strerror_r symbol is the GNU one: it returns the message (a static string or buf).
char* aleks2_glibc_strerror_r(int e, char* buf, size_t len) {
    const char* message = strerror(errno_from_glibc(e));
    if (buf != nullptr && len != 0) {
        std::snprintf(buf, len, "%s", message);
        return buf;
    }
    return const_cast<char*>(message);
}

void aleks2_glibc_perror(const char* s) {
    const char* message = strerror(errno_from_glibc(errno));
    if (s != nullptr && *s != '\0') {
        std::fprintf(stderr, "%s: %s\n", s, message);
    } else {
        std::fprintf(stderr, "%s\n", message);
    }
}

// --- clocks: glibc ids ---

int aleks2_glibc_clock_gettime(int clock, struct timespec* ts) {
    const int native = clock_from_glibc(clock);
    if (native < 0) {
        errno = errno_to_glibc(EINVAL);
        return -1;
    }
    const int r = clock_gettime(static_cast<clockid_t>(native), ts);
    if (r != 0) {
        set_glibc_errno();
    }
    return r;
}

int aleks2_glibc_clock_getres(int clock, struct timespec* ts) {
    const int native = clock_from_glibc(clock);
    if (native < 0) {
        errno = errno_to_glibc(EINVAL);
        return -1;
    }
    const int r = clock_getres(static_cast<clockid_t>(native), ts);
    if (r != 0) {
        set_glibc_errno();
    }
    return r;
}

// --- struct tm writers: glibc's 56-byte layout, tm_gmtoff and tm_zone filled ---

void* aleks2_glibc_localtime_r(const time_t* t, void* out) {
    std::tm local{}, utc{};
    if (localtime_r(t, &local) == nullptr || gmtime_r(t, &utc) == nullptr) {
        set_glibc_errno();
        return nullptr;
    }
    tzset();
    const char* zone = tzname[local.tm_isdst > 0 ? 1 : 0];
    return to_glibc_tm(local, gmtoff_of(local, utc), zone, static_cast<GlibcTm*>(out));
}

void* aleks2_glibc_gmtime_r(const time_t* t, void* out) {
    std::tm utc{};
    if (gmtime_r(t, &utc) == nullptr) {
        set_glibc_errno();
        return nullptr;
    }
    return to_glibc_tm(utc, 0, "GMT", static_cast<GlibcTm*>(out));
}

// One buffer for both, as glibc's _tmbuf (time/localtime.c): not thread-local there either. Also, the
// game's static TLS is part of every libnx thread's stack allocation; growing it shrinks small stacks
// (112 bytes of thread_local here overflowed own_process_handle.c's 4 KB receiver and hung DM2D-c1's boot).
static GlibcTm s_glibcTm;

void* aleks2_glibc_localtime(const time_t* t) {
    return aleks2_glibc_localtime_r(t, &s_glibcTm);
}

void* aleks2_glibc_gmtime(const time_t* t) {
    return aleks2_glibc_gmtime_r(t, &s_glibcTm);
}

}  // extern "C"

namespace dusk::mods::elf::glibc {

std::vector<Shim> shims() {
    constexpr const char* kErrno = "glibc errno values on failure (DM2-d glibc ABI layer)";
    return {
        {"open", addr(&aleks2_glibc_open), "glibc O_* flags; glibc errno (DM2-d)", true},
        {"open64", addr(&aleks2_glibc_open), "glibc O_* flags; glibc errno (DM2-d)", true},
        {"fcntl", addr(&aleks2_glibc_fcntl), "glibc F_* commands and O_* flags; glibc errno (DM2-d)", true},
        {"fcntl64", addr(&aleks2_glibc_fcntl), "glibc F_* commands and O_* flags; glibc errno (DM2-d)", true},
        {"fopen", addr(&aleks2_glibc_fopen), kErrno, true},
        {"fopen64", addr(&aleks2_glibc_fopen), kErrno, true},
        {"opendir", addr(&aleks2_glibc_opendir), kErrno, true},
        {"remove", addr(&aleks2_glibc_remove), kErrno, true},
        {"rename", addr(&aleks2_glibc_rename), kErrno, true},
        {"mbrtowc", addr(&aleks2_glibc_mbrtowc), kErrno, true},
        {"mbtowc", addr(&aleks2_glibc_mbtowc), kErrno, true},
        {"mbsrtowcs", addr(&aleks2_glibc_mbsrtowcs), kErrno, true},
        {"mbsnrtowcs", addr(&aleks2_glibc_mbsnrtowcs), kErrno, true},
        {"wcrtomb", addr(&aleks2_glibc_wcrtomb), kErrno, true},
        {"wcsnrtombs", addr(&aleks2_glibc_wcsnrtombs), kErrno, true},
        {"pthread_cond_timedwait", addr(&aleks2_glibc_pthread_cond_timedwait), "glibc error codes (DM2-d)", true},
        {"strerror", addr(&aleks2_glibc_strerror), "message for a glibc errno value (DM2-d)", true},
        {"strerror_r", addr(&aleks2_glibc_strerror_r), "GNU strerror_r for a glibc errno value (DM2-d)", true},
        {"perror", addr(&aleks2_glibc_perror), "message for a glibc errno value (DM2-d)", true},
        {"clock_gettime", addr(&aleks2_glibc_clock_gettime), "glibc clock ids (DM2-d)", true},
        {"clock_getres", addr(&aleks2_glibc_clock_getres), "glibc clock ids (DM2-d)", true},
        {"localtime_r", addr(&aleks2_glibc_localtime_r), "glibc struct tm with tm_gmtoff/tm_zone (DM2-d)", true},
        {"gmtime_r", addr(&aleks2_glibc_gmtime_r), "glibc struct tm with tm_gmtoff/tm_zone (DM2-d)", true},
        {"localtime", addr(&aleks2_glibc_localtime), "glibc struct tm with tm_gmtoff/tm_zone (DM2-d)", true},
        {"gmtime", addr(&aleks2_glibc_gmtime), "glibc struct tm with tm_gmtoff/tm_zone (DM2-d)", true},
    };
}

}  // namespace dusk::mods::elf::glibc
