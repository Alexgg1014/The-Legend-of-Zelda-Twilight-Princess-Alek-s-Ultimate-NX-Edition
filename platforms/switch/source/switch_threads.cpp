// aleks2 (V1-rc8): pthread_create for the whole program, wrapped at link time (-Wl,--wrap=pthread_create,
// platforms/switch/CMakeLists.txt), so the exit guard (switch_startup.cpp) also sees the threads a mod starts
// through the NRO's own C++ library: a switch-arm64 mod's std::thread is created by libstdc++'s
// std::thread::_M_start_thread in this NRO, not through the mod's pthread_create import that
// elf_runtime_policy.cpp's shim counts. A thread still running when the program returns to hbloader crashes it.
//
// Only threads a mod started are counted: the creating thread's frame chain is walked and a return address outside
// this NRO's code (a mod's code, or a hook trampoline of one) marks the thread as a mod's. The game's own threads
// never count, so its exit and relaunch behave as before. A counted thread that ends with pthread_exit stays counted
// (the exit then takes the safe path).
#include <switch.h>

#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <new>

extern "C" {
int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*fn)(void*), void* arg);
extern const char __code_start[];    // libnx switch.ld: this NRO's code ...
extern const char __rodata_start[];  // ... ends where its read-only data starts
extern const char __start__[];       // and the whole image
extern const char __end__[];
}

namespace {

std::atomic<int> s_liveModThreads{0};
std::atomic<int> s_startedModThreads{0};

struct Start {
    void* (*fn)(void*);
    void* arg;
};

void* counted_entry(void* p) {
    const Start start = *static_cast<Start*>(p);
    delete static_cast<Start*>(p);
    void* result = start.fn(start.arg);
    s_liveModThreads.fetch_sub(1, std::memory_order_relaxed);
    return result;
}

bool in_nro_code(std::uintptr_t pc) {
    return pc >= reinterpret_cast<std::uintptr_t>(__code_start) &&
           pc < reinterpret_cast<std::uintptr_t>(__rodata_start);
}

// An address in a code module other than this NRO: a mod's image (or the hook arena).
bool in_mod_image(std::uintptr_t a) {
    if (a >= reinterpret_cast<std::uintptr_t>(__start__) && a < reinterpret_cast<std::uintptr_t>(__end__)) {
        return false;
    }
    MemoryInfo info{};
    u32 page = 0;
    if (R_FAILED(svcQueryMemory(&info, &page, a))) {
        return false;
    }
    switch (info.type & 0xff) {
    case MemType_CodeStatic:
    case MemType_CodeMutable:
    case MemType_ModuleCodeStatic:
    case MemType_ModuleCodeMutable:
    case MemType_CodeReadOnly:
    case MemType_CodeWritable:
        return true;
    default:
        return false;
    }
}

// std::thread from a mod: the start routine is libstdc++'s (this NRO) and its argument a heap _State whose vtable
// the mod's own code instantiated -- in the mod's image. Read only when the argument is readable memory.
bool state_from_mod(void* (*fn)(void*), void* arg) {
    if (!in_nro_code(reinterpret_cast<std::uintptr_t>(fn)) || arg == nullptr ||
        (reinterpret_cast<std::uintptr_t>(arg) & 0x7) != 0)
    {
        return false;
    }
    MemoryInfo info{};
    u32 page = 0;
    const auto a = reinterpret_cast<std::uintptr_t>(arg);
    if (R_FAILED(svcQueryMemory(&info, &page, a)) || (info.perm & Perm_R) == 0 || a + 8 > info.addr + info.size) {
        return false;
    }
    return in_mod_image(*reinterpret_cast<const std::uintptr_t*>(arg));
}

// True when one of the callers (up to 12 frames, within this thread's stack) is outside this NRO's code.
bool started_by_mod() {
    auto fp = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    MemoryInfo info{};
    u32 page = 0;
    if (R_FAILED(svcQueryMemory(&info, &page, fp)) || (info.perm & Perm_R) == 0) {
        return false;
    }
    const std::uintptr_t lo = info.addr;
    const std::uintptr_t hi = info.addr + info.size;
    for (int depth = 0; depth < 12; ++depth) {
        if (fp < lo || fp + 16 > hi || (fp & 0xf) != 0) {
            return false;
        }
        const auto* record = reinterpret_cast<const std::uintptr_t*>(fp);  // AArch64 frame record: {fp, lr}
        const std::uintptr_t lr = record[1];
        if (lr == 0) {
            return false;
        }
        if (!in_nro_code(lr)) {
            return true;
        }
        if (record[0] <= fp) {
            return false;
        }
        fp = record[0];
    }
    return false;
}

}  // namespace

extern "C" {

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*fn)(void*), void* arg) {
    if (!started_by_mod() && !state_from_mod(fn, arg)) {
        return __real_pthread_create(thread, attr, fn, arg);
    }
    auto* start = new (std::nothrow) Start{fn, arg};
    if (start == nullptr) {
        return __real_pthread_create(thread, attr, fn, arg);
    }
    s_liveModThreads.fetch_add(1, std::memory_order_relaxed);
    s_startedModThreads.fetch_add(1, std::memory_order_relaxed);
    const int rc = __real_pthread_create(thread, attr, &counted_entry, start);
    if (rc != 0) {
        s_liveModThreads.fetch_sub(1, std::memory_order_relaxed);
        s_startedModThreads.fetch_sub(1, std::memory_order_relaxed);
        delete start;
    }
    return rc;
}

// Threads started from a mod's code, through any pthread_create, still running / started in all.
int aleks2_live_threads_from_mods(void) { return s_liveModThreads.load(std::memory_order_relaxed); }
int aleks2_started_threads_from_mods(void) { return s_startedModThreads.load(std::memory_order_relaxed); }

}  // extern "C"
