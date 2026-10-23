// aleks2 (H37): see elf_runtime_policy.hpp.
#include "elf_runtime_policy.hpp"

#include "elf_image_runtime.h"

#include <dirent.h>
#include <atomic>
#include <pthread.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// --- the shims ---------------------------------------------------------------------------
// At file scope and with C linkage: they are bound to by name from a module's GOT, so they
// must be plain functions and objects. Each has glibc's semantics for what a mod built
// against glibc expects of it.
extern "C" {

// glibc's errno accessor; newlib keeps errno per reent and exposes it as the macro errno.
int* aleks2_shim_errno_location() {
    return &errno;
}

// A module is never unloaded (elf_loader keeps every mapping until the game exits), so its
// crtbegin's __do_global_dtors_aux, the only caller, never runs. The game's own
// __cxa_finalize must not be reachable from a mod either: it would run the game's handlers.
void aleks2_shim_cxa_finalize(void*) {}

// glibc's flag that lets libstdc++ skip atomics when only one thread exists. 0 = "there may be
// other threads": always correct, never faster.
char aleks2_shim_libc_single_threaded = 0;

// The canary a module compiled with -fstack-protector compares against. It is the module's
// own: what matters is that the value does not change while a frame is live.
uintptr_t aleks2_shim_stack_chk_guard = 0x2f8a6d1c7e3b5940ull;

// glibc's stdin/stdout/stderr are FILE* variables a module reads through its GOT. Here each is a
// variable holding the C library's own stream, so what the module then passes to fwrite & co.
// (which resolve to the game's C library) is a stream that library knows.
FILE* aleks2_shim_stdin = stdin;
FILE* aleks2_shim_stdout = stdout;
FILE* aleks2_shim_stderr = stderr;

// V1-rc5: a mod that ends the program (abort, exit, a failed stack canary or fortify check, an assert) used to
// reach newlib's, which exits cleanly: no crash report, no line in the log, and sdmc unmounted under the other
// threads (three tester logs with rc4 just stop). Each now says who called it and stops with a panic, which
// Atmosphere turns into a crash report with every thread. aleks2_mod_fatal is the game's (session_log.cpp);
// without it (host builds) they abort as before.
__attribute__((weak, noreturn)) void aleks2_mod_fatal(const char* what, const void* caller);

[[noreturn]] static void mod_fatal(const char* what, const void* caller) {
    if (aleks2_mod_fatal != nullptr) {
        aleks2_mod_fatal(what, caller);
    }
    std::abort();
}

// V1-rc8: threads a mod starts are counted while they run (aleks2_exit_guard, switch_startup.cpp: a mod's thread
// still running when the program returns to hbloader crashes hbloader). Same pthread_create, same attributes; only
// the start routine is wrapped. One that ends with pthread_exit stays counted: the exit then takes the safe path.
static std::atomic<int> s_liveModThreads{0};
struct ModThreadStart {
    void* (*fn)(void*);
    void* arg;
};
static void* mod_thread_entry(void* p) {
    const ModThreadStart start = *static_cast<ModThreadStart*>(p);
    delete static_cast<ModThreadStart*>(p);
    void* result = start.fn(start.arg);
    s_liveModThreads.fetch_sub(1, std::memory_order_relaxed);
    return result;
}
int aleks2_shim_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*fn)(void*), void* arg) {
    auto* start = new ModThreadStart{fn, arg};
    s_liveModThreads.fetch_add(1, std::memory_order_relaxed);
    const int rc = pthread_create(thread, attr, &mod_thread_entry, start);
    if (rc != 0) {
        s_liveModThreads.fetch_sub(1, std::memory_order_relaxed);
        delete start;
    }
    return rc;
}
int aleks2_live_mod_threads(void) { return s_liveModThreads.load(std::memory_order_relaxed); }

[[noreturn]] void aleks2_shim_abort() { mod_fatal("abort()", __builtin_return_address(0)); }
[[noreturn]] void aleks2_shim_exit(int) { mod_fatal("exit()", __builtin_return_address(0)); }
[[noreturn]] void aleks2_shim_stack_chk_fail() {
    mod_fatal("stack smashing detected (__stack_chk_fail)", __builtin_return_address(0));
}
[[noreturn]] void aleks2_shim_chk_fail() { mod_fatal("buffer overflow detected (__chk_fail)", __builtin_return_address(0)); }
[[noreturn]] void aleks2_shim_fortify_fail(const char*) {
    mod_fatal("buffer overflow detected (__fortify_fail)", __builtin_return_address(0));
}
// glibc's assert: (expression, file, line, function).
[[noreturn]] void aleks2_shim_assert_fail(const char*, const char*, unsigned, const char*) {
    mod_fatal("assertion failed (__assert_fail)", __builtin_return_address(0));
}
// newlib's assert: (file, line, function, expression).
[[noreturn]] void aleks2_shim_assert_func(const char*, int, const char*, const char*) {
    mod_fatal("assertion failed (__assert_func)", __builtin_return_address(0));
}

}  // extern "C"

