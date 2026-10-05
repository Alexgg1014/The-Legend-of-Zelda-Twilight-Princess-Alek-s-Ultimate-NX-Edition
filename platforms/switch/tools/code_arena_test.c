/* H37: the Switch hook backend's code arenas (platforms/switch/source/code_arena.c and
 * funchook_arena_glue.c) against a fake Horizon kernel that, like the console, runs out of
 * CodeMemory objects: H36 died on the eleventh (0xCE01, Kernel ResourceExhausted).
 *
 * The fake kernel is real memory. A CodeMemory object is a memfd; its writable view (MapOwner)
 * and executable view (MapSlave) are two MAP_SHARED mappings of it, so a byte written through one
 * is the byte executed through the other, as on the console. It enforces what the kernel does:
 * full-size mappings only, one of each view at a time, no close while mapped -- and it counts
 * every object, reservation and backing allocation, so a leak is a failed check.
 *
 * Part A (any host; built with ASan and UBSan by test-code-arena.sh): the manager alone.
 *   - the fake refuses the 11th object, exactly as the console did;
 *   - Cosmetics' 28 hooks and Roll and Sprint's 4 take ONE object; with Randomizer's 92, two;
 *   - a failed create, MapSlave, MapOwner or UnmapOwner leaves nothing behind;
 *   - releasing one mod's pages leaves another's executable and intact, the executable view of
 *     an arena is never unmapped while it lives, and the writable view is mapped only while a
 *     page is being written;
 *   - freed pages are reused (oldest first, after the fresh ones) and repeated deactivation and
 *     reactivation never grows the number of objects;
 *   - every page is within ADRP reach of its target, and a target with no code space in reach is
 *     refused cleanly;
 *   - trim releases empty arenas and every handle, reservation and backing comes back.
 *
 * Part B (aarch64 only, run under qemu by test-code-arena.sh): funchook itself -- v1.1.3 as the
 * build fetches it, patched by cmake/PatchFunchook.cmake -- installs real hooks on 124 real
 * functions through these pages, one funchook per target as svc/hook.cpp does: the Cosmetics,
 * Roll and Sprint and Randomizer counts, then Cosmetics off and on again five times. Every hooked
 * call must reach its hook, and every hook must reach the original through its trampoline.
 */
#define _GNU_SOURCE
#include "code_arena.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

/* ----------------------------------------------------------------- the fake kernel -- */

#define RESULT_RESOURCE_EXHAUSTED 0xCE01u /* module 1 (Kernel), description 103 */
#define RESULT_INVALID_SIZE 0xCA01u
#define RESULT_INVALID_STATE 0xFA01u
#define RESULT_INJECTED 0xDEAD01u
#define MAX_OBJECTS 64

typedef struct {
    int used;
    int fd;
    size_t size;
    uintptr_t rw, rx; /* mapped views, 0 if not */
} fake_object;

typedef struct {
    uintptr_t addr;
    size_t size;
} fake_reservation;

typedef struct {
    uint32_t limit;      /* objects the kernel will create: 10, like the console */
    fake_object obj[MAX_OBJECTS];
    uint32_t live_objects, peak_objects;
    int live_reservations, live_backings;
    int unmap_rx_calls;
    /* failure injection: the next call of that kind fails */
    int fail_create, fail_map_rx, fail_map_rw, fail_unmap_rw;
    int no_space_near;
    int near_out_of_reach; /* reserve_near answers with an address beyond ADRP's reach */
    int log_lines;
} fake_kernel;

static fake_kernel K;

static void *fk_alloc_backing(void *ctx, size_t size)
{
    (void)ctx;
    void *p = aligned_alloc(CODE_ARENA_PAGE, size);
    if (p != NULL) {
        memset(p, 0, size);
        K.live_backings++;
    }
    return p;
}

static void fk_free_backing(void *ctx, void *backing, size_t size)
{
    (void)ctx;
    (void)size;
    free(backing);
    K.live_backings--;
}

