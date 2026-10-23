// TLS-lab: a module with thread-local storage of every kind the catalog uses, for elf_tls_exec_test.cpp.
// Built by test-elf-tls.sh with aarch64-linux-gnu-g++ -fPIC -shared, three times: the default
// dialect (TLSDESC, what every catalog mod has), -mtls-dialect=trad (__tls_get_addr with
// DTPMOD64/DTPREL64) and -ftls-model=initial-exec (TPREL64 + DF_STATIC_TLS, which must be refused).
#include <atomic>
#include <cstdint>
#include <string>

extern "C" {

// The five exports elf_loader::plan() requires of a mod; never called here.
int mod_meta = 0;
void* mod_ctx = nullptr;
int mod_initialize() { return 0; }
int mod_update() { return 0; }
int mod_shutdown() { return 0; }

int g_global = 7;
std::atomic<int> g_destroyed{0};
std::atomic<int> g_constructed{0};

// Global, default visibility: TLSDESC against the symbol.
thread_local int t_counter = 41;                 // .tdata
thread_local int t_probe = 5;                    // the variable tls_regcheck() reaches (elf_tls_test_regs.S)
thread_local int* t_pointer = &g_global;         // .tdata holding a pointer: relocated in the template
thread_local uint64_t t_big[1452];               // .tbss, 11616 bytes like Dawnlight 4.0.3

// A thread-local of the game, imported: the loader reaches it through an accessor.
extern __thread int aleks2_test_host_var;

int tls_regcheck();
}

namespace {
// Local: TLSDESC against the section, the module's own offset in the addend.
alignas(64) thread_local char t_aligned[64];
thread_local int t_local = 1000;

struct Tracked {
    std::string text;
    Tracked() : text("constructed in this thread") { g_constructed.fetch_add(1); }
    ~Tracked() { g_destroyed.fetch_add(1); }
};
thread_local Tracked t_tracked;  // dynamic initialisation + __cxa_thread_atexit

// A constructor that already touches a thread-local: the template must be registered before it runs.
__attribute__((constructor)) void at_load() { ++t_counter; }
}  // namespace

extern "C" {
int tls_bump() { return ++t_counter; }
void* tls_counter_address() { return &t_counter; }
int tls_pointer_value() { return *t_pointer; }
int tls_local_bump() { return ++t_local; }
uintptr_t tls_aligned_address() { return reinterpret_cast<uintptr_t>(&t_aligned[0]); }
uint64_t tls_big_fill(uint64_t seed) {
    uint64_t sum = 0;
    for (uint64_t i = 0; i < 1452; ++i) {
        t_big[i] += seed + i;
        sum += t_big[i];
    }
    return sum;
}
const char* tls_tracked() { return t_tracked.text.c_str(); }
int tls_destroyed() { return g_destroyed.load(); }
int tls_constructed() { return g_constructed.load(); }
int tls_probe() { return t_probe; }
int tls_host_get() { return aleks2_test_host_var; }
void tls_host_set(int v) { aleks2_test_host_var = v; }
int tls_regs() { return tls_regcheck(); }
}

// std::call_once compiled as a Linux module compiles it (libstdc++'s TLS branch): two thread-locals of
// libstdc++ imported (__once_call, __once_callable), __once_proxy and pthread_once -- the TLS-lab
// shims of elf_tls_once.cpp in the loader, the same code the game uses. The threads of the concurrent
// case are the host's, so the same module runs in the Switch self-test.
#include <mutex>

namespace {
std::once_flag f_basic, f_concurrent, f_outer, f_inner, f_throw;
std::atomic<int> n_basic{0}, n_concurrent{0}, n_outer{0}, n_inner{0}, n_attempts{0}, n_done{0};
std::atomic<int> n_running{0}, n_overlap{0};
struct OnceFailure {};
}  // namespace

extern "C" {
// Five calls, one run.
int once_basic() {
    for (int i = 0; i < 5; ++i) {
        std::call_once(f_basic, [] { n_basic.fetch_add(1); });
    }
    return n_basic.load();
}
// Called by many host threads at once: the routine runs once, slowly, and nobody returns before it ends.
int once_concurrent_entry() {
    std::call_once(f_concurrent, [] {
        if (n_running.fetch_add(1) != 0) n_overlap.fetch_add(1);
        for (int spin = 0; spin < 2000000; ++spin) {
            asm volatile("" ::: "memory");  // a busy routine, so the other callers really wait
        }
        n_concurrent.fetch_add(1);
        n_running.fetch_sub(1);
    });
    return n_concurrent.load();  // 1 for every caller: the routine has finished before call_once returns
}
int once_concurrent_count() { return n_concurrent.load() * 100 + n_overlap.load(); }
// call_once inside call_once, on another flag: both run once.
int once_nested() {
    for (int i = 0; i < 3; ++i) {
        std::call_once(f_outer, [] {
            std::call_once(f_inner, [] { n_inner.fetch_add(1); });
            n_outer.fetch_add(1);
        });
    }
    return n_outer.load() * 10 + n_inner.load();
}
// The first run throws: the exception reaches the caller and the flag is not set, so the next call runs
// again, succeeds, and a third does nothing. Returns attempts * 10 + successes (21 when right).
int once_throw_retry() {
    try {
        std::call_once(f_throw, [] {
            n_attempts.fetch_add(1);
            throw OnceFailure{};  // a type of the module: its typeinfo is not an import
        });
    } catch (const OnceFailure&) {
    }
    std::call_once(f_throw, [] {
        n_attempts.fetch_add(1);
        n_done.fetch_add(1);
    });
    std::call_once(f_throw, [] { n_attempts.fetch_add(100); });
    return n_attempts.load() * 10 + n_done.load();
}
}