// DM1-c: glibc's struct dirent, as a module built against it reads the one readdir returns. newlib's
// (sys/dirent.h) is {d_ino, d_type, d_name} with the name at offset 9; glibc's has d_off and d_reclen
// first and the name at 19. DIR* stays the C library's own, opaque to the module either way.
struct Aleks2GlibcDirent {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;  // DT_* values are the same in both C libraries
    // glibc declares 256; the card's file systems allow longer UTF-8 names, and a module only reads
    // up to the terminator.
    char d_name[1024];
};
static_assert(offsetof(Aleks2GlibcDirent, d_ino) == 0 && offsetof(Aleks2GlibcDirent, d_off) == 8 &&
              offsetof(Aleks2GlibcDirent, d_reclen) == 16 && offsetof(Aleks2GlibcDirent, d_type) == 18 &&
              offsetof(Aleks2GlibcDirent, d_name) == 19);

namespace {
// One entry per open stream, as glibc keeps it: valid until the next readdir or closedir of that DIR.
std::mutex s_dirMutex;
std::unordered_map<DIR*, std::unique_ptr<Aleks2GlibcDirent>> s_dirEntries;
}  // namespace

extern "C" {

Aleks2GlibcDirent* aleks2_shim_readdir(DIR* dir) {
    const dirent* entry = readdir(dir);  // NULL at the end (errno untouched) or on error (errno set)
    if (entry == nullptr) {
        return nullptr;
    }
    std::lock_guard lock{s_dirMutex};
    auto& slot = s_dirEntries[dir];
    if (!slot) {
        slot = std::make_unique<Aleks2GlibcDirent>();
    }
    const size_t length = strnlen(entry->d_name, sizeof slot->d_name - 1);
    slot->d_ino = static_cast<uint64_t>(entry->d_ino);
    slot->d_off = 0;  // opaque in glibc too; telldir() is the portable position
    slot->d_reclen = static_cast<unsigned short>(offsetof(Aleks2GlibcDirent, d_name) + length + 1);
    slot->d_type = static_cast<unsigned char>(entry->d_type);
    std::memcpy(slot->d_name, entry->d_name, length);
    slot->d_name[length] = '\0';
    return slot.get();
}

int aleks2_shim_closedir(DIR* dir) {
    {
        std::lock_guard lock{s_dirMutex};
        s_dirEntries.erase(dir);
    }
    return closedir(dir);
}

}  // extern "C"

namespace dusk::mods::elf {
namespace {

template <class F>
uint64_t addr(F* f) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(f));
}

// Prefix matches for names a Linux-built module may not bind to.
struct Denial {
    std::string_view pattern;
    bool prefix;
    std::string_view reason;
};

constexpr Denial kDenied[] = {
    {"_IO_", true, "glibc's FILE internals"},
    {"_dl_", true, "glibc dynamic-linker internals"},
    {"dl_iterate_phdr", false, "there is no list of loaded objects to walk"},
    {"__tls_get_addr", false, "thread-local storage is not provided"},
    {"__cxa_thread_atexit_impl", false, "thread-local destructors are not provided"},
    {"__register_atfork", false, "there is no fork"},
    {"fork", false, "there is no fork"},
    {"syscall", false, "Linux system calls do not exist on Horizon"},
    {"__libc_", true, "glibc internals"},
    {"gnu_get_libc_version", false, "glibc internals"},
};

// H37-c6: names whose C++ objects are laid out by the libstdc++ headers the module was compiled with.
// A Linux module is built against GCC 13's; the game links devkitA64's libstdc++ (16).
// DM2-b, measured (research/plan-mods-100/iolayout): ios_base, basic_ios, the streambufs, istream/ostream,
// the string streams, locale and string are laid out alike; only basic_filebuf differs (glibc's 48-byte
// __c_lock against newlib's 12: everything from _M_file on moves 32 bytes), and with it the file streams
// built on it. H37-c5's crash (Randomizer's basic_ofstream constructor, a null locale in ios_base::_M_init)
// is that difference. So only those are refused -- and only when no glibc-layout instance of the name is
// in the shim table (linux_stream_shims(): char, in a build with ALEKS2_MOD_LINUX_STREAMS), since the shim
// table is consulted first. Substrings of the Itanium-mangled name; platforms/switch/tools/elf_audit.py
// (UNSAFE_LIBSTDCXX_ABI) says the same. Refused for Linux-built modules only.
constexpr std::string_view kUnsafeAbi[] = {
    "13basic_filebuf", "14basic_ifstream", "14basic_ofstream", "13basic_fstream",
    // derives from basic_filebuf; no glibc-layout instance of it (sol's review, point 2)
    "13stdio_filebuf",
};
constexpr std::string_view kUnsafeAbiExempt[] = {"ios_base4Init"};
constexpr std::string_view kUnsafeAbiReason =
    "libstdc++ ABI not safe: std::basic_filebuf and the file streams are laid out for glibc (48-byte __c_lock) in "
    "a Linux-built module, and this game image has no glibc-layout instance of this one; a switch-arm64 library "
    "is required";

