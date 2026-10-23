/* aleks2 (H37): see code_arena.h.
 *
 * This file is part of Funchook's Horizon layer and is therefore GPL-2.0 with funchook's
 * linking exception (see funchook_os_switch.c).
 */
#include "code_arena.h"

#include <stdio.h>
#include <string.h>

#define ARENA_BYTES ((size_t)CODE_ARENA_PAGES * CODE_ARENA_PAGE)

_Static_assert(CODE_ARENA_MAX == 8 && CODE_ARENA_SLOTS == 1024, "the 'arena is full' message names these");
_Static_assert(CODE_ARENA_SLOT % 64 == 0 && CODE_ARENA_PAGE % CODE_ARENA_SLOT == 0, "slots on their own cache lines");

static void say(const code_arenas *m, const char *line)
{
    if (m->ops->log != NULL) {
        m->ops->log(m->ops->ctx, line);
    }
}

static int fail(code_arenas *m, char *err, size_t errlen, int code, uint32_t rc, const char *op,
                const char *what)
{
    if (rc != 0) {
        m->stats.last_failure = rc;
        snprintf(m->stats.last_failure_op, sizeof m->stats.last_failure_op, "%s", op);
    }
    if (err != NULL && errlen != 0) {
        if (rc != 0) {
            snprintf(err, errlen, "%s failed (0x%x, module %u, description %u): %s", op, rc, rc & 0x1FF,
                     (rc >> 9) & 0x1FFF, what);
        } else {
            snprintf(err, errlen, "%s", what);
        }
    }
    char line[192];
    snprintf(line, sizeof line, "code arena: %s%s%s", rc != 0 ? op : "", rc != 0 ? ": " : "",
             err != NULL && errlen != 0 ? err : what);
    say(m, line);
    return code;
}

int code_arena_within_adrp(uintptr_t from, uintptr_t to)
{
    const int64_t from_page = (int64_t)(from & ~(uintptr_t)0xFFF);
    const int64_t to_page = (int64_t)(to & ~(uintptr_t)0xFFF);
    const int64_t pages = (to_page - from_page) / (int64_t)CODE_ARENA_PAGE;
    return pages >= -(int64_t)0x100000 && pages < (int64_t)0x100000;
}

void code_arenas_init(code_arenas *m, const code_arena_ops *ops)
{
    memset(m, 0, sizeof *m);
    m->ops = ops;
}

/* The arena holding the slot that starts at `rx`; NULL if none does (or `rx` is inside a slot). */
static code_arena *arena_of_slot(code_arenas *m, uintptr_t rx, uint32_t *slot)
{
    for (uint32_t i = 0; i < m->count; i++) {
        code_arena *a = &m->arena[i];
        if (rx >= a->rx && rx < a->rx + ARENA_BYTES) {
            if ((rx - a->rx) % CODE_ARENA_SLOT != 0) {
                return NULL;
            }
            *slot = (uint32_t)((rx - a->rx) / CODE_ARENA_SLOT);
            return a;
        }
    }
    return NULL;
}

uintptr_t code_arena_rx_of(const code_arenas *m, const void *rw)
{
    const uintptr_t p = (uintptr_t)rw;
    for (uint32_t i = 0; i < m->count; i++) {
        const code_arena *a = &m->arena[i];
        if (p >= a->rw && p < a->rw + ARENA_BYTES) {
            return a->rx + (p - a->rw);
        }
    }
    return 0;
}

void *code_arena_rw_of(const code_arenas *m, uintptr_t rx)
{
    for (uint32_t i = 0; i < m->count; i++) {
        const code_arena *a = &m->arena[i];
        if (rx >= a->rx && rx < a->rx + ARENA_BYTES) {
            return (void *)(a->rw + (rx - a->rx));
        }
    }
    return NULL;
}

/* The writable view of one arena, while any of its slots is being written. */
static int map_writable(code_arenas *m, code_arena *a, char *err, size_t errlen)
{
    if (a->rw_mapped) {
        return 0;
    }
    const uint32_t rc = m->ops->map_rw(m->ops->ctx, a->handle, a->rw, ARENA_BYTES);
    if (rc != 0) {
        return fail(m, err, errlen, CODE_ARENA_E_KERNEL, rc, "MapOwner",
                    "could not map the writable view of a code arena");
    }
    a->rw_mapped = 1;
    m->stats.rw_maps++;
    return 0;
}

