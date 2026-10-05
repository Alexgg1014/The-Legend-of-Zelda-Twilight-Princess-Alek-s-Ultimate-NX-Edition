/* aleks2: funchook's OS layer for Horizon.
 *
 * This file is part of Funchook and is therefore GPL-2.0 like the rest of it,
 * with funchook's own linking exception -- which is what lets the NRO be
 * distributed under its own terms. See the licence note at the end.
 *
 * funchook ships os_unix.c and os_windows.c. Neither works here: Horizon has
 * no mmap, no mprotect, and no /proc/self/maps. What it has instead is a set
 * of privileged syscalls the homebrew launcher may or may not hand us, and
 * the console said it does (Settings > Aleks > Diagnostics).
 *
 * Two jobs, two mechanisms:
 *
 *  - Trampoline pages (funchook_page_*): code_arena.c, a few CodeMemory objects
 *    shared by all hooks, each mapped executable near the game's code for its
 *    whole life and writable elsewhere only while a page is written. H36 made
 *    one CodeMemory object per page and ran the kernel out of them on the
 *    eleventh (0xCE01) -- see "code pages" below.
 *
 *  - The game's own code (the two instructions written over a hooked
 *    function): code_patch_switch.c, which writes through a second mapping of
 *    the same pages and never changes the permission of the game's code.
 *    unprotect_begin/end below refuse, on purpose.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include <switch.h>

#include "funchook_internal.h"
#include "code_arena.h"

const size_t page_size = 0x1000;

/* The syscalls this file needs, beyond the ones every process has.
 *
 *   0x4B CreateCodeMemory    one object per code arena (code_arena.c)
 *   0x4C ControlCodeMemory   its writable and executable views
 *
 * Both, or none of this works. Checked once so that a refusal is reported as what it is rather
 * than as a mysterious failure later. (Patching the game's own code needs 0x74/0x75 too; that is
 * code_patch_switch.c's check.)
 */
static int hooks_permitted(funchook_t *funchook)
{
    static int checked = 0;
    static int permitted = 0;

    if (!checked) {
        checked = 1;
        permitted = envIsSyscallHinted(0x4B) && envIsSyscallHinted(0x4C);
    }
    if (!permitted && funchook != NULL) {
        funchook_set_error_message(funchook,
            "this launcher does not permit code memory (svc 0x4B/0x4C)");
    }
    return permitted;
}

const char *funchook_strerror(int errnum, char *buf, size_t buflen)
{
    /* Result codes, not errno: the caller passes whatever we put in errno
     * below, and every one of those is a Horizon Result. Spelled as the
     * module and description the way every other tool on this console prints
     * them, so a search finds the same number.
     */
    const unsigned int rc = (unsigned int)errnum;
    snprintf(buf, buflen, "Result 0x%x (module %u, description %u)", rc, rc & 0x1FF,
             (rc >> 9) & 0x1FFF);
    return buf;
}

funchook_t *funchook_alloc(void)
{
    /* Plain memory: this holds funchook's own bookkeeping, never code. */
    void *mem = aligned_alloc(page_size, (funchook_size + page_size - 1) & ~(page_size - 1));
    if (mem != NULL) {
        memset(mem, 0, funchook_size);
    }
    return (funchook_t *)mem;
}

int funchook_free(funchook_t *funchook)
{
    free(funchook);
    return 0;
}

/* ------------------------------------------------------------ code pages -- */

/* aleks2 (H37): the pages trampolines live in come from code_arena.c -- a few CodeMemory objects
 * shared by every hook, instead of one per hook page. H36 made one per page, and on hardware the
 * eleventh svcCreateCodeMemory failed with 0xCE01 (Kernel, ResourceExhausted) while Cosmetics was
 * installing its 28 hooks. code_arena.h explains the arenas; what matters here is the address
 * funchook gets.
 *
 * Each arena is mapped twice: executable (R-X) near the hooked code for its whole life, and
 * writable (RW-) elsewhere only while a page is being written. funchook is given the EXECUTABLE
 * address of its page: that is what its `adrp x9, page; br x9` must reach and what the trampoline
 * pointer handed back to the hook must be, and it is always readable, which funchook needs after
 * protecting the page (it reads the entries back to patch and unpatch the targets). The three
 * places funchook writes to a page -- get_page's header, prepare's entry, and the target patch it
 * computes from the entry's address -- are routed through the writable view by
 * cmake/PatchFunchook.cmake, with funchook_switch_writable() and funchook_switch_fix_entry() in
 * funchook_arena_glue.c, which also holds funchook_page_*. This file supplies the kernel.
 *
 * Nothing here ever changes the executable view's permission, so a hook being installed or removed
 * for one mod never makes another mod's running trampoline unexecutable.
 */

static code_arenas s_arenas;
static Mutex s_arena_lock;

static void *op_alloc_backing(void *ctx, size_t size)
{
    (void)ctx;
    void *p = aligned_alloc(page_size, size);
    if (p != NULL) {
        memset(p, 0, size);
    }
    return p;
}