static uint32_t fk_create(void *ctx, void *backing, size_t size, uint32_t *handle)
{
    (void)ctx;
    if (K.fail_create) {
        K.fail_create = 0;
        return RESULT_INJECTED;
    }
    if (K.live_objects >= K.limit) {
        return RESULT_RESOURCE_EXHAUSTED;
    }
    for (uint32_t i = 0; i < MAX_OBJECTS; i++) {
        if (!K.obj[i].used) {
            const int fd = memfd_create("codemem", 0);
            if (fd < 0 || ftruncate(fd, (off_t)size) != 0 || pwrite(fd, backing, size, 0) != (ssize_t)size) {
                return RESULT_INJECTED;
            }
            K.obj[i] = (fake_object){1, fd, size, 0, 0};
            K.live_objects++;
            if (K.live_objects > K.peak_objects) {
                K.peak_objects = K.live_objects;
            }
            *handle = i + 1;
            return 0;
        }
    }
    return RESULT_RESOURCE_EXHAUSTED;
}

static fake_object *object(uint32_t handle)
{
    return handle >= 1 && handle <= MAX_OBJECTS && K.obj[handle - 1].used ? &K.obj[handle - 1] : NULL;
}

static uint32_t fk_close(void *ctx, uint32_t handle)
{
    (void)ctx;
    fake_object *o = object(handle);
    if (o == NULL || o->rw != 0 || o->rx != 0) {
        return RESULT_INVALID_STATE; /* the console keeps a mapped object alive */
    }
    close(o->fd);
    o->used = 0;
    K.live_objects--;
    return 0;
}

static uint32_t view(uint32_t handle, uintptr_t addr, size_t size, int prot, uintptr_t *slot)
{
    fake_object *o = object(handle);
    if (o == NULL) {
        return RESULT_INVALID_STATE;
    }
    if (size != o->size) {
        return RESULT_INVALID_SIZE; /* MapOwner and MapSlave map the whole object or nothing */
    }
    if (*slot != 0) {
        return RESULT_INVALID_STATE; /* one view of each kind at a time */
    }
    void *p = mmap((void *)addr, size, prot, MAP_SHARED | MAP_FIXED, o->fd, 0);
    if (p == MAP_FAILED) {
        return RESULT_INVALID_STATE;
    }
    *slot = addr;
    return 0;
}

