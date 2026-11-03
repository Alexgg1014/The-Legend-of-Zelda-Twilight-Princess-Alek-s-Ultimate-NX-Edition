// aleks2 (v1.1-perf-lab-3): two libc paths the stack profile of Twilit Essentials 3 on the console blamed, wrapped at
// link time (-Wl,--wrap=malloc,--wrap=pthread_rwlock_unlock, platforms/switch/CMakeLists.txt). Both can change while
// the game runs (perf-lab.ini malloc_direct, rwlock_quiet; frame_telemetry.cpp), so the tour can compare them.
//
// malloc: devkitPro's newlib sends every request over 31 bytes to memalign(32, n), which takes a larger chunk and
// frees the slack around it -- up to two extra frees, each under the malloc lock. _malloc_r returns 16-byte alignment,
// what C and C++ promise (alignof(max_align_t)); anything wanting more asks for it (memalign, aligned_alloc, the
// aligned operator new). Blocks of both kinds are freed the same way, so switching mid-run is fine.
// The idea is Kawaii's, from KartPadNX (used with permission; THIRD_PARTY_NOTICES).
//
// pthread_rwlock_unlock: newlib broadcasts cond_r whenever the last reader leaves, though only a writer that has
// claimed the lock (cnt_w == 1) ever waits there -- and libnx's condvar has no waiter count, so every broadcast is a
// kernel call (svcSignalProcessWideKey). deko3d's object shadows (dk_objects.cpp: every texture, view and sampler
// AddRef/Release) and SDL's property tables take a read lock thousands of times a frame. Same algorithm otherwise.
#include <pthread.h>
#include <reent.h>
#include <sys/lock.h>

#include <atomic>
#include <cerrno>
#include <cstddef>

extern "C" {
void* __real_malloc(size_t size);
int __real_pthread_rwlock_unlock(pthread_rwlock_t* rwlock);
void* _malloc_r(struct _reent* reent, size_t size);

// v1.1-beta2: both on by default (perf-lab pass H on the console, with Twilit Essentials 3: +2 to +9 FPS per zone
// together with fifo_quiet; perf-lab.ini can still turn each off).
std::atomic<bool> g_aleks2MallocDirect{true};
std::atomic<bool> g_aleks2RwlockQuiet{true};

void* __wrap_malloc(size_t size) {
    if (g_aleks2MallocDirect.load(std::memory_order_relaxed)) {
        return _malloc_r(_REENT, size);
    }
    return __real_malloc(size);
}

int __wrap_pthread_rwlock_unlock(pthread_rwlock_t* rwlock) {
    if (!g_aleks2RwlockQuiet.load(std::memory_order_relaxed)) {
        return __real_pthread_rwlock_unlock(rwlock);
    }
    if (rwlock == nullptr) {
        return EINVAL;
    }
    __libc_lock_acquire(&rwlock->lock);
    if (rwlock->cnt_w == 2) {
        // The writer leaves: readers and writers both wait on cond_w.
        rwlock->cnt_w = 0;
        __libc_cond_broadcast(&rwlock->cond_w);
    } else {
        rwlock->cnt_r = rwlock->cnt_r - 1;
        if (rwlock->cnt_r == 0 && rwlock->cnt_w == 1) {
            __libc_cond_broadcast(&rwlock->cond_r);  // the writer waiting for the readers to drain
        }
    }
    __libc_lock_release(&rwlock->lock);
    return 0;
}
}