static int unmap_writable_if_idle(code_arenas *m, code_arena *a, char *err, size_t errlen)
{
    if (!a->rw_mapped || a->writing != 0) {
        return 0;
    }
    const uint32_t rc = m->ops->unmap_rw(m->ops->ctx, a->handle, a->rw, ARENA_BYTES);
    if (rc != 0) {
        return fail(m, err, errlen, CODE_ARENA_E_KERNEL, rc, "UnmapOwner",
                    "could not unmap the writable view of a code arena");
    }
    a->rw_mapped = 0;
    return 0;
}

/* One CodeMemory object, its executable view near `target` and an address for its writable
 * view. Everything is undone if any step fails: no handle, reservation or memory is left. */
static int create_arena(code_arenas *m, uintptr_t target, char *err, size_t errlen)
{
    if (m->count >= CODE_ARENA_MAX) {
        return fail(m, err, errlen, CODE_ARENA_E_FULL, 0, "",
                    "every code arena is full (8 arenas of 1024 hook slots)");
    }
    const code_arena_ops *ops = m->ops;
    code_arena *a = &m->arena[m->count];
    memset(a, 0, sizeof *a);

    a->backing = ops->alloc_backing(ops->ctx, ARENA_BYTES);
    if (a->backing == NULL) {
        return fail(m, err, errlen, CODE_ARENA_E_MEMORY, 0, "", "out of memory for a code arena");
    }
    a->rx = ops->reserve_near(ops->ctx, target, ARENA_BYTES, &a->rx_token);
    if (a->rx == 0 || !code_arena_within_adrp(target, a->rx) ||
        !code_arena_within_adrp(target, a->rx + ARENA_BYTES - 1)) {
        if (a->rx != 0) {
            ops->release(ops->ctx, a->rx_token);
        }
        ops->free_backing(ops->ctx, a->backing, ARENA_BYTES);
        return fail(m, err, errlen, CODE_ARENA_E_NO_RANGE, 0, "",
                    "no free code address within branch range (+/-4 GiB) of the hooked function");
    }
    a->rw = ops->reserve_any(ops->ctx, ARENA_BYTES, &a->rw_token);
    if (a->rw == 0) {
        ops->release(ops->ctx, a->rx_token);
        ops->free_backing(ops->ctx, a->backing, ARENA_BYTES);
        return fail(m, err, errlen, CODE_ARENA_E_NO_RANGE, 0, "",
                    "no free address space for a code arena's writable view");
    }
    uint32_t rc = ops->create(ops->ctx, a->backing, ARENA_BYTES, &a->handle);
    if (rc != 0) {
        ops->release(ops->ctx, a->rw_token);
        ops->release(ops->ctx, a->rx_token);
        ops->free_backing(ops->ctx, a->backing, ARENA_BYTES);
        return fail(m, err, errlen, CODE_ARENA_E_KERNEL, rc, "svcCreateCodeMemory",
                    "the kernel would not create a code memory object");
    }
    rc = ops->map_rx(ops->ctx, a->handle, a->rx, ARENA_BYTES);
    if (rc != 0) {
        ops->close(ops->ctx, a->handle);
        ops->release(ops->ctx, a->rw_token);
        ops->release(ops->ctx, a->rx_token);
        ops->free_backing(ops->ctx, a->backing, ARENA_BYTES);
        return fail(m, err, errlen, CODE_ARENA_E_KERNEL, rc, "MapSlave",
                    "could not map a code arena executable");
    }

    m->count++;
    m->stats.arenas = m->count;
    m->stats.arenas_created++;
    if (m->count > m->stats.arenas_peak) {
        m->stats.arenas_peak = m->count;
    }
    m->stats.slots_total += CODE_ARENA_SLOTS;
    char line[192];
    snprintf(line, sizeof line,
             "code arena %u: %u pages / %u hook slots, executable at %#lx, writable view at %#lx (%u CodeMemory "
             "object(s))",
             m->count, CODE_ARENA_PAGES, CODE_ARENA_SLOTS, (unsigned long)a->rx, (unsigned long)a->rw, m->count);
    say(m, line);
    return 0;
}

