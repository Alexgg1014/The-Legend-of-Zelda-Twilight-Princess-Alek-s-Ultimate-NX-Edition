/* aleks2 (H37): see code_arena.h.
 *
 * This file is part of Funchook's Horizon layer and is therefore GPL-2.0 with funchook's
 * linking exception (see funchook_os_switch.c).
 */
#include "code_arena.h"

#include <stdio.h>
#include <string.h>

#define ARENA_BYTES ((size_t)CODE_ARENA_PAGES * CODE_ARENA_PAGE)

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

static code_arena *arena_of_rx(code_arenas *m, uintptr_t rx, uint32_t *page)
{
    for (uint32_t i = 0; i < m->count; i++) {
        code_arena *a = &m->arena[i];
        if (rx >= a->rx && rx < a->rx + ARENA_BYTES) {
            if (page != NULL) {
                *page = (uint32_t)((rx - a->rx) / CODE_ARENA_PAGE);
            }
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

/* The writable view of one arena, while any of its pages is being written. */
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

static int writing_in(const code_arena *a)
{
    for (uint32_t i = 0; i < CODE_ARENA_PAGES; i++) {
        if (a->state[i] == CODE_SLOT_WRITING) {
            return 1;
        }
    }
    return 0;
}

static int unmap_writable_if_idle(code_arenas *m, code_arena *a, char *err, size_t errlen)
{
    if (!a->rw_mapped || writing_in(a)) {
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
                    "every code arena is full (" "8" " arenas of 64 pages)");
    }
    const code_arena_ops *ops = m->ops;
    code_arena a;
    memset(&a, 0, sizeof a);

    a.backing = ops->alloc_backing(ops->ctx, ARENA_BYTES);
    if (a.backing == NULL) {
        return fail(m, err, errlen, CODE_ARENA_E_MEMORY, 0, "", "out of memory for a code arena");
    }
    a.rx = ops->reserve_near(ops->ctx, target, ARENA_BYTES, &a.rx_token);
    if (a.rx == 0 || !code_arena_within_adrp(target, a.rx) ||
        !code_arena_within_adrp(target, a.rx + ARENA_BYTES - 1)) {
        if (a.rx != 0) {
            ops->release(ops->ctx, a.rx_token);
        }
        ops->free_backing(ops->ctx, a.backing, ARENA_BYTES);
        return fail(m, err, errlen, CODE_ARENA_E_NO_RANGE, 0, "",
                    "no free code address within branch range (+/-4 GiB) of the hooked function");
    }
    a.rw = ops->reserve_any(ops->ctx, ARENA_BYTES, &a.rw_token);
    if (a.rw == 0) {
        ops->release(ops->ctx, a.rx_token);
        ops->free_backing(ops->ctx, a.backing, ARENA_BYTES);
        return fail(m, err, errlen, CODE_ARENA_E_NO_RANGE, 0, "",
                    "no free address space for a code arena's writable view");
    }
    uint32_t rc = ops->create(ops->ctx, a.backing, ARENA_BYTES, &a.handle);
    if (rc != 0) {
        ops->release(ops->ctx, a.rw_token);
        ops->release(ops->ctx, a.rx_token);
        ops->free_backing(ops->ctx, a.backing, ARENA_BYTES);
        return fail(m, err, errlen, CODE_ARENA_E_KERNEL, rc, "svcCreateCodeMemory",
                    "the kernel would not create a code memory object");
    }
    rc = ops->map_rx(ops->ctx, a.handle, a.rx, ARENA_BYTES);
    if (rc != 0) {
        ops->close(ops->ctx, a.handle);
        ops->release(ops->ctx, a.rw_token);
        ops->release(ops->ctx, a.rx_token);
        ops->free_backing(ops->ctx, a.backing, ARENA_BYTES);
        return fail(m, err, errlen, CODE_ARENA_E_KERNEL, rc, "MapSlave",
                    "could not map a code arena executable");
    }

    m->arena[m->count++] = a;
    m->stats.arenas = m->count;
    m->stats.arenas_created++;
    if (m->count > m->stats.arenas_peak) {
        m->stats.arenas_peak = m->count;
    }
    m->stats.pages_total += CODE_ARENA_PAGES;
    char line[160];
    snprintf(line, sizeof line,
             "code arena %u: %u pages, executable at %#lx, writable view at %#lx (%u CodeMemory object(s))",
             m->count, CODE_ARENA_PAGES, (unsigned long)a.rx, (unsigned long)a.rw, m->count);
    say(m, line);
    return 0;
}

/* The page to hand out in arena `a` for `target`: never used first, else the one freed longest
 * ago. -1 if none is free or none is in reach. */
static int pick_page(const code_arena *a, uintptr_t target)
{
    int best = -1;
    for (uint32_t i = 0; i < CODE_ARENA_PAGES; i++) {
        if (a->state[i] != CODE_SLOT_FREE ||
            !code_arena_within_adrp(target, a->rx + (uintptr_t)i * CODE_ARENA_PAGE)) {
            continue;
        }
        if (!a->ever_used[i]) {
            return (int)i;
        }
        if (best < 0 || a->freed_at[i] < a->freed_at[best]) {
            best = (int)i;
        }
    }
    return best;
}

int code_arena_alloc(code_arenas *m, uintptr_t target, uintptr_t *rx, void **rw, char *err, size_t errlen)
{
    if (rx == NULL || rw == NULL) {
        return fail(m, err, errlen, CODE_ARENA_E_INVALID, 0, "", "invalid argument");
    }
    code_arena *a = NULL;
    int page = -1;
    /* Fresh pages anywhere before reused ones, so a reused page is as old as possible. */
    for (int pass = 0; pass < 2 && page < 0; pass++) {
        for (uint32_t i = 0; i < m->count; i++) {
            const int p = pick_page(&m->arena[i], target);
            if (p >= 0 && (pass == 1 || !m->arena[i].ever_used[p])) {
                a = &m->arena[i];
                page = p;
                break;
            }
        }
    }
    if (page < 0) {
        const int rv = create_arena(m, target, err, errlen);
        if (rv != 0) {
            return rv;
        }
        a = &m->arena[m->count - 1];
        page = pick_page(a, target);
        if (page < 0) {
            return fail(m, err, errlen, CODE_ARENA_E_NO_RANGE, 0, "", "a new code arena has no page in range");
        }
    }
    const int rv = map_writable(m, a, err, errlen);
    if (rv != 0) {
        return rv;  /* the page stays free; nothing to undo */
    }
    if (a->ever_used[page]) {
        m->stats.reuses++;
    }
    a->state[page] = CODE_SLOT_WRITING;
    a->ever_used[page] = 1;
    m->writers++;
    m->stats.allocations++;
    m->stats.pages_used++;
    *rx = a->rx + (uintptr_t)page * CODE_ARENA_PAGE;
    *rw = (void *)(a->rw + (uintptr_t)page * CODE_ARENA_PAGE);
    memset(*rw, 0, CODE_ARENA_PAGE);  /* whatever a previous hook left there */
    return 0;
}

int code_arena_commit(code_arenas *m, uintptr_t rx, char *err, size_t errlen)
{
    uint32_t page = 0;
    code_arena *a = arena_of_rx(m, rx, &page);
    if (a == NULL) {
        return fail(m, err, errlen, CODE_ARENA_E_INVALID, 0, "", "not a code arena page");
    }
    if (a->state[page] == CODE_SLOT_LIVE) {
        return 0;
    }
    if (a->state[page] != CODE_SLOT_WRITING) {
        return fail(m, err, errlen, CODE_ARENA_E_INVALID, 0, "", "committing a page that is not allocated");
    }
    /* The bytes went in through the writable view: out to memory before anyone fetches them. */
    m->ops->dcache_flush(m->ops->ctx, (void *)(a->rw + (uintptr_t)page * CODE_ARENA_PAGE), CODE_ARENA_PAGE);
    a->state[page] = CODE_SLOT_LIVE;
    m->writers--;
    const int rv = unmap_writable_if_idle(m, a, err, errlen);
    /* Whatever this core or another prefetched through the executable view is stale. */
    m->ops->icache_invalidate(m->ops->ctx, rx & ~(uintptr_t)0xFFF, CODE_ARENA_PAGE);
    return rv;
}

int code_arena_begin_write(code_arenas *m, uintptr_t rx, void **rw, char *err, size_t errlen)
{
    uint32_t page = 0;
    code_arena *a = arena_of_rx(m, rx, &page);
    if (a == NULL || a->state[page] == CODE_SLOT_FREE) {
        return fail(m, err, errlen, CODE_ARENA_E_INVALID, 0, "", "not an allocated code arena page");
    }
    const int rv = map_writable(m, a, err, errlen);
    if (rv != 0) {
        return rv;
    }
    if (a->state[page] == CODE_SLOT_LIVE) {
        a->state[page] = CODE_SLOT_WRITING;
        m->writers++;
    }
    if (rw != NULL) {
        *rw = (void *)(a->rw + (uintptr_t)page * CODE_ARENA_PAGE);
    }
    return 0;
}

int code_arena_free(code_arenas *m, uintptr_t rx, char *err, size_t errlen)
{
    uint32_t page = 0;
    code_arena *a = arena_of_rx(m, rx, &page);
    if (a == NULL || a->state[page] == CODE_SLOT_FREE) {
        return fail(m, err, errlen, CODE_ARENA_E_INVALID, 0, "", "freeing a page that is not allocated");
    }
    if (a->state[page] == CODE_SLOT_WRITING) {
        m->writers--;
    }
    a->state[page] = CODE_SLOT_FREE;
    a->freed_at[page] = ++m->clock;
    m->stats.frees++;
    m->stats.pages_used--;
    return unmap_writable_if_idle(m, a, err, errlen);
}

uint32_t code_arena_trim(code_arenas *m)
{
    uint32_t kept = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        code_arena *a = &m->arena[i];
        int busy = 0;
        for (uint32_t p = 0; p < CODE_ARENA_PAGES; p++) {
            busy |= a->state[p] != CODE_SLOT_FREE;
        }
        if (busy) {
            m->arena[kept++] = *a;
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
        m->stats.pages_total -= CODE_ARENA_PAGES;
    }
    m->count = kept;
    m->stats.arenas = kept;
    return kept;
}

void code_arena_describe(const code_arenas *m, char *out, size_t outlen)
{
    const code_arena_stats *s = &m->stats;
    int n = snprintf(out, outlen,
                     "code arenas: %u CodeMemory object(s) (peak %u, created %u), pages %u total / %u used / %u "
                     "free, %llu allocated, %llu freed, %llu reused, writable view mapped %llu time(s)",
                     s->arenas, s->arenas_peak, s->arenas_created, s->pages_total, s->pages_used,
                     s->pages_total - s->pages_used, (unsigned long long)s->allocations,
                     (unsigned long long)s->frees, (unsigned long long)s->reuses,
                     (unsigned long long)s->rw_maps);
    if (s->last_failure != 0 && n > 0 && (size_t)n < outlen) {
        snprintf(out + n, outlen - (size_t)n, "; last failure %s 0x%x", s->last_failure_op, s->last_failure);
    }
}
