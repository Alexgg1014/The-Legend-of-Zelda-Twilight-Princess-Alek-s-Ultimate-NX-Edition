/* aleks2: writing to the game's own code on Horizon, without ever making it
 * unexecutable.
 *
 * This file is part of Funchook and is therefore GPL-2.0, with funchook's
 * linking exception. It is the Switch counterpart of
 * src/dusk/mods/loader/code_patch_macos.cpp, which exists for the same
 * reason: macOS will not let a process make its own code writable either, and
 * upstream already put the hook point in funchook.c for it.
 *
 * Why the obvious way does not work, learned the expensive way:
 *
 * funchook's normal route is unprotect_begin -> memcpy -> unprotect_end,
 * which on this console means svcSetProcessMemoryPermission to Rw and back to
 * Rx. Going to Rw works. Coming back does not. The permission call moves a
 * range from AliasCode to AliasCodeData, and AliasCodeData is not a state it
 * will take back -- the kernel answers 0xd401, InvalidCurrentMemory.
 *
 * The consequence is not a failed hook, it is a dead console: unprotect_end
 * fails after the branch has been written, so four pages of the game's code
 * are left writable and NOT executable, and the process dies the moment
 * execution reaches one of them. That is what the start screen crash was.
 *
 * So this never changes the permission of the code at all. It maps the same
 * physical pages a second time, at a scratch address where they are writable,
 * writes there, and unmaps. The executable mapping is untouched throughout;
 * the bytes appear in it because it is the same memory.
 *
 * If the syscalls for that are not granted, this refuses and the hook fails
 * with a message. Nothing is half-written: the compare-and-write below either
 * completes through the alias or does not start.
 */
#include <string.h>

#include <switch.h>

#include "funchook_internal.h"
#include "own_process_handle.h"

/* Room for the two instructions funchook writes over an arm64 entry, with
 * margin. A patch bigger than this is not something this was designed for. */
#define MAX_PATCH_SIZE 64

/* svc 0x74 and 0x75. Unlike the permission call, these are not part of the
 * JIT set every launcher hints, so they are checked rather than assumed --
 * assuming is what cost the last two builds. */
static int mapping_permitted(void)
{
    static int checked = 0;
    static int permitted = 0;
    if (!checked) {
        checked = 1;
        permitted = envIsSyscallHinted(0x74) && envIsSyscallHinted(0x75) &&
                    aleks2_own_process_handle() != INVALID_HANDLE;  /* DM1-c: IPC fallback off hbloader */
    }
    return permitted;
}

/* Returns 0 on success, or a Horizon Result, or -1 for a bad argument.
 * funchook prints whatever comes back, so the number has to be the real one.
 */
int commit_code_patch(void *target, const void *expected, const void *replacement, size_t size)
{
    if (target == NULL || expected == NULL || replacement == NULL || size == 0 ||
        size > MAX_PATCH_SIZE) {
        return -1;
    }
    if (!mapping_permitted()) {
        return -2;
    }

    const u64 address = (u64)target;
    const u64 page = address & ~(u64)0xFFF;
    const u64 span = ((address + size + 0xFFF) & ~(u64)0xFFF) - page;
    const Handle self = aleks2_own_process_handle();

    /* Somewhere to put the second mapping. Any free address will do -- nothing
     * executes through it -- so libnx's own search is the right tool here,
     * unlike for trampolines, which have to be near what they serve. */
    virtmemLock();
    void *alias = virtmemFindAslr(span, 0x1000);
    VirtmemReservation *reservation = alias != NULL ? virtmemAddReservation(alias, span) : NULL;
    virtmemUnlock();
    if (reservation == NULL) {
        return -3;
    }

    Result rc = svcMapProcessMemory(alias, self, page, span);
    if (R_FAILED(rc)) {
        virtmemLock();
        virtmemRemoveReservation(reservation);
        virtmemUnlock();
        return (int)rc;
    }

    unsigned char *writable = (unsigned char *)alias + (address - page);
    int result = 0;

    /* The same check funchook would do: only replace what we expect to find.
     * If the bytes are not what the caller thinks, something else has already
     * patched this function and writing over it would break both.
     */
    if (memcmp(writable, expected, size) != 0) {
        result = -4;
    } else {
        memcpy(writable, replacement, size);
        /* Push the write out through this mapping, then tell the cores that
         * what they may have already fetched from the executable mapping is
         * stale. Both are needed: they are two addresses for one piece of
         * memory, and the caches do not know that.
         */
        armDCacheFlush(writable, size);
        armICacheInvalidate(target, size);
    }

    const Result unmapped = svcUnmapProcessMemory(alias, self, page, span);
    virtmemLock();
    virtmemRemoveReservation(reservation);
    virtmemUnlock();
    if (R_FAILED(unmapped) && result == 0) {
        result = (int)unmapped;
    }
    return result;
}

/*
 * Funchook is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version, with the linking exception stated in
 * platforms/switch/source/funchook_os_switch.c.
 */
