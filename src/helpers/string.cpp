#include "helpers/string.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstring>

namespace {
void strncpyProxy(char* dst, const char* src, size_t count) {
#if _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    strncpy(dst, src, count);
#if _MSC_VER
#pragma warning(pop)
#endif
}

#ifdef __SWITCH__
// V1-rc7: one line per call site (the first 16), with what did not fit (printable, at most 40 characters).
void report_truncation(const char* what, const void* caller, const char* src, size_t need, size_t have) {
    static const void* s_reported[16] = {};
    for (const void*& slot : s_reported) {
        if (slot == caller) {
            return;
        }
        if (slot == nullptr) {
            slot = caller;
            char shown[41] = {};
            for (size_t i = 0; i < 40 && src[i] != 0; ++i) {
                const unsigned char c = static_cast<unsigned char>(src[i]);
                shown[i] = (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?';
            }
            OSReport("%s from %p: needs %zu bytes, has %zu; truncated \"%s\" (reported once per call site)\n", what,
                caller, need, have, shown);
            return;
        }
    }
}
#endif
}  // namespace

void TextSpan::CrashSpawnEmpty() {
    CRASH("Span is empty!");
}

void SafeStringCopyTruncate(char* buffer, size_t bufSize, const char* src) {
    if (buffer == src) [[unlikely]] {
        CRASH("Cannot copy string to same buffer");
    }

    if (bufSize == 0) [[unlikely]] {
        CRASH("Target buffer cannot be size zero");
    }

    strncpyProxy(buffer, src, bufSize);
    buffer[bufSize - 1] = 0;
}

void SafeStringCopy(char* buffer, size_t bufSize, const char* src) {
    if (bufSize == 0) [[unlikely]] {
        CRASH("Target buffer cannot be size zero");
    }

    if (buffer == src) [[unlikely]] {
        CRASH("Cannot copy string to same buffer");
    }

    const auto srcSize = strlen(src);
    if (srcSize > bufSize - 1) [[unlikely]] {
#ifdef __SWITCH__
        // aleks2 (V1-rc7): truncated and reported (once per call site), not a crash. A tester's console died
        // opening the field map: dMenu_Fmap_c::arrowPosInit copies a stage name into char[8] and got 14
        // characters (the name came from the save or from a mod; GameCube's strcpy would have run over silently).
        report_truncation("SafeStringCopy", __builtin_return_address(0), src, srcSize + 1, bufSize);
        strncpyProxy(buffer, src, bufSize);
        buffer[bufSize - 1] = 0;
        return;
#else
        const auto msg = fmt::format(
            "Destination buffer too small! Need {}, have {}",
            srcSize + 1,
            bufSize);
        CRASH(msg.c_str());
#endif
    }

    strncpyProxy(buffer, src, bufSize);
    buffer[bufSize - 1] = 0;
}

void SafeStringCat(char* buffer, size_t bufSize, const char* src) {
    if (bufSize == 0) [[unlikely]] {
        CRASH("Target buffer cannot be size zero");
    }

    if (buffer == src) [[unlikely]] {
        CRASH("Cannot copy string to same buffer");
    }

    const auto dstSize = strnlen(buffer, bufSize);
    const auto srcSize = strlen(src);
    if (dstSize + srcSize + 1 > bufSize) [[unlikely]] {
#ifdef __SWITCH__
        report_truncation("SafeStringCat", __builtin_return_address(0), src, dstSize + srcSize + 1, bufSize);  // V1-rc7
        const size_t room = bufSize - 1 - std::min(dstSize, bufSize - 1);
        memcpy(buffer + dstSize, src, room);
        buffer[dstSize + room] = 0;
        return;
#else
        CRASH("Destination buffer too small!");
#endif
    }

    memcpy(buffer + dstSize, src, srcSize);
    buffer[dstSize + srcSize] = 0;
}

int SafeStringVPrintf(char* buffer, size_t bufSize, const char* src, std::va_list args) {
    if (bufSize == 0) [[unlikely]] {
        CRASH("Target buffer cannot be size zero");
    }

    return vsnprintf(buffer, bufSize, src, args);
}