/* How good a free slot is for a request, lower first: never used before freed ones (a reused slot
 * is then as old as possible), and for an ordinary request the page-boundary slots last, since only
 * they can take the two-instruction patch. Among fresh slots the lowest address; among freed ones
 * the one freed longest ago. */
static uint64_t slot_rank(const code_arena *a, uint32_t arena_index, uint32_t s, unsigned flags)
{
    const int aligned = s % CODE_ARENA_SLOTS_PER_PAGE == 0;
    const uint64_t fresh = a->ever_used[s] ? 1 : 0;
    const uint64_t boundary = (flags & CODE_ARENA_PAGE_ALIGNED) || !aligned ? 0 : 1;
    const uint64_t order = fresh == 0 ? (uint64_t)arena_index * CODE_ARENA_SLOTS + s : a->freed_at[s];
    return (fresh << 62) | (boundary << 61) | order;
}

/* The best free slot for `target` across the arenas held, or -1 in *arena_out if none. */
static int pick_slot(const code_arenas *m, uintptr_t target, unsigned flags, int *arena_out)
{
    int best_arena = -1;
    int best_slot = -1;
    uint64_t best_rank = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        const code_arena *a = &m->arena[i];
        if (a->used == CODE_ARENA_SLOTS) {
            continue;
        }
        const int whole = code_arena_within_adrp(target, a->rx) &&
                          code_arena_within_adrp(target, a->rx + ARENA_BYTES - 1);
        const uint32_t step = (flags & CODE_ARENA_PAGE_ALIGNED) ? CODE_ARENA_SLOTS_PER_PAGE : 1;
        for (uint32_t s = 0; s < CODE_ARENA_SLOTS; s += step) {
            if (a->state[s] != CODE_SLOT_FREE) {
                continue;
            }
            if (!whole && !code_arena_within_adrp(target, a->rx + (uintptr_t)s * CODE_ARENA_SLOT)) {
                continue;
            }
            const uint64_t rank = slot_rank(a, i, s, flags);
            if (best_slot < 0 || rank < best_rank) {
                best_arena = (int)i;
                best_slot = (int)s;
                best_rank = rank;
            }
            if (rank >> 61 == 0) {
                goto found;  /* fresh and not a boundary slot kept back: nothing later ranks lower */
            }
        }
    }
found:
    *arena_out = best_arena;
    return best_slot;
}

int code_arena_alloc(code_arenas *m, uintptr_t target, unsigned flags, uintptr_t *rx, void **rw, char *err,
                     size_t errlen)
{
    if (rx == NULL || rw == NULL || (flags & ~CODE_ARENA_PAGE_ALIGNED) != 0) {
        return fail(m, err, errlen, CODE_ARENA_E_INVALID, 0, "", "invalid argument");
    }
    int index = -1;
    int slot = pick_slot(m, target, flags, &index);
    if (slot < 0) {
        const int rv = create_arena(m, target, err, errlen);
        if (rv != 0) {
            return rv;
        }
        slot = pick_slot(m, target, flags, &index);
        if (slot < 0) {
            return fail(m, err, errlen, CODE_ARENA_E_NO_RANGE, 0, "", "a new code arena has no slot in range");
        }
    }
    code_arena *a = &m->arena[index];
    const int rv = map_writable(m, a, err, errlen);
    if (rv != 0) {
        return rv;  /* the slot stays free; nothing to undo */
    }
    if (a->ever_used[slot]) {
        m->stats.reuses++;
    } else {
        const uint32_t first = (uint32_t)slot - (uint32_t)slot % CODE_ARENA_SLOTS_PER_PAGE;
        int page_was_touched = 0;
        for (uint32_t s = first; s < first + CODE_ARENA_SLOTS_PER_PAGE; s++) {
            page_was_touched |= a->ever_used[s];
        }
        m->stats.pages_touched += !page_was_touched;
    }
    a->state[slot] = CODE_SLOT_WRITING;
    a->ever_used[slot] = 1;
    a->writing++;
    a->used++;
    m->writers++;
    m->stats.allocations++;
    m->stats.slots_used++;
    if (m->stats.slots_used > m->stats.slots_peak) {
        m->stats.slots_peak = m->stats.slots_used;
    }
    if (slot % CODE_ARENA_SLOTS_PER_PAGE == 0) {
        m->stats.aligned_used++;
    }
    *rx = a->rx + (uintptr_t)slot * CODE_ARENA_SLOT;
    *rw = (void *)(a->rw + (uintptr_t)slot * CODE_ARENA_SLOT);
    memset(*rw, 0, CODE_ARENA_SLOT);  /* whatever a previous hook left there */
    return 0;
}

