#pragma once
// aleks2 (TLS-lab): dynamic thread-local storage for modules mapped by elf_loader.
//
// A linux-aarch64 mod reaches its thread-locals the way any AArch64 shared object does
// (aleks2 workspace: dusklight-aleks2/docs/investigaciones/tls-dinamico-diseno.md): a TLS descriptor in its GOT, two words, a resolver
// and an argument. The code calls the resolver with x0 = &descriptor and adds what it returns to
// TPIDR_EL0, which it may have read before the call (GCC schedules the mrs freely):
//
//     adrp x0, :tlsdesc:v ; ldr x1, [x0, :tlsdesc_lo12:v] ; add x0, x0, :tlsdesc_lo12:v ; blr x1
//     mrs  x8, tpidr_el0  ; ldr w0, [x8, x0]
//
// The game never uses TPIDR_EL0 for its own thread-locals: devkitA64 builds it with -mtp=soft,
// whose thread pointer is libnx's ThreadVars::tls_tp, found through TPIDRRO_EL0. So this runtime
// does not give TPIDR_EL0 a meaning either. It keeps each thread's blocks in a table found through
// the game's OWN thread-local storage (aleks2_tls_dtv, below), and the resolver returns
// `block + offset - TPIDR_EL0`: whatever value the register holds in that thread, the sum the
// module computes is the block's address. The kernel saves and restores TPIDR_EL0 per thread
// (Mesosphere's EL0 exception entry), so the two reads in one sequence always agree.
//
// What follows from it:
//   * every thread works, including those that existed before the module was loaded: a thread's
//     table and blocks are created on its first access, in the resolver's slow path;
//   * nothing is done at thread creation and nothing in libnx changes;
//   * a block is allocated per (thread, module) on first access, from the module's template
//     (the relocated initialisation image, then zeroes);
//   * at thread exit (a pthread key destructor: libnx runs them in threadExit), the C++
//     thread_local destructors the module registered (__cxa_thread_atexit, shimmed here) run
//     first, then the blocks are freed. The main thread never exits: its destructors never run,
//     like the module's FINI.
//   * modules are never unloaded (the loader's own rule), so no generation counter is needed.
//
// Not provided: initial-exec TLS (R_AARCH64_TLS_TPREL64, DF_STATIC_TLS), which would need a
// static block at a fixed offset from TPIDR_EL0 in every thread; elf_loader refuses it.
#include "elf_loader.hpp"
#include "elf_runtime_policy.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

extern "C" {

// The descriptor's argument for a module's own thread-local. Read by the assembly: keep the layout.
struct Aleks2TlsDescArg {
    uint64_t module;  // >= 1
    uint64_t offset;  // in the module's block
};

// The descriptor's argument for a thread-local the module imports from the game.
struct Aleks2TlsAccessorArg {
    void* (*accessor)();  // the variable's address in the calling thread
    int64_t addend;
};

// glibc's tls_index, what __tls_get_addr receives (the traditional dialect, -mtls-dialect=trad).
struct Aleks2TlsIndex {
    uint64_t module;
    uint64_t offset;
};

// The resolvers (elf_tls_aarch64.S). Not callable from C: their convention is the descriptor's. They
// have no unwind information: nothing may propagate an exception through them, so the C++ they call
// is noexcept (an exception there is std::terminate, never an unwind through the assembly).
void aleks2_tlsdesc_dynamic();
void aleks2_tlsdesc_accessor();

// The slow paths the resolvers call, and the functions a module binds to by name.
void* aleks2_tls_slow(const Aleks2TlsDescArg* arg) noexcept;
void* aleks2_tls_accessor_slow(const Aleks2TlsAccessorArg* arg) noexcept;
void* aleks2_tls_get_addr(const Aleks2TlsIndex* index) noexcept;
int aleks2_tls_cxa_thread_atexit(void (*dtor)(void*), void* object, void* dso) noexcept;

// std::call_once's shims for Linux modules (elf_tls_once.cpp); exposed for tests.
void aleks2_shim_once_proxy_linux();
int aleks2_shim_pthread_once_linux(int* control, void (*routine)());
}

namespace dusk::mods::elf::tls {

// A thread-local of the game that a module may import, with its accessor.
struct HostVariable {
    std::string_view name;
    void* (*accessor)();
};

// The TLS provider elf_loader::map() takes. One per process (runtime()).
class Runtime final : public TlsProvider {
public:
    // Creates the thread-exit key the first time; false (with `error`) if none is left. Called by
    // register_module(), so a module with PT_TLS fails map() -- before any of its code runs -- rather
    // than the game failing at the module's first TLS access.
    bool prepare(std::string& error) override;
    void abandon(uint64_t module) override;
    uint64_t register_module(uint64_t image, const TlsTemplate& tls, std::string& error) override;
    std::pair<uint64_t, uint64_t> descriptor(uint64_t module, uint64_t offset) override;
    std::pair<uint64_t, uint64_t> import_descriptor(uint64_t accessor, int64_t addend) override;
    uint64_t import_module(uint64_t accessor) override;
};

// import_module's ids have this bit set; the rest is an index into the accessor table.
inline constexpr uint64_t kAccessorModule = 1ull << 63;
Runtime& runtime();

// The names this runtime stands in for, for make_resolver's shim table: __tls_get_addr,
// __cxa_thread_atexit and __cxa_thread_atexit_impl. Shims are consulted before the denial list.
std::vector<Shim> shims();

// A resolver lookup for thread-local imports (ImportQuery::tls) over `variables`.
std::function<std::optional<uint64_t>(std::string_view name)> host_variables(std::vector<HostVariable> variables);

// std::call_once as a Linux-built module compiles it (elf_tls_once.cpp): shims for __once_proxy and
// pthread_once (Linux modules only), and the accessors of libstdc++'s two once thread-locals, which
// the game defines (switch_mod_libstdcxx.cpp on Switch).
std::vector<Shim> once_shims();
std::vector<HostVariable> once_variables();

// For tests and reports.
struct Stats {
    uint64_t modules = 0;          // registered templates
    uint64_t threads = 0;          // threads that ever touched module TLS
    uint64_t blocks = 0;           // blocks allocated, all threads
    uint64_t freedBlocks = 0;      // blocks freed at thread exit
    uint64_t destructorsRun = 0;   // __cxa_thread_atexit destructors run at thread exit
    uint64_t abandoned = 0;        // templates of mappings that failed after registering
    bool keyReady = false;         // the thread-exit key exists
};

// Fault injection, for tests: the next `count` attempts of `fault` fail. KeyCreate fails prepare()
// (recoverable: map() refuses the module); SetSpecific and Alloc fail inside a TLS access (fatal).
enum class Fault { KeyCreate = 0, SetSpecific = 1, Alloc = 2 };
void inject_fault(Fault fault, int count);

// Where a fatal TLS error is reported before the process stops (default: stderr). The Switch loader
// sends it to the game's log.
void set_fatal_handler(void (*handler)(const char* what));
Stats stats();

// DM2-d: the calling thread's block of `module` if it has one already (never allocates), else nullptr --
// what glibc reports as dl_phdr_info::dlpi_tls_data.
void* current_block(uint64_t module);

}  // namespace dusk::mods::elf::tls