// Names that resolve, with a caveat the report carries. Measured against devkitPro's newlib
// (sys/_pthreadtypes.h, machine/aarch64/sys/lock.h): newlib's pthread_mutex_t is 12 bytes and
// glibc's 48, and an all-zero glibc mutex is a valid unlocked newlib one; but glibc's
// recursive-mutex static initialiser sets a field newlib does not read (so it acts as a normal
// mutex), and an all-zero glibc condition variable carries clock 0, CLOCK_REALTIME_COARSE.
constexpr Denial kCaveats[] = {
    {"pthread_", true, "glibc-built pthread object on newlib: zero-initialised mutexes and condition "
                       "variables match; static recursive-mutex initialisers act as normal mutexes"},
    {"sem_", true, "glibc-built sem_t on newlib: layouts differ unless sem_init is called"},
};

}  // namespace

// Only what nothing else provides. The other glibc-only names published mods use
// (__ctype_*_loc, __isoc23_strtol & co., __printf_chk, getrandom...) are defined by the NRO
// itself in src/dusk/switch_mod_libc.cpp and resolve through the game's symbols.
std::vector<Shim> shims() {
    std::vector<Shim> table = {
        {"__cxa_finalize", addr(&aleks2_shim_cxa_finalize), "no-op: modules are never unloaded"},
        {"__errno_location", addr(&aleks2_shim_errno_location), "errno of the calling thread (newlib's errno)"},
        {"__libc_single_threaded", addr(&aleks2_shim_libc_single_threaded), "0: libstdc++ always uses atomics"},
        {"__stack_chk_guard", addr(&aleks2_shim_stack_chk_guard), "the module's own stack canary"},
        {"stdin", addr(&aleks2_shim_stdin), "the C library's stdin stream"},
        {"stdout", addr(&aleks2_shim_stdout), "the C library's stdout stream"},
        {"stderr", addr(&aleks2_shim_stderr), "the C library's stderr stream"},
        // V1-rc5: logged with the caller, then a panic (a crash report) instead of a clean exit.
        {"pthread_create", addr(&aleks2_shim_pthread_create), "pthread_create, the thread counted while it runs"},
        {"abort", addr(&aleks2_shim_abort), "abort: logged, then a crash report"},
        {"exit", addr(&aleks2_shim_exit), "exit from a mod: logged, then a crash report"},
        {"_exit", addr(&aleks2_shim_exit), "_exit from a mod: logged, then a crash report"},
        {"__stack_chk_fail", addr(&aleks2_shim_stack_chk_fail), "stack canary failure: logged, then a crash report"},
        {"__chk_fail", addr(&aleks2_shim_chk_fail), "fortify failure: logged, then a crash report"},
        {"__fortify_fail", addr(&aleks2_shim_fortify_fail), "fortify failure: logged, then a crash report"},
        {"__assert_fail", addr(&aleks2_shim_assert_fail), "glibc assert: logged, then a crash report"},
        {"__assert_func", addr(&aleks2_shim_assert_func), "newlib assert: logged, then a crash report"},
        // DM1-c: Linux-built modules only; a switch-arm64 one gets the game's own (newlib layout).
        {"readdir", addr(&aleks2_shim_readdir), "readdir with glibc's struct dirent layout", true},
        {"closedir", addr(&aleks2_shim_closedir), "closedir, releasing the glibc-layout entry", true},
    };
#if ALEKS2_MOD_LINUX_STREAMS
    for (const auto& s : linux_stream_shims()) {
        table.push_back(s);
    }
    for (const auto& s : linux_ctype_shims()) {
        table.push_back(s);
    }
#endif
    return table;
}

std::vector<Shim> image_runtime() {
    std::vector<Shim> table;
    for (size_t i = 0; i < aleks2_image_runtime_count; ++i) {
        const auto& e = aleks2_image_runtime[i];
        table.push_back({e.name, addr(e.address), e.library});
    }
    return table;
}