int code_arena_commit(code_arenas *m, uintptr_t rx, char *err, size_t errlen)
{
    uint32_t slot = 0;
    code_arena *a = arena_of_slot(m, rx, &slot);
    if (a == NULL) {
        return fail(m, err, errlen, CODE_ARENA_E_INVALID, 0, "", "not a code arena slot");
    }
    if (a->state[slot] == CODE_SLOT_LIVE) {
        return 0;
    }
    if (a->state[slot] != CODE_SLOT_WRITING) {
        return fail(m, err, errlen, CODE_ARENA_E_INVALID, 0, "", "committing a slot that is not allocated");
    }
    /* The bytes went in through the writable view: out to memory before anyone fetches them. */
    m->ops->dcache_flush(m->ops->ctx, (void *)(a->rw + (uintptr_t)slot * CODE_ARENA_SLOT), CODE_ARENA_SLOT);
    a->state[slot] = CODE_SLOT_LIVE;
    a->writing--;
    m->writers--;
    const int rv = unmap_writable_if_idle(m, a, err, errlen);
    /* Whatever this core or another prefetched through the executable view is stale. */
    m->ops->icache_invalidate(m->ops->ctx, rx, CODE_ARENA_SLOT);
    return rv;
}

int code_arena_begin_write(code_arenas *m, uintptr_t rx, void **rw, char *err, size_t errlen)
{
    uint32_t slot = 0;
    code_arena *a = arena_of_slot(m, rx, &slot);
    if (a == NULL || a->state[slot] == CODE_SLOT_FREE) {
        return fail(m, err, errlen, CODE_ARENA_E_INVALID, 0, "", "not an allocated code arena slot");
    }
    const int rv = map_writable(m, a, err, errlen);
    if (rv != 0) {
        return rv;
    }
    if (a->state[slot] == CODE_SLOT_LIVE) {
        a->state[slot] = CODE_SLOT_WRITING;
        a->writing++;
        m->writers++;
    }
    if (rw != NULL) {
        *rw = (void *)(a->rw + (uintptr_t)slot * CODE_ARENA_SLOT);
    }
    return 0;
}

int code_arena_free(code_arenas *m, uintptr_t rx, char *err, size_t errlen)
{
    uint32_t slot = 0;
    code_arena *a = arena_of_slot(m, rx, &slot);
    if (a == NULL || a->state[slot] == CODE_SLOT_FREE) {
        return fail(m, err, errlen, CODE_ARENA_E_INVALID, 0, "", "freeing a slot that is not allocated");
    }
    if (a->state[slot] == CODE_SLOT_WRITING) {
        a->writing--;
        m->writers--;
    }
    a->state[slot] = CODE_SLOT_FREE;
    a->freed_at[slot] = ++m->clock;
    a->used--;
    m->stats.frees++;
    m->stats.slots_used--;
    if (slot % CODE_ARENA_SLOTS_PER_PAGE == 0) {
        m->stats.aligned_used--;
    }
    return unmap_writable_if_idle(m, a, err, errlen);
}

/* aleks2 (V1-rc9): at exit, every arena's views unmapped and its CodeMemory object closed, so the heap pages
 * backing it are ordinary heap again when the program returns to hbloader. Returns how many steps failed. */
