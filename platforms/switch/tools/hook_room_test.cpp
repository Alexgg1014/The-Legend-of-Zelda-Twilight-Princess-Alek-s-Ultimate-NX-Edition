// DM2-a: the loader's check that a mod's hooks fit before it runs any code (svc::hook_room_check in
// src/dusk/mods/svc/hook.cpp, compiled unchanged). The code arenas' count, funchook_switch_usage, is
// supplied here; hooks that are already held are installed through a fake prepatch backend (no
// funchook on the host), so the hook service's own tables say who holds what.
//
// Checked:
//   * the slots a mod will take are its declared targets that no hook holds yet: a target another
//     mod already hooks takes none, and one declared twice (two names, one address) takes one;
//   * a mod that fits starts; one that does not is refused with how many it needs, how many are
//     left, and the mods using the most -- before anything of it is installed;
//   * a mod without hooks always fits; the arenas' totals are passed through unchanged.
#include "dusk/mods/svc/hook.hpp"
#include "dusk/mods/svc/hook_conflicts.hpp"
#include "dusk/mods/svc/registry.hpp"
#include "dusk/mods/loader/loader.hpp"
#include "dusk/mods/loader/native_module.hpp"
#include "dusk/mods/loader/prepatch.hpp"
#include "dusk/mods/log_buffer.hpp"
#include "dusk/mods/manifest.hpp"

#include <borealis/log.hpp>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace dusk::mods;

static int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

// --- what hook.cpp takes from the rest of the game, here ---------------------------------------

static std::vector<std::string> g_log;
static std::map<std::string, void*> g_symbols;
static uint32_t g_used = 0, g_capacity = 0;

extern "C" void funchook_switch_usage(uint32_t* used, uint32_t* free_now, uint32_t* capacity) {
    if (used != nullptr) *used = g_used;
    if (free_now != nullptr) *free_now = 0;
    if (capacity != nullptr) *capacity = g_capacity;
}

namespace dusk::mods {
LoadedMod* mod_from_context(ModContext* context) {
    return context != nullptr ? context->mod : nullptr;
}
const LoadedMod* mod_from_context(const ModContext* context) {
    return context != nullptr ? context->mod : nullptr;
}
const char* mod_id_from_context(ModContext* context) {
    return context != nullptr && context->mod != nullptr ? context->mod->metadata.id.c_str() : "?";
}
void fail_mod(LoadedMod& mod, ModResult, std::string_view message) {
    mod.loadFailed = true;
    mod.failureReason = message;
}
namespace log {
void emit(Source, const std::string& modId, LogLevel, const std::string& message) {
    g_log.push_back(modId + ": " + message);
}
}  // namespace log
namespace manifest {
bool has_inline_sites(const void*, const char**) {
    return false;
}
const char* name_at(const void*) {  // V1-rc16 (hook statistics' labels)
    return nullptr;
}
ResolveStatus resolve(const char* name, void** outAddr, HookSymbolFlags* outFlags) {
    const auto it = g_symbols.find(name);
    if (it == g_symbols.end()) {
        return ResolveStatus::NotFound;
    }
    *outAddr = it->second;
    if (outFlags != nullptr) {
        *outFlags = HOOK_SYMBOL_CODE;
    }
    return ResolveStatus::Ok;
}
}  // namespace manifest
// t0..t2 are "prepatched": that is how this test gets hooks installed with no funchook. A prepatched
// target takes no arena slot, which the count must know.
static std::set<void*> g_prepatched;
namespace prepatch {
static void* g_slots[8];
void initialize() {}
bool available() {
    return true;
}
const char* unavailable_reason() {
    return "";
}
std::optional<Site> lookup(void* runtimeTarget) {
    if (!g_prepatched.contains(runtimeTarget)) {
        return std::nullopt;
    }
    return Site{&g_slots[g_prepatched.size() % 8], runtimeTarget};
}
void publish(const Site& site, void* trampoline) {
    *site.slot = trampoline;
}
}  // namespace prepatch
}  // namespace dusk::mods

namespace dusk::mods::loader {
NativeModule::~NativeModule() = default;
}  // namespace dusk::mods::loader
dusk::config::ConfigVarBase::~ConfigVarBase() = default;

namespace borealis::log::detail {
bool enabled(LogLevel, std::string_view) noexcept {
    return true;
}
void write(LogLevel, std::string_view, std::string_view text) {
    g_log.emplace_back(text);
}
void write_fatal(std::string_view, std::string_view text) {
    std::fprintf(stderr, "fatal: %.*s\n", static_cast<int>(text.size()), text.data());
    std::abort();
}
}  // namespace borealis::log::detail

namespace {

alignas(16) const unsigned char g_fn[8][16] = {};
void* addr(int i) {
    return const_cast<unsigned char*>(g_fn[i]);
}

struct FakeMod {
    LoadedMod mod;
    std::vector<std::unique_ptr<unsigned char[]>> records;
    void* orig[8] = {};

