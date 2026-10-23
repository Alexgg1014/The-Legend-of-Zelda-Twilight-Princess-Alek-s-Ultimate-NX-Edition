// DM2-d: a Linux module that walks the loaded objects with dl_iterate_phdr the way Twilit Essentials 2.8.4
// does (find the PT_LOAD holding its own code, then its first executable PT_LOAD), checks its TLS entry, and
// throws and catches a C++ exception. Built by test-dm2d.sh with aarch64-linux-gnu-g++ -fPIC -shared; once
// more with -static-libgcc as dm2d_unwind.so, whose own libgcc_eh unwinder finds FDEs through dl_iterate_phdr.
#include <link.h>
#include <unwind.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>

extern "C" {
int mod_meta = 0;
void* mod_ctx = nullptr;
int mod_initialize() { return 0; }
int mod_update() { return 0; }
int mod_shutdown() { return 0; }
}

namespace {
thread_local uint64_t t_marker = 0x5eed;

struct Search {
    uintptr_t inside;       // an address of this module's code
    uintptr_t execStart = 0;
    uintptr_t execSize = 0;
    uintptr_t tlsVar = 0;   // &t_marker in this thread
    size_t tlsModid = 0;
    int tlsDataOk = 0;      // 1: dlpi_tls_data is this thread's block (t_marker inside it)
    int objects = 0;
    size_t infoSize = 0;
};

int find_self(dl_phdr_info* info, size_t size, void* data) {
    auto* s = static_cast<Search*>(data);
    ++s->objects;
    s->infoSize = size;
    bool mine = false;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto& p = info->dlpi_phdr[i];
        if (p.p_type == PT_LOAD && s->inside >= info->dlpi_addr + p.p_vaddr &&
            s->inside < info->dlpi_addr + p.p_vaddr + p.p_memsz)
        {
            mine = true;
        }
    }
    if (!mine) {
        return 0;
    }
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto& p = info->dlpi_phdr[i];
        if (p.p_type == PT_LOAD && (p.p_flags & PF_X)) {
            s->execStart = info->dlpi_addr + p.p_vaddr;
            s->execSize = p.p_memsz;
            break;
        }
    }
    s->tlsModid = info->dlpi_tls_modid;
    const auto block = reinterpret_cast<uintptr_t>(info->dlpi_tls_data);
    s->tlsDataOk = block != 0 && s->tlsVar >= block && s->tlsVar < block + 4096;
    return 1;
}

__attribute__((noinline)) int frames_below(int depth);
volatile int g_sink = 0;  // a store after each call: no tail call, so every level keeps its frame

_Unwind_Reason_Code count_frame(_Unwind_Context*, void* n) {
    ++*static_cast<int*>(n);
    return _URC_NO_REASON;
}

__attribute__((noinline)) int frames_below(int depth) {
    if (depth > 0) {
        const int n = frames_below(depth - 1);
        g_sink = depth;
        return n;
    }
    int n = 0;
    _Unwind_Backtrace(count_frame, &n);
    return n;
}
}  // namespace

extern "C" {
// 1 if this module found itself and its executable segment holds this very function; the details out.
int dm2d_walk(uintptr_t* execStart, uintptr_t* execSize, int* objects, size_t* modid, int* tlsOk, size_t* infoSize) {
    t_marker += 1;  // this thread's block exists from here
    Search s{reinterpret_cast<uintptr_t>(&dm2d_walk)};
    s.tlsVar = reinterpret_cast<uintptr_t>(&t_marker);
    const int r = dl_iterate_phdr(find_self, &s);
    *execStart = s.execStart;
    *execSize = s.execSize;
    *objects = s.objects;
    *modid = s.tlsModid;
    *tlsOk = s.tlsDataOk;
    *infoSize = s.infoSize;
    const auto self = reinterpret_cast<uintptr_t>(&dm2d_walk);
    return r == 1 && self >= s.execStart && self < s.execStart + s.execSize;
}

// FNV-1a 64 over the executable segment, as Twilit does with what dl_iterate_phdr gave it.
uint64_t dm2d_fingerprint() {
    uintptr_t start = 0, size = 0;
    int objects = 0, ok = 0;
    size_t modid = 0, infoSize = 0;
    if (!dm2d_walk(&start, &size, &objects, &modid, &ok, &infoSize)) {
        return 0;
    }
    uint64_t h = 0x14650fb0739d0383ull;
    for (uintptr_t i = 0; i < size; ++i) {
        h = (h ^ reinterpret_cast<const uint8_t*>(start)[i]) * 0x100000001b3ull;
    }
    return h;
}

// Frames _Unwind_Backtrace sees from 8 calls deep (with -static-libgcc, through this module's own unwinder).
int dm2d_backtrace_depth() {
    const int n = frames_below(8);
    g_sink = 0;
    return n;
}

int dm2d_throw_catch() {
    try {
        throw std::runtime_error("thrown in the module");
    } catch (const std::exception& e) {
        return std::strcmp(e.what(), "thrown in the module") == 0;
    }
    return 0;
}
}
