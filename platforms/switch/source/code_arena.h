/* aleks2 (H37): the code pages hook trampolines live in, shared between hooks.
 *
 * This file is part of Funchook's Horizon layer and is therefore GPL-2.0 with funchook's
 * linking exception (see funchook_os_switch.c).
 *
 * Why it exists. H36 made one CodeMemory kernel object per hook page. On hardware the eleventh
 * svcCreateCodeMemory failed with 0xCE01 (Kernel, ResourceExhausted): KCodeMemory objects come
 * from a small fixed kernel pool, shared by the whole system, and Cosmetics alone needs 28 hook
 * pages. Asking for a bigger internal table changed nothing; the kernel ran out first.
 *
 * What this does instead. A few arenas, each ONE CodeMemory object over many pages, mapped twice
 * at the same time, the way libnx's own Jit does it:
 *
 *   - the executable view (svcControlCodeMemory MapSlave, R-X) near the hooked code, so that
 *     funchook's `adrp x9, page; br x9` reaches it (ADRP: +/-4 GiB, page granular). It is mapped
 *     once when the arena is made and never unmapped or changed while the arena lives, so a
 *     trampoline that is running -- on any thread, for any mod -- never sees its page go away.
 *   - the writable view (MapOwner, RW-) anywhere, mapped only while a page is being written
 *     (between allocation and protect, and to wipe a freed page) and unmapped as soon as no
 *     page is being written. No mapping is ever writable and executable.
 *
 * Both views show the same physical pages; a write through the writable view is flushed from
 * the data cache and the executable view invalidated in the instruction cache before the page
 * is used.
 *
 * Pages are handed out one per hook (funchook puts one entry at each page boundary on arm64).
 * A freed page is left as it is -- a thread can still be inside an old trampoline just after its
 * hook was removed -- and reused only when no page was ever used, the one freed longest ago
 * first. An arena is made only when no existing one has a free page within reach of the target,
 * and arenas are kept for the session: unmapping an executable view could pull it from under such
 * a thread. code_arena_trim() releases empty ones for callers that know no thread can be there.
 *
 * The kernel is reached only through code_arena_ops, so this is tested on a PC against a fake
 * kernel that runs out of CodeMemory objects after ten (platforms/switch/tools/
 * code_arena_test.c).
 */
#ifndef ALEKS2_CODE_ARENA_H
#define ALEKS2_CODE_ARENA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CODE_ARENA_PAGE 0x1000u
/* Pages per arena: one CodeMemory object each. 64 pages hold Cosmetics (28) and Roll and Sprint
 * (4) in one object, and Randomizer on top of them in two. */
#define CODE_ARENA_PAGES 64u
/* Upper bound on arenas, i.e. on CodeMemory objects this ever holds. */
#define CODE_ARENA_MAX 8u

/* The kernel, as the arena manager needs it. Every call returns 0 or a Horizon Result. */
typedef struct code_arena_ops {
    void *ctx;
    /* Page-aligned heap memory for an arena's backing, and its release. */
    void *(*alloc_backing)(void *ctx, size_t size);
    void (*free_backing)(void *ctx, void *backing, size_t size);
    /* svcCreateCodeMemory / svcCloseHandle. */
    uint32_t (*create)(void *ctx, void *backing, size_t size, uint32_t *handle);
    uint32_t (*close)(void *ctx, uint32_t handle);
    /* svcControlCodeMemory: MapOwner (RW) / UnmapOwner, MapSlave (RX) / UnmapSlave. */
    uint32_t (*map_rw)(void *ctx, uint32_t handle, uintptr_t addr, size_t size);
    uint32_t (*unmap_rw)(void *ctx, uint32_t handle, uintptr_t addr, size_t size);
    uint32_t (*map_rx)(void *ctx, uint32_t handle, uintptr_t addr, size_t size);
    uint32_t (*unmap_rx)(void *ctx, uint32_t handle, uintptr_t addr, size_t size);
    /* Reserve free address space: near `target` (for the executable view; 0 if none) or
     * anywhere (for the writable view). `token` is what release() needs back. */
    uintptr_t (*reserve_near)(void *ctx, uintptr_t target, size_t size, void **token);
    uintptr_t (*reserve_any)(void *ctx, size_t size, void **token);
    void (*release)(void *ctx, void *token);
    void (*dcache_flush)(void *ctx, void *addr, size_t size);
    void (*icache_invalidate)(void *ctx, uintptr_t addr, size_t size);
    /* One line, no newline. Arena creation, failures and summaries only. */
    void (*log)(void *ctx, const char *line);
} code_arena_ops;