    FakeMod(const char* id, const char* name, std::vector<const char*> hookNames) {
        mod.metadata.id = id;
        mod.metadata.name = name;
        mod.context = std::make_unique<ModContext>();
        mod.context->mod = &mod;
        mod.native = std::make_unique<NativeMod>();
        for (const char* hookName : hookNames) {
            const size_t size = sizeof(ModMetaHookName) + std::strlen(hookName) + 1;
            auto buffer = std::make_unique<unsigned char[]>(size);
            std::memset(buffer.get(), 0, size);
            std::memcpy(buffer.get() + sizeof(ModMetaHookName), hookName, std::strlen(hookName) + 1);
            mod.native->parsed.hookNames.push_back(reinterpret_cast<ModMetaHookName*>(buffer.get()));
            records.push_back(std::move(buffer));
        }
        svc::hook_resolve_mod_records(mod);
    }
    ModContext* ctx() { return mod.context.get(); }
};

void trampoline_stub() {}

const HookService& hooks_service() {
    return *static_cast<const HookService*>(svc::g_hookModule.service);
}

bool logged(const std::string& text) {
    for (const auto& line : g_log) {
        if (line.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void room() {
    const auto& h = hooks_service();
    // Twilit is running and holds three hooks.
    FakeMod twilit("com.dusklight.twilit_essentials", "Twilit Essentials", {"t0", "t1", "t2"});
    CHECK(svc::hook_new_targets(twilit.mod) == 0);  // prepatched gateways: no arena slot
    for (int i = 0; i < 3; i++) {
        CHECK(h.install(twilit.ctx(), addr(i), reinterpret_cast<void*>(&trampoline_stub), &twilit.orig[i]) ==
              MOD_OK);
    }
    CHECK(svc::hook_new_targets(twilit.mod) == 0);  // all held now

    // Dawnlight shares t0 and t1 and declares t3 twice (two names, one address): three new slots.
    FakeMod dawn("dev.bezide.dawnlight", "Dawnlight", {"t0", "t1", "t3", "t3alias", "t4", "t5"});
    CHECK(svc::hook_new_targets(dawn.mod) == 3);

    // Room for four more: it starts.
    g_used = 1020;
    g_capacity = 1024;
    std::string why;
    g_log.clear();
    CHECK(svc::hook_room_check(dawn.mod, why));
    CHECK(why.empty());
    CHECK(logged("takes 3 new hook slot(s); 1020 of 1024 in use"));
    const auto room = svc::hook_room();
    CHECK(room.known && room.used == 1020 && room.capacity == 1024);

    // Room for two: refused, with the numbers and who holds the room, before anything is installed.
    g_used = 1022;
    CHECK(!svc::hook_room_check(dawn.mod, why));
    CHECK(why.find("it needs 3 and 2 of 1024 are left") != std::string::npos);
    CHECK(why.find("Using the most: Twilit Essentials 3") != std::string::npos);
    CHECK(why.find("Disable one of them") != std::string::npos);
    CHECK(svc::hook_counts(dawn.mod).installed == 0);
    CHECK(logged("dev.bezide.dawnlight refused before it started"));

    // Full: a mod that only shares held targets, or has none, still starts.
    g_used = 1024;
    FakeMod sharer("dev.test.sharer", "Sharer", {"t2"});
    CHECK(svc::hook_new_targets(sharer.mod) == 0);
    CHECK(svc::hook_room_check(sharer.mod, why));
    FakeMod assets("dev.test.assets", "Assets", {});
    CHECK(svc::hook_room_check(assets.mod, why));
    // Over-full (the arenas' count above the capacity): left is zero, never negative.
    g_used = 2000;
    CHECK(!svc::hook_room_check(dawn.mod, why));
    CHECK(why.find("0 of 1024 are left") != std::string::npos);

    svc::g_hookModule.modDetached(dawn.mod);
    svc::g_hookModule.modDetached(sharer.mod);
    svc::g_hookModule.modDetached(assets.mod);
    svc::g_hookModule.modDetached(twilit.mod);
    std::printf("room         shared targets take no slot, a target declared twice takes one; fits -> starts, "
                "does not fit -> refused before installing, naming need, room left and Twilit Essentials 3\n");
    std::printf("             %s\n", why.c_str());
}

}  // namespace

int main() {
    for (int i = 0; i < 8; i++) {
        g_symbols["t" + std::to_string(i)] = addr(i);
    }
    g_symbols["t3alias"] = addr(3);
    g_prepatched = {addr(0), addr(1), addr(2)};
    room();
    if (g_failures) {
        std::fprintf(stderr, "hook_room_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("hook_room_test: all checks passed\n");
    return 0;
}
