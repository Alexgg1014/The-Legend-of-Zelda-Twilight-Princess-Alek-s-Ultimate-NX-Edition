#pragma once
// aleks2 (H37): native mods loaded from the card at run time on Switch, with
// src/dusk/mods/loader/elf_loader.cpp. Built when ALEKS2_NATIVE_MOD_LOADER is on.
//
// The mod loader (natives.cpp, loader.cpp) is unchanged in what it does: it extracts a mod's
// lib/<platform>/mod.so and opens it through NativeModule. On Switch, NativeModule
// (switch_native_module.cpp) comes here for anything not compiled into the NRO. What happens
// then is the Loader Mode, a setting (Diagnostics, "Native Mods (H37)"), so one NRO can test any
// number of packages:
//
//   Off      nothing is read; the mod is refused with that reason.
//   Dry Run  the file is validated and every import and relocation planned against this build
//            (default). A JSON report goes to sdmc:/switch/dusklight/mods/.reports/<id>.json.
//            Nothing is mapped executable and no mod code runs; the mod is listed as refused,
//            with the result.
//   Execute  as Dry Run, then mapped (switch_elf_memory.cpp), its .eh_frame registered, its
//            exports found. Its constructors run later, when the mod is activated, under the
//            boot guard (boot_guard.hpp) -- a disabled or quarantined mod never runs any code.
//
// Mappings are never undone: a disabled mod's code stays mapped, inert, until the game exits.
#include "dusk/mods/loader/boot_guard.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dusk::mods {
class ModBundle;
}

namespace dusk::switchelf {

enum class LoaderMode : int32_t { Off = 0, DryRun = 1, Execute = 2 };
LoaderMode loader_mode();
void set_loader_mode(LoaderMode mode);  // persisted
std::string_view loader_mode_name(LoaderMode mode);

// Where mods live (the first search directory). Starts the boot guard; call once, before any
// mod is loaded. Returns what the guard found.
mods::guard::BootGuard::Startup start(const std::filesystem::path& modsDirectory);
// The boot guard, once start() ran; nullptr before.
mods::guard::BootGuard* boot_guard();
const mods::guard::BootGuard::Startup& startup();

// A load refusal, with its safe-mode category; already reported to the guard when thrown.
class LoadError : public std::runtime_error {
public:
    LoadError(mods::guard::Category category, const std::string& message)
        : std::runtime_error(message), m_category(category) {}
    mods::guard::Category category() const { return m_category; }

private:
    mods::guard::Category m_category;
};

// One module loaded by this file.
class Module {
public:
    virtual ~Module() = default;
    virtual void* lookup(std::string_view name) const = 0;
    // Runs the module's constructors, once; false (and why) if one threw.
    virtual bool run_constructors(std::string& error) = 0;
};

// Throws LoadError. `modId` names reports and incidents.
std::unique_ptr<Module> load(const std::filesystem::path& library, std::string_view modId);

// For the installer: would this package's native code load on this build? Dry Run of its
// lib/switch-arm64 (or lib/linux-aarch64) library, nothing mapped or run, report written.
// nullopt if it has no native code or it would load; otherwise the reason, for the player.
std::optional<std::string> check_package(mods::ModBundle& bundle, std::string_view modId);

// The game image's exported surface, for diagnostics: how many names it exports.
size_t exported_symbol_count();

}  // namespace dusk::switchelf
