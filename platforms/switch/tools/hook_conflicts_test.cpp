// H37: hook composition and conflicts, through the game's own HookService (src/dusk/mods/svc/
// hook.cpp, compiled unchanged with no patching backend: registration and dispatch are what is
// under test, not funchook) and src/dusk/mods/svc/hook_conflicts.cpp.
//
// Two mods are set up the way the loader sets them up -- a context each, hook targets declared
// by their metadata records and resolved by name through the symbol manifest -- and then:
//   * Randomizer and Cosmetics both post-hook dMenu_Ring_c::_create: both callbacks run, each
//     with its own context, in priority then registration order; removing one mod leaves the
//     other's hook working; pre-hooks compose the same way;
//   * replace-hooks: a second mod's CONFLICT and lower-PRIORITY replace fail with MOD_CONFLICT
//     and are recorded with both names and the target's name; OVERRIDE and higher PRIORITY
//     displace the holder only where displacement is allowed (desktop) and are recorded and
//     logged even then; with displacement off (Switch V1) they fail like a conflict and the
//     holder's replacement is still the one that runs;
//   * a mod replacing its own hook again is not a conflict;
//   * an undeclared target is refused;
//   * H38-c6: member hooks resolve the loader's way -- a nested class's non-virtual member by address, a virtual one by
//     its full name or through a nested Itanium vtable symbol; an ambiguous name without a vtable and a this
//     adjustment are refused; a failure gives both reasons (by name, by vtable).
#include "dusk/mods/svc/hook.hpp"
#include "dusk/mods/svc/hook_conflicts.hpp"
#include "dusk/mods/svc/registry.hpp"
#include "dusk/mods/loader/loader.hpp"
#include "dusk/mods/loader/native_module.hpp"
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
static std::map<std::string, void*> g_symbols;  // the symbol manifest of this "game"
static std::set<std::string> g_ambiguous;       // names it maps to more than one address

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
    if (g_ambiguous.contains(name)) {
        return ResolveStatus::Ambiguous;
    }
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
}  // namespace dusk::mods

// No library behind these mods: NativeMod only needs NativeModule to be destructible.
namespace dusk::mods::loader {
NativeModule::~NativeModule() = default;
}  // namespace dusk::mods::loader
// LoadedMod holds a ConfigVar; none is ever made here (config.cpp has the real one).
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

// --- the "game" and the "mods" ----------------------------------------------------------------

namespace {

// Stand-ins for game functions: only their addresses matter (4-aligned, as code is).
alignas(16) const unsigned char g_menuRingCreate[16] = {};
alignas(16) const unsigned char g_setNextStage[16] = {};
alignas(16) const unsigned char g_undeclared[16] = {};
void* addr(const unsigned char* f) {
    return const_cast<unsigned char*>(f);
}

struct FakeMod {
    LoadedMod mod;
    std::vector<std::unique_ptr<unsigned char[]>> records;
    std::vector<std::string> calls;  // what its callbacks saw

