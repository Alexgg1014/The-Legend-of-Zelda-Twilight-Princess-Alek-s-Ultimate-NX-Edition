// aleks2 (TLS-lab): see elf_tls.hpp.
#include "elf_tls.hpp"

#include <pthread.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

namespace {

struct Template {
    uint64_t image = 0;  // runtime address of the initialisation image
    dusk::mods::elf::TlsTemplate tls;
    bool abandoned = false;  // map() failed after registering it: nothing may reach it
};

// Append-only: modules are never unloaded. A deque keeps every element where it is, so a slow path
// may read one after dropping the lock.
std::mutex g_mutex;
std::deque<Template> g_templates;  // module id N is g_templates[N - 1]
std::deque<Aleks2TlsDescArg> g_descArgs;
std::deque<Aleks2TlsAccessorArg> g_accessorArgs;
std::deque<void* (*)()> g_accessorModules;  // import_module(): id kAccessorModule | index

std::atomic<uint64_t> g_threads{0}, g_blocks{0}, g_freedBlocks{0}, g_destructorsRun{0}, g_abandoned{0};

// Faults a test injects (inject_fault): each counter is how many of the next attempts fail.
std::atomic<int> g_faults[3]{};
bool injected(dusk::mods::elf::tls::Fault f) {
    auto& n = g_faults[static_cast<int>(f)];
    int v = n.load();
    while (v > 0) {
        if (n.compare_exchange_weak(v, v - 1)) {
            return true;
        }
    }
    return false;
}

void default_fatal(const char* what) {
    std::fprintf(stderr, "aleks2 TLS: %s\n", what);
}
void (*g_fatal)(const char*) = default_fatal;

struct Destructor {
    void (*fn)(void*);
    void* object;
};

// One thread's state. dtv[0] holds the capacity, dtv[N] module N's block or null; the assembly reads
// it through aleks2_tls_dtv.
struct ThreadState {
    void** dtv = nullptr;
    Destructor* dtors = nullptr;
    size_t dtorCount = 0, dtorCapacity = 0;
};

// The one key whose destructor ends a thread's module TLS. Created by Runtime::prepare(), before any
// module with TLS is mapped -- so before any of its code runs -- and never deleted.
std::mutex g_keyMutex;
pthread_key_t g_exitKey;
std::atomic<bool> g_keyReady{false};

}  // namespace

extern "C" {
// The game's own thread-locals (on Switch: -mtp=soft, libnx's tls_tp), not the module's register.
// __thread with C linkage: plain storage the assembly reaches with local-exec relocations.
__thread void** aleks2_tls_dtv = nullptr;
}

