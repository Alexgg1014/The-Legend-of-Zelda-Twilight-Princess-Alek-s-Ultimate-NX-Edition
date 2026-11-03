// aleks2 (V1-rc12): see sampler.hpp.
#include "dusk/sampler.hpp"

#include "dusk/benchmark.hpp"
#include "dusk/frame_telemetry.hpp"
#include "dusk/logging.h"

#include <aurora/tuning.h>
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" char _start[];  // the NRO's first byte: addresses in the profile are written with this base

namespace dusk::sampler {
namespace {

inline constexpr borealis::Log Log{"sampler"};

struct Sample {
    u64 pc, lr, f1, f2;
    char thread;  // v1.1-perf-lab: 'm' main, 'f' GX FIFO worker
};

constexpr u64 kIntervalNs = 1'000'000;  // ~1 ms between samples
constexpr s32 kPriority = 0x20;         // above the game's threads: the samples come on time
constexpr int kCore = 2;

// v1.1-perf-lab: one target per thread sampled (the main thread; with profile_fifo, Aurora's FIFO worker too, whose
// handle only exists once the worker has started). Each keeps its own stack bounds for the frame-pointer walk.
struct Target {
    Handle handle = INVALID_HANDLE;
    char tag = 'm';
    u64 stackLo = 0, stackHi = 0;
};
Target g_targets[2];
int g_targetCount = 0;
bool g_wantFifo = false;

bool g_started = false;
Thread g_thread;
std::vector<Sample> g_buffer;
std::atomic<size_t> g_count{0};
std::atomic<int> g_state{0};          // 0 idle, 1 sampling
std::atomic<bool> g_endRequest{false};
char g_zone[48] = "free";
double g_seconds = 30.0, g_delay = 90.0;
unsigned g_files = 0;

double seconds_since(u64 tick) {
    return static_cast<double>(armTicksToNs(armGetSystemTick() - tick)) * 1e-9;
}

// V1-rc16: `fp + 16 <= hi` wrapped for a garbage fp near 2^64 and let it through; the room left is compared instead
// (bound issue noted in Chansey's sa2-nx review).
bool in_stack(const Target& t, u64 fp) {
    return (fp & 0xF) == 0 && fp >= t.stackLo && fp < t.stackHi && t.stackHi - fp >= 16;
}

// The target thread is paused between the two svcSetThreadActivity calls: only svcs and plain loads of its stack.
void sample_once(Target& t) {
    if (R_FAILED(svcSetThreadActivity(t.handle, ThreadActivity_Paused))) {
        return;
    }
    ThreadContext ctx;
    const Result rc = svcGetThreadContext3(&ctx, t.handle);
    Sample s{};
    s.thread = t.tag;
    if (R_SUCCEEDED(rc)) {
        s.pc = ctx.pc.x;
        s.lr = ctx.lr;
        u64 fp = ctx.fp;
        if (t.stackHi != 0 && in_stack(t, fp)) {
            const u64* rec = reinterpret_cast<const u64*>(fp);
            s.f1 = rec[1];
            fp = rec[0];
            if (in_stack(t, fp)) {
                s.f2 = reinterpret_cast<const u64*>(fp)[1];
            }
        }
    }
    svcSetThreadActivity(t.handle, ThreadActivity_Runnable);
    if (R_FAILED(rc)) {
        return;
    }
    if (t.stackHi == 0) {
        // First sample: the thread's stack, from its SP (outside the pause).
        MemoryInfo mi{};
        u32 pi = 0;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, ctx.sp))) {
            t.stackLo = mi.addr;
            t.stackHi = mi.addr + mi.size;
        }
    }
    const size_t n = g_count.load(std::memory_order_relaxed);
    if (n < g_buffer.size()) {
        g_buffer[n] = s;
        g_count.store(n + 1, std::memory_order_relaxed);
    }
}

void sample_all() {
    if (g_wantFifo && g_targetCount == 1) {
        if (const u32 h = aurora_gx_fifo_thread_handle(); h != 0) {
            g_targets[1] = Target{h, 'f'};
            g_targetCount = 2;
        }
    }
    for (int i = 0; i < g_targetCount; ++i) {
        sample_once(g_targets[i]);
    }
}