enum { CODE_SLOT_FREE = 0, CODE_SLOT_WRITING = 1, CODE_SLOT_LIVE = 2 };

typedef struct code_arena {
    uint32_t handle;
    void *backing;
    uintptr_t rx;      /* executable view, mapped for the arena's whole life */
    uintptr_t rw;      /* writable view's address; mapped only while rw_mapped */
    void *rx_token;
    void *rw_token;
    int rw_mapped;
    uint8_t state[CODE_ARENA_PAGES];
    uint8_t ever_used[CODE_ARENA_PAGES];
    uint64_t freed_at[CODE_ARENA_PAGES];
} code_arena;

typedef struct code_arena_stats {
    uint32_t arenas;           /* CodeMemory objects held now */
    uint32_t arenas_peak;      /* the most held at once this session */
    uint32_t arenas_created;
    uint32_t pages_total;
    uint32_t pages_used;
    uint64_t allocations;
    uint64_t frees;
    uint64_t reuses;           /* allocations that took a page freed before */
    uint64_t rw_maps;          /* times the writable view was mapped */
    uint32_t last_failure;     /* Result of the last failed kernel call, 0 if none */
    char last_failure_op[32];  /* which call that was */
} code_arena_stats;

typedef struct code_arenas {
    const code_arena_ops *ops;
    code_arena arena[CODE_ARENA_MAX];
    uint32_t count;
    uint32_t writers;  /* pages being written now, across arenas */
    uint64_t clock;    /* orders frees, for reuse */
    code_arena_stats stats;
} code_arenas;

void code_arenas_init(code_arenas *m, const code_arena_ops *ops);

/* A page within ADRP reach of `target`, writable at *rw until code_arena_commit(). 0 or an
 * error: CODE_ARENA_E_* below, with `err` saying why. */
int code_arena_alloc(code_arenas *m, uintptr_t target, uintptr_t *rx, void **rw, char *err, size_t errlen);
/* Done writing the page at `rx`: flush, unmap the writable view if nothing else is being
 * written, invalidate the instruction cache for the page. */
int code_arena_commit(code_arenas *m, uintptr_t rx, char *err, size_t errlen);
/* Make the page at `rx` writable again (at the returned address). */
int code_arena_begin_write(code_arenas *m, uintptr_t rx, void **rw, char *err, size_t errlen);
/* Give the page back for reuse. Its bytes stay until it is handed out again. */
int code_arena_free(code_arenas *m, uintptr_t rx, char *err, size_t errlen);

/* Translations between the two views of a page; 0 if the address is not in an arena. */
uintptr_t code_arena_rx_of(const code_arenas *m, const void *rw);
void *code_arena_rw_of(const code_arenas *m, uintptr_t rx);

/* Releases every arena with no page in use. Returns how many CodeMemory objects remain. */
uint32_t code_arena_trim(code_arenas *m);

/* One summary line (for the log after mods start, and the diagnostics screen). */
void code_arena_describe(const code_arenas *m, char *out, size_t outlen);

/* ADRP's reach: the page of `to` within +/-4 GiB of the page of `from`. */
int code_arena_within_adrp(uintptr_t from, uintptr_t to);

#define CODE_ARENA_E_INVALID 1
#define CODE_ARENA_E_NO_RANGE 2
#define CODE_ARENA_E_KERNEL 3
#define CODE_ARENA_E_FULL 4
#define CODE_ARENA_E_MEMORY 5

#ifdef __cplusplus
}
#endif

#endif