static void op_free_backing(void *ctx, void *backing, size_t size)
{
    (void)ctx;
    (void)size;
    free(backing);
}

static uint32_t op_create(void *ctx, void *backing, size_t size, uint32_t *handle)
{
    (void)ctx;
    Handle h = INVALID_HANDLE;
    const Result rc = svcCreateCodeMemory(&h, backing, size);
    *handle = h;
    return rc;
}

static uint32_t op_close(void *ctx, uint32_t handle)
{
    (void)ctx;
    return svcCloseHandle(handle);
}

static uint32_t op_map_rw(void *ctx, uint32_t handle, uintptr_t addr, size_t size)
{
    (void)ctx;
    return svcControlCodeMemory(handle, CodeMapOperation_MapOwner, (void *)addr, size, Perm_Rw);
}

static uint32_t op_unmap_rw(void *ctx, uint32_t handle, uintptr_t addr, size_t size)
{
    (void)ctx;
    return svcControlCodeMemory(handle, CodeMapOperation_UnmapOwner, (void *)addr, size, Perm_None);
}

static uint32_t op_map_rx(void *ctx, uint32_t handle, uintptr_t addr, size_t size)
{
    (void)ctx;
    return svcControlCodeMemory(handle, CodeMapOperation_MapSlave, (void *)addr, size, Perm_Rx);
}

static uint32_t op_unmap_rx(void *ctx, uint32_t handle, uintptr_t addr, size_t size)
{
    (void)ctx;
    return svcControlCodeMemory(handle, CodeMapOperation_UnmapSlave, (void *)addr, size, Perm_None);
}

/* Free address space in the code region, near a given function.
 *
 * virtmemFindCodeMemory is libnx's answer to this and it is the wrong one here: it looks for
 * somewhere in the code region, not somewhere near anything, and on hardware it handed back
 * addresses 3.8 GB away on one call and further than the branch could reach on the next. What
 * funchook needs is space within ADRP's reach of the function being hooked, so the address space
 * is walked instead, outward from the target, and the first unmapped gap big enough wins.
 * svcQueryMemory reports whole regions, so this is a handful of calls rather than a scan.
 */
static void *find_code_space_near(u64 target, size_t size)
{
    MemoryInfo info;
    u32 page_info;

    /* Forwards first: the code region tends to have room above the NRO. */
    u64 addr = (target + 0xFFF) & ~(u64)0xFFF;
    const u64 high_limit = target + ((u64)1 << 32) - size;
    while (addr < high_limit) {
        if (R_FAILED(svcQueryMemory(&info, &page_info, addr))) {
            break;
        }
        if (info.size == 0) {
            break;
        }
        if (info.type == MemType_Unmapped) {
            const u64 start = (info.addr + 0xFFF) & ~(u64)0xFFF;
            if (start + size <= info.addr + info.size && start < high_limit) {
                return (void *)start;
            }
        }
        addr = info.addr + info.size;
    }

    /* Then backwards, walking region starts downward. */
    const u64 low_limit = target > ((u64)1 << 32) ? target - ((u64)1 << 32) : 0x1000;
    addr = target & ~(u64)0xFFF;
    while (addr > low_limit) {
        if (R_FAILED(svcQueryMemory(&info, &page_info, addr))) {
            break;
        }
        if (info.size == 0 || info.addr == 0) {
            break;
        }
        if (info.type == MemType_Unmapped && info.size >= size) {
            /* The end of the gap, so the arena sits as close to the target as this region allows. */
            const u64 start = (info.addr + info.size - size) & ~(u64)0xFFF;
            if (start >= info.addr && start > low_limit) {
                return (void *)start;
            }
        }
        if (info.addr <= low_limit) {
            break;
        }
        addr = info.addr - 0x1000;
    }
    return NULL;
}

static uintptr_t op_reserve_near(void *ctx, uintptr_t target, size_t size, void **token)
{
    (void)ctx;
    virtmemLock();
    void *addr = find_code_space_near((u64)target, size);
    VirtmemReservation *reservation = addr != NULL ? virtmemAddReservation(addr, size) : NULL;
    virtmemUnlock();
    *token = reservation;
    return reservation != NULL ? (uintptr_t)addr : 0;
}

static uintptr_t op_reserve_any(void *ctx, size_t size, void **token)
{
    (void)ctx;
    /* Nothing executes through the writable view, so anywhere libnx likes will do. */
    virtmemLock();
    void *addr = virtmemFindAslr(size, page_size);
    VirtmemReservation *reservation = addr != NULL ? virtmemAddReservation(addr, size) : NULL;
    virtmemUnlock();
    *token = reservation;
    return reservation != NULL ? (uintptr_t)addr : 0;
}

static void op_release(void *ctx, void *token)
{
    (void)ctx;
    if (token != NULL) {
        virtmemLock();
        virtmemRemoveReservation((VirtmemReservation *)token);
        virtmemUnlock();
    }
}

