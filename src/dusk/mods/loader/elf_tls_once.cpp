// aleks2 (TLS-lab): std::call_once for Linux-built modules. See elf_tls.hpp (once_shims).
//
// A module compiled by GCC 13 against glibc inlines std::call_once from <mutex>'s _GLIBCXX_HAVE_TLS
// branch: it stores its callable in two thread-locals of libstdc++, std::__once_callable and
// std::__once_call, and calls __gthread_once(&flag, __once_proxy) -- pthread_once on glibc, with
// glibc's pthread_once_t, one int that starts at 0. __once_proxy (libstdc++-v3/src/c++11/mutex.cc,
// same branch) just calls __once_call().
//
// The game's libstdc++ (devkitA64) is built without TLS: its __once_proxy reads __once_functor under a
// mutex and would run an empty std::function. And its pthread_once (libsysbase) sets the flag to
// "running", calls the routine with no cleanup, and waits on that state: if the routine throws (a
// call_once callable may), the flag stays "running" and the next call_once on it waits forever --
// glibc's resets it. So a Linux module gets, by name:
//   * the two thread-locals, through accessors (the game defines them: switch_mod_libstdcxx.cpp on
//     Switch; libstdc++ itself in the Linux host tests);
//   * __once_proxy: calls __once_call, as glibc's libstdc++ does;
//   * pthread_once: glibc's semantics on glibc's int -- 0 not run, 1 running, 2 done -- with the flag
//     back to 0 and the waiters woken when the routine throws, the exception going on to the caller.
// A switch-arm64 module is compiled against the game's own headers and keeps the game's names.
#include "elf_tls.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace std {
extern __thread void* __once_callable;
extern __thread void (*__once_call)();
}  // namespace std

namespace {
std::mutex s_onceMutex;
std::condition_variable s_onceDone;
}  // namespace

extern "C" {

void aleks2_shim_once_proxy_linux() {
    std::__once_call();
}

int aleks2_shim_pthread_once_linux(int* control, void (*routine)()) {
    auto* state = reinterpret_cast<std::atomic<int>*>(control);
    if (state->load(std::memory_order_acquire) == 2) {
        return 0;
    }
    std::unique_lock lock(s_onceMutex);
    for (;;) {
        const int v = state->load(std::memory_order_acquire);
        if (v == 2) {
            return 0;
        }
        if (v == 0) {
            break;
        }
        s_onceDone.wait(lock);
    }
    state->store(1, std::memory_order_relaxed);
    lock.unlock();
    try {
        routine();
    } catch (...) {
        lock.lock();
        state->store(0, std::memory_order_release);
        lock.unlock();
        s_onceDone.notify_all();
        throw;
    }
    lock.lock();
    state->store(2, std::memory_order_release);
    lock.unlock();
    s_onceDone.notify_all();
    return 0;
}

}  // extern "C"

namespace dusk::mods::elf::tls {

std::vector<Shim> once_shims() {
    return {
        {"__once_proxy", static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&aleks2_shim_once_proxy_linux)),
            "std::call_once as glibc's libstdc++ builds it: calls the thread-local __once_call (TLS-lab)", true},
        {"pthread_once", static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&aleks2_shim_pthread_once_linux)),
            "glibc's pthread_once: an exception from the routine resets the flag (TLS-lab)", true},
    };
}

std::vector<HostVariable> once_variables() {
    return {
        {"_ZSt11__once_call", []() -> void* { return reinterpret_cast<void*>(&std::__once_call); }},
        {"_ZSt15__once_callable", []() -> void* { return &std::__once_callable; }},
    };
}

}  // namespace dusk::mods::elf::tls