int code_arenas_release(code_arenas *m, uint32_t *released, uint32_t *last_rc)
{
    int failures = 0;
    for (uint32_t i = 0; i < m->count; ++i) {
        code_arena *a = &m->arena[i];
        const code_arena_ops *ops = m->ops;
        uint32_t rc;
        if (a->rw_mapped) {
            rc = ops->unmap_rw(ops->ctx, a->handle, a->rw, ARENA_BYTES);
            if (rc != 0) {
                failures++;
                *last_rc = rc;
            } else {
                a->rw_mapped = 0;
            }
        }
        rc = ops->unmap_rx(ops->ctx, a->handle, a->rx, ARENA_BYTES);
        if (rc != 0) {
            failures++;
            *last_rc = rc;
            continue;
        }
        rc = ops->close(ops->ctx, a->handle);
        if (rc != 0) {
            failures++;
            *last_rc = rc;
            continue;
        }
        (*released)++;
    }
    m->count = 0;
    m->stats.arenas = 0;
    return failures;
}

void code_arena_usage(const code_arenas *m, uint32_t *used, uint32_t *free_now, uint32_t *capacity)
{
    if (used != NULL) {
        *used = m->stats.slots_used;
    }
    if (free_now != NULL) {
        *free_now = m->stats.slots_total - m->stats.slots_used;
    }
    if (capacity != NULL) {
        *capacity = m->stats.slots_total + (CODE_ARENA_MAX - m->count) * CODE_ARENA_SLOTS;
    }
}

uint32_t code_arena_trim(code_arenas *m)
{
    uint32_t kept = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        code_arena *a = &m->arena[i];
        if (a->used != 0) {
            if (kept != i) {
                m->arena[kept] = *a;
            }
            kept++;
            continue;
        }
        const code_arena_ops *ops = m->ops;
        if (a->rw_mapped) {
            ops->unmap_rw(ops->ctx, a->handle, a->rw, ARENA_BYTES);
        }
        ops->unmap_rx(ops->ctx, a->handle, a->rx, ARENA_BYTES);
        ops->close(ops->ctx, a->handle);
        ops->release(ops->ctx, a->rw_token);
        ops->release(ops->ctx, a->rx_token);
        ops->free_backing(ops->ctx, a->backing, ARENA_BYTES);
        m->stats.slots_total -= CODE_ARENA_SLOTS;
        for (uint32_t p = 0; p < CODE_ARENA_PAGES; p++) {
            int touched = 0;
            for (uint32_t s = p * CODE_ARENA_SLOTS_PER_PAGE; s < (p + 1) * CODE_ARENA_SLOTS_PER_PAGE; s++) {
                touched |= a->ever_used[s];
            }
            m->stats.pages_touched -= (uint32_t)touched;
        }
    }
    m->count = kept;
    m->stats.arenas = kept;
    return kept;
}

void code_arena_describe(const code_arenas *m, char *out, size_t outlen)
{
    const code_arena_stats *s = &m->stats;
    uint32_t capacity = 0;
    code_arena_usage(m, NULL, NULL, &capacity);
    int n = snprintf(out, outlen,
                     "code arenas: %u CodeMemory object(s) (peak %u, created %u, at most %u), hook slots %u used / %u "
                     "total / room for %u (peak %u, %u at a page boundary; %u B each, %u a page), %u page(s) "
                     "touched, %u KiB, %llu allocated, %llu freed, %llu reused, writable view mapped %llu time(s)",
                     s->arenas, s->arenas_peak, s->arenas_created, CODE_ARENA_MAX, s->slots_used, s->slots_total,
                     capacity, s->slots_peak, s->aligned_used, CODE_ARENA_SLOT, CODE_ARENA_SLOTS_PER_PAGE,
                     s->pages_touched, (unsigned)(s->arenas * (ARENA_BYTES / 1024)),
                     (unsigned long long)s->allocations, (unsigned long long)s->frees,
                     (unsigned long long)s->reuses, (unsigned long long)s->rw_maps);
    if (s->last_failure != 0 && n > 0 && (size_t)n < outlen) {
        snprintf(out + n, outlen - (size_t)n, "; last failure %s 0x%x", s->last_failure_op, s->last_failure);
    }
}