static uint32_t unview(uint32_t handle, uintptr_t addr, size_t size, uintptr_t *slot)
{
    fake_object *o = object(handle);
    if (o == NULL || *slot != addr || size != o->size) {
        return RESULT_INVALID_STATE;
    }
    /* The reservation stays: an inaccessible hole, like an unmapped reserved range. */
    mmap((void *)addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    *slot = 0;
    return 0;
}

static uint32_t fk_map_rw(void *ctx, uint32_t h, uintptr_t addr, size_t size)
{
    (void)ctx;
    if (K.fail_map_rw) {
        K.fail_map_rw = 0;
        return RESULT_INJECTED;
    }
    fake_object *o = object(h);
    return o != NULL ? view(h, addr, size, PROT_READ | PROT_WRITE, &o->rw) : RESULT_INVALID_STATE;
}

static uint32_t fk_unmap_rw(void *ctx, uint32_t h, uintptr_t addr, size_t size)
{
    (void)ctx;
    if (K.fail_unmap_rw) {
        K.fail_unmap_rw = 0;
        return RESULT_INJECTED;
    }
    fake_object *o = object(h);
    return o != NULL ? unview(h, addr, size, &o->rw) : RESULT_INVALID_STATE;
}

static uint32_t fk_map_rx(void *ctx, uint32_t h, uintptr_t addr, size_t size)
{
    (void)ctx;
    if (K.fail_map_rx) {
        K.fail_map_rx = 0;
        return RESULT_INJECTED;
    }
    fake_object *o = object(h);
    return o != NULL ? view(h, addr, size, PROT_READ | PROT_EXEC, &o->rx) : RESULT_INVALID_STATE;
}

static uint32_t fk_unmap_rx(void *ctx, uint32_t h, uintptr_t addr, size_t size)
{
    (void)ctx;
    K.unmap_rx_calls++;
    fake_object *o = object(h);
    return o != NULL ? unview(h, addr, size, &o->rx) : RESULT_INVALID_STATE;
}

static uintptr_t reserve_at(uintptr_t hint, size_t size, void **token)
{
    const int fixed = hint != 0 ? MAP_FIXED_NOREPLACE : 0;  /* 0: anywhere the host likes */
    void *p = mmap((void *)hint, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | fixed, -1, 0);
    if (p == MAP_FAILED || (hint != 0 && (uintptr_t)p != hint)) {
        if (p != MAP_FAILED) {
            munmap(p, size);
        }
        return 0;
    }
    fake_reservation *r = malloc(sizeof *r);
    r->addr = (uintptr_t)p;
    r->size = size;
    *token = r;
    K.live_reservations++;
    return (uintptr_t)p;
}

static uintptr_t fk_reserve_near(void *ctx, uintptr_t target, size_t size, void **token)
{
    (void)ctx;
    if (K.no_space_near) {
        return 0;
    }
    if (K.near_out_of_reach) {
        return reserve_at((target ^ ((uintptr_t)1 << 45)) & ~(uintptr_t)0xFFFFF, size, token);
    }
    /* Walk upward from just past the target in 16 MiB steps, then downward, as the console's
     * search walks the address space outward from the hooked function. */
    const uintptr_t base = (target + (16u << 20)) & ~(uintptr_t)0xFFFFF;
    for (uintptr_t step = 0; step < 256; step++) {
        uintptr_t a = reserve_at(base + step * (16u << 20), size, token);
        if (a != 0) {
            return a;
        }
        if (base > (step + 2) * (16u << 20)) {
            a = reserve_at(base - (step + 2) * (16u << 20), size, token);
            if (a != 0) {
                return a;
            }
        }
    }
    return 0;
}

static uintptr_t fk_reserve_any(void *ctx, size_t size, void **token)
{
    (void)ctx;
    return reserve_at(0, size, token);
}

static void fk_release(void *ctx, void *token)
{
    (void)ctx;
    fake_reservation *r = token;
    munmap((void *)r->addr, r->size);
    free(r);
    K.live_reservations--;
}

static void fk_dcache(void *ctx, void *addr, size_t size)
{
    (void)ctx;
    (void)addr;
    (void)size;
}

static void fk_icache(void *ctx, uintptr_t addr, size_t size)
{
    (void)ctx;
    __builtin___clear_cache((char *)addr, (char *)addr + size);
}

static void fk_log(void *ctx, const char *line)
{
    (void)ctx;
    K.log_lines++;
    if (getenv("CODE_ARENA_TEST_VERBOSE") != NULL) {
        printf("  log: %s\n", line);
    }
}

static const code_arena_ops OPS = {
    NULL, fk_alloc_backing, fk_free_backing, fk_create, fk_close, fk_map_rw, fk_unmap_rw,
    fk_map_rx, fk_unmap_rx, fk_reserve_near, fk_reserve_any, fk_release, fk_dcache, fk_icache, fk_log,
};

static void reset_kernel(void)
{
    memset(&K, 0, sizeof K);
    K.limit = 10;
}

/* ------------------------------------------------------------------ part A: the manager -- */

/* Hook targets: addresses inside this program's code, as the game's functions are inside the NRO. */
static uintptr_t target(int i)
{
    return ((uintptr_t)&reset_kernel & ~(uintptr_t)0xFFF) + (uintptr_t)i * 0x40;
}

typedef struct {
    uintptr_t rx[128];
    void *rw[128];
    int n;
} mod_pages;

static int install(code_arenas *m, mod_pages *mod, int first, int count, uint8_t tag)
{
    char err[192];
    for (int i = 0; i < count; i++) {
        uintptr_t rx = 0;
        void *rw = NULL;
        const int rv = code_arena_alloc(m, target(first + i), &rx, &rw, err, sizeof err);
        if (rv != 0) {
            return rv;
        }
        memset(rw, tag, 64); /* the "trampoline" */
        CHECK(code_arena_commit(m, rx, err, sizeof err) == 0);
        CHECK(code_arena_within_adrp(target(first + i), rx));
        mod->rx[mod->n] = rx;
        mod->rw[mod->n] = rw;
        mod->n++;
    }
    return 0;
}

static void uninstall(code_arenas *m, mod_pages *mod)
{
    char err[192];
    for (int i = 0; i < mod->n; i++) {
        CHECK(code_arena_free(m, mod->rx[i], err, sizeof err) == 0);
    }
    mod->n = 0;
}

static int intact(const mod_pages *mod, uint8_t tag)
{
    for (int i = 0; i < mod->n; i++) {
        const uint8_t *p = (const uint8_t *)mod->rx[i]; /* read through the EXECUTABLE view */
        for (int b = 0; b < 64; b++) {
            if (p[b] != tag) {
                return 0;
            }
        }
    }
    return 1;
}

static int rw_views_mapped(void)
{
    int n = 0;
    for (int i = 0; i < MAX_OBJECTS; i++) {
        n += K.obj[i].used && K.obj[i].rw != 0;
    }
    return n;
}

static void control_kernel_limit(void)
{
    /* The fake behaves as the console did under H36: one object per hook page, the 11th refused. */
    reset_kernel();
    void *backing = fk_alloc_backing(NULL, CODE_ARENA_PAGE);
    uint32_t handles[11];
    uint32_t rc = 0;
    int created = 0;
    for (int i = 0; i < 11; i++) {
        rc = fk_create(NULL, backing, CODE_ARENA_PAGE, &handles[i]);
        if (rc != 0) {
            break;
        }
        created++;
    }
    CHECK(created == 10 && rc == RESULT_RESOURCE_EXHAUSTED);
    for (int i = 0; i < created; i++) {
        fk_close(NULL, handles[i]);
    }
    fk_free_backing(NULL, backing, CODE_ARENA_PAGE);
    printf("control      one object per page, as in H36: the fake kernel refuses object 11 with 0x%x\n", rc);
}

static void mods_together(void)
{
    reset_kernel();
    code_arenas m;
    code_arenas_init(&m, &OPS);
    static mod_pages cosmetics, rollsprint, rando;
    memset(&cosmetics, 0, sizeof cosmetics);
    memset(&rollsprint, 0, sizeof rollsprint);
    memset(&rando, 0, sizeof rando);

    CHECK(install(&m, &cosmetics, 0, 28, 0xC0) == 0);
    CHECK(K.live_objects == 1);
    CHECK(install(&m, &rollsprint, 100, 4, 0x55) == 0);
    CHECK(K.live_objects == 1 && m.stats.pages_used == 32);
    const uint32_t objectsCosmeticsRoll = K.live_objects;
    CHECK(install(&m, &rando, 200, 92, 0xAA) == 0);
    CHECK(K.live_objects == 2 && K.live_objects < K.limit);
    CHECK(intact(&cosmetics, 0xC0) && intact(&rollsprint, 0x55) && intact(&rando, 0xAA));
    CHECK(rw_views_mapped() == 0);  /* nothing being written: no writable view */
    CHECK(m.writers == 0);

    /* Cosmetics off: the others keep running, from views that never moved. */
    uninstall(&m, &cosmetics);
    CHECK(intact(&rollsprint, 0x55) && intact(&rando, 0xAA));
    CHECK(K.unmap_rx_calls == 0);
    /* ...and on again, five times: no growth. */
    for (int cycle = 0; cycle < 5; cycle++) {
        CHECK(install(&m, &cosmetics, 0, 28, (uint8_t)(0xC1 + cycle)) == 0);
        CHECK(intact(&cosmetics, (uint8_t)(0xC1 + cycle)) && intact(&rollsprint, 0x55) && intact(&rando, 0xAA));
        uninstall(&m, &cosmetics);
    }
    CHECK(install(&m, &cosmetics, 0, 28, 0xC9) == 0);
    CHECK(K.live_objects == 2 && K.peak_objects == 2);
    CHECK(m.stats.reuses > 0);
    CHECK(K.unmap_rx_calls == 0);
    char line[320];
    code_arena_describe(&m, line, sizeof line);
    printf("mods         Cosmetics 28 + Roll and Sprint 4: %u object(s); + Randomizer 92: %u; "
           "Cosmetics off/on x6, others intact; kernel limit %u\n",
        objectsCosmeticsRoll, K.live_objects, K.limit);
    printf("             %s\n", line);

    uninstall(&m, &cosmetics);
    uninstall(&m, &rollsprint);
    uninstall(&m, &rando);
    CHECK(code_arena_trim(&m) == 0);
    CHECK(K.live_objects == 0 && K.live_reservations == 0 && K.live_backings == 0);
    CHECK(K.unmap_rx_calls == 2);  /* only by trim, only once every page was free */
}

static void failures(void)
{
    char err[192];
    reset_kernel();
    code_arenas m;
    code_arenas_init(&m, &OPS);
    uintptr_t rx = 0;
    void *rw = NULL;

    K.fail_create = 1;
    CHECK(code_arena_alloc(&m, target(0), &rx, &rw, err, sizeof err) == CODE_ARENA_E_KERNEL);
    CHECK(strstr(err, "svcCreateCodeMemory") != NULL && strstr(err, "0xdead01") != NULL);
    CHECK(m.stats.last_failure == RESULT_INJECTED);
    CHECK(K.live_objects == 0 && K.live_reservations == 0 && K.live_backings == 0 && m.count == 0);

    K.fail_map_rx = 1;
    CHECK(code_arena_alloc(&m, target(0), &rx, &rw, err, sizeof err) == CODE_ARENA_E_KERNEL);
    CHECK(strstr(err, "MapSlave") != NULL);
    CHECK(K.live_objects == 0 && K.live_reservations == 0 && K.live_backings == 0 && m.count == 0);

    K.no_space_near = 1;
    CHECK(code_arena_alloc(&m, target(0), &rx, &rw, err, sizeof err) == CODE_ARENA_E_NO_RANGE);
    CHECK(K.live_objects == 0 && K.live_reservations == 0 && K.live_backings == 0);
    K.no_space_near = 0;

    /* A kernel that answers with space beyond ADRP's reach: refused, the reservation given back. */
    K.near_out_of_reach = 1;
    CHECK(code_arena_alloc(&m, target(0), &rx, &rw, err, sizeof err) == CODE_ARENA_E_NO_RANGE);
    CHECK(K.live_objects == 0 && K.live_reservations == 0 && K.live_backings == 0);
    K.near_out_of_reach = 0;

    /* A target far beyond 4 GiB of every arena, with no space near it: refused cleanly, and the
     * existing arena is untouched. */
    mod_pages some;
    memset(&some, 0, sizeof some);
    CHECK(install(&m, &some, 0, 3, 0x11) == 0);
    const uint32_t before = K.live_objects;
    const uintptr_t far = target(0) ^ ((uintptr_t)1 << 45);
    K.no_space_near = 1;
    CHECK(code_arena_alloc(&m, far, &rx, &rw, err, sizeof err) == CODE_ARENA_E_NO_RANGE);
    K.no_space_near = 0;
    CHECK(K.live_objects == before && K.live_reservations == 2 * (int)before && K.live_backings == (int)before);
    CHECK(intact(&some, 0x11));

    /* MapOwner refused when a page is to be written: the page stays free, nothing leaks. */
    const uint32_t used = m.stats.pages_used;
    K.fail_map_rw = 1;
    CHECK(code_arena_alloc(&m, target(5), &rx, &rw, err, sizeof err) == CODE_ARENA_E_KERNEL);
    CHECK(strstr(err, "MapOwner") != NULL);
    CHECK(m.stats.pages_used == used && m.writers == 0);
    CHECK(install(&m, &some, 5, 1, 0x11) == 0);  /* and the next attempt works */

    /* UnmapOwner refused after a write: reported; the page is live and executable regardless. */
    CHECK(code_arena_alloc(&m, target(6), &rx, &rw, err, sizeof err) == 0);
    memset(rw, 0x22, 64);
    K.fail_unmap_rw = 1;
    CHECK(code_arena_commit(&m, rx, err, sizeof err) == CODE_ARENA_E_KERNEL);
    CHECK(strstr(err, "UnmapOwner") != NULL);
    CHECK(((const uint8_t *)rx)[0] == 0x22);
    CHECK(rw_views_mapped() == 1);  /* still mapped: the next commit retries the unmap */
    CHECK(code_arena_alloc(&m, target(7), &rx, &rw, err, sizeof err) == 0);
    CHECK(code_arena_commit(&m, rx, err, sizeof err) == 0);
    CHECK(rw_views_mapped() == 0);

    /* Misuse is refused, not followed. */
    CHECK(code_arena_free(&m, (uintptr_t)0x1000, err, sizeof err) == CODE_ARENA_E_INVALID);
    CHECK(code_arena_commit(&m, (uintptr_t)0x1000, err, sizeof err) == CODE_ARENA_E_INVALID);

    uninstall(&m, &some);
    for (uint32_t a = 0; a < m.count; a++) {
        for (uint32_t p = 0; p < CODE_ARENA_PAGES; p++) {
            if (m.arena[a].state[p] != CODE_SLOT_FREE) {
                code_arena_free(&m, m.arena[a].rx + p * CODE_ARENA_PAGE, err, sizeof err);
            }
        }
    }
    CHECK(code_arena_trim(&m) == 0);
    CHECK(K.live_objects == 0 && K.live_reservations == 0 && K.live_backings == 0);
    printf("failures     create, MapSlave, MapOwner, UnmapOwner, no space, out of reach: refused "
           "with the Result, nothing leaked\n");
}

static void reuse_order(void)
{
    char err[192];
    reset_kernel();
    code_arenas m;
    code_arenas_init(&m, &OPS);
    mod_pages all;
    memset(&all, 0, sizeof all);
    CHECK(install(&m, &all, 0, CODE_ARENA_PAGES, 0x33) == 0);  /* one arena, full */
    CHECK(K.live_objects == 1);
    /* Free pages 10, 3, 40 in that order: they come back 10, 3, 40. */
    const int order[3] = {10, 3, 40};
    for (int i = 0; i < 3; i++) {
        CHECK(code_arena_free(&m, all.rx[order[i]], err, sizeof err) == 0);
    }
    for (int i = 0; i < 3; i++) {
        uintptr_t rx = 0;
        void *rw = NULL;
        CHECK(code_arena_alloc(&m, target(0), &rx, &rw, err, sizeof err) == 0);
        CHECK(rx == all.rx[order[i]]);
        CHECK(code_arena_commit(&m, rx, err, sizeof err) == 0);
    }
    CHECK(K.live_objects == 1 && m.stats.reuses == 3);
    /* One more than fits: a second arena, a second object. */
    uintptr_t rx = 0;
    void *rw = NULL;
    CHECK(code_arena_alloc(&m, target(1), &rx, &rw, err, sizeof err) == 0);
    CHECK(code_arena_commit(&m, rx, err, sizeof err) == 0);
    CHECK(K.live_objects == 2);
    printf("reuse        freed pages come back oldest first; a full arena makes a second object\n");
    for (uint32_t a = 0; a < m.count; a++) {
        for (uint32_t p = 0; p < CODE_ARENA_PAGES; p++) {
            if (m.arena[a].state[p] != CODE_SLOT_FREE) {
                code_arena_free(&m, m.arena[a].rx + p * CODE_ARENA_PAGE, err, sizeof err);
            }
        }
    }
    code_arena_trim(&m);
    CHECK(K.live_objects == 0 && K.live_reservations == 0 && K.live_backings == 0);
}

/* H38: a mod whose hooks do not fit. Cosmetics + Roll and Sprint (32 pages) are running and a mod asks for more than
 * the 8 arenas (512 pages) can give. The allocation that does not fit fails with a message that names the capacity,
 * the loader then drops that mod's partial hooks (hook_remove_mod frees page by page, done here the same way), the
 * running mods stay intact, every page is available again, and neither the failure nor repeating it adds an object. */
static void capacity_exhaustion(void)
{
    char err[192];
    reset_kernel();
    code_arenas m;
    code_arenas_init(&m, &OPS);
    static mod_pages running;
    memset(&running, 0, sizeof running);
    CHECK(install(&m, &running, 0, 32, 0xB0) == 0);
    CHECK(K.live_objects == 1);

    enum { ASK = 600, ROOM = CODE_ARENA_MAX * CODE_ARENA_PAGES - 32 };
    static uintptr_t big[ASK];
    for (int cycle = 0; cycle < 6; cycle++) {
        int got = 0;
        int rv = 0;
        err[0] = '\0';
        for (int i = 0; i < ASK; i++) {
            uintptr_t rx = 0;
            void *rw = NULL;
            rv = code_arena_alloc(&m, target(1000 + i), &rx, &rw, err, sizeof err);
            if (rv != 0) {
                break;
            }
            memset(rw, 0xBB, 64);
            CHECK(code_arena_commit(&m, rx, err, sizeof err) == 0);
            big[got++] = rx;
        }
        /* every page that exists was handed out, then the next one failed, naming the capacity */
        CHECK(got == ROOM && rv == CODE_ARENA_E_FULL);
        CHECK(strstr(err, "every code arena is full") != NULL && strstr(err, "8 arenas") != NULL);
        CHECK(K.live_objects == CODE_ARENA_MAX && K.peak_objects == CODE_ARENA_MAX && K.peak_objects <= K.limit);
        CHECK(m.stats.pages_used == CODE_ARENA_MAX * CODE_ARENA_PAGES);
        CHECK(intact(&running, 0xB0));  /* the running mods are untouched */
        /* the mod is dropped: its partial hooks go back one by one */
        for (int i = 0; i < got; i++) {
            CHECK(code_arena_free(&m, big[i], err, sizeof err) == 0);
        }
        CHECK(m.stats.pages_used == 32);
        CHECK(intact(&running, 0xB0));
        CHECK(K.live_objects == CODE_ARENA_MAX && K.peak_objects == CODE_ARENA_MAX);  /* no growth, in any cycle */
    }
    /* the pages are reusable: a mod that fits goes in, from the arenas that exist */
    static mod_pages fits;
    memset(&fits, 0, sizeof fits);
    CHECK(install(&m, &fits, 2000, 100, 0xCC) == 0);
    CHECK(intact(&fits, 0xCC) && intact(&running, 0xB0));
    CHECK(K.live_objects == CODE_ARENA_MAX && m.stats.reuses > 0);
    uninstall(&m, &fits);
    uninstall(&m, &running);
    CHECK(code_arena_trim(&m) == 0);
    CHECK(K.live_objects == 0 && K.live_reservations == 0 && K.live_backings == 0);
    printf("capacity     a mod asking for more than 8 arenas fails with 'every code arena is full'; its partial pages "
           "go back, the running mods stay intact, objects stay at %u over 6 cycles\n",
        (unsigned)CODE_ARENA_MAX);
}

#ifdef CODE_ARENA_TEST_FUNCHOOK
void run_funchook_part(void);
#endif

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    control_kernel_limit();
    mods_together();
    failures();
    reuse_order();
    capacity_exhaustion();
#ifdef CODE_ARENA_TEST_FUNCHOOK
    run_funchook_part();
#endif
    if (g_failures) {
        fprintf(stderr, "code_arena_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("code_arena_test: all checks passed\n");
    return 0;
}

/* Shared with the funchook part. */
const code_arena_ops *code_arena_test_ops(void)
{
    return &OPS;
}
void code_arena_test_reset_kernel(uint32_t limit)
{
    reset_kernel();
    K.limit = limit;
}
uint32_t code_arena_test_live_objects(void)
{
    return K.live_objects;
}
uint32_t code_arena_test_peak_objects(void)
{
    return K.peak_objects;
}
int code_arena_test_failures(void)
{
    return g_failures;
}
void code_arena_test_fail(const char *what)
{
    fprintf(stderr, "funchook part: CHECK failed: %s\n", what);
    ++g_failures;
}
