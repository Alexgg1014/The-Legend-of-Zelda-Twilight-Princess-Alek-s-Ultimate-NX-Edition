#pragma once
// aleks2 (H37): safe mode for native mods -- a boot that crashes in a mod's code does not crash
// again on the next one.
//
// Mod code runs inside the game's process. On the console a crash in it takes everything down,
// and if it happens while mods load, every boot after it would crash the same way. So, around
// each step that runs a mod's code for the first time (mapping and relocating it, its
// constructors, its mod_initialize), the loader writes a marker naming the mod and the step,
// and removes it once the step returns. A marker still there at the next boot names the mod
// that crashed and how far it got: that mod is quarantined -- not loaded, with the reason shown
// -- until the player enables it again. Failures that do return (a refused import, an
// unsupported relocation, mod_initialize returning an error...) are reported the same way but
// need no quarantine: they fail cleanly every time.
//
// A boot that crashes with no marker (in a hook during the title screen, say) cannot be pinned
// on one mod. Two of those in a row and the next boot starts without mods. The player can ask
// for that too (start_without_mods, or the file of that name in the guard directory). Without
// mods means nothing is loaded; nothing is deleted -- mods, their configuration, saves and the
// game's own assets stay as they are.
//
// Everything is kept in one directory (sdmc:/switch/dusklight/mods/.guard on Switch): the
// markers, and report.json, the persistent record of every incident with its category, which
// the mods UI shows and a player can send along with a log.
//
// Platform-neutral and synchronous; tested on the host (platforms/switch/tools/
// boot_guard_test.cpp).
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dusk::mods::guard {

// What went wrong, in the categories the mods UI and the report use.
enum class Category : uint8_t {
    Validation,             // the file is not a module this loader can map (bad ELF, TLS...)
    MissingImport,          // a symbol the module needs is not exported by this build
    UnsupportedRelocation,  // a relocation type the loader does not apply
    MissingDependency,      // DT_NEEDED on a library the game does not stand in for
    ConstructorFailure,     // a constructor threw, or crashed (found at the next boot)
    InitializeFailure,      // mod_initialize returned an error
    CrashDuringInit,        // the process died while the module loaded or initialised
    HookConflict,           // mod_initialize failed because another mod holds a hook it needs
    RuntimeFailure,         // a started mod reported a failure (fail_mod) after the boot; quarantined
};
std::string_view category_code(Category category);
std::optional<Category> category_from_code(std::string_view code);
// A sentence for players.
std::string_view category_text(Category category);

// The category of a load refusal, from the loader's error code (elf::error_code()).
Category category_for_load_error(std::string_view errorCode);

// The step a marker names.
enum class Stage : uint8_t { Load, Constructors, Initialize };
std::string_view stage_code(Stage stage);

struct Incident {
    std::string modId;
    Category category = Category::Validation;
    std::string stage;    // stage_code(), or empty for failures outside the marked steps
    std::string message;  // what the loader said
    uint64_t boot = 0;    // the boot it happened on (1, 2, ...)
};

class BootGuard {
public:
    explicit BootGuard(std::filesystem::path directory);

    struct Startup {
        uint64_t boot = 0;
        // The previous boot crashed while a mod's code ran (now quarantined).
        std::optional<Incident> crashed;
        // V1-rc12: the previous boot stopped while a mod was being loaded (mapped and relocated), the first time
        // for that mod: not quarantined, tried once more. Closing the game during boot looks exactly like a crash
        // here (rc12 console: Crests quarantined after a boot was closed while it loaded, then loaded fine), and a
        // real crash in that step -- the loader's own code reading the file -- happens again on the next boot.
        std::optional<Incident> stoppedOnce;
        // The previous boot crashed with no mod to blame.
        bool unattributedCrash = false;
        // This boot loads no mods, and why.
        bool withoutMods = false;
        std::string withoutModsReason;
    };
    // Call once, before any mod is loaded: reads what the previous boot left.
    Startup begin_boot();
    // The boot got far enough (the loader calls it a while after startup): a crash from now on is
    // not a boot loop.
    void end_boot();

    // Around each step that runs a mod's code. The marker is on disk before enter() returns.
    void enter(std::string_view modId, Stage stage);
    void leave();

    // A failure that returned; reported, not quarantined.
    void report(Incident incident);

    // A mod that had started reported a failure while the game ran (fail_mod after the boot):
    // the incident is recorded and the mod quarantined -- both on disk before this returns, so a
    // crash during the teardown that follows still leaves the next boot without it. Only a failure
    // the mod itself identified: a crash inside a hook still cannot be pinned on a mod. Returns
    // false, writing nothing, when the mod is already quarantined (one incident per failure).
    // The player releases it as for any quarantined mod: turn it off and on again.
    bool record_runtime_failure(Incident incident);

    [[nodiscard]] bool quarantined(std::string_view modId, std::string* why = nullptr) const;
    // The player enabled the mod again: try it on the next load.
    void release(std::string_view modId);
    [[nodiscard]] std::vector<std::string> quarantine() const;

    // Next boot without mods (the player asked); cleared by that boot.
    void request_boot_without_mods();
    [[nodiscard]] bool without_mods() const { return m_withoutMods; }

    [[nodiscard]] std::vector<Incident> incidents() const;
    [[nodiscard]] uint64_t boot() const { return m_boot; }
    [[nodiscard]] const std::filesystem::path& directory() const { return m_dir; }

    // How many incidents report.json keeps (oldest dropped first).
    static constexpr size_t kMaxIncidents = 64;
    // Unattributed boot crashes in a row before a boot without mods.
    static constexpr uint32_t kCrashesBeforeSafeMode = 2;

private:
    struct State;
    State load() const;
    void save(const State& state) const;

    std::filesystem::path m_dir;
    uint64_t m_boot = 0;
    bool m_withoutMods = false;
};

}  // namespace dusk::mods::guard
