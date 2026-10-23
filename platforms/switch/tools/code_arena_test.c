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
 *   - DM2-a: Twilit 343 + Dawnlight 318 + Randomizer 92 + Zora Swim 43 + Dark Link 28 hooks (824;
 *     H37's one page per hook needed 13 objects) take ONE object; ALBT's 331 on top, two;
 *   - DM2-a: 16 slots a page; writing one never changes a byte of a live neighbour; ordinary hooks
 *     leave the page-boundary slots (the 8-byte patch's) for last; misaligned slots are refused;
 *   - a failed create, MapSlave, MapOwner or UnmapOwner leaves nothing behind;
 *   - releasing one mod's slots leaves another's executable and intact, the executable view of
 *     an arena is never unmapped while it lives, and the writable view is mapped only while a
 *     slot is being written;
 *   - freed slots are reused (oldest first, after every fresh one) and repeated deactivation and
 *     reactivation never grows the number of objects;
 *   - every slot is within ADRP reach of its target, and a target with no code space in reach is
 *     refused cleanly;
 *   - more hooks than eight arenas hold, or a kernel that runs out first, fail with the reason and
 *     leave the running mods intact; code_arena_usage says so beforehand (the loader's check);
 *   - trim releases empty arenas and every handle, reservation and backing comes back.
 *
 * Part B (aarch64 only, run under qemu by test-code-arena.sh): funchook itself -- v1.1.3 as the
 * build fetches it, patched by cmake/PatchFunchook.cmake -- installs real hooks on real functions
 * through these slots, one funchook per target as svc/hook.cpp does (code_arena_funchook_test.c
 * says which). Every hooked call must reach its hook, and every hook must reach the original
 * through its trampoline.
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
    /* DM2-a: qemu-user does not see a write made through the writable view (another address of the
     * same memfd) and would keep running what it translated from a reused slot before -- what the
     * instruction cache invalidation prevents on hardware. Mapping the page again, same file, same
     * offset, same protection, makes qemu drop those translations; a thread running elsewhere in
     * the page sees the same bytes throughout (MAP_FIXED replaces the mapping at once). */
    for (int i = 0; i < MAX_OBJECTS; i++) {
        const fake_object *o = &K.obj[i];
        if (o->used && o->rx != 0 && addr >= o->rx && addr < o->rx + o->size) {
            const uintptr_t lo = addr & ~(uintptr_t)0xFFF;
            const uintptr_t hi = (addr + size + 0xFFF) & ~(uintptr_t)0xFFF;
            mmap((void *)lo, hi - lo, PROT_READ | PROT_EXEC, MAP_SHARED | MAP_FIXED, o->fd, (off_t)(lo - o->rx));
        }
    }
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

/* One mod's slots, as hook.cpp holds them: one per hooked function. */
#define MOD_SLOTS_MAX 1200
typedef struct {
    uintptr_t rx[MOD_SLOTS_MAX];
    void *rw[MOD_SLOTS_MAX];
    uint8_t tag[MOD_SLOTS_MAX];
    int n;
} mod_slots;

static mod_slots *new_mod(void)
{
    mod_slots *mod = calloc(1, sizeof *mod);
    return mod;
}

/* Every byte of a slot carries the tag, so an overlap or a stray write shows up anywhere in it. */
static int install_flags(code_arenas *m, mod_slots *mod, int first, int count, uint8_t tag, unsigned flags)
{
    char err[192];
    for (int i = 0; i < count; i++) {
        uintptr_t rx = 0;
        void *rw = NULL;
        const int rv = code_arena_alloc(m, target(first + i), flags, &rx, &rw, err, sizeof err);
        if (rv != 0) {
            return rv;
        }
        CHECK(((const uint8_t *)rw)[0] == 0 && ((const uint8_t *)rw)[CODE_ARENA_SLOT - 1] == 0);  /* zeroed */
        memset(rw, tag, CODE_ARENA_SLOT); /* the "trampoline" */
        CHECK(code_arena_commit(m, rx, err, sizeof err) == 0);
        CHECK(code_arena_within_adrp(target(first + i), rx));
        CHECK(rx % CODE_ARENA_SLOT == 0);
        if (flags & CODE_ARENA_PAGE_ALIGNED) {
            CHECK(rx % CODE_ARENA_PAGE == 0);
        }
        mod->rx[mod->n] = rx;
        mod->rw[mod->n] = rw;
        mod->tag[mod->n] = tag;
        mod->n++;
    }
    return 0;
}

static int install(code_arenas *m, mod_slots *mod, int first, int count, uint8_t tag)
{
    return install_flags(m, mod, first, count, tag, 0);
}

static void uninstall(code_arenas *m, mod_slots *mod)
{
    char err[192];
    for (int i = 0; i < mod->n; i++) {
        CHECK(code_arena_free(m, mod->rx[i], err, sizeof err) == 0);
    }
    mod->n = 0;
}

static int intact(const mod_slots *mod)
{
    for (int i = 0; i < mod->n; i++) {
        const uint8_t *p = (const uint8_t *)mod->rx[i]; /* read through the EXECUTABLE view */
        for (uint32_t b = 0; b < CODE_ARENA_SLOT; b++) {
            if (p[b] != mod->tag[i]) {
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

static void free_everything(code_arenas *m)
{
    char err[192];
    for (uint32_t a = 0; a < m->count; a++) {
        for (uint32_t s = 0; s < CODE_ARENA_SLOTS; s++) {
            if (m->arena[a].state[s] != CODE_SLOT_FREE) {
                code_arena_free(m, m->arena[a].rx + s * CODE_ARENA_SLOT, err, sizeof err);
            }
        }
    }
    CHECK(code_arena_trim(m) == 0);
    CHECK(K.live_objects == 0 && K.live_reservations == 0 && K.live_backings == 0);
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

/* DM2-a: the combination that did not fit. Declared hooks (package_audit.py, catalogue of
 * 2026-10-14): Twilit Essentials 343, Dawnlight 318, Randomizer 92, Zora Swim 43, Dark Link 28 --
 * 824, against 512 in H37's one-page-per-hook arenas. Then A Link Between Twilight (331) on top. */
static void catalogue_together(void)
{
    reset_kernel();
    static code_arenas m;
    code_arenas_init(&m, &OPS);
    mod_slots *twilit = new_mod(), *dawn = new_mod(), *rando = new_mod(), *zora = new_mod(), *dark = new_mod(),
              *albt = new_mod();

    CHECK(install(&m, twilit, 0, 343, 0x71) == 0);
    CHECK(install(&m, dawn, 400, 318, 0xD1) == 0);
    CHECK(install(&m, rando, 800, 92, 0xAA) == 0);
    CHECK(install(&m, zora, 1000, 43, 0x2A) == 0);
    CHECK(install(&m, dark, 1100, 28, 0xDA) == 0);
    const uint32_t five_objects = K.live_objects;
    const uint32_t five_pages = m.stats.pages_touched;
    CHECK(five_objects == 1);                  /* H37 needed 13 (824 / 64): more than the kernel has */
    CHECK(m.stats.slots_used == 824);
    CHECK(five_pages == (824 + 14) / 15);     /* 15 ordinary slots a page: the boundary slot is kept free */
    CHECK(m.stats.aligned_used == 0);
    CHECK(intact(twilit) && intact(dawn) && intact(rando) && intact(zora) && intact(dark));
    CHECK(rw_views_mapped() == 0 && m.writers == 0);
    uint32_t used = 0, free_now = 0, capacity = 0;
    code_arena_usage(&m, &used, &free_now, &capacity);
    CHECK(used == 824 && free_now == CODE_ARENA_SLOTS - 824 && capacity == CODE_ARENA_MAX * CODE_ARENA_SLOTS);

    CHECK(install(&m, albt, 1200, 331, 0xAB) == 0);
    CHECK(K.live_objects == 2 && m.stats.slots_used == 1155);
    CHECK(intact(twilit) && intact(dawn) && intact(rando) && intact(zora) && intact(dark) && intact(albt));

    /* Dawnlight off and on again five times: the others intact, no object added, slots reused. */
    for (int cycle = 0; cycle < 5; cycle++) {
        uninstall(&m, dawn);
        CHECK(intact(twilit) && intact(rando) && intact(albt));
        CHECK(install(&m, dawn, 400, 318, (uint8_t)(0xD2 + cycle)) == 0);
        CHECK(intact(dawn) && intact(twilit) && intact(rando) && intact(zora) && intact(dark) && intact(albt));
    }
    CHECK(K.live_objects == 2 && K.peak_objects == 2 && K.unmap_rx_calls == 0);
    char line[512];
    code_arena_describe(&m, line, sizeof line);
    printf("catalogue    Twilit 343 + Dawnlight 318 + Randomizer 92 + Zora Swim 43 + Dark Link 28 = 824 hooks: "
           "%u CodeMemory object, %u pages (H37: 13 objects, beyond the kernel's ~10); + ALBT 331: %u objects; "
           "Dawnlight off/on x5, others intact\n",
           five_objects, five_pages, K.live_objects);
    printf("             %s\n", line);
    uninstall(&m, twilit);
    uninstall(&m, dawn);
    uninstall(&m, rando);
    uninstall(&m, zora);
    uninstall(&m, dark);
    uninstall(&m, albt);
    CHECK(code_arena_trim(&m) == 0);
    CHECK(K.live_objects == 0 && K.live_reservations == 0 && K.live_backings == 0);
    CHECK(K.unmap_rx_calls == 2);  /* only by trim, only once every slot was free */
    free(twilit), free(dawn), free(rando), free(zora), free(dark), free(albt);
}

/* DM2-a: slots of one page are independent. Writing one (its arena's writable view mapped) never
 * changes a byte of its neighbours, which other threads may be executing; the executable view is
 * never touched; page-boundary slots go to the two-instruction patch and ordinary requests take
 * them last. */
static void packing(void)
{
    char err[192];
    reset_kernel();
    static code_arenas m;
    code_arenas_init(&m, &OPS);
    mod_slots *a = new_mod(), *b = new_mod(), *narrow = new_mod();

    CHECK(install(&m, a, 0, 15, 0x11) == 0);  /* one page's ordinary slots */
    CHECK(m.stats.pages_touched == 1);
    for (int i = 0; i < 15; i++) {
        CHECK(a->rx[i] / CODE_ARENA_PAGE == a->rx[0] / CODE_ARENA_PAGE);  /* the same page */
        CHECK(a->rx[i] % CODE_ARENA_PAGE != 0);                         /* not its boundary slot */
        for (int j = 0; j < i; j++) {
            CHECK(a->rx[i] != a->rx[j]);
        }
    }
    /* A narrow hook: the boundary slot of the first page, next to the 15 live ones. */
    CHECK(install_flags(&m, narrow, 15, 1, 0x88, CODE_ARENA_PAGE_ALIGNED) == 0);
    CHECK(narrow->rx[0] == a->rx[0] - CODE_ARENA_SLOT);
    CHECK(m.stats.aligned_used == 1 && m.stats.pages_touched == 1);

    /* A slot held in WRITING (writable view mapped) while its neighbours are live: they are intact,
     * through the executable view, before and after the write. */
    CHECK(code_arena_free(&m, a->rx[7], err, sizeof err) == 0);  /* slot 8 of page 0, freed first */
    a->rx[7] = a->rx[--a->n];
    uintptr_t rx = 0;
    void *rw = NULL;
    CHECK(code_arena_alloc(&m, target(7), 0, &rx, &rw, err, sizeof err) == 0);
    CHECK(rx / CODE_ARENA_PAGE != a->rx[0] / CODE_ARENA_PAGE);  /* a fresh slot, not the freed one */
    CHECK(rw_views_mapped() == 1);
    memset(rw, 0x5A, CODE_ARENA_SLOT);
    CHECK(intact(a) && intact(narrow));
    CHECK(code_arena_commit(&m, rx, err, sizeof err) == 0);
    CHECK(rw_views_mapped() == 0);
    CHECK(intact(a) && intact(narrow) && ((const uint8_t *)rx)[0] == 0x5A);
    const uintptr_t freed_second = rx;
    CHECK(code_arena_free(&m, rx, err, sizeof err) == 0);  /* freed second */

    /* Rewriting a live slot (funchook's unprotect then protect) touches only that slot. */
    void *again = NULL;
    CHECK(code_arena_begin_write(&m, a->rx[3], &again, err, sizeof err) == 0);
    memset(again, 0x33, CODE_ARENA_SLOT);
    a->tag[3] = 0x33;
    CHECK(code_arena_commit(&m, a->rx[3], err, sizeof err) == 0);
    CHECK(rw_views_mapped() == 0 && intact(a) && intact(narrow));

    /* Misaligned or interior addresses are not slots; unknown flags are refused. */
    CHECK(code_arena_free(&m, a->rx[0] + 4, err, sizeof err) == CODE_ARENA_E_INVALID);
    CHECK(code_arena_commit(&m, a->rx[0] + CODE_ARENA_SLOT / 2, err, sizeof err) == CODE_ARENA_E_INVALID);
    CHECK(code_arena_alloc(&m, target(0), 0x80, &rx, &rw, err, sizeof err) == CODE_ARENA_E_INVALID);

    /* Every fresh ordinary slot goes before any boundary slot: 960 a arena, 16 taken (15 + one freed). */
    const int fresh_ordinary = (int)(CODE_ARENA_SLOTS - CODE_ARENA_PAGES) - 16;
    CHECK(install(&m, b, 100, fresh_ordinary, 0x22) == 0);
    CHECK(m.stats.aligned_used == 1);  /* still only the narrow hook's */
    CHECK(m.stats.pages_touched == CODE_ARENA_PAGES && m.stats.reuses == 0);
    /* Narrow hooks still find every other boundary slot. */
    mod_slots *narrow2 = new_mod();
    CHECK(install_flags(&m, narrow2, 3000, (int)CODE_ARENA_PAGES - 1, 0x99, CODE_ARENA_PAGE_ALIGNED) == 0);
    CHECK(m.stats.aligned_used == CODE_ARENA_PAGES && K.live_objects == 1);
    /* Then the two freed slots, the one freed first first. */
    mod_slots *c = new_mod();
    CHECK(install(&m, c, 5000, 2, 0x77) == 0);
    CHECK(c->rx[0] / CODE_ARENA_PAGE == a->rx[0] / CODE_ARENA_PAGE && c->rx[1] == freed_second);
    CHECK(m.stats.reuses == 2 && m.stats.slots_used == CODE_ARENA_SLOTS && K.live_objects == 1);
    /* The arena is full: the next one makes a second object. */
    mod_slots *narrow3 = new_mod();
    CHECK(install_flags(&m, narrow3, 4000, 1, 0x9A, CODE_ARENA_PAGE_ALIGNED) == 0);
    CHECK(K.live_objects == 2);
    CHECK(intact(a) && intact(b) && intact(c) && intact(narrow) && intact(narrow2) && intact(narrow3));
    printf("packing      %u slots of %u B a page, cache-line aligned; writing one never changes its live "
           "neighbours; ordinary hooks leave the %u boundary slots of an arena to the 8-byte patch until "
           "the other %u are gone; reuse only after every fresh slot, oldest first\n",
           CODE_ARENA_SLOTS_PER_PAGE, CODE_ARENA_SLOT, CODE_ARENA_PAGES, CODE_ARENA_SLOTS - CODE_ARENA_PAGES);
    free_everything(&m);
    free(a), free(b), free(c), free(narrow), free(narrow2), free(narrow3);
}

static void failures(void)
{
    char err[192];
    reset_kernel();
    static code_arenas m;
    code_arenas_init(&m, &OPS);
    uintptr_t rx = 0;
    void *rw = NULL;

    K.fail_create = 1;
    CHECK(code_arena_alloc(&m, target(0), 0, &rx, &rw, err, sizeof err) == CODE_ARENA_E_KERNEL);
    CHECK(strstr(err, "svcCreateCodeMemory") != NULL && strstr(err, "0xdead01") != NULL);
    CHECK(m.stats.last_failure == RESULT_INJECTED);
    CHECK(K.live_objects == 0 && K.live_reservations == 0 && K.live_backings == 0 && m.count == 0);

    K.fail_map_rx = 1;
    CHECK(code_arena_alloc(&m, target(0), 0, &rx, &rw, err, sizeof err) == CODE_ARENA_E_KERNEL);
    CHECK(strstr(err, "MapSlave") != NULL);
    CHECK(K.live_objects == 0 && K.live_reservations == 0 && K.live_backings == 0 && m.count == 0);

    K.no_space_near = 1;
    CHECK(code_arena_alloc(&m, target(0), 0, &rx, &rw, err, sizeof err) == CODE_ARENA_E_NO_RANGE);
    CHECK(K.live_objects == 0 && K.live_reservations == 0 && K.live_backings == 0);
    K.no_space_near = 0;

    /* A kernel that answers with space beyond ADRP's reach: refused, the reservation given back. */
    K.near_out_of_reach = 1;
    CHECK(code_arena_alloc(&m, target(0), 0, &rx, &rw, err, sizeof err) == CODE_ARENA_E_NO_RANGE);
    CHECK(K.live_objects == 0 && K.live_reservations == 0 && K.live_backings == 0);
    K.near_out_of_reach = 0;

    /* A target far beyond 4 GiB of every arena, with no space near it: refused cleanly, and the
     * existing arena is untouched. */
    mod_slots *some = new_mod();
    CHECK(install(&m, some, 0, 3, 0x11) == 0);
    const uint32_t before = K.live_objects;
    const uintptr_t far = target(0) ^ ((uintptr_t)1 << 45);
    K.no_space_near = 1;
    CHECK(code_arena_alloc(&m, far, 0, &rx, &rw, err, sizeof err) == CODE_ARENA_E_NO_RANGE);
    CHECK(code_arena_alloc(&m, far, CODE_ARENA_PAGE_ALIGNED, &rx, &rw, err, sizeof err) == CODE_ARENA_E_NO_RANGE);
    K.no_space_near = 0;
    CHECK(K.live_objects == before && K.live_reservations == 2 * (int)before && K.live_backings == (int)before);
    CHECK(intact(some));

    /* MapOwner refused when a slot is to be written: the slot stays free, nothing leaks. */
    const uint32_t used = m.stats.slots_used;
    K.fail_map_rw = 1;
    CHECK(code_arena_alloc(&m, target(5), 0, &rx, &rw, err, sizeof err) == CODE_ARENA_E_KERNEL);
    CHECK(strstr(err, "MapOwner") != NULL);
    CHECK(m.stats.slots_used == used && m.writers == 0);
    CHECK(install(&m, some, 5, 1, 0x11) == 0);  /* and the next attempt works */

    /* UnmapOwner refused after a write: reported; the slot is live and executable regardless. */
    CHECK(code_arena_alloc(&m, target(6), 0, &rx, &rw, err, sizeof err) == 0);
    memset(rw, 0x22, 64);
    K.fail_unmap_rw = 1;
    CHECK(code_arena_commit(&m, rx, err, sizeof err) == CODE_ARENA_E_KERNEL);
    CHECK(strstr(err, "UnmapOwner") != NULL);
    CHECK(((const uint8_t *)rx)[0] == 0x22);
    CHECK(rw_views_mapped() == 1);  /* still mapped: the next commit retries the unmap */
    CHECK(code_arena_alloc(&m, target(7), 0, &rx, &rw, err, sizeof err) == 0);
    CHECK(code_arena_commit(&m, rx, err, sizeof err) == 0);
    CHECK(rw_views_mapped() == 0);

    /* Misuse is refused, not followed. */
    CHECK(code_arena_free(&m, (uintptr_t)0x1000, err, sizeof err) == CODE_ARENA_E_INVALID);
    CHECK(code_arena_commit(&m, (uintptr_t)0x1000, err, sizeof err) == CODE_ARENA_E_INVALID);
    CHECK(code_arena_free(&m, some->rx[0], err, sizeof err) == 0);
    CHECK(code_arena_free(&m, some->rx[0], err, sizeof err) == CODE_ARENA_E_INVALID);  /* twice */
    some->rx[0] = some->rx[--some->n];

    uninstall(&m, some);
    free_everything(&m);
    free(some);
    printf("failures     create, MapSlave, MapOwner, UnmapOwner, no space, out of reach, double free: refused "
           "with the Result, nothing leaked\n");
}

static void reuse_order(void)
{
    char err[192];
    reset_kernel();
    static code_arenas m;
    code_arenas_init(&m, &OPS);
    mod_slots *all = new_mod();
    CHECK(install(&m, all, 0, (int)CODE_ARENA_SLOTS, 0x33) == 0);  /* one arena, full */
    CHECK(K.live_objects == 1 && m.stats.reuses == 0);
    /* Free slots 10, 3, 40, and a boundary one, in that order: ordinary requests get 10, 3, 40 back,
     * then the boundary one; nothing in between. */
    const int order[4] = {10, 3, 40, (int)CODE_ARENA_SLOTS - 1};
    CHECK(all->rx[order[3]] % CODE_ARENA_PAGE == 0);  /* boundary slots were handed out last */
    for (int i = 0; i < 4; i++) {
        CHECK(code_arena_free(&m, all->rx[order[i]], err, sizeof err) == 0);
    }
    for (int i = 0; i < 4; i++) {
        uintptr_t rx = 0;
        void *rw = NULL;
        CHECK(code_arena_alloc(&m, target(0), 0, &rx, &rw, err, sizeof err) == 0);
        CHECK(rx == all->rx[order[i]]);
        CHECK(code_arena_commit(&m, rx, err, sizeof err) == 0);
    }
    CHECK(K.live_objects == 1 && m.stats.reuses == 4);
    /* A freed boundary slot goes to a narrow request ahead of fresh ones elsewhere... */
    CHECK(code_arena_free(&m, all->rx[order[3]], err, sizeof err) == 0);
    uintptr_t rx = 0;
    void *rw = NULL;
    CHECK(code_arena_alloc(&m, target(1), CODE_ARENA_PAGE_ALIGNED, &rx, &rw, err, sizeof err) == 0);
    CHECK(rx == all->rx[order[3]]);
    CHECK(code_arena_commit(&m, rx, err, sizeof err) == 0);
    /* ...and one more than fits makes a second arena, a second object. */
    CHECK(code_arena_alloc(&m, target(1), 0, &rx, &rw, err, sizeof err) == 0);
    CHECK(code_arena_commit(&m, rx, err, sizeof err) == 0);
    CHECK(K.live_objects == 2);
    printf("reuse        freed slots come back oldest first, after every fresh one; a full arena makes a "
           "second object\n");
    free_everything(&m);
    free(all);
}

/* A mod whose hooks do not fit. Two mods are running and a mod asks for more than the 8 arenas can
 * give. The allocation that does not fit fails with a message that names the capacity, the loader
 * then drops that mod's partial hooks (hook_remove_mod frees slot by slot, done here the same way),
 * the running mods stay intact, every slot is available again, and neither the failure nor
 * repeating it adds an object. code_arena_usage says beforehand that it would not fit -- what the
 * loader checks before the mod runs any code (svc::hook_room_check), so this path is the last line. */
static void capacity_exhaustion(void)
{
    char err[192];
    reset_kernel();
    static code_arenas m;
    code_arenas_init(&m, &OPS);
    mod_slots *running = new_mod();
    CHECK(install(&m, running, 0, 32, 0xB0) == 0);
    CHECK(K.live_objects == 1);

    enum { ASK = CODE_ARENA_MAX * CODE_ARENA_SLOTS + 8, ROOM = CODE_ARENA_MAX * CODE_ARENA_SLOTS - 32 };
    uint32_t used = 0, capacity = 0;
    code_arena_usage(&m, &used, NULL, &capacity);
    CHECK(used == 32 && capacity - used == ROOM && ASK > capacity - used);  /* the check would refuse it */
    static uintptr_t big[ASK];
    for (int cycle = 0; cycle < 3; cycle++) {
        int got = 0;
        int rv = 0;
        err[0] = '\0';
        for (int i = 0; i < ASK; i++) {
            uintptr_t rx = 0;
            void *rw = NULL;
            rv = code_arena_alloc(&m, target(1000 + i % 500), 0, &rx, &rw, err, sizeof err);
            if (rv != 0) {
                break;
            }
            memset(rw, 0xBB, 64);
            CHECK(code_arena_commit(&m, rx, err, sizeof err) == 0);
            big[got++] = rx;
        }
        /* every slot that exists was handed out, then the next one failed, naming the capacity */
        CHECK(got == ROOM && rv == CODE_ARENA_E_FULL);
        CHECK(strstr(err, "every code arena is full") != NULL && strstr(err, "8 arenas") != NULL);
        CHECK(K.live_objects == CODE_ARENA_MAX && K.peak_objects == CODE_ARENA_MAX && K.peak_objects <= K.limit);
        CHECK(m.stats.slots_used == CODE_ARENA_MAX * CODE_ARENA_SLOTS);
        CHECK(intact(running));  /* the running mods are untouched */
        /* the mod is dropped: its partial hooks go back one by one */
        for (int i = 0; i < got; i++) {
            CHECK(code_arena_free(&m, big[i], err, sizeof err) == 0);
        }
        CHECK(m.stats.slots_used == 32);
        CHECK(intact(running));
        CHECK(K.live_objects == CODE_ARENA_MAX && K.peak_objects == CODE_ARENA_MAX);  /* no growth, in any cycle */
    }
    /* the slots are reusable: a mod that fits goes in, from the arenas that exist */
    mod_slots *fits = new_mod();
    CHECK(install(&m, fits, 2000, 1000, 0xCC) == 0);
    CHECK(intact(fits) && intact(running));
    CHECK(K.live_objects == CODE_ARENA_MAX && m.stats.reuses > 0);
    uninstall(&m, fits);
    uninstall(&m, running);
    CHECK(code_arena_trim(&m) == 0);
    CHECK(K.live_objects == 0 && K.live_reservations == 0 && K.live_backings == 0);
    printf("capacity     a mod asking for more than 8 arenas (%u hooks) fails with 'every code arena is full'; its "
           "partial slots go back, the running mods stay intact, objects stay at %u over 3 cycles\n",
           (unsigned)(CODE_ARENA_MAX * CODE_ARENA_SLOTS), (unsigned)CODE_ARENA_MAX);
    free(running), free(fits);
}

/* The kernel runs out before the table does: the fake's pool is smaller than CODE_ARENA_MAX here
 * (other users of CodeMemory in the system). The failure names the kernel's Result, and the
 * arenas made before are intact. */
static void kernel_runs_out_first(void)
{
    char err[192];
    reset_kernel();
    K.limit = 1;
    static code_arenas m;
    code_arenas_init(&m, &OPS);
    mod_slots *one = new_mod();
    CHECK(install(&m, one, 0, (int)CODE_ARENA_SLOTS, 0x44) == 0);
    uintptr_t rx = 0;
    void *rw = NULL;
    CHECK(code_arena_alloc(&m, target(0), 0, &rx, &rw, err, sizeof err) == CODE_ARENA_E_KERNEL);
    CHECK(strstr(err, "svcCreateCodeMemory") != NULL && strstr(err, "0xce01") != NULL);
    CHECK(intact(one) && K.live_objects == 1 && K.live_reservations == 2 && K.live_backings == 1);
    printf("kernel       a kernel with room for one object: the 1025th hook fails with 0xce01, the 1024 before "
           "stay intact\n");
    free_everything(&m);
    free(one);
}

#ifdef CODE_ARENA_TEST_FUNCHOOK
void run_funchook_part(void);
#endif

/* V1-rc9: the release at exit, with hooks still installed (a mod turned off keeps its slots until the game
 * ends): every arena's views unmapped and its CodeMemory object closed, so its backing is ordinary heap again
 * when the program returns to hbloader. */
static void release_at_exit(void)
{
    reset_kernel();
    static code_arenas m;
    code_arenas_init(&m, &OPS);
    mod_slots *twilit = new_mod(), *albt = new_mod();
    CHECK(install(&m, twilit, 0, 343, 0x71) == 0);
    CHECK(install(&m, albt, 400, 900, 0xAB) == 0);
    const uint32_t objects = K.live_objects;
    CHECK(objects == 2);
    uint32_t released = 0, last = 0;
    CHECK(code_arenas_release(&m, &released, &last) == 0);
    CHECK(released == objects && K.live_objects == 0 && (uint32_t)K.unmap_rx_calls == objects && last == 0);
    CHECK(m.count == 0 && rw_views_mapped() == 0);
    /* nothing left to do a second time */
    released = 0;
    CHECK(code_arenas_release(&m, &released, &last) == 0 && released == 0);
    printf("exit release %u CodeMemory object(s) unmapped and closed with 1243 hooks still installed\n", objects);
    free(twilit), free(albt);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    control_kernel_limit();
    catalogue_together();
    packing();
    failures();
    reuse_order();
    capacity_exhaustion();
    kernel_runs_out_first();
    release_at_exit();
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
