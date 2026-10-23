// aleks2 (DM1-c): a real handle to our own process, for the calls that refuse the CUR_PROCESS_HANDLE
// pseudo-handle -- svcMapProcessCodeMemory, svcSetProcessMemoryPermission (switch_elf_memory.cpp) and
// svcMapProcessMemory (code_patch_switch.c).
//
// On the console hbloader passes one (envGetOwnProcessHandle), and that is all this returns there: the
// fallback below never runs on hardware started from hbmenu. An emulator that starts the NRO itself
// (Ryujinx, Eden) passes none. Then we do what hbloader does: send the pseudo-handle to ourselves over a
// fresh IPC session as a copied handle, and the kernel hands the receiver a real one. From ak's notes
// (research/RYUJINX_TESTING-ak.md, 3.3).
#include "own_process_handle.h"

#include <switch.h>

static Mutex s_mutex;
static Handle s_own = INVALID_HANDLE;
static bool s_resolved = false;
static bool s_fromIpc = false;

static void receiver(void* arg) {
    Handle session = (Handle)(uintptr_t)arg;
    void* tls = armGetTls();
    hipcMakeRequestInline(tls);
    s32 index = 0;
    if (R_SUCCEEDED(svcReplyAndReceive(&index, &session, 1, INVALID_HANDLE, UINT64_MAX))) {
        HipcParsedRequest request = hipcParseRequest(tls);
        if (request.meta.num_copy_handles == 1) {
            s_own = request.data.copy_handles[0];
        }
    }
    svcCloseHandle(session);
}

static void resolve(void) {
    s_own = envGetOwnProcessHandle();
    if (s_own != INVALID_HANDLE) {
        return;  // hbloader gave it: the console path
    }
    // svcCreateSession, svcSendSyncRequest, svcReplyAndReceive: refused cleanly if not permitted.
    if (!envIsSyscallHinted(0x40) || !envIsSyscallHinted(0x21) || !envIsSyscallHinted(0x43)) {
        return;
    }
    Handle server = INVALID_HANDLE;
    Handle client = INVALID_HANDLE;
    if (R_FAILED(svcCreateSession(&server, &client, 0, 0))) {
        return;
    }
    Thread thread;
    // libnx carves the thread's newlib reent and the game's whole static TLS (__tls_start..__tls_end, 0x530
    // bytes in DM2D-c1) out of this stack: at 0x1000 the receiver had little left, and 112 more bytes of
    // game TLS made it overflow and the boot hang in svcSendSyncRequest (DM2-d, Ryujinx). 32 KB, freed on exit.
    if (R_SUCCEEDED(threadCreate(&thread, receiver, (void*)(uintptr_t)server, NULL, 0x8000, 0x20, -2))) {
        if (R_SUCCEEDED(threadStart(&thread))) {
            hipcMakeRequestInline(armGetTls(), .num_copy_handles = 1).copy_handles[0] = CUR_PROCESS_HANDLE;
            svcSendSyncRequest(client);  // fails once the receiver closes the session: expected
            threadWaitForExit(&thread);
        } else {
            svcCloseHandle(server);
        }
        threadClose(&thread);
    } else {
        svcCloseHandle(server);
    }
    svcCloseHandle(client);
    s_fromIpc = s_own != INVALID_HANDLE;
}

Handle aleks2_own_process_handle(void) {
    mutexLock(&s_mutex);
    if (!s_resolved) {
        s_resolved = true;
        resolve();
    }
    const Handle own = s_own;
    mutexUnlock(&s_mutex);
    return own;
}

bool aleks2_own_process_handle_from_ipc(void) {
    (void)aleks2_own_process_handle();
    return s_fromIpc;
}
