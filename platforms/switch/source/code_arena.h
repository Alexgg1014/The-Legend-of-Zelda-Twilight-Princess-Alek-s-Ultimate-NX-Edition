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
 *     funchook's patch (ADRP: +/-4 GiB, page granular) reaches it. It is mapped once when the
 *     arena is made and never unmapped or changed while the arena lives, so a trampoline that is
 *     running -- on any thread, for any mod -- never sees its page go away.
 *   - the writable view (MapOwner, RW-) anywhere, mapped only while a slot is being written
 *     (between allocation and protect, and to wipe a freed slot) and unmapped as soon as no slot
 *     of the arena is being written. No mapping is ever writable and executable.
 *
 * Both views show the same physical pages; a write through the writable view is flushed from
 * the data cache and the executable view invalidated in the instruction cache before the slot
 * is used.
 *
 * DM2-a: slots, not pages. funchook's arm64 patch was `adrp x9, page; br x9`, which can only land
 * on a page boundary, so every hook took a whole 4 KiB page and eight arenas held 512 hooks --
 * fewer than Twilit Essentials and Dawnlight declare together. The patch is now `adrp x9, page;
 * add x9, x9, #offset; br x9` (cmake/PatchFunchook.cmake), which lands anywhere, and the arena
 * hands out SLOTS of CODE_ARENA_SLOT bytes, CODE_ARENA_SLOTS_PER_PAGE to a page: 1024 hooks per
 * arena. A function too short for three instructions, or with a branch back into them, keeps the
 * old two-instruction patch; that needs a slot at a page boundary (slot 0 of a page), asked for
 * with CODE_ARENA_PAGE_ALIGNED. Other requests take those slots last, so they stay free for it.
 *
 * Slots never overlap and each sits on its own cache lines. Writing one goes through the writable
 * view only; the executable view of its page -- where other hooks' slots may be running -- is
 * never unmapped or reprotected, and the bytes of the other slots are never touched. A freed slot
 * is left as it is -- a thread can still be inside an old trampoline just after its hook was
 * removed -- and reused only when no slot was ever used, the one freed longest ago first. An arena
 * is made only when no existing one has a free slot within reach of the target, and arenas are
 * kept for the session: unmapping an executable view could pull it from under such a thread.
 * code_arena_trim() releases empty ones for callers that know no thread can be there.
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
/* Pages per arena: one CodeMemory object (256 KiB) each. */
#define CODE_ARENA_PAGES 64u
/* DM2-a: one hook per slot. funchook's page header plus one entry is 160 bytes with the wide patch
 * (funchook_arm64.h after cmake/PatchFunchook.cmake); 256 keeps every slot on its own cache lines. */
#define CODE_ARENA_SLOT 0x100u
#define CODE_ARENA_SLOTS_PER_PAGE (CODE_ARENA_PAGE / CODE_ARENA_SLOT)
#define CODE_ARENA_SLOTS (CODE_ARENA_PAGES * CODE_ARENA_SLOTS_PER_PAGE)
/* Upper bound on arenas, i.e. on CodeMemory objects this ever holds. The kernel's pool is about
 * ten objects for the whole system (0xCE01 on the eleventh in H36). */
#define CODE_ARENA_MAX 8u

/* code_arena_alloc flags. */
#define CODE_ARENA_PAGE_ALIGNED 1u /* a slot at a page boundary, for the two-instruction patch */

/* The kernel, as the arena manager needs it. Every call returns 0 or a Horizon Result. */
struct code_arenas;
/* aleks2 (V1-rc9): unmaps and closes every arena (at exit). Returns the failed steps. */
int code_arenas_release(struct code_arenas *m, uint32_t *released, uint32_t *last_rc);

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
    uint32_t writing;  /* slots of this arena being written */
    uint32_t used;     /* slots of this arena not free */
    uint8_t state[CODE_ARENA_SLOTS];
    uint8_t ever_used[CODE_ARENA_SLOTS];
    uint32_t freed_at[CODE_ARENA_SLOTS];
} code_arena;

typedef struct code_arena_stats {
    uint32_t arenas;           /* CodeMemory objects held now */
    uint32_t arenas_peak;      /* the most held at once this session */
    uint32_t arenas_created;
    uint32_t slots_total;      /* in the arenas held now */
    uint32_t slots_used;
    uint32_t slots_peak;
    uint32_t aligned_used;     /* slots in use at a page boundary (two-instruction patches) */
    uint32_t pages_touched;    /* pages of the arenas held that ever had a slot in use */
    uint64_t allocations;
    uint64_t frees;
    uint64_t reuses;           /* allocations that took a slot freed before */
    uint64_t rw_maps;          /* times the writable view was mapped */
    uint32_t last_failure;     /* Result of the last failed kernel call, 0 if none */
    char last_failure_op[32];  /* which call that was */
} code_arena_stats;

typedef struct code_arenas {
    const code_arena_ops *ops;
    code_arena arena[CODE_ARENA_MAX];
    uint32_t count;
    uint32_t writers;  /* slots being written now, across arenas */
    uint32_t clock;    /* orders frees, for reuse */
    code_arena_stats stats;
} code_arenas;

void code_arenas_init(code_arenas *m, const code_arena_ops *ops);

/* A zeroed slot of CODE_ARENA_SLOT bytes within ADRP reach of `target` (at a page boundary with
 * CODE_ARENA_PAGE_ALIGNED), writable at *rw until code_arena_commit(). 0 or an error:
 * CODE_ARENA_E_* below, with `err` saying why. */
int code_arena_alloc(code_arenas *m, uintptr_t target, unsigned flags, uintptr_t *rx, void **rw, char *err,
                     size_t errlen);
/* Done writing the slot at `rx`: flush it, unmap the writable view if nothing else in its arena
 * is being written, invalidate the instruction cache for the slot. */
int code_arena_commit(code_arenas *m, uintptr_t rx, char *err, size_t errlen);
/* Make the slot at `rx` writable again (at the returned address). */
int code_arena_begin_write(code_arenas *m, uintptr_t rx, void **rw, char *err, size_t errlen);
/* Give the slot back for reuse. Its bytes stay until it is handed out again. */
int code_arena_free(code_arenas *m, uintptr_t rx, char *err, size_t errlen);

/* Translations between the two views of a slot; 0 if the address is not in an arena. */
uintptr_t code_arena_rx_of(const code_arenas *m, const void *rw);
void *code_arena_rw_of(const code_arenas *m, uintptr_t rx);

/* DM2-a: room for hooks, for the loader's check before a mod starts and for the mods menu.
 * `used`: slots in use. `free_now`: free slots in the arenas held. `capacity`: every slot the
 * arenas held and the ones still allowed (CODE_ARENA_MAX) could give, used ones included. Reach is
 * not counted: every function of the game is within reach of one arena. Any pointer may be NULL. */
void code_arena_usage(const code_arenas *m, uint32_t *used, uint32_t *free_now, uint32_t *capacity);

/* Releases every arena with no slot in use. Returns how many CodeMemory objects remain. */
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
