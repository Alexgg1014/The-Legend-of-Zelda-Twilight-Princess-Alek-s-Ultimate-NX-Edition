// aleks2: the glibc-shaped corners a published mod expects to find.
//
// Native mods ship lib/linux-aarch64/mod.so, built against glibc, and we run on
// newlib. Almost all of it matches by name -- 16 of the 22 published mods now
// want nothing the NRO lacks -- but a handful of names are glibc's alone. This
// is that handful, and nothing else: every symbol here was reported missing by
// tools/probe-mod-abi.py, not guessed at.
//
// Three kinds live here, and it is worth knowing which is which:
//
//   Real. __isoc23_strtol and the ctype tables do exactly what glibc's do; a
//   mod cannot tell the difference.
//
//   Adapted. getrandom goes to Horizon's CSRNG, pthread_cond_clockwait to
//   pthread_cond_timedwait. Different underneath, same contract.
//
//   Honest failures. dlopen has no meaning on a console with no dynamic
//   loader, and getifaddrs has nothing to enumerate. They fail the way the real
//   ones fail when they cannot do their job, so a mod that checks the return
//   value copes and a mod that does not gets a null instead of a crash at a
//   random address.
//
// Nothing in the game calls any of it, so --gc-sections would drop the lot;
// they stay because platforms/switch/mod_abi_surface.txt names them.
#ifdef __SWITCH__
#include <switch.h>

#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pthread.h>

namespace {

// glibc's ctype tables are indexed from -128, so the arrays carry the negative
// half in front and the pointer handed out points at element 128.
constexpr int kCtypeLow = 128;
constexpr int kCtypeSize = 384;

// From glibc's <ctype.h>: _ISbit(n) is (1 << n) << 8 below 8, (1 << n) >> 8
// above. Spelled out rather than computed so the values can be read against
// glibc's header.
enum : unsigned short {
    kUpper = 0x0100,
    kLower = 0x0200,
    kAlpha = 0x0400,
    kDigit = 0x0800,
    kXdigit = 0x1000,
    kSpace = 0x2000,
    kPrint = 0x4000,
    kGraph = 0x8000,
    kBlank = 0x0001,
    kCntrl = 0x0002,
    kPunct = 0x0004,
    kAlnum = 0x0008,
};

struct CtypeTables {
    unsigned short classes[kCtypeSize];
    int lower[kCtypeSize];
    int upper[kCtypeSize];

    CtypeTables() {
        for (int i = 0; i < kCtypeSize; i++) {
            const int c = i - kCtypeLow;
            classes[i] = 0;
            lower[i] = c;
            upper[i] = c;
            if (c < 0 || c > 255) {
                continue;
            }
            // Built from the C library's own answers, so the classification
            // matches whatever locale the rest of the program is in.
            unsigned short bits = 0;
            if (std::isupper(c)) bits |= kUpper;
            if (std::islower(c)) bits |= kLower;
            if (std::isalpha(c)) bits |= kAlpha;
            if (std::isdigit(c)) bits |= kDigit;
            if (std::isxdigit(c)) bits |= kXdigit;
            if (std::isspace(c)) bits |= kSpace;
            if (std::isprint(c)) bits |= kPrint;
            if (std::isgraph(c)) bits |= kGraph;
            if (c == ' ' || c == '\t') bits |= kBlank;
            if (std::iscntrl(c)) bits |= kCntrl;
            if (std::ispunct(c)) bits |= kPunct;
            if (std::isalnum(c)) bits |= kAlnum;
            classes[i] = bits;
            lower[i] = std::tolower(c);
            upper[i] = std::toupper(c);
        }
    }
};

const CtypeTables& ctype_tables() {
    static const CtypeTables tables;
    return tables;
}

}  // namespace