void write_window() {
    const size_t n = g_count.load(std::memory_order_relaxed);
    char zone[48];
    std::snprintf(zone, sizeof zone, "%s", g_zone);
    for (char* c = zone; *c != 0; ++c) {
        if (!std::isalnum(static_cast<unsigned char>(*c))) {
            *c = '_';
        }
    }
    char path[160];
    std::snprintf(path, sizeof path, "sdmc:/switch/dusklight/cache/logs/profile-%s-%u.txt", zone, g_files++);
    if (std::FILE* f = std::fopen(path, "w")) {
        std::fprintf(f,
            "# dusklight sampler v%d: build %s, NRO base 0x%llx, interval ~1 ms, %zu samples, zone %s\n"
            "# %spc lr f1 f2 (absolute, hex; f1/f2 the return addresses of the first two frame records, 0 if none)\n",
            g_wantFifo ? 2 : 1, benchmark::build_id().c_str(),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(_start)), n, g_zone,
            g_wantFifo ? "thread (m main, f GX FIFO worker) " : "");
        for (size_t i = 0; i < n; ++i) {
            const Sample& s = g_buffer[i];
            if (g_wantFifo) {
                std::fprintf(f, "%c ", s.thread);  // v1.1-perf-lab: sampler v2 tags each sample with its thread
            }
            std::fprintf(f, "%llx %llx %llx %llx\n", static_cast<unsigned long long>(s.pc),
                static_cast<unsigned long long>(s.lr), static_cast<unsigned long long>(s.f1),
                static_cast<unsigned long long>(s.f2));
        }
        std::fclose(f);
        Log.info("profile written: {} ({} samples)", path, n);
    } else {
        Log.warn("profile: cannot write {}", path);
    }
}

void begin_window(const char* zone) {
    std::snprintf(g_zone, sizeof g_zone, "%s", zone != nullptr ? zone : "zone");
    g_count.store(0, std::memory_order_relaxed);
    g_endRequest.store(false, std::memory_order_relaxed);
    g_state.store(1, std::memory_order_release);
}

void thread_main(void*) {
    const u64 boot = armGetSystemTick();
    bool freeRunPending = g_delay > 0.0;
    bool freeRun = false;
    u64 windowStart = 0;
    for (;;) {
        if (g_state.load(std::memory_order_acquire) == 0) {
            if (freeRunPending && seconds_since(boot) >= g_delay) {
                freeRunPending = false;
                freeRun = true;
                windowStart = armGetSystemTick();
                begin_window("free");
                Log.info("profile: free-run window for {:.0f} s", g_seconds);
            } else {
                svcSleepThread(50'000'000);
                continue;
            }
        }
        sample_all();
        const bool timeUp = freeRun && seconds_since(windowStart) >= g_seconds;
        if (g_endRequest.load(std::memory_order_relaxed) || timeUp) {
            g_state.store(0, std::memory_order_release);
            freeRun = false;
            write_window();
            continue;
        }
        svcSleepThread(kIntervalNs);
    }
}

}  // namespace

void start_if_enabled() {
    const auto& lab = frame_telemetry::lab();
    if (g_started || !lab.profileMain) {
        return;
    }
    if (!envIsSyscallHinted(0x32) || !envIsSyscallHinted(0x33)) {
        Log.warn("profile_main: svcSetThreadActivity / svcGetThreadContext3 not permitted; profiler off");
        return;
    }
    g_targets[0] = Target{envGetMainThreadHandle(), 'm'};
    g_targetCount = 1;
    g_wantFifo = lab.profileFifo;
    g_seconds = static_cast<double>(lab.profileSeconds);
    g_delay = static_cast<double>(lab.profileDelayS);
    const double longest = std::max(g_seconds, 120.0);  // a tour zone's window is ~30 s; room for more
    g_buffer.assign(static_cast<size_t>(longest * 1100.0) * (g_wantFifo ? 2 : 1), Sample{});
    if (R_FAILED(threadCreate(&g_thread, thread_main, nullptr, nullptr, 0x10000, kPriority, kCore)) ||
        R_FAILED(threadStart(&g_thread))) {
        Log.warn("profile_main: could not start the sampler thread");
        g_buffer.clear();
        return;
    }
    g_started = true;
    Log.info("profile_main: sampling the main thread{} every ~1 ms on core {} ({} s free-run after {} s; every tour "
             "zone); buffer {} samples",
        g_wantFifo ? " and the GX FIFO worker" : "", kCore, lab.profileSeconds, lab.profileDelayS, g_buffer.size());
}

void zone_begin(const char* zone) {
    if (!g_started || g_state.load(std::memory_order_acquire) != 0) {
        return;
    }
    begin_window(zone);
}

void zone_end() {
    if (!g_started) {
        return;
    }
    g_endRequest.store(true, std::memory_order_relaxed);
}

}  // namespace dusk::sampler