namespace {
__thread ThreadState* t_state = nullptr;

// Fatal: reached only from a module's TLS access (the resolver's slow path, __tls_get_addr), which has
// no way to report an error -- its caller expects an address. Returning a made-up one would let the
// module write somewhere else's memory, so the process stops, after the fatal handler has logged why.
[[noreturn]] void die(const char* what) {
    g_fatal(what);
    std::abort();
}

void thread_exit(void* p);

ThreadState* state() {
    if (t_state != nullptr) {
        return t_state;
    }
    if (!g_keyReady.load(std::memory_order_acquire)) {
        die("a module accessed TLS before the runtime was prepared (no thread-exit key)");
    }
    auto* s = injected(dusk::mods::elf::tls::Fault::Alloc) ?
                  nullptr :
                  static_cast<ThreadState*>(std::calloc(1, sizeof(ThreadState)));
    if (s == nullptr) {
        die("out of memory for a thread's TLS state (first module TLS access in this thread)");
    }
    // Associated with the thread's exit before it is published: a state the exit key does not know
    // would never be freed, nor its thread_local destructors run.
    if (injected(dusk::mods::elf::tls::Fault::SetSpecific) || pthread_setspecific(g_exitKey, s) != 0) {
        std::free(s);
        die("pthread_setspecific failed for a thread's TLS state (first module TLS access in this thread)");
    }
    t_state = s;
    g_threads.fetch_add(1, std::memory_order_relaxed);
    return s;
}

const Template& template_of(uint64_t module) {
    std::lock_guard lock(g_mutex);
    if (module == 0 || module > g_templates.size()) {
        die("a TLS descriptor names a module that was never registered");
    }
    if (g_templates[module - 1].abandoned) {
        die("a TLS descriptor names a module whose mapping failed");
    }
    return g_templates[module - 1];
}

void* block(uint64_t module) {
    ThreadState* s = state();
    const uint64_t capacity = s->dtv != nullptr ? reinterpret_cast<uintptr_t>(s->dtv[0]) : 0;
    if (module >= capacity) {
        uint64_t grown = capacity < 8 ? 8 : capacity;
        while (grown <= module) {
            grown *= 2;
        }
        auto** dtv = static_cast<void**>(std::calloc(grown, sizeof(void*)));
        if (dtv == nullptr) {
            die("out of memory for a thread's TLS table");
        }
        if (s->dtv != nullptr) {
            std::memcpy(dtv + 1, s->dtv + 1, (capacity - 1) * sizeof(void*));
            std::free(s->dtv);
        }
        dtv[0] = reinterpret_cast<void*>(static_cast<uintptr_t>(grown));
        s->dtv = dtv;
        aleks2_tls_dtv = dtv;
    }
    if (s->dtv[module] == nullptr) {
        const Template& t = template_of(module);
        const uint64_t align = t.tls.align < alignof(std::max_align_t) ? alignof(std::max_align_t) : t.tls.align;
        const uint64_t size = (t.tls.memsz + align - 1) & ~(align - 1);
        void* b = injected(dusk::mods::elf::tls::Fault::Alloc) ? nullptr :
                                                                 std::aligned_alloc(align, size == 0 ? align : size);
        if (b == nullptr) {
            die("out of memory for a module's TLS block");
        }
        std::memcpy(b, reinterpret_cast<const void*>(static_cast<uintptr_t>(t.image)), t.tls.filesz);
        std::memset(static_cast<uint8_t*>(b) + t.tls.filesz, 0, size - t.tls.filesz);
        s->dtv[module] = b;
        g_blocks.fetch_add(1, std::memory_order_relaxed);
    }
    return s->dtv[module];
}

// pthread key destructor. C++ destructors first, newest first, including any they register; then the
// blocks they may still have used.
void thread_exit(void* p) {
    auto* s = static_cast<ThreadState*>(p);
    for (uint64_t guard = 0; s->dtorCount != 0 && guard < (1u << 20); ++guard) {
        const Destructor d = s->dtors[--s->dtorCount];
        d.fn(d.object);
        g_destructorsRun.fetch_add(1, std::memory_order_relaxed);
    }
    if (s->dtv != nullptr) {
        const uint64_t capacity = reinterpret_cast<uintptr_t>(s->dtv[0]);
        for (uint64_t m = 1; m < capacity; ++m) {
            if (s->dtv[m] != nullptr) {
                std::free(s->dtv[m]);
                g_freedBlocks.fetch_add(1, std::memory_order_relaxed);
            }
        }
        std::free(s->dtv);
    }
    std::free(s->dtors);
    aleks2_tls_dtv = nullptr;
    t_state = nullptr;
    std::free(s);
}

}  // namespace

extern "C" {

void* aleks2_tls_slow(const Aleks2TlsDescArg* arg) noexcept {
    return static_cast<uint8_t*>(block(arg->module)) + arg->offset;
}

void* aleks2_tls_accessor_slow(const Aleks2TlsAccessorArg* arg) noexcept {
    return static_cast<uint8_t*>(arg->accessor()) + arg->addend;
}

void* aleks2_tls_get_addr(const Aleks2TlsIndex* index) noexcept {
    if (index->module & dusk::mods::elf::tls::kAccessorModule) {
        void* (*accessor)() = nullptr;
        {
            std::lock_guard lock(g_mutex);
            const uint64_t i = index->module & ~dusk::mods::elf::tls::kAccessorModule;
            if (i >= g_accessorModules.size()) {
                die("a tls_index names an accessor that was never registered");
            }
            accessor = g_accessorModules[i];
        }
        return static_cast<uint8_t*>(accessor()) + index->offset;
    }
    return static_cast<uint8_t*>(block(index->module)) + index->offset;
}

// The module's dso_handle is not needed: modules are never unloaded.
int aleks2_tls_cxa_thread_atexit(void (*dtor)(void*), void* object, void*) noexcept {
    ThreadState* s = state();
    if (s->dtorCount == s->dtorCapacity) {
        const size_t grown = s->dtorCapacity == 0 ? 8 : s->dtorCapacity * 2;
        auto* d = static_cast<Destructor*>(std::realloc(s->dtors, grown * sizeof(Destructor)));
        if (d == nullptr) {
            return -1;
        }
        s->dtors = d;
        s->dtorCapacity = grown;
    }
    s->dtors[s->dtorCount++] = {dtor, object};
    return 0;
}

}  // extern "C"