extern "C" {

// ---------------------------------------------------------------------------
// ctype. A mod holds on to the pointer these return, so the tables outlive the
// call; they are function-local statics built once.

const unsigned short** __ctype_b_loc() {
    static thread_local const unsigned short* pointer = ctype_tables().classes + kCtypeLow;
    return &pointer;
}

const int** __ctype_tolower_loc() {
    static thread_local const int* pointer = ctype_tables().lower + kCtypeLow;
    return &pointer;
}

const int** __ctype_toupper_loc() {
    static thread_local const int* pointer = ctype_tables().upper + kCtypeLow;
    return &pointer;
}

// ---------------------------------------------------------------------------
// glibc 2.38 renamed the string-to-number functions when it took up C23's rule
// that strtol accepts binary literals. The behaviour a mod expects from these
// is the old one for everything except 0b..., which no mod here parses.

long __isoc23_strtol(const char* text, char** end, int base) {
    return std::strtol(text, end, base);
}

long long __isoc23_strtoll(const char* text, char** end, int base) {
    return std::strtoll(text, end, base);
}

unsigned long __isoc23_strtoul(const char* text, char** end, int base) {
    return std::strtoul(text, end, base);
}

unsigned long long __isoc23_strtoull(const char* text, char** end, int base) {
    return std::strtoull(text, end, base);
}

int __isoc23_sscanf(const char* text, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = vsscanf(text, format, args);
    va_end(args);
    return result;
}

int __isoc99_sscanf(const char* text, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = vsscanf(text, format, args);
    va_end(args);
    return result;
}

// ---------------------------------------------------------------------------
// _FORTIFY_SOURCE. The flag argument says how hard to check and the size is the
// compiler's idea of the destination; newlib has no fortified printf family, so
// the checks are the ones printf itself makes.

int __printf_chk(int, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = std::vprintf(format, args);
    va_end(args);
    return result;
}

int __fprintf_chk(FILE* stream, int, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = std::vfprintf(stream, format, args);
    va_end(args);
    return result;
}

int __vfprintf_chk(FILE* stream, int, const char* format, va_list args) {
    return std::vfprintf(stream, format, args);
}

// ---------------------------------------------------------------------------
// Odds and ends of glibc's own surface.

// Never true here: the render, FIFO and shader-compile threads are all running.
char __libc_single_threaded = 0;

// The aarch64 auxiliary vector, which Horizon has no equivalent of. Zero is
// what glibc returns for a key it does not know, and callers read it as "that
// hardware capability is not advertised".
unsigned long __getauxval(unsigned long) {
    return 0;
}

ssize_t getrandom(void* buffer, size_t length, unsigned int) {
    if (buffer == nullptr) {
        errno = EFAULT;
        return -1;
    }
    // Horizon's cryptographically secure generator, which is what curl uses on
    // this platform too.
    csrngGetRandomBytes(buffer, length);
    return static_cast<ssize_t>(length);
}

int pthread_cond_clockwait(pthread_cond_t* cond, pthread_mutex_t* mutex, clockid_t,
                           const struct timespec* deadline) {
    // The clock is ignored: newlib's condition variables only know one, and the
    // deadlines a mod sets are short enough that the difference does not show.
    return pthread_cond_timedwait(cond, mutex, deadline);
}

int pipe(int[2]) {
    errno = ENOSYS;
    return -1;
}

int prctl(int, ...) {
    errno = ENOSYS;
    return -1;
}

struct ifaddrs;

int getifaddrs(struct ifaddrs** list) {
    // Horizon exposes the network through nifm, not through an interface list.
    if (list != nullptr) {
        *list = nullptr;
    }
    errno = ENOSYS;
    return -1;
}

void freeifaddrs(struct ifaddrs*) {}

// There is no dynamic loader on Horizon, which is the whole reason mods are
// compiled into the NRO. A mod that tries anyway gets the answer the real
// dlopen gives for a library it cannot find.
void* dlopen(const char*, int) {
    return nullptr;
}

void* dlsym(void*, const char*) {
    return nullptr;
}

const char* dlerror() {
    return "no dynamic loader on this platform";
}

// ---------------------------------------------------------------------------
// zstd's tracing hooks. Weak and unimplemented in zstd's own build; a mod that
// links zstd statically still leaves the references behind.

unsigned long long ZSTD_trace_compress_begin(void*) { return 0; }
void ZSTD_trace_compress_end(unsigned long long, const void*) {}
unsigned long long ZSTD_trace_decompress_begin(void*) { return 0; }
void ZSTD_trace_decompress_end(unsigned long long, const void*) {}

}  // extern "C"

// ---------------------------------------------------------------------------
// stdout and stderr.
//
// In newlib these are macros over the reentrancy structure, so there is no such
// symbol to bind to; on glibc they are plain globals and a mod references them
// by name. Defined here under their real names through asm labels, and pointed
// at this program's streams before anything can use them.

extern "C" {
FILE* dusk_glibc_stdout __asm__("stdout") = nullptr;
FILE* dusk_glibc_stderr __asm__("stderr") = nullptr;
}

namespace {
struct StdioAliases {
    StdioAliases() {
        dusk_glibc_stdout = stdout;
        dusk_glibc_stderr = stderr;
    }
};
const StdioAliases s_stdioAliases;
}  // namespace
#endif
