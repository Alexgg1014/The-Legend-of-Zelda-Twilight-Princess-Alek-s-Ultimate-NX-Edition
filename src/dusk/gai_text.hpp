#pragma once
// aleks2 (DM3): getaddrinfo's error codes as text, without the C library's gai_strerror.
//
// libnx's gai_strerror (resolver.o) formats into a 128-byte thread-local buffer. Linking it grows the game's static
// TLS, which is part of every libnx thread's stack allocation: DM3-c1's first build hung at boot in Ryujinx exactly
// as DM2D-c1 did with a 112-byte thread_local (WORK_STATE, DM2-d). Static strings, no TLS. Native EAI_* values.
#include <netdb.h>

namespace dusk {

inline const char* gai_text(int code) {
    switch (code) {
    case 0: return "Success";
#ifdef EAI_AGAIN
    case EAI_AGAIN: return "Temporary failure in name resolution";
#endif
#ifdef EAI_BADFLAGS
    case EAI_BADFLAGS: return "Bad value for ai_flags";
#endif
#ifdef EAI_FAIL
    case EAI_FAIL: return "Non-recoverable failure in name resolution";
#endif
#ifdef EAI_FAMILY
    case EAI_FAMILY: return "ai_family not supported";
#endif
#ifdef EAI_MEMORY
    case EAI_MEMORY: return "Memory allocation failure";
#endif
#ifdef EAI_NONAME
    case EAI_NONAME: return "Name or service not known";
#endif
#ifdef EAI_SERVICE
    case EAI_SERVICE: return "Servname not supported for ai_socktype";
#endif
#ifdef EAI_SOCKTYPE
    case EAI_SOCKTYPE: return "ai_socktype not supported";
#endif
#ifdef EAI_SYSTEM
    case EAI_SYSTEM: return "System error";
#endif
#ifdef EAI_OVERFLOW
    case EAI_OVERFLOW: return "Argument buffer overflow";
#endif
    default: return "Unknown name resolution error";
    }
}

}  // namespace dusk
