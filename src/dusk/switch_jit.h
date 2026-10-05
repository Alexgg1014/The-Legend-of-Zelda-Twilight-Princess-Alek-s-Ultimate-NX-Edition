#pragma once
// aleks2: can this console give us memory it will agree to execute?
//
// Every remaining way to run other people's code depends on the answer:
// funchook needs somewhere to put a trampoline, and a real `.so` loader needs
// somewhere to put the whole library. Horizon has no mmap(PROT_EXEC) and no
// mprotect; what it has is the JIT mechanism, which libnx wraps as `Jit` and
// which is granted by the launcher rather than asked for by the program.
//
// So this is not a feature. It is a question the hardware has to answer, and
// the answer decides whether native mods are possible at all -- see
// aleks2/HITO7_MODS_NATIVOS.md. It is asked in two halves, deliberately:
//
//   availability -- one Jit allocated and freed, at startup. Cheap, executes
//                   nothing, and tells us which of the two kernel mechanisms
//                   (if either) this launcher handed us.
//   execution    -- four instructions written, transitioned, called. Only on
//                   request, from the Aleks tab, because a page that faults
//                   when executed takes the process with it, and that must
//                   never be something an ordinary boot can do.
#include <cstdint>

namespace dusk::switchjit {

enum class Backend {
    None,           ///< Neither mechanism available: no executable memory here.
    CodeMemory,     ///< [4.0.0+] svcCreateCodeMemory. Separate RW and RX mappings.
    ProcessMemory,  ///< svcSetProcessMemoryPermission on our own process.
};

enum class SelfTest {
    NotRun,
    Passed,
    CreateFailed,      ///< jitCreate refused, after availability said yes.
    TransitionFailed,  ///< Could not move the page to executable.
    WrongResult,       ///< It ran, and returned the wrong number.
};

/// The second question, and a different one: not "can we make new code" but
/// "can we change the code we already have". funchook needs the latter -- it
/// installs a hook by writing a branch over the target function's first
/// instruction -- and CodeMemory does not provide it. The documented route is
/// svcSetProcessMemoryPermission moving our own text from ModuleCodeStatic to
/// ModuleCodeMutable, which is a privileged syscall the launcher may or may
/// not have handed us.
struct SelfPatch {
    bool syscallHinted = false;   ///< The launcher permits svcSetProcessMemoryPermission (0x73).
    bool haveProcessHandle = false;
    bool queried = false;         ///< svcQueryMemory answered for our own text.
    std::uint32_t memoryType = 0; ///< MemType_* of the page holding our code.
    std::uint32_t permission = 0; ///< Its current permissions.
    std::uint64_t regionSize = 0; ///< The containing region, which is what svcQueryMemory
                                  ///< reports -- NOT a unit the permission call works in.
};

struct Report {
    Backend backend = Backend::None;
    std::uint32_t availabilityError = 0;  ///< jitCreate's Result, when backend is None.
    SelfTest selfTest = SelfTest::NotRun;
    std::uint32_t selfTestError = 0;      ///< The failing call's Result, where there was one.
    std::uint32_t selfTestValue = 0;      ///< What the generated function actually returned.
    SelfPatch selfPatch{};
};

/// Availability only. Runs once, at first call; executes nothing.
const Report& probe();

/// Writes four instructions into a JIT page and calls them, then updates the
/// report. Can fault the process if the kernel disagrees with libnx about
/// permissions, which is precisely what it is here to find out.
const Report& run_self_test();

/// One short line, for the Aleks tab.
const char* summary();

/// The same, for the self-patch question.
const char* patch_summary();

/// Whether the two facts together say funchook has a chance. Not a promise:
/// nothing here has actually written to our own code.
bool self_patch_plausible();

/// One line into the session log at startup.
void log_status();

}  // namespace dusk::switchjit