    explicit FakeMod(const char* id, std::vector<const char*> hookNames) {
        mod.metadata.id = id;
        mod.context = std::make_unique<ModContext>();
        mod.context->mod = &mod;
        mod.native = std::make_unique<NativeMod>();
        for (const char* name : hookNames) {
            // A MOD_META_HOOK_NAME record as the SDK lays it out: header, then the name.
            const size_t size = sizeof(ModMetaHookName) + std::strlen(name) + 1;
            auto buffer = std::make_unique<unsigned char[]>(size);
            std::memset(buffer.get(), 0, size);
            std::memcpy(buffer.get() + sizeof(ModMetaHookName), name, std::strlen(name) + 1);
            mod.native->parsed.hookNames.push_back(reinterpret_cast<ModMetaHookName*>(buffer.get()));
            records.push_back(std::move(buffer));
        }
        svc::hook_resolve_mod_records(mod);
    }
    ModContext* ctx() { return mod.context.get(); }
};

const HookService& hooks_service() {
    return *static_cast<const HookService*>(svc::g_hookModule.service);
}

HookOptions options(int32_t priority, HookReplacePolicy policy) {
    HookOptions o = HOOK_OPTIONS_INIT;
    o.priority = priority;
    o.replace_policy = policy;
    return o;
}

FakeMod* as_mod(ModContext* ctx) {
    return reinterpret_cast<FakeMod*>(reinterpret_cast<char*>(ctx->mod) - offsetof(FakeMod, mod));
}

void post_record(ModContext* ctx, void*, void*, void* userdata) {
    as_mod(ctx)->calls.push_back(std::string("post ") + static_cast<const char*>(userdata));
}
HookAction pre_record(ModContext* ctx, void*, void*, void* userdata) {
    as_mod(ctx)->calls.push_back(std::string("pre ") + static_cast<const char*>(userdata));
    return HOOK_CONTINUE;
}
void replace_record(ModContext* ctx, void*, void*, void* userdata) {
    as_mod(ctx)->calls.push_back(std::string("replace ") + static_cast<const char*>(userdata));
}

void composition() {
    FakeMod rando("dev.twilitrealm.randomizer", {"dMenu_Ring_c::_create"});
    FakeMod cosmetics("dev.twilitrealm.cosmetics", {"dMenu_Ring_c::_create"});
    const auto& h = hooks_service();
    auto o = options(0, HOOK_REPLACE_CONFLICT);
    o.userdata = const_cast<char*>("rando");
    CHECK(h.add_post(rando.ctx(), addr(g_menuRingCreate), post_record, &o) == MOD_OK);
    o.userdata = const_cast<char*>("cosmetics");
    CHECK(h.add_post(cosmetics.ctx(), addr(g_menuRingCreate), post_record, &o) == MOD_OK);
    o.userdata = const_cast<char*>("rando");
    CHECK(h.add_pre(rando.ctx(), addr(g_menuRingCreate), pre_record, &o) == MOD_OK);
    o.userdata = const_cast<char*>("cosmetics");
    o.priority = 5;  // higher priority runs first, whatever the registration order
    CHECK(h.add_pre(cosmetics.ctx(), addr(g_menuRingCreate), pre_record, &o) == MOD_OK);

    int skip = -1;
    CHECK(h.dispatch_pre(nullptr, addr(g_menuRingCreate), nullptr, nullptr, &skip) == MOD_OK);
    CHECK(skip == 0);
    CHECK(h.dispatch_post(nullptr, addr(g_menuRingCreate), nullptr, nullptr) == MOD_OK);
    CHECK((rando.calls == std::vector<std::string>{"pre rando", "post rando"}));
    CHECK((cosmetics.calls == std::vector<std::string>{"pre cosmetics", "post cosmetics"}));
    CHECK(hooks::all().empty());  // pre/post never conflict

    // Order across mods: cosmetics' pre (priority 5) before rando's (0); posts in registration order.
    std::vector<std::string> order;
    rando.calls.clear();
    cosmetics.calls.clear();
    h.dispatch_pre(nullptr, addr(g_menuRingCreate), nullptr, nullptr, &skip);
    CHECK(cosmetics.calls.size() == 1 && rando.calls.size() == 1);

    // One mod leaves (disabled, failed, or unloaded): the other keeps its hooks.
    svc::g_hookModule.modDetached(rando.mod);
    rando.calls.clear();
    cosmetics.calls.clear();
    h.dispatch_post(nullptr, addr(g_menuRingCreate), nullptr, nullptr);
    CHECK(rando.calls.empty());
    CHECK((cosmetics.calls == std::vector<std::string>{"post cosmetics"}));
    svc::g_hookModule.modDetached(cosmetics.mod);
    std::printf("composition  two mods' pre/post hooks on dMenu_Ring_c::_create run together, each with its own context\n");
}

void replace_conflicts(bool displacement) {
    hooks::clear();
    hooks::set_displacement_allowed(displacement);
    FakeMod a("dev.test.first", {"dComIfGp_setNextStage"});
    FakeMod b("dev.test.second", {"dComIfGp_setNextStage"});
    const auto& h = hooks_service();
    auto o = options(10, HOOK_REPLACE_CONFLICT);
    o.userdata = const_cast<char*>("first");
    CHECK(h.replace(a.ctx(), addr(g_setNextStage), replace_record, &o) == MOD_OK);

    // The same mod again, OVERRIDE: its own business, no conflict.
    o.replace_policy = HOOK_REPLACE_OVERRIDE;
    CHECK(h.replace(a.ctx(), addr(g_setNextStage), replace_record, &o) == MOD_OK);
    CHECK(hooks::all().empty());

    o.userdata = const_cast<char*>("second");
    o.replace_policy = HOOK_REPLACE_CONFLICT;
    CHECK(h.replace(b.ctx(), addr(g_setNextStage), replace_record, &o) == MOD_CONFLICT);
    o.replace_policy = HOOK_REPLACE_PRIORITY;
    o.priority = 10;  // not strictly higher
    CHECK(h.replace(b.ctx(), addr(g_setNextStage), replace_record, &o) == MOD_CONFLICT);
    auto recorded = hooks::involving("dev.test.second");
    CHECK(recorded.size() == 1);  // the same pair on the same target is one record
    if (!recorded.empty()) {
        CHECK(recorded[0].target == "dComIfGp_setNextStage");
        CHECK(recorded[0].holder == "dev.test.first" && recorded[0].newcomer == "dev.test.second");
        CHECK(recorded[0].outcome == hooks::Outcome::Refused);
        CHECK(hooks::describe(recorded[0]).find("only one of them can be enabled") != std::string::npos);
    }

    hooks::clear();
    o.replace_policy = HOOK_REPLACE_OVERRIDE;
    const ModResult override = h.replace(b.ctx(), addr(g_setNextStage), replace_record, &o);
    recorded = hooks::involving("dev.test.first");
    CHECK(recorded.size() == 1);  // displaced or refused: recorded either way
    a.calls.clear();
    b.calls.clear();
    int skip = 0;
    h.dispatch_pre(nullptr, addr(g_setNextStage), nullptr, nullptr, &skip);
    CHECK(skip == 1);
    if (displacement) {
        CHECK(override == MOD_OK);
        CHECK(!recorded.empty() && recorded[0].outcome == hooks::Outcome::Displaced);
        CHECK(b.calls.size() == 1 && a.calls.empty());
    } else {
        CHECK(override == MOD_CONFLICT);
        CHECK(!recorded.empty() && recorded[0].outcome == hooks::Outcome::Refused);
        CHECK(a.calls.size() == 1 && b.calls.empty());  // the holder still runs
    }
    bool logged = false;
    for (const auto& line : g_log) {
        logged |= line.find("dev.test.second") != std::string::npos && line.find("dComIfGp_setNextStage") != std::string::npos;
    }
    CHECK(logged);

    // A target the mod never declared is refused, whatever the kind.
    CHECK(h.add_post(a.ctx(), addr(g_undeclared), post_record, &o) == MOD_INVALID_ARGUMENT);
    svc::g_hookModule.modDetached(a.mod);
    svc::g_hookModule.modDetached(b.mod);
    std::printf("replace      displacement %s: conflicts refused and recorded; %s\n",
        displacement ? "allowed (desktop)" : "off (Switch V1)",
        displacement ? "OVERRIDE displaced the holder, recorded and logged" : "OVERRIDE refused, the holder still runs");
}

void decisions() {
    hooks::set_displacement_allowed(false);
    using hooks::ReplacePolicy;
    CHECK(!hooks::decide("t", "a", "b", ReplacePolicy::Conflict, 0, 0).allow);
    CHECK(!hooks::decide("t", "a", "b", ReplacePolicy::Priority, 9, 0).allow);
    CHECK(!hooks::decide("t", "a", "b", ReplacePolicy::Override, 0, 0).allow);
    CHECK(hooks::decide("t", "a", "a", ReplacePolicy::Override, 0, 0).allow);
    CHECK(!hooks::decide("t", "a", "a", ReplacePolicy::Override, 0, 0).record);
    hooks::set_displacement_allowed(true);
    CHECK(!hooks::decide("t", "a", "b", ReplacePolicy::Conflict, 9, 0).allow);
    CHECK(!hooks::decide("t", "a", "b", ReplacePolicy::Priority, 0, 0).allow);
    CHECK(hooks::decide("t", "a", "b", ReplacePolicy::Priority, 1, 0).allow);
    CHECK(hooks::decide("t", "a", "b", ReplacePolicy::Override, 0, 0).record.has_value());
    std::printf("decisions    policy table as specified\n");
}

// --- H38-c6: hooks on members of nested classes, resolved the way hook_resolve_mod_records does ----------------
// The member pointers are the compiler's own (this host's Itanium representation), so the records carry exactly what
// a mod built for this ABI would carry.
namespace nested {
struct Outer {
    struct Inner {
        virtual ~Inner() = default;
        virtual void draw();
        virtual void other();
        void plain();
    };
};
void Outer::Inner::draw() {}
void Outer::Inner::other() {}
void Outer::Inner::plain() {}
struct A {
    virtual ~A() = default;
    virtual void a();
};
struct B {
    virtual ~B() = default;
    virtual void b();
};
struct C : A, B {};
void A::a() {}
void B::b() {}
}  // namespace nested

struct MemberMod {
    LoadedMod mod;
    std::unique_ptr<unsigned char[]> record;