std::optional<std::string_view> denied(std::string_view name) {
    for (const auto& d : kDenied) {
        if (d.prefix ? name.starts_with(d.pattern) : name == d.pattern) {
            return d.reason;
        }
    }
    if (name.starts_with("_Z")) {
        for (const auto sub : kUnsafeAbi) {
            if (name.find(sub) == std::string_view::npos) {
                continue;
            }
            for (const auto exempt : kUnsafeAbiExempt) {
                if (name.find(exempt) != std::string_view::npos) {
                    return std::nullopt;
                }
            }
            return kUnsafeAbiReason;
        }
    }
    return std::nullopt;
}

std::string newlib_spelling(std::string_view name) {
    constexpr std::string_view kGlibc = "11__mbstate_t";
    constexpr std::string_view kNewlib = "10_mbstate_t";
    std::string out(name);
    for (size_t at = out.find(kGlibc); at != std::string::npos; at = out.find(kGlibc, at + kNewlib.size())) {
        out.replace(at, kGlibc.size(), kNewlib);
    }
    return out;
}

bool provided_needed(std::string_view soname) {
    // V1-rc1: glibc 2.34+ moved libpthread, libdl and librt into libc and keeps the old names as
    // empty stubs, so a module linked against them asks libc for those functions anyway.
    return soname == "libc.so.6" || soname == "libm.so.6" || soname == "libstdc++.so.6" ||
           soname == "libgcc_s.so.1" || soname == "ld-linux-aarch64.so.1" || soname == "libpthread.so.0" ||
           soname == "libdl.so.2" || soname == "librt.so.1";
}

Resolver make_resolver(PackageAbi abi, GameLookup game, std::vector<Shim> shimTable, std::vector<Shim> imageRuntime,
    GameLookup tlsImports) {
    return [abi, game = std::move(game), shimTable = std::move(shimTable), imageRuntime = std::move(imageRuntime),
               tlsImports = std::move(tlsImports)](const ImportQuery& q, std::string& why) -> std::optional<Resolved> {
        if (q.tls) {
            if (tlsImports) {
                if (const auto a = tlsImports(q.name)) {
                    why = "a thread-local of the game, through its accessor";
                    return Resolved{*a, Source::Runtime};
                }
            }
            why = "a thread-local the game does not offer to modules";
            return std::nullopt;
        }
        // DM2-b: a Linux module spells libstdc++ names the glibc way (fpos<__mbstate_t>, codecvt<..., __mbstate_t>);
        // the game's are newlib's (_mbstate_t), the same 8-byte struct. Looked up under the newlib spelling.
        const std::string respelled = abi == PackageAbi::Linux ? newlib_spelling(q.name) : std::string{};
        const std::string_view name = abi == PackageAbi::Linux ? std::string_view{respelled} : q.name;
        for (const auto& s : shimTable) {
            if (s.name == name && (!s.linuxOnly || abi == PackageAbi::Linux)) {
                why = std::string(s.reason);
                return Resolved{s.address, Source::Shim};
            }
        }
        if (abi == PackageAbi::Linux) {
            if (const auto reason = denied(name)) {
                why = std::string(*reason);
                return Resolved{0, Source::Denied};
            }
        } else if (!q.version.empty()) {
            why = "a versioned import (" + std::string(q.version) + ") in a package built for Switch";
            return Resolved{0, Source::Denied};
        }
        if (game) {
            if (const auto a = game(name)) {
                if (abi == PackageAbi::Linux) {
                    for (const auto& c : kCaveats) {
                        if (c.prefix ? name.starts_with(c.pattern) : name == c.pattern) {
                            why = std::string(c.reason);
                        }
                    }
                    if (name != q.name) {
                        why = "glibc's __mbstate_t is newlib's _mbstate_t (same layout): bound as " + respelled;
                    }
                }
                return Resolved{*a, Source::Runtime};
            }
        }
        // Only now: a name the image exports is always the exported one.
        for (const auto& r : imageRuntime) {
            if (r.name == name) {
                why = "the game image's own " + std::string(r.reason) + " (hidden there, so not exported)";
                return Resolved{r.address, Source::Runtime};
            }
        }
        why = "not in the game's exported surface";
        return std::nullopt;
    };
}

NeededPolicy make_needed_policy(PackageAbi abi) {
    return [abi](std::string_view soname, std::string& why) {
        if (abi == PackageAbi::Linux && provided_needed(soname)) {
            return true;
        }
        why = abi == PackageAbi::Linux ?
                  "only the Linux C/C++ runtime libraries are stood in for by the game image" :
                  "a package built for Switch may not depend on another library";
        return false;
    };
}

}  // namespace dusk::mods::elf
