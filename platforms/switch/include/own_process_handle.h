#pragma once
// aleks2 (DM1-c): see platforms/switch/source/own_process_handle.c.
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A real handle to this process, or INVALID_HANDLE (0) when none can be had. Resolved once.
uint32_t aleks2_own_process_handle(void);
// True when the handle came from the IPC fallback (no hbloader: an emulator), for logs.
bool aleks2_own_process_handle_from_ipc(void);

#ifdef __cplusplus
}
#endif