    template <class P>
    MemberMod(P pmf, const char* vtable, const char* display) {
        static_assert(sizeof(P) <= MOD_META_HOOK_MEM_CAPACITY);
        mod.metadata.id = "dev.test.member";
        mod.context = std::make_unique<ModContext>();
        mod.context->mod = &mod;
        mod.native = std::make_unique<NativeMod>();
        // A MOD_META_HOOK_MEM record as the SDK lays it out: header, member pointer, then "vtable\0display\0".
        const size_t size = sizeof(ModMetaHookMem) + std::strlen(vtable) + 1 + std::strlen(display) + 1;
        record = std::make_unique<unsigned char[]>(size);
        std::memset(record.get(), 0, size);
        auto* rec = reinterpret_cast<ModMetaHookMem*>(record.get());
        std::memcpy(rec->pmf, &pmf, sizeof(pmf));
        char* names = reinterpret_cast<char*>(record.get() + sizeof(ModMetaHookMem));
        std::memcpy(names, vtable, std::strlen(vtable) + 1);
        std::memcpy(names + std::strlen(vtable) + 1, display, std::strlen(display) + 1);
        mod.native->parsed.hookMems.push_back(rec);
        g_log.clear();
        svc::hook_resolve_mod_records(mod);
    }
    void* resolved() const { return reinterpret_cast<const ModMetaHookMem*>(record.get())->resolved; }
    bool logged(const char* text) const {
        for (const auto& line : g_log) {
            if (line.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

void member_resolution() {
    alignas(16) static const unsigned char overrider[16] = {};
    alignas(16) static const unsigned char slot0[16] = {};
    alignas(16) static const unsigned char slot1[16] = {};
    // A fake vtable as the ABI lays it out: offset-to-top, typeinfo, then the slots: the two destructors, draw, other.
    static void* vtable[2 + 4] = {nullptr, nullptr, nullptr, nullptr, addr(slot0), addr(slot1)};

    // a non-virtual member of a nested class: the member pointer is the function's address
    {
        MemberMod m(&nested::Outer::Inner::plain, "", "nested::Outer::Inner::plain");
        void (nested::Outer::Inner::*p)() = &nested::Outer::Inner::plain;
        void* direct = nullptr;
        std::memcpy(&direct, &p, sizeof(direct));
        CHECK(m.resolved() == direct);
    }
    // a virtual member of a nested class whose full name resolves: the named overrider, exactly
    g_symbols["nested::Outer::Inner::draw"] = addr(overrider);
    {
        MemberMod m(&nested::Outer::Inner::draw, "", "nested::Outer::Inner::draw");
        CHECK(m.resolved() == addr(overrider));
    }
    // the vtable fallback with a nested class's Itanium vtable symbol (_ZTVN...E): the member pointer's slot
    g_symbols["_ZTVN6nested5Outer5InnerE"] = vtable;
    {
        MemberMod m(&nested::Outer::Inner::other, "_ZTVN6nested5Outer5InnerE", "nested::Outer::Inner::other");
        CHECK(m.resolved() == addr(slot1));  // other is slot 3 (draw, slot 2, holds slot0)
    }
    // an ambiguous name with no vtable to fall back on is refused, and says why
    g_ambiguous.insert("nested::Outer::Inner::other");
    {
        MemberMod m(&nested::Outer::Inner::other, "", "nested::Outer::Inner::other");
        CHECK(m.resolved() == nullptr);
        CHECK(m.logged("maps to more than one address"));
        CHECK(m.logged("not representable as a vtable symbol"));
    }
    // ...but its vtable slot is exact, so with the vtable it resolves
    {
        MemberMod m(&nested::Outer::Inner::other, "_ZTVN6nested5Outer5InnerE", "nested::Outer::Inner::other");
        CHECK(m.resolved() == addr(slot1));
    }
    g_ambiguous.clear();
    // a virtual member reached through a secondary base (this adjustment) is refused unless named
    (void)&nested::A::a;  // A is only there to make B a secondary base
    {
        void (nested::C::*p)() = &nested::B::b;
        MemberMod m(p, "_ZTVN6nested1CE", "nested::B::b");
        CHECK(m.resolved() == nullptr);
        CHECK(m.logged("secondary base"));
    }
    // H38-c5's Deferred Fog: the function was not in the image; both reasons are given, not only the class name's
    {
        MemberMod m(&nested::Outer::Inner::other, "", "dBgp_c::modelMaterial_c::drawSimple");
        CHECK(m.resolved() == nullptr);
        CHECK(m.logged("symbol 'dBgp_c::modelMaterial_c::drawSimple' not found"));
        CHECK(m.logged("not representable as a vtable symbol"));
    }
    std::printf("members      nested non-virtual, by full name, nested vtable fallback, ambiguity and this "
                "adjustment refused, both reasons reported\n");
}

}  // namespace

extern bool g_duskHookFast;  // src/dusk/mods/svc/hook.cpp (V1-rc16)

// V1-rc16: the dispatch's slot cache (game thread, hook_fast). A target looked up before it had any callback is cached
// as a miss; adding one must be seen at once. And hook_fast off runs exactly the same callbacks.
void slot_cache() {
    FakeMod mod("dev.test.cache", {"dComIfGp_setNextStage"});
    const auto& h = hooks_service();
    int skip = -1;
    CHECK(h.dispatch_pre(nullptr, addr(g_setNextStage), nullptr, nullptr, &skip) == MOD_OK && skip == 0);  // miss
    auto o = options(0, HOOK_REPLACE_CONFLICT);
    o.userdata = const_cast<char*>("cache");
    CHECK(h.add_pre(mod.ctx(), addr(g_setNextStage), pre_record, &o) == MOD_OK);
    CHECK(h.add_post(mod.ctx(), addr(g_setNextStage), post_record, &o) == MOD_OK);
    for (const bool fast : {true, false, true}) {
        g_duskHookFast = fast;
        mod.calls.clear();
        h.dispatch_pre(nullptr, addr(g_setNextStage), nullptr, nullptr, &skip);
        h.dispatch_post(nullptr, addr(g_setNextStage), nullptr, nullptr);
        CHECK((mod.calls == std::vector<std::string>{"pre cache", "post cache"}));
    }
    svc::g_hookModule.modDetached(mod.mod);
    mod.calls.clear();
    h.dispatch_pre(nullptr, addr(g_setNextStage), nullptr, nullptr, &skip);  // the slot is gone: nothing runs
    h.dispatch_post(nullptr, addr(g_setNextStage), nullptr, nullptr);
    CHECK(mod.calls.empty());
    std::printf("slot cache   a cached miss sees a new callback; hook_fast off runs the same ones; a removed slot is not reached\n");
}

int main() {
    g_symbols["dMenu_Ring_c::_create"] = addr(g_menuRingCreate);
    g_symbols["dComIfGp_setNextStage"] = addr(g_setNextStage);
    svc::cpu_profile_set_thread();  // V1-rc16: dispatch as the game thread does, through the slot cache
    slot_cache();
    composition();
    replace_conflicts(true);
    replace_conflicts(false);
    decisions();
    member_resolution();
    if (g_failures) {
        std::fprintf(stderr, "hook_conflicts_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("hook_conflicts_test: all checks passed\n");
    return 0;
}