namespace dusk::mods::elf::tls {

template <class F>
uint64_t addr(F* f) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(f));
}

bool Runtime::prepare(std::string& error) {
    std::lock_guard lock(g_keyMutex);
    if (g_keyReady.load(std::memory_order_acquire)) {
        return true;
    }
    if (injected(Fault::KeyCreate) || pthread_key_create(&g_exitKey, thread_exit) != 0) {
        error = "no thread-local key left for module TLS (pthread_key_create failed; libnx has a fixed number of "
                "slots): the module is not loaded";
        return false;
    }
    g_keyReady.store(true, std::memory_order_release);
    return true;
}

uint64_t Runtime::register_module(uint64_t image, const TlsTemplate& t, std::string& error) {
    if (t.filesz > t.memsz || (t.align & (t.align - 1)) != 0) {
        error = "malformed TLS template";
        return 0;
    }
    if (!prepare(error)) {
        return 0;
    }
    std::lock_guard lock(g_mutex);
    g_templates.push_back({image, t});
    return g_templates.size();
}

std::pair<uint64_t, uint64_t> Runtime::descriptor(uint64_t module, uint64_t offset) {
    std::lock_guard lock(g_mutex);
    g_descArgs.push_back({module, offset});
    return {addr(&aleks2_tlsdesc_dynamic), addr(&g_descArgs.back())};
}

std::pair<uint64_t, uint64_t> Runtime::import_descriptor(uint64_t accessor, int64_t addend) {
    std::lock_guard lock(g_mutex);
    g_accessorArgs.push_back({reinterpret_cast<void* (*)()>(static_cast<uintptr_t>(accessor)), addend});
    return {addr(&aleks2_tlsdesc_accessor), addr(&g_accessorArgs.back())};
}

void Runtime::abandon(uint64_t module) {
    std::lock_guard lock(g_mutex);
    if (module != 0 && module <= g_templates.size() && !g_templates[module - 1].abandoned) {
        g_templates[module - 1].abandoned = true;
        g_templates[module - 1].image = 0;
        g_abandoned.fetch_add(1, std::memory_order_relaxed);
    }
}

void inject_fault(Fault fault, int count) {
    g_faults[static_cast<int>(fault)].store(count);
}

void set_fatal_handler(void (*handler)(const char* what)) {
    g_fatal = handler != nullptr ? handler : default_fatal;
}

uint64_t Runtime::import_module(uint64_t accessor) {
    std::lock_guard lock(g_mutex);
    g_accessorModules.push_back(reinterpret_cast<void* (*)()>(static_cast<uintptr_t>(accessor)));
    return kAccessorModule | (g_accessorModules.size() - 1);
}

Runtime& runtime() {
    static Runtime s_runtime;
    return s_runtime;
}

std::vector<Shim> shims() {
    return {
        {"__tls_get_addr", addr(&aleks2_tls_get_addr), "the module's dynamic TLS (TLS-lab)", true},
        {"__cxa_thread_atexit", addr(&aleks2_tls_cxa_thread_atexit),
            "thread_local destructors, run at thread exit before the thread's TLS blocks are freed (TLS-lab)", true},
        {"__cxa_thread_atexit_impl", addr(&aleks2_tls_cxa_thread_atexit),
            "thread_local destructors, run at thread exit before the thread's TLS blocks are freed (TLS-lab)", true},
    };
}

std::function<std::optional<uint64_t>(std::string_view)> host_variables(std::vector<HostVariable> variables) {
    return [variables = std::move(variables)](std::string_view name) -> std::optional<uint64_t> {
        for (const auto& v : variables) {
            if (v.name == name) {
                return addr(v.accessor);
            }
        }
        return std::nullopt;
    };
}

void* current_block(uint64_t module) {
    void** dtv = aleks2_tls_dtv;
    if (dtv == nullptr || module == 0 || module >= reinterpret_cast<uintptr_t>(dtv[0])) {
        return nullptr;
    }
    return dtv[module];
}

Stats stats() {
    Stats s;
    {
        std::lock_guard lock(g_mutex);
        s.modules = g_templates.size();
    }
    s.threads = g_threads.load();
    s.blocks = g_blocks.load();
    s.freedBlocks = g_freedBlocks.load();
    s.destructorsRun = g_destructorsRun.load();
    s.abandoned = g_abandoned.load();
    s.keyReady = g_keyReady.load();
    return s;
}

}  // namespace dusk::mods::elf::tls
