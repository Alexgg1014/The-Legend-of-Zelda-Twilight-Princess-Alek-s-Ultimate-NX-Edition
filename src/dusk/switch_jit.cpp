// aleks2: the executable-memory probe. See switch_jit.h for why it exists and
// why it is split in two.
#ifdef __SWITCH__
#include "dusk/switch_jit.h"

#include <switch.h>

#include <cstdio>

#include "dusk/logging.h"

namespace dusk::switchjit {
namespace {

// One page is enough to answer the question, and small enough that failing to
// get it says something real rather than "the console is out of memory".
constexpr std::size_t kProbeSize = 0x1000;

// The self-test's body, hand-assembled. Four A64 instructions:
//
//   movz w1, #0x5A5A, lsl #16    0x52AB4B41
//   movk w1, #0x5A5A             0x728B4B41
//   eor  w0, w0, w1              0x4A010000
//   ret                          0xD65F03C0
//
// It takes an argument and transforms it, rather than returning a constant,
// on purpose: a constant could be matched by a page that was never written --
// left-over memory, a mapping that silently aliases something else, an
// optimiser that decided it knew the answer. A value the caller chooses at run
// time and XORs back can only come out right if these exact bytes ran.
constexpr std::uint32_t kSelfTestCode[] = {0x52AB4B41u, 0x728B4B41u, 0x4A010000u, 0xD65F03C0u};
constexpr std::uint32_t kSelfTestMask = 0x5A5A5A5Au;
constexpr std::uint32_t kSelfTestInput = 0x12345678u;

using SelfTestFn = std::uint32_t (*)(std::uint32_t);

Report s_report;
bool s_probed = false;
bool s_patchChecked = false;
char s_summary[96] = "not probed";
char s_patchSummary[128] = "not probed";

const char* backendName(Backend backend) {
    switch (backend) {
    case Backend::CodeMemory: return "CodeMemory";
    case Backend::ProcessMemory: return "SetProcessMemoryPermission";
    default: return "none";
    }
}

void refreshSummary() {
    if (s_report.backend == Backend::None) {
        std::snprintf(s_summary, sizeof s_summary, "unavailable (0x%x)", s_report.availabilityError);
        return;
    }
    switch (s_report.selfTest) {
    case SelfTest::Passed:
        std::snprintf(s_summary, sizeof s_summary, "%s, executes", backendName(s_report.backend));
        break;
    case SelfTest::NotRun:
        std::snprintf(s_summary, sizeof s_summary, "%s, untested", backendName(s_report.backend));
        break;
    case SelfTest::WrongResult:
        std::snprintf(s_summary, sizeof s_summary, "%s, wrong result 0x%x",
            backendName(s_report.backend), s_report.selfTestValue);
        break;
    default:
        std::snprintf(s_summary, sizeof s_summary, "%s, failed (0x%x)", backendName(s_report.backend),
            s_report.selfTestError);
        break;
    }
}

const char* memTypeName(std::uint32_t type) {
    switch (type) {
    case MemType_CodeStatic: return "CodeStatic";
    case MemType_CodeMutable: return "CodeMutable";
    case MemType_ModuleCodeStatic: return "ModuleCodeStatic";
    case MemType_ModuleCodeMutable: return "ModuleCodeMutable";
    default: return "other";
    }
}

void refreshPatchSummary() {
    const SelfPatch& patch = s_report.selfPatch;
    if (!patch.syscallHinted) {
        std::snprintf(s_patchSummary, sizeof s_patchSummary,
            "no: the launcher does not permit svcSetProcessMemoryPermission");
        return;
    }
    if (!patch.haveProcessHandle) {
        std::snprintf(s_patchSummary, sizeof s_patchSummary,
            "no: syscall permitted but we have no handle to our own process");
        return;
    }
    if (!patch.queried) {
        std::snprintf(s_patchSummary, sizeof s_patchSummary,
            "unknown: svcQueryMemory would not answer for our own code");
        return;
    }
    std::snprintf(s_patchSummary, sizeof s_patchSummary, "%s: syscall permitted, code is %s (%.1f MB), perm %u",
        (patch.memoryType == MemType_CodeStatic || patch.memoryType == MemType_ModuleCodeStatic)
            ? "likely"
            : "no",
        memTypeName(patch.memoryType), static_cast<double>(patch.regionSize) / (1024.0 * 1024.0),
        patch.permission);
}

}  // namespace

const Report& probe() {
    if (s_probed) {
        return s_report;
    }
    s_probed = true;

    // Rather than work out which mechanism libnx would choose -- it reads the
    // launcher's syscall hints, and that logic is libnx's business and moves --
    // allocate one and read back the type it settled on. Exact, and it cannot
    // drift from what the next libnx does.
    Jit jit{};
    const Result rc = jitCreate(&jit, kProbeSize);
    if (R_FAILED(rc)) {
        s_report.backend = Backend::None;
        s_report.availabilityError = static_cast<std::uint32_t>(rc);
        refreshSummary();
        return s_report;
    }
    s_report.backend =
        jit.type == JitType_CodeMemory ? Backend::CodeMemory : Backend::ProcessMemory;
    jitClose(&jit);
    refreshSummary();
    return s_report;
}

const Report& run_self_test() {
    probe();
    if (s_report.backend == Backend::None || s_report.selfTest == SelfTest::Passed) {
        return s_report;
    }

    Jit jit{};
    const Result created = jitCreate(&jit, kProbeSize);
    if (R_FAILED(created)) {
        s_report.selfTest = SelfTest::CreateFailed;
        s_report.selfTestError = static_cast<std::uint32_t>(created);
        refreshSummary();
        return s_report;
    }

    const Result writable = jitTransitionToWritable(&jit);
    if (R_FAILED(writable)) {
        s_report.selfTest = SelfTest::TransitionFailed;
        s_report.selfTestError = static_cast<std::uint32_t>(writable);
        jitClose(&jit);
        refreshSummary();
        return s_report;
    }

    auto* code = static_cast<std::uint32_t*>(jitGetRwAddr(&jit));
    for (std::size_t i = 0; i < std::size(kSelfTestCode); ++i) {
        code[i] = kSelfTestCode[i];
    }

    // The two mappings are different addresses under CodeMemory, and the cores
    // do not share a view of them for free: the writes have to reach memory
    // through the data cache, and the instruction cache has to be told that
    // what it may have already fetched from the executable address is stale.
    // libnx does this inside the transition as well; doing it here does not
    // depend on that staying true.
    armDCacheFlush(code, sizeof kSelfTestCode);
    const Result executable = jitTransitionToExecutable(&jit);
    if (R_FAILED(executable)) {
        s_report.selfTest = SelfTest::TransitionFailed;
        s_report.selfTestError = static_cast<std::uint32_t>(executable);
        jitClose(&jit);
        refreshSummary();
        return s_report;
    }
    armICacheInvalidate(jitGetRxAddr(&jit), sizeof kSelfTestCode);

    // If the kernel and libnx disagree about this page, the process dies here.
    // That is the honest outcome of the question and it is why nothing calls
    // this during a boot.
    auto fn = reinterpret_cast<SelfTestFn>(jitGetRxAddr(&jit));
    s_report.selfTestValue = fn(kSelfTestInput);
    s_report.selfTest = s_report.selfTestValue == (kSelfTestInput ^ kSelfTestMask)
                            ? SelfTest::Passed
                            : SelfTest::WrongResult;
    jitClose(&jit);
    refreshSummary();
    return s_report;
}

// ---------------------------------------------------------- self-patching --
//
// Asked without touching anything, and that is the whole design.
//
// It does not attempt the write, and the reason is narrower than it first
// looked. svcSetProcessMemoryPermission takes any page-aligned range -- the
// kernel checks that the range is uniformly code and splits the block around
// it (Atmosphere, KPageTableBase::SetProcessMemoryPermission) -- so patching
// does NOT mean making the whole segment writable. The size reported below is
// the containing region, because that is what svcQueryMemory answers with;
// it is not the unit the permission call works in.
//
// What makes the write unsafe to try from here is simpler: if the kernel
// refuses partway, or another thread is executing in the page during the
// window, the process dies, and the crash does not say which happened. That
// is worth doing deliberately, once, in a place where the answer is readable
// -- not as a diagnostic somebody presses mid-game.
//
// So: two facts that cost nothing. Does the launcher permit syscall 0x73 at
// all, and what does the kernel say our code pages are? Together they decide
// whether the dangerous version is worth designing (it would have to run
// before the other threads start), or whether funchook is off the table and
// the answer is a build-time gateway instead.
const SelfPatch& probe_self_patch() {
    if (s_report.selfPatch.queried || s_patchChecked) {
        return s_report.selfPatch;
    }
    s_patchChecked = true;
    SelfPatch& patch = s_report.selfPatch;

    patch.syscallHinted = envIsSyscallHinted(0x73);
    patch.haveProcessHandle = envGetOwnProcessHandle() != INVALID_HANDLE;

    // Ask about this function's own address: it is in the same segment as
    // everything a mod would want to hook.
    MemoryInfo info{};
    u32 pageInfo = 0;
    const auto here = reinterpret_cast<u64>(&probe_self_patch);
    if (R_SUCCEEDED(svcQueryMemory(&info, &pageInfo, here))) {
        patch.queried = true;
        patch.memoryType = info.type;
        patch.permission = info.perm;
        patch.regionSize = info.size;
    }
    refreshPatchSummary();
    return patch;
}

const char* summary() {
    probe();
    return s_summary;
}

const char* patch_summary() {
    probe_self_patch();
    return s_patchSummary;
}

bool self_patch_plausible() {
    const SelfPatch& patch = probe_self_patch();
    // Both halves, and the kernel has to agree the pages are the kind that can
    // make the transition: MemType_CodeStatic or MemType_ModuleCodeStatic are
    // the two the manual says become mutable.
    return patch.syscallHinted && patch.haveProcessHandle && patch.queried &&
           (patch.memoryType == MemType_CodeStatic || patch.memoryType == MemType_ModuleCodeStatic);
}

void log_status() {
    const Report& report = probe();
    if (report.backend == Backend::None) {
        // Not a warning: nothing in the game needs this today, and on a console
        // that does not grant it there is nothing to fix. It is a finding.
        DuskLog.info("JIT: no executable memory (jitCreate 0x{:x}); native mods stay compiled in",
            report.availabilityError);
        return;
    }
    DuskLog.info("JIT: executable memory available through {} -- run the self-test from Settings > "
                 "Aleks to find out whether it really runs",
        backendName(report.backend));
    const SelfPatch& patch = probe_self_patch();
    DuskLog.info("JIT: patching our own code looks {} (svc 0x73 {}, code {} over {:.1f} MB)",
        self_patch_plausible() ? "possible" : "impossible",
        patch.syscallHinted ? "permitted" : "refused", memTypeName(patch.memoryType),
        static_cast<double>(patch.regionSize) / (1024.0 * 1024.0));
}

}  // namespace dusk::switchjit
#endif