static void op_dcache_flush(void *ctx, void *addr, size_t size)
{
    (void)ctx;
    armDCacheFlush(addr, size);
}

static void op_icache_invalidate(void *ctx, uintptr_t addr, size_t size)
{
    (void)ctx;
    armICacheInvalidate((void *)addr, size);
}

/* Where arena lines go: the hook service sets this to the game's log (svc/hook.cpp). Arena
 * creation, failures and summaries only -- never a line per frame or per call. */
static void (*s_logger)(const char *line);

void funchook_switch_set_logger(void (*logger)(const char *line))
{
    s_logger = logger;
}

static void op_log(void *ctx, const char *line)
{
    (void)ctx;
    if (s_logger != NULL) {
        s_logger(line);
    }
}

static const code_arena_ops s_ops = {
    .ctx = NULL,
    .alloc_backing = op_alloc_backing,
    .free_backing = op_free_backing,
    .create = op_create,
    .close = op_close,
    .map_rw = op_map_rw,
    .unmap_rw = op_unmap_rw,
    .map_rx = op_map_rx,
    .unmap_rx = op_unmap_rx,
    .reserve_near = op_reserve_near,
    .reserve_any = op_reserve_any,
    .release = op_release,
    .dcache_flush = op_dcache_flush,
    .icache_invalidate = op_icache_invalidate,
    .log = op_log,
};

/* A zero Mutex is an unlocked one, so the lock needs no initialisation of its own. */
code_arenas *funchook_arenas_acquire(void)
{
    static int initialized = 0;
    mutexLock(&s_arena_lock);
    if (!initialized) {
        initialized = 1;
        code_arenas_init(&s_arenas, &s_ops);
    }
    return &s_arenas;
}

void funchook_arenas_release(void)
{
    mutexUnlock(&s_arena_lock);
}

int funchook_arenas_permitted(funchook_t *funchook)
{
    return hooks_permitted(funchook);
}

/* ------------------------------------------------------ the game's code -- */

/* Going Rw moves a range from AliasCode to AliasCodeData, and asking for Rw
 * again from AliasCodeData is a case the kernel treats as unreachable. Two
 * hooks landing in one page would do exactly that, so the ranges currently
 * made writable are tracked and a second request is refused rather than
 * aimed at the kernel.
 */
#define MAX_UNPROTECTED 8

typedef struct {
    size_t addr;
    size_t size;
} unprotected_range_t;

static unprotected_range_t unprotected[MAX_UNPROTECTED];

static int overlaps_unprotected(size_t addr, size_t size)
{
    for (int i = 0; i < MAX_UNPROTECTED; i++) {
        if (unprotected[i].size == 0) {
            continue;
        }
        if (addr < unprotected[i].addr + unprotected[i].size && unprotected[i].addr < addr + size) {
            return 1;
        }
    }
    return 0;
}

/* These two are deliberately refusals now.
 *
 * Their job is to make the game's own code writable and put it back, and the
 * putting back is the part Horizon will not do: svcSetProcessMemoryPermission
 * moves a range from AliasCode to AliasCodeData and refuses to bring it home,
 * so unprotect_end failed AFTER the branch had been written and left four
 * pages of the game writable and not executable. The process died at the
 * start screen, which is the worst possible outcome -- worse than the hook
 * simply not installing.
 *
 * Patching goes through code_patch_switch.c instead, which never touches the
 * permissions of the code at all. cmake/PatchFunchook.cmake routes funchook's
 * two patch sites there, the same way upstream routes them on macOS, so
 * nothing should reach these. If something does, it fails here rather than
 * halfway through.
 */
int funchook_unprotect_begin(funchook_t *funchook, mem_state_t *mstate, void *start, size_t len)
{
    (void)mstate;
    (void)start;
    (void)len;
    funchook_set_error_message(funchook,
        "code on this console is patched through an alias, not by unprotecting it");
    return FUNCHOOK_ERROR_MEMORY_FUNCTION;
}

int funchook_unprotect_end(funchook_t *funchook, const mem_state_t *mstate)
{
    (void)mstate;
    funchook_set_error_message(funchook, "unprotect_end has nothing to undo on this console");
    return FUNCHOOK_ERROR_MEMORY_FUNCTION;
}

void *funchook_resolve_func(funchook_t *funchook, void *func)
{
    /* On Unix this walks a PLT entry to the real function. Nothing here is
     * dynamically linked -- the game and every mod in it are one static
     * executable -- so the address given is already the function.
     */
    (void)funchook;
    return func;
}

/*
 * Funchook is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version.
 *
 * As a special exception, the copyright holders of this library give you
 * permission to link this library with independent modules to produce an
 * executable, regardless of the license terms of these independent modules,
 * and to copy and distribute the resulting executable under terms of your
 * choice, provided that you also meet, for each linked independent module,
 * the terms and conditions of the license of that module.
 *
 * Funchook is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 */
