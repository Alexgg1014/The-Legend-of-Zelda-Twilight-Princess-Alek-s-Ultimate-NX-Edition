// aleks2 (H37): see elf_runtime_policy.hpp.
#include "elf_runtime_policy.hpp"

#include "elf_image_runtime.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>

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
// A Linux module is built against GCC 13's; the game links devkitA64's libstdc++ (16), and the two
// disagree about ios_base/basic_ios and the streams built on them (H37-c5: Randomizer's basic_ofstream
// constructor ran std::locale::operator= on a null locale in the game's ios_base::_M_init). std::locale
// on its own is not listed: Cosmetics imports six locale symbols and ran on the console. Substrings of the
// Itanium-mangled name -- constructors, members, vtables and VTTs of the family all contain one;
// platforms/switch/tools/elf_audit.py (UNSAFE_LIBSTDCXX_ABI) says the same. Refused for Linux-built
// modules only: a switch-arm64 module is compiled against the game's own headers.
constexpr std::string_view kUnsafeAbi[] = {
    "8ios_base", "9basic_ios", "basic_ifstream", "basic_ofstream", "basic_fstream", "basic_stringstream",
    "basic_ostringstream", "basic_istringstream",
};
// ios_base::Init is the static initialiser <iostream> adds to every translation unit: a library call.
constexpr std::string_view kUnsafeAbiExempt[] = {"ios_base4Init"};
constexpr std::string_view kUnsafeAbiReason =
    "libstdc++ ABI not safe: a Linux-built module's inline std::ios_base/basic_ios/fstream/stringstream "
    "code is laid out for the libstdc++ it was compiled with, not the game's; a switch-arm64 library is required";

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
    return {
        {"__cxa_finalize", addr(&aleks2_shim_cxa_finalize), "no-op: modules are never unloaded"},
        {"__errno_location", addr(&aleks2_shim_errno_location), "errno of the calling thread (newlib's errno)"},
        {"__libc_single_threaded", addr(&aleks2_shim_libc_single_threaded), "0: libstdc++ always uses atomics"},
        {"__stack_chk_guard", addr(&aleks2_shim_stack_chk_guard), "the module's own stack canary"},
        {"stdin", addr(&aleks2_shim_stdin), "the C library's stdin stream"},
        {"stdout", addr(&aleks2_shim_stdout), "the C library's stdout stream"},
        {"stderr", addr(&aleks2_shim_stderr), "the C library's stderr stream"},
    };
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

bool provided_needed(std::string_view soname) {
    return soname == "libc.so.6" || soname == "libm.so.6" || soname == "libstdc++.so.6" ||
           soname == "libgcc_s.so.1" || soname == "ld-linux-aarch64.so.1";
}

Resolver make_resolver(PackageAbi abi, GameLookup game, std::vector<Shim> shimTable, std::vector<Shim> imageRuntime) {
    return [abi, game = std::move(game), shimTable = std::move(shimTable), imageRuntime = std::move(imageRuntime)](
               const ImportQuery& q, std::string& why) -> std::optional<Resolved> {
        for (const auto& s : shimTable) {
            if (s.name == q.name) {
                why = std::string(s.reason);
                return Resolved{s.address, Source::Shim};
            }
        }
        if (abi == PackageAbi::Linux) {
            if (const auto reason = denied(q.name)) {
                why = std::string(*reason);
                return Resolved{0, Source::Denied};
            }
        } else if (!q.version.empty()) {
            why = "a versioned import (" + std::string(q.version) + ") in a package built for Switch";
            return Resolved{0, Source::Denied};
        }
        if (game) {
            if (const auto a = game(q.name)) {
                if (abi == PackageAbi::Linux) {
                    for (const auto& c : kCaveats) {
                        if (c.prefix ? q.name.starts_with(c.pattern) : q.name == c.pattern) {
                            why = std::string(c.reason);
                        }
                    }
                }
                return Resolved{*a, Source::Runtime};
            }
        }
        // Only now: a name the image exports is always the exported one.
        for (const auto& r : imageRuntime) {
            if (r.name == q.name) {
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
