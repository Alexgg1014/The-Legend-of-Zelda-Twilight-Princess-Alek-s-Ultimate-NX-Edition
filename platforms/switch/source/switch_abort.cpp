// aleks2 (V1-rc5): abort() for the whole program, wrapped at link time (-Wl,--wrap=abort,
// platforms/switch/CMakeLists.txt). newlib's abort ends in __appExit, which unmounts sdmc while the other threads
// still use it: a tester's rc4 died of bad_alloc -> std::abort, and the Dawn worker then crashed in _lseek_r on a
// devoptab entry already gone -- a second, misleading crash report, and no line in the log. Here the call is
// logged with its caller and the process stops with a panic, which Atmosphere turns into a crash report with
// every thread as they were. Mods' abort goes through elf_runtime_policy.cpp's shim, to the same end.
#include <switch.h>

extern "C" {
__attribute__((weak)) void aleks2_note_abort(const void* caller);

[[noreturn]] void __wrap_abort(void) {
    const void* caller = __builtin_return_address(0);
    if (aleks2_note_abort != nullptr) {
        aleks2_note_abort(caller);
    }
    svcBreak(BreakReason_Panic, 0, 0);
    for (;;) {
        svcSleepThread(1'000'000'000);
    }
}
}
