#include "registry.hpp"
#include "hook.hpp"

#include "dusk/mods/loader/loader.hpp"
#if DUSK_HAS_PREPATCH
#include "dusk/mods/loader/prepatch.hpp"
#endif
#include "dusk/mods/manifest.hpp"
#include "hook_conflicts.hpp"
#include "mods/svc/hook.h"

#if DUSK_CODE_MODS
#include "dusk/logging.h"
#include "dusk/mods/log_buffer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fmt/format.h>
#if DUSK_HAS_FUNCHOOK
#include <funchook.h>
#if defined(__SWITCH__)
#include "dusk/switch_code_arena.h"
#endif
#endif
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <string>
#include <unordered_map>

#include "arg_shield.hpp"

// V1-rc9: perf-lab.ini arg_shield (frame_telemetry.cpp); on unless it says 0.
bool g_duskArgShield = true;
// V1-rc16: perf-lab.ini hook_fast (frame_telemetry.cpp); on unless it says 0. The dispatch's lookup cache and early-outs.
bool g_duskHookFast = true;
// V1-PGO: perf-lab.ini hook_fast = verify. The cache is used, but every hit is checked against the registry; a cached
// slot that differs is logged (the first 16) and the registry's used. Diagnosis for the PGO build's Dawnlight crash.
bool g_duskHookVerify = false;
#include <vector>
#endif

