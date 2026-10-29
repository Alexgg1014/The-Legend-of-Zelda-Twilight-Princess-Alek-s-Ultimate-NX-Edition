#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct ModContext;

namespace dusk::mods {
struct LoadedMod;
}

namespace dusk::mods::svc {

void hook_resolve_mod_records(LoadedMod& mod);
// aleks2 (1.0.3): takes the companion setting for companion_hidden.hpp, once, and logs it. The loader passes it when
// the first mod is activated; later calls change nothing.
void companion_hidden_snapshot(bool companionOn);
// True when `symbol` must be reported as not found to a mod's lookup (and the mod is told why, once).
bool companion_hidden_blocks(::ModContext* context, const char* symbol);

// aleks2 (loader-only lab mode): what a mod holds in the hook system, for its summary line.
struct HookCounts {
    size_t callbacks = 0;  // pre, replace and post callbacks registered by the mod
    size_t installed = 0;  // targets whose trampoline (funchook or prepatch) the mod took part in
    size_t declared = 0;   // targets its metadata declared
};
HookCounts hook_counts(const LoadedMod& mod);

// aleks2 (DM2-a): room for hooks. On Switch every hooked function takes one slot of the code
// arenas (platforms/switch/source/code_arena.h), and the arenas are bounded by the kernel's few
// CodeMemory objects; elsewhere there is no such bound and `known` is false.
struct HookRoom {
    bool known = false;
    uint32_t used = 0;      // slots in use (one per hooked function, shared by the mods hooking it)
    uint32_t capacity = 0;  // every slot the code arenas can hold
};
HookRoom hook_room();
// The functions a mod declares that no hook holds yet: the slots it will take when it starts.
size_t hook_new_targets(const LoadedMod& mod);
// Before a mod runs any code: false, with the reason in `why`, when its new hook targets do not
// fit the room left. It must then not start at all -- never with half of its hooks.
bool hook_room_check(const LoadedMod& mod, std::string& why);

// aleks2 (V1-rc6): game-thread time spent in each mod's code (hook callbacks and mod_update), for the log: with
// nine mods the game thread took ~8 ms more per frame on the console and nothing said whose. Only the thread that
// calls cpu_profile_set_thread is measured.
void cpu_profile_set_thread();
void cpu_profile_add(const void* modContext, uint64_t nanoseconds);
struct CpuProfileEntry {
    const void* modContext;
    uint64_t nanoseconds;
};
// What was measured since the last call, and resets it.
std::vector<CpuProfileEntry> cpu_profile_take();

// aleks2 (V1-rc9): hooked calls on the game thread since the last call, per frame: how many, how long their
// dispatch took (lookup + callbacks + bookkeeping; one call in 64 timed, scaled), and the five costliest targets.
struct HookStats {
    double callsPerFrame = 0;
    double dispatchMsPerFrame = 0;
    double callbacksMsPerFrame = 0;  // V1-rc16: the mods' callbacks inside that dispatch
    size_t targets = 0;
    std::string top;
    std::string topCallbacks;  // V1-rc16: the top target's callbacks, slowest first
};
HookStats hook_stats_take(uint32_t frames);
HookStats hook_stats_last();  // the last report (heartbeat)

}  // namespace dusk::mods::svc
