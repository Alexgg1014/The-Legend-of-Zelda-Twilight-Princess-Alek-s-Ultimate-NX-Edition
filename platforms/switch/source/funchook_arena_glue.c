/* aleks2 (H37): funchook's page functions over code_arena.c.
 *
 * This file is part of Funchook's Horizon layer and is therefore GPL-2.0 with funchook's
 * linking exception (see funchook_os_switch.c).
 *
 * Kept apart from funchook_os_switch.c, which holds everything that needs libnx, so the very
 * same code runs in the host test (platforms/switch/tools/test-code-arena.sh): there funchook
 * itself, patched by cmake/PatchFunchook.cmake, hooks real functions through these pages under
 * qemu, against a fake kernel that runs out of CodeMemory objects after ten.
 *
 * The page funchook is given is the EXECUTABLE address: what its `adrp x9, page; br x9` must
 * reach, what the trampoline pointer handed back to the hook must be, and always readable --
 * funchook reads the entries back after protecting the page, to patch and unpatch the targets.
 * Its three writes to a page go through funchook_switch_writable(), and the target patch it
 * computed from the writable address is recomputed for the executable one by
 * funchook_switch_fix_entry().
 *
 * The platform provides funchook_arenas_acquire()/release() (the manager, under a lock) and
 * funchook_arenas_permitted().
 */
#include <stdint.h>
#include <string.h>

#include "funchook_internal.h"
#include "code_arena.h"

code_arenas *funchook_arenas_acquire(void);
void funchook_arenas_release(void);
int funchook_arenas_permitted(funchook_t *funchook);

void *funchook_switch_writable(void *executable)
{
    code_arenas *m = funchook_arenas_acquire();
    void *rw = code_arena_rw_of(m, (uintptr_t)executable);
    funchook_arenas_release();
    return rw != NULL ? rw : executable;
}

void funchook_switch_fix_entry(const funchook_entry_t *executable, funchook_entry_t *writable)
{
    /* Same encoding as funchook's own funchook_write_jump32, aimed at the executable view. */
    const uintptr_t src = (uintptr_t)writable->target_func & ~(uintptr_t)0xFFF;
    const uintptr_t dst = (uintptr_t)executable->transit & ~(uintptr_t)0xFFF;
    const intptr_t imm = (intptr_t)(dst - src);
    const uint32_t immlo = (uint32_t)(imm >> 12) & 0x03;
    const uint32_t immhi = (uint32_t)(imm >> 14) & 0x7FFFFul;
    writable->new_code[0] = 0x90000009 | (immlo << 29) | (immhi << 5); /* adrp x9, transit */
    writable->new_code[1] = 0xd61f0120;                                /* br x9 */
}

void funchook_switch_describe(char *out, size_t outlen)
{
    code_arenas *m = funchook_arenas_acquire();
    code_arena_describe(m, out, outlen);
    funchook_arenas_release();
}

int funchook_page_alloc(funchook_t *funchook, funchook_page_t **page_out, uint8_t *func,
                        ip_displacement_t *disp)
{
    (void)disp;
    if (!funchook_arenas_permitted(funchook)) {
        return FUNCHOOK_ERROR_MEMORY_ALLOCATION;
    }
    uintptr_t rx = 0;
    void *rw = NULL;
    char err[192];
    code_arenas *m = funchook_arenas_acquire();
    const int rv = code_arena_alloc(m, (uintptr_t)func, &rx, &rw, err, sizeof err);
    funchook_arenas_release();
    if (rv != 0) {
        funchook_set_error_message(funchook, "%s", err);
        return FUNCHOOK_ERROR_MEMORY_ALLOCATION;
    }
    *page_out = (funchook_page_t *)rx;
    funchook_log(funchook, "  allocate page %p (writable view %p)\n", (void *)rx, rw);
    return 0;
}

int funchook_page_free(funchook_t *funchook, funchook_page_t *page)
{
    char err[192];
    code_arenas *m = funchook_arenas_acquire();
    const int rv = code_arena_free(m, (uintptr_t)page, err, sizeof err);
    funchook_arenas_release();
    if (rv != 0) {
        funchook_set_error_message(funchook, "%s", err);
        return FUNCHOOK_ERROR_MEMORY_FUNCTION;
    }
    funchook_log(funchook, "  deallocate page %p\n", (void *)page);
    return 0;
}

int funchook_page_protect(funchook_t *funchook, funchook_page_t *page)
{
    char err[192];
    code_arenas *m = funchook_arenas_acquire();
    const int rv = code_arena_commit(m, (uintptr_t)page, err, sizeof err);
    funchook_arenas_release();
    if (rv != 0) {
        funchook_set_error_message(funchook, "%s", err);
        return FUNCHOOK_ERROR_MEMORY_FUNCTION;
    }
    funchook_log(funchook, "  protect page %p\n", (void *)page);
    return 0;
}

int funchook_page_unprotect(funchook_t *funchook, funchook_page_t *page)
{
    char err[192];
    code_arenas *m = funchook_arenas_acquire();
    const int rv = code_arena_begin_write(m, (uintptr_t)page, NULL, err, sizeof err);
    funchook_arenas_release();
    if (rv != 0) {
        funchook_set_error_message(funchook, "%s", err);
        return FUNCHOOK_ERROR_MEMORY_FUNCTION;
    }
    funchook_log(funchook, "  unprotect page %p\n", (void *)page);
    return 0;
}