namespace dusk::mods::svc {

// V1-rc6: see hook.hpp. A few dozen slots, searched linearly: there are a handful of mods.
namespace {
constexpr size_t kCpuSlots = 32;
CpuProfileEntry s_cpu[kCpuSlots];
size_t s_cpuCount = 0;
std::thread::id s_cpuThread;

// V1-rc9: one hooked call in kSampleEvery is timed (on the game thread only) and counted kSampleEvery times: the
// rc8 dispatcher read the clock twice per callback on every call, ~258 hooked functions with Twilit Essentials.
constexpr uint32_t kSampleEvery = 64;

uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

struct ScopedModCpu {
    const void* context;
    uint64_t start = 0;
    // V1-rc16: the same sampled time also goes to the hook's slot and to the callback itself (hook statistics: how
    // much of a target's dispatch is its callbacks, and which callback).
    uint64_t* slotSink = nullptr;
    uint64_t* hookSink = nullptr;
    ScopedModCpu(const void* modContext, bool sampled, uint64_t* slotNs = nullptr, uint64_t* hookNs = nullptr)
        : context{sampled ? modContext : nullptr}, slotSink{slotNs}, hookSink{hookNs} {
        if (context != nullptr) {
            start = now_ns();
        }
    }
    ~ScopedModCpu() {
        if (context != nullptr) {
            const uint64_t ns = (now_ns() - start) * kSampleEvery;
            cpu_profile_add(context, ns);
            if (slotSink != nullptr) {
                *slotSink += ns;
            }
            if (hookSink != nullptr) {
                *hookSink += ns;
            }
        }
    }
};
}  // namespace

void cpu_profile_set_thread() {
    s_cpuThread = std::this_thread::get_id();
}

bool on_profiled_thread() {
    return std::this_thread::get_id() == s_cpuThread;
}

void cpu_profile_add(const void* modContext, uint64_t nanoseconds) {
    if (modContext == nullptr || std::this_thread::get_id() != s_cpuThread) {
        return;
    }
    for (size_t i = 0; i < s_cpuCount; ++i) {
        if (s_cpu[i].modContext == modContext) {
            s_cpu[i].nanoseconds += nanoseconds;
            return;
        }
    }
    if (s_cpuCount < kCpuSlots) {
        s_cpu[s_cpuCount++] = {modContext, nanoseconds};
    }
}

std::vector<CpuProfileEntry> cpu_profile_take() {
    std::vector<CpuProfileEntry> out(s_cpu, s_cpu + s_cpuCount);
    s_cpuCount = 0;
    return out;
}

namespace {

#if DUSK_CODE_MODS

struct PreHookFn {
    ModContext* context = nullptr;
    HookPreFn callback = nullptr;
    HookOptions options = HOOK_OPTIONS_INIT;
    uint64_t order = 0;
    uint64_t sampledNs = 0;  // V1-rc16: this callback's time since the last report, sampled and scaled
};

struct VoidHookFn {
    ModContext* context = nullptr;
    HookReplaceFn replaceCallback = nullptr;
    HookPostFn postCallback = nullptr;
    HookOptions options = HOOK_OPTIONS_INIT;
    uint64_t order = 0;
    uint64_t sampledNs = 0;  // V1-rc16: as PreHookFn::sampledNs
};

struct HookSlot {
    std::vector<PreHookFn> pre;
    VoidHookFn replace{};
    std::vector<VoidHookFn> post;
    // V1-rc9: game-thread statistics since the last report (hook_stats_take): every call is counted, one in
    // kSampleEvery is timed -- the whole dispatch and the callbacks in it, scaled.
    uint64_t calls = 0;
    uint64_t dispatchNs = 0;
    uint64_t callbackNs = 0;
    uint32_t preTick = 0;
    uint32_t postTick = 0;
};

// One per mod that requested a hook on a target: its template-generated trampoline and the
// address of its Hook::g_orig, both living in the mod's dylib. Any candidate's trampoline
// is interchangeable (dispatch walks the shared HookSlot), so when the active installer's mod
// unloads, the backend is handed off to a surviving candidate.
struct HookCandidate {
    ModContext* context = nullptr;
    void* trampoline = nullptr;
    void** origStore = nullptr;
    uint64_t order = 0;
};

enum class BackendKind {
    None,
#if DUSK_HAS_FUNCHOOK
    Funchook,
#endif
#if DUSK_HAS_PREPATCH
    Prepatch,
#endif
};

struct InstalledBackend {
    BackendKind kind = BackendKind::None;
#if DUSK_HAS_FUNCHOOK
    funchook_t* handle = nullptr;
#endif
#if DUSK_HAS_PREPATCH
    prepatch::Site prepatchSite{};
#endif
};

struct InstalledHook {
    InstalledBackend backend{};
    void* original = nullptr;
    ModContext* active = nullptr;
    void** activeStore = nullptr;
    std::vector<HookCandidate> candidates;
};

std::unordered_map<uintptr_t, HookSlot> s_registry;
std::unordered_map<uintptr_t, InstalledHook> s_installed;
std::unordered_map<const ModContext*, std::vector<uintptr_t>> s_declaredTargets;
uint64_t s_nextOrder = 0;
HookStats s_lastStats;  // V1-rc9: the last report, for the heartbeat

// V1-rc16: a direct-mapped cache in front of s_registry for the dispatch, game thread only (no other thread reads or
// writes it, so an entry can never be seen half-written). Keyed by the address the trampoline passes, before
// resolve_target; a miss is cached too (slot nullptr). Every change to the registry bumps s_registryGen, which drops
// all entries at once -- including cached misses when a target gains its first callback. unordered_map keeps element
// addresses across rehashing, and an erased slot is never reached again because the generation moved.
struct SlotCacheEntry {
    uintptr_t key = 0;
    struct HookSlot* slot = nullptr;
    uint32_t gen = 0;
};
constexpr size_t kSlotCacheSize = 512;
SlotCacheEntry s_slotCache[kSlotCacheSize];
uint32_t s_registryGen = 1;
void registry_changed() {
    ++s_registryGen;
}

bool declared_target(const ModContext* context, void* fnAddr) {
    if (context == nullptr) {
        return false;
    }
    const auto it = s_declaredTargets.find(context);
    if (it == s_declaredTargets.end()) {
        return false;
    }
    const auto addr = reinterpret_cast<uintptr_t>(fnAddr);
    return std::ranges::find(it->second, addr) != it->second.end();
}

ModResult reject_undeclared(ModContext* context, void* fnAddr) {
    log::write(mod_id_from_context(context), LOG_LEVEL_ERROR,
        "tried to hook undeclared target {:p}; hook targets must be declared with "
        "DEFINE_HOOK/DEFINE_HOOK_SYMBOL",
        fnAddr);
    return MOD_INVALID_ARGUMENT;
}

// aleks2 (H37): what a hooked function is called, for conflict reports: the name a mod's own
// records gave it (DEFINE_HOOK_SYMBOL name, member display name), else its address.
std::string target_label(const ModContext* context, void* fnAddr) {
    if (const auto* mod = mod_from_context(context); mod && mod->native) {
        const auto& parsed = mod->native->parsed;
        for (const auto* record : parsed.hookNames) {
            if (record->resolved == fnAddr) {
                return hook_name_symbol(*record);
            }
        }
        for (const auto* record : parsed.hookMems) {
            if (record->resolved == fnAddr) {
                return hook_mem_display_name(*record);
            }
        }
        for (const auto* record : parsed.hookMemExts) {
            if (record->resolved == fnAddr) {
                return hook_mem_display_name(*record);
            }
        }
    }
    // V1-rc16: a game function hooked by address has no record above; the symbol manifest names it.
    if (const char* name = manifest::name_at(fnAddr)) {
        return name;
    }
    return fmt::format("{:p}", fnAddr);
}

HookOptions normalize_options(const HookOptions* options) {
    if (options == nullptr || options->struct_size < sizeof(HookOptions)) {
        return HOOK_OPTIONS_INIT;
    }
    return *options;
}

void fail_hook_callback(ModContext* context, const char* kind, const std::exception& e) {
    if (auto* mod = mod_from_context(context)) {
        fail_mod(*mod, MOD_ERROR, fmt::format("Exception in {} hook callback: {}", kind, e.what()));
    }
}

void fail_unknown_hook_callback(ModContext* context, const char* kind) {
    if (auto* mod = mod_from_context(context)) {
        fail_mod(*mod, MOD_ERROR, fmt::format("Unknown exception in {} hook callback", kind));
    }
}

template <class T>
void sort_hooks(std::vector<T>& hooks) {
    std::ranges::stable_sort(hooks, [](const T& a, const T& b) {
        if (a.options.priority != b.options.priority) {
            return a.options.priority > b.options.priority;
        }
        return a.order < b.order;
    });
}

bool erase_callbacks(HookSlot& slot, ModContext* context) {
    std::erase_if(slot.pre, [&](const PreHookFn& hook) { return hook.context == context; });
    std::erase_if(slot.post, [&](const VoidHookFn& hook) { return hook.context == context; });
    if (slot.replace.context == context) {
        slot.replace = {};
    }
    return slot.pre.empty() && slot.post.empty() && slot.replace.replaceCallback == nullptr;
}

// Once a hook is installed, funchook has patched the target's entry: its bytes lead to the
// detour, not the function, so canonicalization must stop there.
[[maybe_unused]] bool installed_target(void* addr) {
    return s_installed.contains(reinterpret_cast<uintptr_t>(addr));
}

// Follow E9/FF25 chains to skip MSVC incremental-link and import stubs.
void* resolve_import_thunk(void* addr) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    for (int i = 0; i < 8; ++i) {
        if (installed_target(addr)) {
            break;
        }
        const auto* p = static_cast<const uint8_t*>(addr);
        if (p[0] == 0x48 && p[1] == 0xFF && p[2] == 0x25) {  // lld emits a REX.W prefix
            ++p;
        }
        if (p[0] == 0xFF && p[1] == 0x25) {
            int32_t offset;
            std::memcpy(&offset, p + 2, 4);
            addr = const_cast<void*>(*reinterpret_cast<const void* const*>(p + 6 + offset));
            break;
        }
        if (p[0] == 0xE9) {
            int32_t offset;
            std::memcpy(&offset, p + 1, 4);
            addr = const_cast<uint8_t*>(p) + 5 + offset;
        } else {
            break;
        }
    }
#elif defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
    // Import thunks are `adrp x16; ldr x16, [x16, #off]; br x16` (deref the IAT slot);
    // incremental-link stubs are a plain `b`, or `adrp x16; add x16, x16, #off; br x16`
    // range-extension thunks when the target is out of B range.
    for (int i = 0; i < 8; ++i) {
        if (installed_target(addr)) {
            break;
        }
        const auto* p = static_cast<const uint8_t*>(addr);
        uint32_t insn0, insn1, insn2;
        std::memcpy(&insn0, p, 4);
        if ((insn0 & 0xFC000000u) == 0x14000000u) {  // b imm26
            auto imm26 = static_cast<int32_t>(insn0 << 6) >> 6;
            addr = const_cast<uint8_t*>(p) + static_cast<intptr_t>(imm26) * 4;
            continue;
        }
        if ((insn0 & 0x9F00001Fu) != 0x90000010u) {  // adrp x16, page
            break;
        }
        std::memcpy(&insn1, p + 4, 4);
        std::memcpy(&insn2, p + 8, 4);
        if (insn2 != 0xD61F0200u) {  // br x16
            break;
        }
        auto immhi = static_cast<int64_t>(static_cast<int32_t>(insn0 << 8) >> 13);  // bits 23:5
        auto immlo = static_cast<int64_t>((insn0 >> 29) & 3);
        auto page = (reinterpret_cast<uintptr_t>(p) & ~uintptr_t{0xFFF}) +
                    (static_cast<intptr_t>((immhi << 2) | immlo) << 12);
        if ((insn1 & 0xFFC003FFu) == 0xF9400210u) {  // ldr x16, [x16, #imm12*8]
            auto slot = page + ((insn1 >> 10) & 0xFFF) * 8;
            addr = *reinterpret_cast<void**>(slot);
            break;
        }
        if ((insn1 & 0xFF8003FFu) == 0x91000210u) {  // add x16, x16, #imm12{, lsl #12}
            auto imm = static_cast<uintptr_t>((insn1 >> 10) & 0xFFF);
            addr = reinterpret_cast<void*>(page + (((insn1 >> 22) & 1) != 0 ? imm << 12 : imm));
            continue;
        }
        break;
    }
#endif
    return addr;
}

// Resolve thunks recursively (max of 8 steps) until we find our target.
void* resolve_target(void* addr) {
    for (int i = 0; i < 8; ++i) {
        void* next = resolve_import_thunk(addr);
        if (next == addr) {
            break;
        }
        addr = next;
    }
    return addr;
}

#if DUSK_HAS_FUNCHOOK
#if defined(__SWITCH__)
// aleks2 (H37): the code arenas' lines (one per arena made, failures, summaries) go to this log.
void log_code_arena_line(const char* line) {
    DuskLog.info("HookSystem: {}", line);
}
#endif

funchook_t* install_funchook(void* fnAddr, void* trampoline, void** outOriginal) {
#if defined(__SWITCH__)
    static const bool s_loggerSet = [] {
        funchook_switch_set_logger(log_code_arena_line);
        return true;
    }();
    (void)s_loggerSet;
#endif
    funchook_t* fh = funchook_create();
    if (fh == nullptr) {
        DuskLog.warn("HookSystem: funchook_create failed for {:p}", fnAddr);
        return nullptr;
    }

    void* fn = fnAddr;
    const int prep = funchook_prepare(fh, &fn, trampoline);
    if (prep == 0) {
        *outOriginal = fn;
    }
    const int inst = prep == 0 ? funchook_install(fh, 0) : -1;
    if (prep != 0 || inst != 0) {
        const char* message = funchook_error_message(fh);
        DuskLog.warn("HookSystem: funchook failed for {:p} (prepare={} install={}): {}", fnAddr,
            prep, inst, message != nullptr && message[0] != '\0' ? message : "no details");
#if defined(__SWITCH__)
        char summary[512];
        funchook_switch_describe(summary, sizeof summary);
        DuskLog.warn("HookSystem: {}", summary);
#endif
        funchook_destroy(fh);
        *outOriginal = nullptr;
        return nullptr;
    }
    return fh;
}
#endif

bool install_backend(
    void* fnAddr, void* trampoline, InstalledBackend& outBackend, void** originalStore) {
#if DUSK_HAS_PREPATCH
    if (const auto site = prepatch::lookup(fnAddr)) {
        *originalStore = site->original;
        prepatch::publish(*site, trampoline);
        outBackend.kind = BackendKind::Prepatch;
        outBackend.prepatchSite = *site;
        return true;
    }
#endif

#if DUSK_HAS_FUNCHOOK
    funchook_t* handle = install_funchook(fnAddr, trampoline, originalStore);
    if (handle == nullptr) {
        return false;
    }
    outBackend.kind = BackendKind::Funchook;
    outBackend.handle = handle;
    // aleks2: successes were silent, so a log with no failures in it could
    // mean "all four installed" or "none was ever attempted" -- and on Switch
    // those two look identical and need completely different work.
    // V1-rc5: Debug, not Info: one flushed line per hook cost ~18 ms on the console's card (265 hooks of
    // Twilit Essentials ~5 s of the boot). The per-mod declared/installed count still says what ran.
    DuskLog.debug("HookSystem: installed at {:p}", fnAddr);
    // V1-rc6: a trace every 64, so that a crash while installing shows how far it got.
    static uint32_t s_installedCount = 0;
    if (++s_installedCount % 64 == 0) {
        DuskLog.info("HookSystem: {} hooks installed so far (last at {:p})", s_installedCount, fnAddr);
    }
    return true;
#else
#if DUSK_HAS_PREPATCH
    DuskLog.warn("HookSystem: prepatch backend cannot install target {:p}: {}", fnAddr,
        prepatch::available() ? "target has no valid gateway" : prepatch::unavailable_reason());
#else
    DuskLog.warn("HookSystem: no hook backend can install target {:p}", fnAddr);
#endif
    return false;
#endif
}

bool deactivate_backend(void* target, InstalledBackend& backend) {
#if DUSK_HAS_PREPATCH
    if (backend.kind == BackendKind::Prepatch) {
        prepatch::publish(backend.prepatchSite, nullptr);
        backend = {};
        return true;
    }
#endif
#if DUSK_HAS_FUNCHOOK
    if (backend.kind == BackendKind::Funchook) {
        const int uninst = funchook_uninstall(backend.handle, 0);
        if (uninst != 0) {
            DuskLog.warn("HookSystem: funchook uninstall for {:p} failed: {}", target,
                funchook_error_message(backend.handle));
            return false;
        }
        const int destr = funchook_destroy(backend.handle);
        if (destr != 0) {
            DuskLog.warn("HookSystem: funchook destroy for {:p} returned {}", target, destr);
        }
    }
#else
    (void)target;
#endif
    backend = {};
    return true;
}

bool handoff_backend(
    void* target, InstalledHook& entry, const HookCandidate& candidate, void** outOriginal) {
#if DUSK_HAS_PREPATCH
    if (entry.backend.kind == BackendKind::Prepatch) {
        *candidate.origStore = entry.original;
        prepatch::publish(entry.backend.prepatchSite, candidate.trampoline);
        *outOriginal = entry.original;
        return true;
    }
#endif
#if DUSK_HAS_FUNCHOOK
    InstalledBackend backend;
    if (!install_backend(target, candidate.trampoline, backend, candidate.origStore)) {
        return false;
    }
    entry.backend = backend;
    *outOriginal = *candidate.origStore;
    return true;
#else
    (void)target;
    (void)candidate;
    (void)outOriginal;
    return false;
#endif
}

bool handoff_hook(void* target, InstalledHook& entry) {
#if DUSK_HAS_PREPATCH
    const bool prepatched = entry.backend.kind == BackendKind::Prepatch;
#else
    constexpr bool prepatched = false;
#endif
    if (!prepatched && !deactivate_backend(target, entry.backend)) {
        DuskLog.fatal("HookSystem: cannot hand off a hook that remains installed at {:p}", target);
    }

    entry.active = nullptr;
    entry.activeStore = nullptr;
    for (auto& candidate : entry.candidates) {
        void* original = nullptr;
        if (!handoff_backend(target, entry, candidate, &original)) {
            continue;
        }
        entry.original = original;
        entry.active = candidate.context;
        entry.activeStore = candidate.origStore;
        break;
    }

    if (entry.active == nullptr) {
        DuskLog.warn("HookSystem: no reinstallable trampoline for {:p}; hooks there are "
                     "disabled until a mod reinstalls one",
            target);
        for (auto& candidate : entry.candidates) {
            *candidate.origStore = target;
        }
        deactivate_backend(target, entry.backend);
        return false;
    }

    for (auto& candidate : entry.candidates) {
        *candidate.origStore = entry.original;
    }
    return true;
}

ModResult hook_install(ModContext* context, void* fnAddr, void* trampolineFn, void** outOriginal) {
    if (fnAddr == nullptr || trampolineFn == nullptr || outOriginal == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    // Try to detect an invalid function pointer (possibly a vtable slot offset or Itanium mfp
    // value) and provide a helpful warning instead of faulting
    const auto raw = reinterpret_cast<uintptr_t>(fnAddr);
    if (raw < 0x10000
#if defined(__aarch64__) || defined(_M_ARM64)
        || (raw & 3) != 0  // code is 4-aligned
#endif
    )
    {
        DuskLog.warn("HookSystem: {:p} from {} is not a code address (virtual member function "
                     "pointer? hook via dusk::mods::Hook or resolve())",
            fnAddr, mod_id_from_context(context));
        return MOD_INVALID_ARGUMENT;
    }

    fnAddr = resolve_target(fnAddr);
    if (!declared_target(context, fnAddr)) {
        return reject_undeclared(context, fnAddr);
    }
    const auto key = reinterpret_cast<uintptr_t>(fnAddr);
    if (const auto it = s_installed.find(key); it != s_installed.end()) {
        auto& entry = it->second;
        // hook_add_pre + hook_add_post on the same target share one g_orig per mod.
        const bool known = std::ranges::any_of(entry.candidates, [&](const HookCandidate& cand) {
            return cand.context == context && cand.origStore == outOriginal;
        });
        if (!known) {
            entry.candidates.push_back({context, trampolineFn, outOriginal, s_nextOrder++});
        }
        *outOriginal = entry.original;
        return MOD_OK;
    }

    // Inlining can't be intercepted by an entry patch: warn once per target when this
    // build inlined the function into callers.
    if (const char* name = nullptr; manifest::has_inline_sites(fnAddr, &name)) {
        DuskLog.warn("HookSystem: '{}' ({:p}) for {} was inlined into callers in this build; "
                     "the hook only covers the calls that were not inlined",
            name != nullptr ? name : "?", fnAddr, mod_id_from_context(context));
    }

    InstalledBackend backend;
    if (!install_backend(fnAddr, trampolineFn, backend, outOriginal)) {
        return MOD_ERROR;
    }

    auto& entry = s_installed[key];
    entry.backend = backend;
    entry.original = *outOriginal;
    entry.active = context;
    entry.activeStore = outOriginal;
    entry.candidates.push_back({context, trampolineFn, outOriginal, s_nextOrder++});
    return MOD_OK;
}

ModResult hook_add_pre(
    ModContext* context, void* fnAddr, HookPreFn callback, const HookOptions* options) {
    if (fnAddr == nullptr || context == nullptr || callback == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    fnAddr = resolve_target(fnAddr);
    if (!declared_target(context, fnAddr)) {
        return reject_undeclared(context, fnAddr);
    }
    registry_changed();  // V1-rc16
    auto& hooks = s_registry[reinterpret_cast<uintptr_t>(fnAddr)].pre;
    hooks.push_back({context, callback, normalize_options(options), s_nextOrder++});
    sort_hooks(hooks);
    return MOD_OK;
}

ModResult hook_add_post(
    ModContext* context, void* fnAddr, HookPostFn callback, const HookOptions* options) {
    if (fnAddr == nullptr || context == nullptr || callback == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    fnAddr = resolve_target(fnAddr);
    if (!declared_target(context, fnAddr)) {
        return reject_undeclared(context, fnAddr);
    }
    registry_changed();  // V1-rc16
    auto& hooks = s_registry[reinterpret_cast<uintptr_t>(fnAddr)].post;
    hooks.push_back({context, nullptr, callback, normalize_options(options), s_nextOrder++});
    sort_hooks(hooks);
    return MOD_OK;
}

ModResult hook_replace(
    ModContext* context, void* fnAddr, HookReplaceFn callback, const HookOptions* options) {
    if (fnAddr == nullptr || context == nullptr || callback == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }

    const HookOptions normalized = normalize_options(options);
    fnAddr = resolve_target(fnAddr);
    if (!declared_target(context, fnAddr)) {
        return reject_undeclared(context, fnAddr);
    }
    registry_changed();  // V1-rc16
    auto& slot = s_registry[reinterpret_cast<uintptr_t>(fnAddr)];
    if (slot.replace.replaceCallback == nullptr) {
        slot.replace = {context, callback, nullptr, normalized, s_nextOrder++};
        return MOD_OK;
    }

    // aleks2 (H37): a replace already held. Between two mods this is recorded, never silent
    // (hook_conflicts.hpp), and on Switch V1 one mod may not take a target from another.
    hooks::ReplacePolicy policy;
    switch (normalized.replace_policy) {
    case HOOK_REPLACE_CONFLICT:
        policy = hooks::ReplacePolicy::Conflict;
        break;
    case HOOK_REPLACE_PRIORITY:
        policy = hooks::ReplacePolicy::Priority;
        break;
    case HOOK_REPLACE_OVERRIDE:
        policy = hooks::ReplacePolicy::Override;
        break;
    default:
        return MOD_INVALID_ARGUMENT;
    }
    const auto decision = hooks::decide(target_label(slot.replace.context, fnAddr),
        mod_id_from_context(slot.replace.context), mod_id_from_context(context), policy,
        normalized.priority, slot.replace.options.priority);
    if (decision.record) {
        if (decision.allow) {
            DuskLog.warn("HookSystem: {}", hooks::describe(*decision.record));
        } else {
            DuskLog.error("HookSystem: {}", hooks::describe(*decision.record));
        }
        hooks::record(*decision.record);
    }
    if (!decision.allow) {
        return MOD_CONFLICT;
    }
    slot.replace = {context, callback, nullptr, normalized, s_nextOrder++};
    return MOD_OK;
}

ModResult hook_uninstall(ModContext* context, void* fnAddr, void** originalFnSlot) {
    if (context == nullptr || fnAddr == nullptr || originalFnSlot == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }

    fnAddr = resolve_target(fnAddr);
    if (!declared_target(context, fnAddr)) {
        return reject_undeclared(context, fnAddr);
    }

    const auto key = reinterpret_cast<uintptr_t>(fnAddr);
    const auto installedIt = s_installed.find(key);
    if (installedIt == s_installed.end()) {
        return MOD_INVALID_ARGUMENT;
    }

    auto& entry = installedIt->second;
    const auto candidateIt =
        std::ranges::find_if(entry.candidates, [&](const HookCandidate& candidate) {
            return candidate.context == context && candidate.origStore == originalFnSlot;
        });
    if (candidateIt == entry.candidates.end()) {
        return MOD_INVALID_ARGUMENT;
    }

    const bool removedActive = entry.activeStore == originalFnSlot;
#if DUSK_HAS_FUNCHOOK
    if (removedActive && entry.backend.kind == BackendKind::Funchook &&
        !deactivate_backend(fnAddr, entry.backend)) {
        return MOD_ERROR;
    }
#endif

    registry_changed();  // V1-rc16
    if (const auto registryIt = s_registry.find(key);
        registryIt != s_registry.end() && erase_callbacks(registryIt->second, context))
    {
        s_registry.erase(registryIt);
    }

    entry.candidates.erase(candidateIt);
    *originalFnSlot = nullptr;
    if (!removedActive) {
        return MOD_OK;
    }

    auto* target = reinterpret_cast<void*>(key);
    if (entry.candidates.empty()) {
        deactivate_backend(target, entry.backend);
        s_installed.erase(installedIt);
        return MOD_OK;
    }
    if (!handoff_hook(target, entry)) {
        s_installed.erase(installedIt);
        return MOD_ERROR;
    }
    return MOD_OK;
}

// V1-rc16: the slot a dispatched call belongs to, or nullptr. On the game thread with hook_fast, through the cache.
// (Returning early when there is nothing to do: an idea from Chansey's sa2-nx docs; THIRD_PARTY_NOTICES.)
HookSlot* find_slot(void* fnAddr, bool gameThread) {
    const auto raw = reinterpret_cast<uintptr_t>(fnAddr);
    SlotCacheEntry* entry = nullptr;
    if (gameThread && g_duskHookFast) {
        entry = &s_slotCache[((raw >> 2) ^ (raw >> 11)) & (kSlotCacheSize - 1)];
        if (entry->key == raw && entry->gen == s_registryGen) {
            if (!g_duskHookVerify) {
                return entry->slot;
            }
            const auto fresh = s_registry.find(reinterpret_cast<uintptr_t>(resolve_target(fnAddr)));
            HookSlot* freshSlot = fresh == s_registry.end() ? nullptr : &fresh->second;
            if (freshSlot != entry->slot) {
                static int s_reported = 0;
                if (s_reported++ < 16) {
                    DuskLog.warn("HookSystem: hook_fast verify: cached slot {} for {:p} (index {}, gen {}) but the "
                                 "registry has {}",
                        static_cast<const void*>(entry->slot), fnAddr, entry - s_slotCache, entry->gen,
                        static_cast<const void*>(freshSlot));
                }
                entry->slot = freshSlot;
            }
            return freshSlot;
        }
    }
    const auto it = s_registry.find(reinterpret_cast<uintptr_t>(resolve_target(fnAddr)));
    HookSlot* slot = it == s_registry.end() ? nullptr : &it->second;
    if (entry != nullptr) {
        *entry = {raw, slot, s_registryGen};
    }
    return slot;
}

// V1-rc9: the dispatch of a sampled call, timed whole (lookup, callbacks, bookkeeping), scaled to all calls.
struct SlotTimer {
    HookSlot& slot;
    bool sampled;
    uint64_t start;
    ~SlotTimer() {
        if (sampled) {
            slot.dispatchNs += (now_ns() - start) * kSampleEvery;
        }
    }
};

ModResult hook_dispatch_pre(
    ModContext*, void* fnAddr, void* args, void* retval, int* outSkipOriginal) {
    if (outSkipOriginal != nullptr) {
        *outSkipOriginal = 0;
    }
    if (fnAddr == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }

    const bool gameThread = on_profiled_thread();
    HookSlot* found = find_slot(fnAddr, gameThread);
    if (found == nullptr) {
        return MOD_OK;
    }
    auto& slot = *found;
    if (gameThread) {
        ++slot.calls;
    }
    // V1-rc16: a target with only post callbacks has nothing to do here (no copy of the first argument either).
    if (g_duskHookFast && slot.pre.empty() && slot.replace.replaceCallback == nullptr) {
        return MOD_OK;
    }
    const bool sampled = gameThread && (slot.preTick++ % kSampleEvery) == 0;
    const uint64_t t0 = sampled ? now_ns() : 0;
    const SlotTimer timer{slot, sampled, t0};
    const ArgShield shield{args, g_duskArgShield};  // V1-rc9: args[0] is a copy here (arg_shield.hpp)
    for (auto& hook : slot.pre) {
        if (hook.callback == nullptr) {
            continue;
        }
        HookAction action = HOOK_CONTINUE;
        try {
            const ScopedModCpu cpu{hook.context, sampled, &slot.callbackNs, &hook.sampledNs};
            action = hook.callback(hook.context, args, retval, hook.options.userdata);
        } catch (const std::exception& e) {
            fail_hook_callback(hook.context, "pre", e);
            continue;
        } catch (...) {
            fail_unknown_hook_callback(hook.context, "pre");
            continue;
        }
        if (action == HOOK_SKIP_ORIGINAL) {
            if (outSkipOriginal != nullptr) {
                *outSkipOriginal = 1;
            }
            return MOD_OK;
        }
    }
    if (slot.replace.replaceCallback != nullptr) {
        try {
            const ScopedModCpu cpu{slot.replace.context, sampled, &slot.callbackNs, &slot.replace.sampledNs};
            slot.replace.replaceCallback(
                slot.replace.context, args, retval, slot.replace.options.userdata);
        } catch (const std::exception& e) {
            fail_hook_callback(slot.replace.context, "replace", e);
            return MOD_ERROR;
        } catch (...) {
            fail_unknown_hook_callback(slot.replace.context, "replace");
            return MOD_ERROR;
        }
        if (outSkipOriginal != nullptr) {
            *outSkipOriginal = 1;
        }
    }
    return MOD_OK;
}

// V1-rc9: the hook statistics since the last call, per frame, and resets them.
// V1-rc16: with the callbacks' share (sampled the same way), so dispatch - callbacks is what the dispatcher itself
// costs, and the top target's callbacks one by one (mod and callback address: "mapped at" in the log gives the offset).
HookStats hook_stats_take_impl(uint32_t frames) {
    HookStats out;
    if (frames == 0) {
        frames = 1;
    }
    struct Row {
        uintptr_t key;
        HookSlot* slot;
    };
    std::vector<Row> rows;
    uint64_t calls = 0, dispatchNs = 0, callbackNs = 0;
    for (auto& [key, slot] : s_registry) {
        calls += slot.calls;
        dispatchNs += slot.dispatchNs;
        callbackNs += slot.callbackNs;
        if (slot.calls != 0) {
            rows.push_back({key, &slot});
        }
    }
    std::ranges::sort(rows, std::greater{}, [](const Row& r) { return r.slot->dispatchNs; });
    const double perFrame = 1e6 * static_cast<double>(frames);
    out.callsPerFrame = static_cast<double>(calls) / frames;
    out.dispatchMsPerFrame = static_cast<double>(dispatchNs) / perFrame;
    out.callbacksMsPerFrame = static_cast<double>(callbackNs) / perFrame;
    out.targets = rows.size();
    for (size_t i = 0; i < rows.size() && i < 5; ++i) {
        const HookSlot& slot = *rows[i].slot;
        ModContext* owner = !slot.pre.empty()    ? slot.pre.front().context
                            : !slot.post.empty() ? slot.post.front().context
                                                 : slot.replace.context;
        const size_t callbacks = slot.pre.size() + slot.post.size() + (slot.replace.replaceCallback != nullptr);
        out.top += fmt::format("{}{} {:.0f}/f {:.3f} ms (callbacks {:.3f} ms, {})", out.top.empty() ? "" : ", ",
            target_label(owner, reinterpret_cast<void*>(rows[i].key)), static_cast<double>(slot.calls) / frames,
            static_cast<double>(slot.dispatchNs) / perFrame, static_cast<double>(slot.callbackNs) / perFrame,
            callbacks);
    }
    if (!rows.empty()) {
        struct Cb {
            ModContext* context;
            const void* fn;
            const char* kind;
            uint64_t ns;
        };
        std::vector<Cb> cbs;
        const HookSlot& top = *rows.front().slot;
        for (const auto& h : top.pre) {
            cbs.push_back({h.context, reinterpret_cast<const void*>(h.callback), "pre", h.sampledNs});
        }
        for (const auto& h : top.post) {
            cbs.push_back({h.context, reinterpret_cast<const void*>(h.postCallback), "post", h.sampledNs});
        }
        if (top.replace.replaceCallback != nullptr) {
            cbs.push_back({top.replace.context, reinterpret_cast<const void*>(top.replace.replaceCallback), "replace",
                top.replace.sampledNs});
        }
        std::ranges::sort(cbs, std::greater{}, [](const Cb& c) { return c.ns; });
        for (size_t i = 0; i < cbs.size() && i < 12; ++i) {
            out.topCallbacks += fmt::format("{}{} {} {:p} {:.3f} ms", i == 0 ? "" : ", ",
                mod_id_from_context(cbs[i].context), cbs[i].kind, cbs[i].fn,
                static_cast<double>(cbs[i].ns) / perFrame);
        }
    }
    for (auto& [key, slot] : s_registry) {
        slot.calls = slot.dispatchNs = slot.callbackNs = 0;
        for (auto& h : slot.pre) {
            h.sampledNs = 0;
        }
        for (auto& h : slot.post) {
            h.sampledNs = 0;
        }
        slot.replace.sampledNs = 0;
    }
    s_lastStats = out;
    return out;
}

HookStats hook_stats_last_impl() {
    return s_lastStats;
}

ModResult hook_dispatch_post(ModContext*, void* fnAddr, void* args, void* retval) {
    if (fnAddr == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }

    const bool gameThread = on_profiled_thread();
    HookSlot* found = find_slot(fnAddr, gameThread);
    if (found == nullptr) {
        return MOD_OK;
    }
    auto& slot = *found;
    if (slot.post.empty()) {
        return MOD_OK;  // V1-rc9: most targets have only pre hooks
    }
    const bool sampled = gameThread && (slot.postTick++ % kSampleEvery) == 0;
    const SlotTimer timer{slot, sampled, sampled ? now_ns() : 0};
    const ArgShield shield{args, g_duskArgShield};
    for (auto& hook : slot.post) {
        if (hook.postCallback != nullptr) {
            try {
                const ScopedModCpu cpu{hook.context, sampled, &slot.callbackNs, &hook.sampledNs};
                hook.postCallback(hook.context, args, retval, hook.options.userdata);
            } catch (const std::exception& e) {
                fail_hook_callback(hook.context, "post", e);
            } catch (...) {
                fail_unknown_hook_callback(hook.context, "post");
            }
        }
    }
    return MOD_OK;
}

#if defined(_WIN32)
/* Follow jump stubs, then match the MSVC vcall thunk a virtual mfp points at.
 * Returns the vtable slot's byte offset, or npos when fn is not a vcall thunk. */
size_t vcall_slot_offset(const void*& fn) noexcept {
    constexpr size_t npos = static_cast<size_t>(-1);
#if defined(_M_X64) || defined(__x86_64__)
    const auto* p = static_cast<const uint8_t*>(fn);
    for (int i = 0; i < 8 && p[0] == 0xE9; ++i) {  // incremental-link stubs
        int32_t rel;
        std::memcpy(&rel, p + 1, 4);
        p += 5 + rel;
    }
    fn = p;
    // The vptr load. Unoptimized clang-cl thunks spill/reload rcx first
    // (push rax; mov [rsp], rcx; mov rcx, [rsp]), so scan a short window.
    const uint8_t* q = nullptr;
    for (int i = 0; i <= 12; ++i) {
        if (p[i] == 0x48 && p[i + 1] == 0x8B && p[i + 2] == 0x01) {  // mov rax, [rcx]
            q = p + i + 3;
            break;
        }
    }
    if (q == nullptr) {
        return npos;
    }
    if (q[0] == 0xFF && q[1] == 0x20) {  // jmp [rax]  (MSVC)
        return 0;
    }
    if (q[0] == 0xFF && q[1] == 0x60) {  // jmp [rax + imm8]
        return static_cast<int8_t>(q[2]);
    }
    if (q[0] == 0xFF && q[1] == 0xA0) {  // jmp [rax + imm32]
        int32_t off;
        std::memcpy(&off, q + 2, 4);
        return off;
    }
    // clang-cl: mov rax, [rax + off]; (pop r10;) jmp rax. Requiring the jmp rax
    // distinguishes the thunk from an ordinary getter that begins the same way.
    if (q[0] == 0x48 && q[1] == 0x8B && (q[2] == 0x00 || q[2] == 0x40 || q[2] == 0x80)) {
        size_t off = 0;
        const uint8_t* r = q + 3;
        if (q[2] == 0x40) {
            off = static_cast<int8_t>(q[3]);
            r = q + 4;
        } else if (q[2] == 0x80) {
            int32_t off32;
            std::memcpy(&off32, q + 3, 4);
            off = off32;
            r = q + 7;
        }
        for (int i = 0; i <= 8; ++i) {
            if (r[i] == 0xFF && r[i + 1] == 0xE0) {  // jmp rax (48 REX optional)
                return off;
            }
        }
    }
    return npos;
#elif defined(_M_ARM64) || defined(__aarch64__)
    const auto* p = static_cast<const uint8_t*>(fn);
    uint32_t insn[3];
    for (int i = 0; i < 8; ++i) {  // incremental-link `b` stubs
        std::memcpy(insn, p, 4);
        if ((insn[0] & 0xFC000000u) != 0x14000000u) {
            break;
        }
        const auto imm26 = static_cast<int32_t>(insn[0] << 6) >> 6;
        p += static_cast<intptr_t>(imm26) * 4;
    }
    fn = p;
    std::memcpy(insn, p, 12);
    // ldr Xt, [x0]; ldr Xs, [Xt, #imm12*8]; br Xs
    if ((insn[0] & 0xFFFFFFE0u) != 0xF9400000u) {
        return npos;
    }
    const uint32_t t = insn[0] & 0x1Fu;
    if ((insn[1] & 0xFFC003E0u) != (0xF9400000u | (t << 5))) {
        return npos;
    }
    const uint32_t s = insn[1] & 0x1Fu;
    if (insn[2] != (0xD61F0000u | (s << 5))) {
        return npos;
    }
    return ((insn[1] >> 10) & 0xFFFu) * 8;
#else
    (void)fn;
    return npos;
#endif
}
#endif  // _WIN32

bool resolve_symbol_checked(const char* symbol, bool requireCode, void** out, std::string& why) {
    HookSymbolFlags flags{};
    switch (manifest::resolve(symbol, out, &flags)) {
    case manifest::ResolveStatus::Ok:
        if (requireCode && (flags & HOOK_SYMBOL_CODE) == 0) {
            why = fmt::format("'{}' is not a code symbol", symbol);
            return false;
        }
        return true;
    case manifest::ResolveStatus::Unavailable:
        why = "no symbol manifest for this build";
        return false;
    case manifest::ResolveStatus::NotFound:
        why = fmt::format("symbol '{}' not found", symbol);
        return false;
    case manifest::ResolveStatus::Ambiguous:
        why = fmt::format(
            "'{}' maps to more than one address; use a qualified or mangled name", symbol);
        return false;
    }
    why = "unexpected resolve failure";
    return false;
}

/*
 * Decodes a member hook record's pointer-to-member representation into the target code address.
 * Virtual members prefer their named overrider, then fall back to reading the primary vtable slot.
 */
void* resolve_member_record(const unsigned char* pmf, size_t pmfSize, const char* vtableSymbol,
    const char* displayName, std::string& why) {
    if (pmfSize < sizeof(uintptr_t)) {
        why = "truncated pointer-to-member representation";
        return nullptr;
    }
    uintptr_t words[2]{};
    std::memcpy(words, pmf, std::min(sizeof(words), pmfSize));

#if defined(_WIN32)
    const void* fn = reinterpret_cast<const void*>(words[0]);
    if (fn == nullptr) {
        why = "null pointer-to-member target";
        return nullptr;
    }
    const size_t slot = vcall_slot_offset(fn);
    if (slot == static_cast<size_t>(-1)) {  // not a vcall thunk: direct address
        return const_cast<void*>(fn);
    }

    // A display name resolves the actual overrider rather than an ABI vcall thunk. Besides being
    // more direct, this covers secondary vtables: the MSVC representation has `this` adjustment
    // but does not have the base-path suffix used by the decorated vtable symbol.
    std::string displayWhy;
    void* displayTarget = nullptr;
    if (displayName[0] != '\0' &&
        resolve_symbol_checked(displayName, true, &displayTarget, displayWhy))
    {
        return displayTarget;
    }

    int32_t thisAdjustment = 0;
    if (pmfSize >= sizeof(uintptr_t) + sizeof(thisAdjustment)) {
        std::memcpy(&thisAdjustment, pmf + sizeof(uintptr_t), sizeof(thisAdjustment));
    }
    if (thisAdjustment != 0) {
        why = fmt::format(
            "virtual member requires a {}-byte this adjustment and its overrider did not "
            "resolve by name ({})",
            thisAdjustment, displayWhy);
        return nullptr;
    }
    int32_t firstVirtualField = 0;
    if (pmfSize > MOD_META_HOOK_MEM_CAPACITY && pmfSize >= sizeof(uintptr_t) + 2 * sizeof(int32_t))
    {
        std::memcpy(&firstVirtualField, pmf + sizeof(uintptr_t) + sizeof(int32_t), sizeof(int32_t));
    }
    int32_t secondVirtualField = 0;
    if (pmfSize > MOD_META_HOOK_MEM_CAPACITY && pmfSize >= sizeof(uintptr_t) + 3 * sizeof(int32_t))
    {
        std::memcpy(
            &secondVirtualField, pmf + sizeof(uintptr_t) + 2 * sizeof(int32_t), sizeof(int32_t));
    }
    if (firstVirtualField != 0 || secondVirtualField != 0) {
        why = fmt::format("virtual-base member did not resolve by name ({})", displayWhy);
        return nullptr;
    }
    if (vtableSymbol[0] == '\0') {
        why = fmt::format(
            "did not resolve by name ({}) and the class name is not representable as a vtable symbol",
            displayName[0] != '\0' ? displayWhy : std::string{"no display name"});
        return nullptr;
    }
    void* vtable = nullptr;
    std::string vtableWhy;
    if (!resolve_symbol_checked(vtableSymbol, false, &vtable, vtableWhy)) {
        why = fmt::format("did not resolve by name ({}) nor by vtable ({})",
            displayName[0] != '\0' ? displayWhy : std::string{"no display name"}, vtableWhy);
        return nullptr;
    }
    // ??_7 points at the first slot.
    return *reinterpret_cast<void**>(static_cast<char*>(vtable) + slot);
#else
#if defined(__aarch64__) || defined(__arm__)
    // AAPCS C++ ABI: the virtual flag is bit 0 of the adjustment word (function
    // addresses can't spare their low bit), and ptr holds the slot offset directly.
    const bool isVirtual = (words[1] & 1) != 0;
    const uintptr_t thisAdjust = words[1] >> 1;
    const uintptr_t slotOffset = words[0];
#else
    // Itanium C++ ABI: virtual mfps set bit 0 of ptr; the slot offset is ptr - 1.
    const bool isVirtual = (words[0] & 1) != 0;
    const uintptr_t thisAdjust = words[1];
    const uintptr_t slotOffset = words[0] - 1;
#endif
    if (!isVirtual) {  // non-virtual: the address itself
        return reinterpret_cast<void*>(words[0]);
    }

    std::string displayWhy;
    void* displayTarget = nullptr;
    if (displayName[0] != '\0' &&
        resolve_symbol_checked(displayName, true, &displayTarget, displayWhy))
    {
        return displayTarget;
    }
    if (thisAdjust != 0) {
        // The slot is relative to a secondary vtable whose base path is not encoded in the mfp.
        why = fmt::format(
            "virtual member of a secondary base did not resolve by name ({})", displayWhy);
        return nullptr;
    }
    // aleks2 (H38-c6): both reasons. H38-c5 reported only the second for a member whose function the
    // Switch link had dropped, which read as a limit on nested class names.
    if (vtableSymbol[0] == '\0') {
        why = fmt::format(
            "did not resolve by name ({}) and the class name is not representable as a vtable symbol",
            displayName[0] != '\0' ? displayWhy : std::string{"no display name"});
        return nullptr;
    }
    void* vtable = nullptr;
    std::string vtableWhy;
    if (!resolve_symbol_checked(vtableSymbol, false, &vtable, vtableWhy)) {
        why = fmt::format("did not resolve by name ({}) nor by vtable ({})",
            displayName[0] != '\0' ? displayWhy : std::string{"no display name"}, vtableWhy);
        return nullptr;
    }
    // _ZTV points at the offset-to-top slot; the address point mfps index from is
    // two pointers in (past offset-to-top and the typeinfo pointer).
    void* target =
        *reinterpret_cast<void**>(static_cast<char*>(vtable) + 2 * sizeof(void*) + slotOffset);
    if (target == nullptr) {
        why = "vtable slot is empty";
    }
    return target;
#endif
}

HookCounts hook_counts_impl(const LoadedMod& mod) {
    HookCounts c;
    const ModContext* context = mod.context.get();
    for (const auto& [target, slot] : s_registry) {
        for (const auto& pre : slot.pre) c.callbacks += pre.context == context;
        c.callbacks += slot.replace.context == context && slot.replace.replaceCallback != nullptr;
        for (const auto& post : slot.post) c.callbacks += post.context == context;
    }
    for (const auto& [target, entry] : s_installed) {
        for (const auto& cand : entry.candidates) {
            if (cand.context == context) {
                ++c.installed;
                break;
            }
        }
    }
    if (const auto it = s_declaredTargets.find(context); it != s_declaredTargets.end()) {
        c.declared = it->second.size();
    }
    return c;
}

void hook_remove_mod(LoadedMod& mod) {
    ModContext* context = mod.context.get();
    s_declaredTargets.erase(context);

    registry_changed();  // V1-rc16
    for (auto it = s_registry.begin(); it != s_registry.end();) {
        if (erase_callbacks(it->second, context)) {
            it = s_registry.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = s_installed.begin(); it != s_installed.end();) {
        auto& entry = it->second;
        // The departing mod's g_orig slots are about to be unmapped; drop its candidates before
        // any orig_store rewrites below.
        std::erase_if(entry.candidates,
            [context](const HookCandidate& cand) { return cand.context == context; });
        if (entry.active != context) {
            ++it;
            continue;
        }

        auto* target = reinterpret_cast<void*>(it->first);
        if (entry.candidates.empty()) {
            if (!deactivate_backend(target, entry.backend)) {
                DuskLog.fatal("HookSystem: cannot detach a mod with a live hook at {:p}", target);
            }
            it = s_installed.erase(it);
            continue;
        }

        if (!handoff_hook(target, entry)) {
            it = s_installed.erase(it);
            continue;
        }
        DuskLog.info("HookSystem: replaced trampoline for {:p}: {} -> {}", target,
            mod_id_from_context(context), mod_id_from_context(entry.active));
        ++it;
    }
}

#else  // DUSK_CODE_MODS

HookCounts hook_counts_impl(const LoadedMod&) {
    return {};
}

ModResult hook_install(ModContext*, void*, void*, void**) {
    return MOD_UNSUPPORTED;
}
ModResult hook_add_pre(ModContext*, void*, HookPreFn, const HookOptions*) {
    return MOD_UNSUPPORTED;
}
ModResult hook_add_post(ModContext*, void*, HookPostFn, const HookOptions*) {
    return MOD_UNSUPPORTED;
}
ModResult hook_replace(ModContext*, void*, HookReplaceFn, const HookOptions*) {
    return MOD_UNSUPPORTED;
}
ModResult hook_uninstall(ModContext*, void*, void**) {
    return MOD_UNSUPPORTED;
}
ModResult hook_dispatch_pre(ModContext*, void*, void*, void*, int* outSkipOriginal) {
    if (outSkipOriginal != nullptr) {
        *outSkipOriginal = 0;
    }
    return MOD_UNSUPPORTED;
}
ModResult hook_dispatch_post(ModContext*, void*, void*, void*) {
    return MOD_UNSUPPORTED;
}
HookStats hook_stats_take_impl(uint32_t) {
    return {};
}
HookStats hook_stats_last_impl() {
    return {};
}
void hook_remove_mod(LoadedMod&) {}

#endif  // DUSK_CODE_MODS

// By-name resolution reads the symbol manifest, which is independent of the hook engine.
ModResult hook_resolve(ModContext*, const char* symbol, void** outAddr, HookSymbolFlags* outFlags) {
    if (symbol == nullptr || outAddr == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    switch (manifest::resolve(symbol, outAddr, outFlags)) {
    case manifest::ResolveStatus::Ok:
        return MOD_OK;
    case manifest::ResolveStatus::Unavailable:
        return MOD_UNSUPPORTED;
    case manifest::ResolveStatus::NotFound:
        return MOD_UNAVAILABLE;
    case manifest::ResolveStatus::Ambiguous:
        return MOD_CONFLICT;
    }
    return MOD_ERROR;
}

constexpr HookService s_hookService{
    .header = SERVICE_HEADER(HookService, HOOK_SERVICE_MAJOR, HOOK_SERVICE_MINOR),
    .install = hook_install,
    .add_pre = hook_add_pre,
    .add_post = hook_add_post,
    .replace = hook_replace,
    .dispatch_pre = hook_dispatch_pre,
    .dispatch_post = hook_dispatch_post,
    .resolve = hook_resolve,
    .uninstall = hook_uninstall,
};

}  // namespace

// V1-rc9: the hook statistics (hook.hpp), outside the anonymous namespace the dispatch lives in.
HookStats hook_stats_take(uint32_t frames) {
    return hook_stats_take_impl(frames);
}
HookStats hook_stats_last() {
    return hook_stats_last_impl();
}

HookCounts hook_counts(const LoadedMod& mod) {
    return hook_counts_impl(mod);
}

#if DUSK_CODE_MODS && DUSK_HAS_FUNCHOOK && defined(__SWITCH__) && !defined(ALEKS2_HOOK_ROOM)
#define ALEKS2_HOOK_ROOM 1
#endif
#if defined(ALEKS2_HOOK_ROOM)
// The code arenas' count (funchook_arena_glue.c; hook_room_test.cpp supplies its own).
extern "C" void funchook_switch_usage(uint32_t* used, uint32_t* free_now, uint32_t* capacity);
#endif

HookRoom hook_room() {
    HookRoom room;
#if defined(ALEKS2_HOOK_ROOM)
    room.known = true;
    funchook_switch_usage(&room.used, nullptr, &room.capacity);
#endif
    return room;
}

size_t hook_new_targets(const LoadedMod& mod) {
#if DUSK_CODE_MODS
    const auto it = s_declaredTargets.find(mod.context.get());
    if (it == s_declaredTargets.end()) {
        return 0;
    }
    std::vector<uintptr_t> fresh;
    for (const uintptr_t target : it->second) {
        if (s_installed.contains(target) || std::ranges::find(fresh, target) != fresh.end()) {
            continue;  // already hooked (shared, no new slot) or declared twice
        }
#if DUSK_HAS_PREPATCH
        if (prepatch::lookup(reinterpret_cast<void*>(target))) {
            continue;  // a prepatched gateway, no trampoline
        }
#endif
        fresh.push_back(target);
    }
    return fresh.size();
#else
    (void)mod;
    return 0;
#endif
}

bool hook_room_check(const LoadedMod& mod, std::string& why) {
    const HookRoom room = hook_room();
    const size_t need = hook_new_targets(mod);
    if (!room.known || need == 0) {
        return true;
    }
    const size_t left = room.capacity > room.used ? room.capacity - room.used : 0;
    if (need <= left) {
        DuskLog.info("HookSystem: {} takes {} new hook slot(s); {} of {} in use before it",
            mod.metadata.id, need, room.used, room.capacity);
        return true;
    }
#if DUSK_CODE_MODS
    // Who holds the room: the hooked functions each running mod takes part in.
    std::vector<std::pair<size_t, std::string>> holders;
    for (const auto& [target, entry] : s_installed) {
        for (const auto& candidate : entry.candidates) {
            const auto* holder = mod_from_context(candidate.context);
            if (holder == nullptr) {
                continue;
            }
            auto found = std::ranges::find_if(holders,
                [&](const auto& h) { return h.second == holder->metadata.name; });
            if (found == holders.end()) {
                holders.emplace_back(1, holder->metadata.name);
            } else {
                ++found->first;
            }
        }
    }
    std::ranges::sort(holders, [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string most;
    for (size_t i = 0; i < holders.size() && i < 3; ++i) {
        most += fmt::format("{}{} {}", i == 0 ? "" : ", ", holders[i].second, holders[i].first);
    }
    why = fmt::format("No room for its hooks: it needs {} and {} of {} are left. Using the most: {}. "
                      "Disable one of them and restart.",
        need, left, room.capacity, most.empty() ? "nothing yet" : most);
#else
    why = fmt::format("No room for its hooks: it needs {} and {} of {} are left.", need, left,
        room.capacity);
#endif
    DuskLog.warn("HookSystem: {} refused before it started: {}", mod.metadata.id, why);
    return false;
}

#if DUSK_CODE_MODS
void hook_resolve_mod_records(LoadedMod& mod) {
    auto& declared = s_declaredTargets[mod.context.get()];
    declared.clear();
    if (!mod.native) {
        return;
    }

    const auto resolved = [&](void* target, void** slot) {
        target = resolve_target(target);
        *slot = target;
        declared.push_back(reinterpret_cast<uintptr_t>(target));
    };
    const auto unresolved = [&](const char* what, std::string_view why, void** slot) {
        *slot = nullptr;
        log::write(mod.metadata.id, LOG_LEVEL_WARN,
            "hook target '{}' did not resolve ({}); installing this hook will fail", what, why);
    };

    for (auto* record : mod.native->parsed.hookFns) {
        if (record->target != nullptr) {
            resolved(record->target, &record->resolved);
        } else {
            unresolved("<fn>", "null link-time target", &record->resolved);
        }
    }
    const auto resolveMember = [&](auto* record, const unsigned char* pmf, size_t pmfSize) {
        const char* displayName = hook_mem_display_name(*record);
        std::string why;
        void* target =
            resolve_member_record(pmf, pmfSize, hook_mem_vtable_symbol(*record), displayName, why);
        if (target != nullptr) {
            resolved(target, &record->resolved);
        } else {
            unresolved(displayName, why, &record->resolved);
        }
    };
    for (auto* record : mod.native->parsed.hookMems) {
        resolveMember(record, record->pmf, sizeof(record->pmf));
    }
    for (auto* record : mod.native->parsed.hookMemExts) {
        alignas(std::max_align_t) unsigned char pmf[MOD_META_HOOK_MEM_EXT_CAPACITY]{};
        record->materialize(pmf);
        resolveMember(record, pmf, record->pmf_size);
    }
    for (auto* record : mod.native->parsed.hookNames) {
        const char* name = hook_name_symbol(*record);
        std::string why;
        void* target = nullptr;
        if (resolve_symbol_checked(name, true, &target, why)) {
            resolved(target, &record->resolved);
        } else {
            unresolved(name, why, &record->resolved);
        }
    }
}
#else
void hook_resolve_mod_records(LoadedMod&) {}
#endif  // DUSK_CODE_MODS

constinit const ServiceModule g_hookModule{
    .id = HOOK_SERVICE_ID,
    .majorVersion = HOOK_SERVICE_MAJOR,
    .minorVersion = HOOK_SERVICE_MINOR,
    .service = &s_hookService,
    .modDetached = hook_remove_mod,
};

}  // namespace dusk::mods::svc
