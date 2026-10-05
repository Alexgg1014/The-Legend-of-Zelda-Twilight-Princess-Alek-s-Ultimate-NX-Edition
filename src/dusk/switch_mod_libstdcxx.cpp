// aleks2: the few libstdc++ pieces a mod expects and devkitA64's does not ship.
//
// Companion to switch_mod_libc.cpp, and a different kind of gap. These are not
// glibc's, they are libstdc++'s own -- but devkitA64 builds it for bare metal,
// so the parts that assume a Linux kernel or that only ever got instantiated in
// the shared library are absent from the static one. Three published mods stop
// on exactly six names, which is why this file exists and why it is this short.
//
// Kept deliberately free of the game's precompiled header: everything here is
// standard-library territory and the PCH drags in headers that would collide.
#ifdef __SWITCH__
#include <chrono>
#include <istream>
#include <cwchar>
#include <locale>
#include <thread>

// These two are not missing at all: they are spelled differently.
//
// A C++ symbol carries the names of the types in it, and mbstate_t is a typedef
// -- of `struct __mbstate_t` on glibc, of `struct _mbstate_t` on newlib. So the
// mod asks for basic_istream<char>::seekg(fpos<__mbstate_t>) while libstdc++
// here provides seekg(fpos<_mbstate_t>). Same function, same layout, same
// calling convention; different spelling. Both are therefore aliases, written as
// definitions with the name the mod expects.
//
// The alias is safe because the two structs match: {int, union of 4 bytes},
// eight bytes each, so fpos is sixteen either way and travels in the same
// registers.

// A pointer rather than a reference, and a leading self, because that is what a
// member function is once the compiler is done with it: this in x0, the return
// in x0.
std::basic_istream<char>* dusk_istream_seekg(std::basic_istream<char>* self,
                                             std::fpos<mbstate_t> position)
    __asm__("_ZNSi5seekgESt4fposI11__mbstate_tE");

std::basic_istream<char>* dusk_istream_seekg(std::basic_istream<char>* self,
                                             std::fpos<mbstate_t> position) {
    return &self->seekg(position);
}

// The facet id is a plain object, so the alias is just a second one under the
// other name. Worth knowing what that means: a mod that actually converts
// char16_t through std::use_facet will not find a facet registered against this
// id and will throw bad_cast, because the locale holds the one libstdc++
// registered under its own spelling. Nothing on the list does that -- they
// reference the id and never reach for the facet -- and a throw is in any case
// better than the mod not loading at all.
std::locale::id dusk_codecvt_char16_id __asm__("_ZNSt7codecvtIDsc11__mbstate_tE2idE");

namespace std {

// std::call_once hands its callable to the C library's once-handler through
// these two thread-locals. libstdc++ defines them in mutex.cc, which this
// toolchain does not build.
__thread void* __once_callable;
__thread void (*__once_call)();

// Futex-backed waiting, for std::atomic<T>::wait and the futures that build on
// it. There is no futex on Horizon, so the wait is a poll: the caller is
// blocked either way, and the only cost is latency measured against a sleep
// rather than a wakeup. Notifying is then nothing -- the sleeper rechecks.
//
// The class is declared here because libstdc++ only declares it when it has a
// Linux futex to call, so on this toolchain there is nothing to include.
struct __atomic_futex_unsigned_base {
    static bool _M_futex_wait_until(unsigned* addr, unsigned value, bool hasTimeout,
                                    chrono::seconds seconds, chrono::nanoseconds nanoseconds);
    static void _M_futex_notify_all(unsigned* addr);
};

bool __atomic_futex_unsigned_base::_M_futex_wait_until(
    unsigned* addr, unsigned value, bool hasTimeout,
    chrono::seconds seconds, chrono::nanoseconds nanoseconds) {
    // 200 us keeps a handoff from feeling stalled without spinning a core; the
    // callers here are mod bookkeeping, not a render loop.
    constexpr auto kPoll = chrono::microseconds{200};
    const auto deadline = chrono::system_clock::time_point{seconds + nanoseconds};
    while (__atomic_load_n(addr, __ATOMIC_ACQUIRE) == value) {
        if (hasTimeout && chrono::system_clock::now() >= deadline) {
            return false;
        }
        this_thread::sleep_for(kPoll);
    }
    return true;
}

void __atomic_futex_unsigned_base::_M_futex_notify_all(unsigned*) {}

// GCC 13 moved the standard streams' initialisation behind this, called from
// <iostream>. By the time a mod runs, the program has long since initialised
// them, so there is nothing left to do -- but the symbol has to be there.
void ios_base_library_init() {}

}  // namespace std
#endif
