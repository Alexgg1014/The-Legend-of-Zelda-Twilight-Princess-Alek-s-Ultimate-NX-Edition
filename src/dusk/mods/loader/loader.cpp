#include "loader.hpp"

#include "depgraph.hpp"
#include "manifest.hpp"
#include "native_module.hpp"
#include "natives.hpp"
#include "lib_cache.hpp"  // V1-rc19
#include "packages.hpp"
#include "update_journal.hpp"
#if DUSK_HAS_PREPATCH
#include "prepatch.hpp"
#endif

#include "dusk/config.hpp"
#include "dusk/data.hpp"
#include "dusk/logging.h"
#include "dusk/mod_loader.hpp"
#include "dusk/mods/log_buffer.hpp"
#include "dusk/mods/manifest.hpp"
#include "dusk/mods/queue.hpp"
#include "dusk/mods/svc/config.hpp"
#include "dusk/mods/svc/hook.hpp"
#include "dusk/mods/svc/hook_conflicts.hpp"
#include "dusk/mods/svc/registry.hpp"
#include "dusk/settings.h"
#include "dusk/mods/updates.hpp"
#include "dusk/ui/mod_texture_provider.hpp"
#include "dusk/ui/mods_window.hpp"
#include "dusk/ui/ui.hpp"
#include "dusk/ui/boot_splash.hpp"  // V1-rc19
#include "dusk/utilities.hpp"
#include "dusk/file_copy.hpp"  // aleks2 (rc18)
#if ALEKS2_NATIVE_MOD_LOADER
#include "dusk/switch_native_loader.hpp"
#endif
#if defined(__SWITCH__) && DUSK_HAS_FUNCHOOK
#include "dusk/switch_code_arena.h"
#include "same_path.hpp"  // V1: equivalent() is wrong on the Switch (st_ino 0)
#endif

#include <borealis/io.hpp>
#include <borealis/update.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <functional>
#include <atomic>
#include <cstddef>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace fs = std::filesystem;

extern bool g_duskModLibCache;  // natives.cpp (V1-rc19)

namespace dusk::mods {
namespace {
constexpr borealis::Log Log{"dusk::mods::loader"};
ModLoader g_modLoader;

void complete_operation(const std::shared_ptr<ModOperation>& operation, const bool success = true,
    std::string message = {}) {
    if (operation == nullptr) {
        return;
    }
    operation->state = success ? ModOperation::State::Succeeded : ModOperation::State::Failed;
    operation->message = std::move(message);
}

LoadedMod::FileIdentity file_identity(const fs::path& path) {
    std::error_code error;
    const bool isDirectory = fs::is_directory(path, error);
    if (error) {
        return {};
    }
    const auto size = isDirectory ? 0 : fs::file_size(path, error);
    if (error) {
        return {};
    }
    const auto modified = fs::last_write_time(path, error);
    if (error) {
        return {};
    }
    return {.size = size, .modified = modified, .valid = true};
}

#if ALEKS2_NATIVE_MOD_LOADER
// DM1-b: the journal's file operations, as the rest of the loader does them.
bool journal_readable(const fs::path& package) {
    ModMetadata metadata;
    std::string error;
    return inspect_mod_bundle(package, metadata, error);
}
bool journal_replace(const fs::path& from, const fs::path& to, std::string& error) {
    return borealis::io::atomic_replace(from, to, error);
}
#endif

}  // namespace
}  // namespace dusk::mods

#if defined(__SWITCH__)
// DM1-b: defined by platforms/switch/patches/dawn-worker-stack.patch in Dawn's WorkerThread.cpp.
namespace aleks2::dawn_worker {
extern std::atomic<std::size_t> stackDefault;
extern std::atomic<std::size_t> stackSize;
extern std::atomic<unsigned> threads;
extern std::atomic<unsigned> fallbacks;
extern std::atomic<int> lastError;
}  // namespace aleks2::dawn_worker
#endif

namespace dusk::mods {
namespace {

}  // namespace

ModLoader& ModLoader::instance() {
    return g_modLoader;
}

static std::string lifecycle_error_message(
    const char* fnName, const ModResult result, const ModError& error) {
    if (error.message[0] != '\0') {
        return error.message;
    }
    return fmt::format("{} failed with result {}", fnName, static_cast<int>(result));
}

std::string escape_mod_id_for_config(std::string_view const id) {
    std::string buf;

    // Simple escaping. All characters in mod IDs literal, except for '.' and '_'.
    // '.' -> '_', '_' -> '__'
    for (char const chr : id) {
        if (chr == '.') {
            buf.push_back('_');
        } else if (chr == '_') {
            buf.push_back('_');
            buf.push_back('_');
        } else {
            buf.push_back(chr);
        }
    }

    return buf;
}

static std::string mod_enabled_cvar_name(std::string_view const id) {
    return fmt::format("mod.{}.enabled", escape_mod_id_for_config(id));
}

static bool required_deps_active(const LoadedMod& mod) {
    return std::ranges::all_of(mod.dependencies,
        [](const ModDependencyEdge& edge) { return !edge.required || edge.mod->active; });
}

// A deferred export that was not published by the end of the provider's initialization can
// never resolve, which is almost certainly a bug in the provider.
static void warn_unpublished_deferred_exports(const LoadedMod& mod) {
    if (!mod.active || !mod.native) {
        return;
    }

    for (const auto* serviceExport : mod.native->parsed.exports) {
        if ((serviceExport->rec.flags & SERVICE_EXPORT_DEFERRED) == 0) {
            continue;
        }
        const auto* record =
            svc::find_service_record(serviceExport->service_id.chars, serviceExport->major_version);
        if (record != nullptr && record->service == nullptr) {
            log::write(mod.metadata.id, LOG_LEVEL_WARN,
                "declared deferred service '{}@{}' but never published it during initialization",
                serviceExport->service_id.chars, serviceExport->major_version);
        }
    }
}

LoadedMod* ModLoader::try_load_mod(const fs::path& modPath, bool fromDir, uint32_t searchDirIndex,
    std::unique_ptr<ModBundle> bundle) {
    if (bundle == nullptr) {
        try {
            bundle = load_bundle(modPath, fromDir);
        } catch (const std::exception& e) {
            Log.error(
                "Failed to open {} bundle: {}", data::abbreviated_path_string(modPath), e.what());
            return nullptr;
        }
    }

    LoadedManifest manifest;
    try {
        manifest = load_manifest(modPath, *bundle);
    } catch (const std::exception& e) {
        Log.error("bad mod.json in {}: {}", data::abbreviated_path_string(modPath), e.what());
        return nullptr;
    }

    if (const auto* existing = find_mod(manifest.metadata.id)) {
        if (existing->searchDirIndex < searchDirIndex) {
            log::write(manifest.metadata.id, LOG_LEVEL_INFO,
                "{} shadowed by higher-priority duplicate {}",
                data::abbreviated_path_string(modPath),
                data::abbreviated_path_string(existing->modPath));
        } else {
            log::write(manifest.metadata.id, LOG_LEVEL_ERROR, "duplicate mod id, not loading {}",
                data::abbreviated_path_string(modPath));
        }
        return nullptr;
    }

    const auto& inserted = m_mods.emplace_back(std::make_unique<LoadedMod>());
    auto& mod = *inserted;
    mod.active = true;
    mod.modPath = data::absolute_path(modPath);
    mod.searchDirIndex = searchDirIndex;
    mod.fromDirectory = fromDir;
    mod.nativeInPlace = m_searchDirs[searchDirIndex].inPlaceNative && fromDir;
    mod.fileIdentity = file_identity(modPath);
    mod.metadata = std::move(manifest.metadata);
    mod.runtime = std::move(manifest.runtime);
    mod.bundle = std::move(bundle);
    mod.context = std::make_unique<ModContext>();
    mod.context->mod = &mod;
    mod.cvarIsEnabled =
        std::make_unique<ConfigVar<bool>>(mod_enabled_cvar_name(mod.metadata.id), true);
    if (mod.runtime.has_value()) {
        const auto& runtime = *mod.runtime;
        mod.manifestInfo.imports.push_back({runtime.id, runtime.major, runtime.minMinor, true});

        std::error_code ec;
        mod.dir = data::absolute_path(m_cacheDir / mod.metadata.id / "data");
        if (!ec) {
            fs::create_directories(mod.dir, ec);
        }
        if (ec) {
            fail_mod(mod, MOD_ERROR,
                fmt::format("Failed to create script scratch directory: {}", ec.message()));
        } else {
            mod.dirUtf8 = borealis::io::fs_path_to_string(mod.dir);
        }
    }
    if (load_native_if_present(mod) && mod.native) {
        mod.manifestInfo = build_manifest_info(mod.native->parsed);
    }

    log::write(mod.metadata.id, LOG_LEVEL_INFO, "found '{}' v{} by {} ({})", mod.metadata.name,
        mod.metadata.version, mod.metadata.author, data::abbreviated_path_string(modPath));
    return &mod;
}

bool ModLoader::activate_mod(LoadedMod& mod) {
    // aleks2 (1.0.3): the first activation fixes it for the session (svc/companion_hidden.hpp).
    svc::companion_hidden_snapshot(getSettings().game.dualScreen.getValue());
    log::write(mod.metadata.id, LOG_LEVEL_INFO, "activating mod");
    mod.active = true;

    // Asset-only mods have no lifecycle beyond their overlay files.
    if (!mod.native && !mod.runtime.has_value()) {
        mod.enabledApplied = true;
        return true;
    }

    if (mod.native && !mod.servicesRegistered) {
        if (!register_static_service_exports(mod)) {
            log::write(mod.metadata.id, LOG_LEVEL_ERROR, "failed to register service exports");
            deactivate_mod(mod);
            return false;
        }
        mod.servicesRegistered = true;
    }

    if (mod.native && !resolve_service_imports(mod)) {
        log::write(mod.metadata.id, LOG_LEVEL_ERROR, "failed to resolve service imports");
        deactivate_mod(mod);
        return false;
    }

    if (mod.native) {
        svc::hook_resolve_mod_records(mod);
        // aleks2 (DM2-a): before any of its code runs, its hooks must fit what the code arenas
        // have left; a mod that would only get some of them does not start at all.
        std::string hookRoom;
        if (!svc::hook_room_check(mod, hookRoom)) {
            fail_mod(mod, MOD_UNAVAILABLE, hookRoom);
            deactivate_mod(mod);
            return false;
        }
        *mod.native->contextSymbol = mod.context.get();
    } else {
        auto& runtime = *mod.runtime;
        const auto* record = svc::find_service(runtime.id.c_str(), runtime.major, runtime.minMinor);
        if (record == nullptr) {
            fail_mod(mod, MOD_UNAVAILABLE,
                describe_missing_import(runtime.id.c_str(), runtime.major, runtime.minMinor));
            deactivate_mod(mod);
            return false;
        }

        const auto* service = static_cast<const ModRuntimeService*>(record->service);
        constexpr size_t kRequiredSize = offsetof(ModRuntimeService, deactivate) +
                                         sizeof(((ModRuntimeService*)nullptr)->deactivate);
        if (record->provider == nullptr || service == nullptr ||
            service->header.struct_size < kRequiredSize || service->activate == nullptr ||
            service->update == nullptr || service->deactivate == nullptr)
        {
            fail_mod(mod, MOD_UNAVAILABLE,
                fmt::format("Runtime service {}@{} has an invalid lifecycle contract", runtime.id,
                    runtime.major));
            deactivate_mod(mod);
            return false;
        }
        runtime.service = service;
        runtime.providerContext = record->provider->context.get();
    }

    const char* initializeName = mod.native ? "mod_initialize" : "runtime activate";
#if ALEKS2_NATIVE_MOD_LOADER
    // aleks2 (H37): a module loaded from the card runs its constructors now, the first time it is
    // activated, and both steps are marked for the boot guard: a crash in either quarantines it.
    auto* bootGuard = switchelf::boot_guard();
    if (mod.native && mod.native->handle) {
        if (bootGuard != nullptr) {
            bootGuard->enter(mod.metadata.id, guard::Stage::Constructors);
        }
        std::string why;
        log::write(mod.metadata.id, LOG_LEVEL_INFO, "running constructors");  // V1-rc6: where a crash stopped
        const bool constructed = mod.native->handle->RunDeferredInitializers(why);
        if (bootGuard != nullptr) {
            bootGuard->leave();
        }
        if (!constructed) {
            if (bootGuard != nullptr) {
                bootGuard->report({mod.metadata.id, guard::Category::ConstructorFailure,
                    std::string(guard::stage_code(guard::Stage::Constructors)), why});
            }
            fail_mod(mod, MOD_ERROR, why);
            deactivate_mod(mod);
            return false;
        }
    }
    if (bootGuard != nullptr && mod.native) {
        bootGuard->enter(mod.metadata.id, guard::Stage::Initialize);
    }
#endif
    log::write(mod.metadata.id, LOG_LEVEL_INFO, "calling {}", initializeName);  // V1-rc6: Info, see above
    try {
        ModError error = MOD_ERROR_INIT;
        const auto result = mod.native ?
                                mod.native->fn_initialize(&error) :
                                mod.runtime->service->activate(
                                    mod.runtime->providerContext, mod.context.get(), &error);
        if (result == MOD_OK && !mod.loadFailed) {
            mod.initialized = true;
            log::write(mod.metadata.id, LOG_LEVEL_TRACE, "{} succeeded", initializeName);
        } else if (result != MOD_OK && !mod.loadFailed) {
            fail_mod(mod, result, lifecycle_error_message(initializeName, result, error));
        }
    } catch (const std::exception& e) {
        fail_mod(mod, MOD_ERROR, fmt::format("Exception in {}: {}", initializeName, e.what()));
    } catch (...) {
        fail_mod(mod, MOD_ERROR, fmt::format("Unknown exception in {}", initializeName));
    }

#if ALEKS2_NATIVE_MOD_LOADER
    if (bootGuard != nullptr && mod.native) {
        bootGuard->leave();
        if (!mod.active) {
            // A hook another mod holds is the usual reason a mod's initialisation fails at once.
            auto category = guard::Category::InitializeFailure;
            std::string why = mod.failureReason;
            for (const auto& conflict : hooks::involving(mod.metadata.id)) {
                if (conflict.newcomer == mod.metadata.id && conflict.outcome == hooks::Outcome::Refused) {
                    category = guard::Category::HookConflict;
                    why = hooks::describe(conflict);
                    mod.failureReason = why;
                    break;
                }
            }
            bootGuard->report({mod.metadata.id, category,
                std::string(guard::stage_code(guard::Stage::Initialize)), why});
        }
    }
#endif
    warn_unpublished_deferred_exports(mod);

    if (!mod.active) {
        // Failed initialization may have left hooks or other service state behind
        deactivate_mod(mod);
        return false;
    }

    mod.enabledApplied = true;
#if defined(__SWITCH__) && DUSK_HAS_FUNCHOOK
    if (m_startupComplete && mod.native) {
        // Enabled while playing: say what the hooks took (startup says it once for all).
        char arenas[512];
        funchook_switch_describe(arenas, sizeof arenas);
        log::write(mod.metadata.id, LOG_LEVEL_INFO, "{}", arenas);
    }
#endif
    return true;
}

void ModLoader::deactivate_mod(LoadedMod& mod) {
    svc::modules_mod_deactivating(mod);
    if (mod.initialized && ((mod.native && mod.native->fn_shutdown) ||
                               (mod.runtime.has_value() && mod.runtime->service != nullptr)))
    {
        const char* shutdownName = mod.native ? "mod_shutdown" : "runtime deactivate";
        log::write(mod.metadata.id, LOG_LEVEL_TRACE, "calling {}", shutdownName);
        try {
            ModError error = MOD_ERROR_INIT;
            const auto result = mod.native ?
                                    mod.native->fn_shutdown(&error) :
                                    mod.runtime->service->deactivate(
                                        mod.runtime->providerContext, mod.context.get(), &error);
            if (result == MOD_OK) {
                log::write(mod.metadata.id, LOG_LEVEL_TRACE, "{} succeeded", shutdownName);
            } else {
                log::write(mod.metadata.id, LOG_LEVEL_ERROR, "{} failed: {}", shutdownName,
                    lifecycle_error_message(shutdownName, result, error));
            }
        } catch (const std::exception& exception) {
            log::write(
                mod.metadata.id, LOG_LEVEL_ERROR, "{} threw: {}", shutdownName, exception.what());
        } catch (...) {
            log::write(
                mod.metadata.id, LOG_LEVEL_ERROR, "{} threw an unknown exception", shutdownName);
        }
    }
    mod.initialized = false;
    if (mod.runtime.has_value()) {
        mod.runtime->service = nullptr;
        mod.runtime->providerContext = nullptr;
    }

    if (mod.servicesRegistered) {
        svc::remove_services_for_provider(mod);
        mod.servicesRegistered = false;
    }
    svc::modules_mod_detached(mod);
    unload_native(mod);

    mod.active = false;
    mod.enabledApplied = false;
}

void ModLoader::init() {
    if (m_initialized) {
        return;
    }
    m_initialized = true;
    m_loaderThread = std::this_thread::get_id();

    manifest::initialize();
#if DUSK_HAS_PREPATCH
    prepatch::initialize();
#endif

    if (m_searchDirs.empty()) {
        Log.warn("no mod search directories configured; mod loading skipped");
        return;
    }

    if (m_cacheDir.empty()) {
        m_cacheDir = m_searchDirs.front().path / ".cache";
    }

#if ALEKS2_NATIVE_MOD_LOADER
    // aleks2 (H37): safe mode reads what the previous boot left before any mod is touched.
    if (switchelf::start(m_searchDirs.front().path).withoutMods) {
        init_services();
        Log.warn("starting without mods: {}", switchelf::startup().withoutModsReason);
        svc::modules_lifecycle_applied();
        m_startupComplete = true;
        return;
    }
#endif

#if ALEKS2_NATIVE_MOD_LOADER
    // aleks2 (V1-rc19): updates that waited for this start (their package was open) go in place first, before the
    // journal below looks at them and before any package is opened.
    apply_pending_packages(m_searchDirs.front().path);

    // aleks2 (DM1-b): an update the previous session left unfinished, or that crashed the last
    // start, is undone before anything loads (update_journal.hpp).
    {
        std::error_code userError;
        const auto userDir = data::canonical_path(m_searchDirs.front().path, userError);
        if (!userError) {
            const auto& crashed = switchelf::startup().crashed;
            for (const auto& action : journal::recover_at_start(
                     userDir, crashed ? std::string_view{crashed->modId} : std::string_view{},
                     journal_readable, journal_replace))
            {
                Log.warn("update journal: {}{}{}", action.id, action.id.empty() ? "" : ": ", action.message);
                if (action.restored) {
                    ui::push_toast({.type = "warning", .title = "Mod update undone", .content = action.message,
                        .duration = std::chrono::seconds{8}});
                }
            }
        }
    }
#endif

    std::error_code ec;

    // Stale libs from previous sessions (see load_native).
    { const auto bootT = dusk::boot_timing::now(); fs::remove_all(m_cacheDir, ec); dusk::boot_timing::phase("mod cache cleanup (.cache)", bootT); }

    const auto bootScanT = dusk::boot_timing::now(); const auto packages = scan_packages(m_searchDirs); dusk::boot_timing::phase("mod package scan", bootScanT);
    int bootIndex = 0, bootStarted = 0;  // V1-rc19: the boot splash's counts
    for (const auto& package : packages) {
        if (!m_startupComplete) {
            ++bootIndex;
            ui::boot_splash::progress(fmt::format("Loading mods {}/{}: {}", bootIndex, packages.size(),
                                          package.metadata.name).c_str(),
                bootIndex - 1, static_cast<int>(packages.size()) * 2);  // loading is the first half of the bar
        }
        const auto* selected = select_package(packages, package.metadata.id);
        if (selected != &package) {
            // aleks2 (H38): a newer package is set behind an older one only for the Switch preference.
            const bool keptSwitchCopy =
                compare_package_versions(package.metadata.version, selected->metadata.version) > 0;
            log::write(package.metadata.id, LOG_LEVEL_INFO, "{} v{} shadowed by {} v{}{}",
                data::abbreviated_path_string(package.path), package.metadata.version,
                data::abbreviated_path_string(selected->path), selected->metadata.version,
                keptSwitchCopy ? " (the newer package has no switch-arm64 library; the Switch copy is kept)" :
                                 "");
            continue;
        }
        const auto bootModT = dusk::boot_timing::now(); if (auto* mod = try_load_mod(package.path, package.fromDirectory, package.searchDirIndex)) {
            record_package_sources(*mod, packages);
        } dusk::boot_timing::mod_loaded(package.metadata.id.c_str(), bootModT);
    }

#if ALEKS2_NATIVE_MOD_LOADER
    // aleks2 (V1-rc19): mods/.libcache entries that no package used this boot go (natives.cpp).
    if (g_duskModLibCache) {
        if (const size_t removed = lib_cache::prune(m_cacheDir.parent_path() / ".libcache"); removed != 0) {
            Log.info("lib cache: {} entr{} no installed package uses removed", removed, removed == 1 ? "y" : "ies");
        }
    }
#endif

    if (m_mods.empty()) {
        init_services();
        Log.info("no mods found");
        svc::modules_lifecycle_applied();
        m_startupComplete = true;
        return;
    }

    std::stable_sort(m_mods.begin(), m_mods.end(),
        [](const auto& a, const auto& b) { return a->searchDirIndex > b->searchDirIndex; });

    Log.info("initializing {} mod(s)...", m_mods.size());
    for (auto& mod : mods()) {
        mod.enabledSubscription = Register(*mod.cvarIsEnabled,
            [this, &mod](const bool&, const bool&) { on_enabled_changed(mod); });
    }

    init_services();

    // Providers must initialize (and publish deferred services) before their importers, so
    // imports are resolved per mod, interleaved with initialization, in dependency order.
    loader::sort_mods(m_mods);

    // Decide the startup lifecycle state before publishing exports. Config-disabled mods and
    // mods blocked by required providers keep dependency edges but must not resolve or provide
    // services until they can actually initialize.
    for (auto& mod : mods()) {
        if (!mod.cvarIsEnabled->getValue()) {
            log::write(mod.metadata.id, LOG_LEVEL_INFO, "disabled by config");
            mod.active = false;
            mod.suspendedByProvider = false;
            continue;
        }
        if (!mod.loadFailed && !required_deps_active(mod)) {
            log::write(
                mod.metadata.id, LOG_LEVEL_INFO, "suspended: a required provider is disabled");
            mod.active = false;
            mod.suspendedByProvider = true;
            continue;
        }
        mod.suspendedByProvider = false;
    }

    for (auto& mod : mods()) {
        if (!mod.active || !mod.native) {
            continue;
        }
        if (register_static_service_exports(mod)) {
            mod.servicesRegistered = true;
        } else {
            log::write(mod.metadata.id, LOG_LEVEL_ERROR, "failed to register service exports");
            deactivate_mod(mod);
        }
    }

    for (auto& mod : mods()) {
        if (mod.active) {
            ++bootStarted;
            ui::boot_splash::progress(fmt::format("Starting mods {}/{}: {}", bootStarted, m_mods.size(), mod.metadata.name).c_str(),
                bootIndex + bootStarted, bootIndex + static_cast<int>(m_mods.size()));
            const auto bootModT = dusk::boot_timing::now(); activate_mod(mod); dusk::boot_timing::mod_activated(mod.metadata.id.c_str(), bootModT);
        }
    }

    svc::modules_lifecycle_applied(); dusk::boot_timing::mods_report();

    auto active = std::ranges::count_if(mods(), [](const LoadedMod& m) { return m.active; });
    Log.info("{}/{} mod(s) active", active, m_mods.size());
#if defined(__SWITCH__) && DUSK_HAS_FUNCHOOK
    // aleks2 (H37): how many kernel code memory objects the hooks took (code_arena.h).
    char arenas[512];
    funchook_switch_describe(arenas, sizeof arenas);
    Log.info("{}", arenas);
#endif

    m_startupComplete = true;
}

LoadedMod* ModLoader::find_mod(std::string_view id) {
    for (auto& mod : m_mods) {
        if (mod->metadata.id == id) {
            return mod.get();
        }
    }
    return nullptr;
}

const LoadedMod* ModLoader::find_mod(std::string_view id) const {
    for (const auto& mod : m_mods) {
        if (mod->metadata.id == id) {
            return mod.get();
        }
    }
    return nullptr;
}

void ModLoader::request_enable(std::string_view id) {
    if (auto* mod = find_mod(id)) {
        mod->cvarIsEnabled->setValue(true);
    }
}

void ModLoader::request_disable(std::string_view id) {
    if (auto* mod = find_mod(id)) {
        mod->cvarIsEnabled->setValue(false);
    }
}

ModOperationHandle ModLoader::request_reload(std::string_view id) {
    auto operation = std::make_shared<ModOperation>();
    if (find_mod(id) != nullptr) {
        m_pendingRequests.push_back(ReloadRequest{
            .modId = std::string{id},
            .operation = operation,
        });
    } else {
        complete_operation(operation, false, "The mod is no longer installed");
    }
    return operation;
}

ModOperationHandle ModLoader::request_install(
    fs::path path, std::optional<UpdatePrecondition> update) {
    auto operation = std::make_shared<ModOperation>();
    m_pendingRequests.push_back(InstallRequest{
        .stagedPath = std::move(path),
        .update = std::move(update),
        .operation = operation,
    });
    return operation;
}

ModOperationHandle ModLoader::request_uninstall(std::string_view id) {
    auto operation = std::make_shared<ModOperation>();
    if (find_mod(id) != nullptr) {
        m_pendingRequests.push_back(UninstallRequest{
            .modId = std::string{id},
            .operation = operation,
        });
    } else {
        complete_operation(operation);
    }
    return operation;
}

ModOperationHandle ModLoader::request_reactivate(std::string_view id) {
    auto operation = std::make_shared<ModOperation>();
    m_pendingRequests.push_back(LifecycleRequest{
        .modId = std::string{id},
        .action = LifecycleAction::Reactivate,
        .operation = operation,
    });
    return operation;
}

fs::path ModLoader::user_mods_dir() const {
    return m_searchDirs.empty() ? fs::path{} : m_searchDirs.front().path;
}

bool ModLoader::restart_required(std::string_view id) const {
    return std::ranges::any_of(m_restartRequired, [&](const auto& entry) { return entry.first == id; });
}

void ModLoader::require_restart(std::string_view id, std::string reason) {
    const auto existing = std::ranges::find_if(m_restartRequired, [&](const auto& entry) { return entry.first == id; });
    if (existing != m_restartRequired.end()) {
        existing->second = reason;
    } else {
        m_restartRequired.emplace_back(std::string{id}, reason);
    }
    ++m_generation;
    Log.info("{}: restart required ({})", id, reason);
    ui::push_toast({.type = "warning", .title = "Restart required", .content = fmt::format("{}: {}", id, reason),
        .duration = std::chrono::seconds{6}});
}

std::string_view ModLoader::staged_version(std::string_view id) const {
    const auto staged = std::ranges::find_if(m_stagedVersions, [&](const auto& entry) { return entry.first == id; });
    return staged != m_stagedVersions.end() ? std::string_view{staged->second} : std::string_view{};
}

void ModLoader::clear_restart(std::string_view id) {
    if (const auto staged = staged_version(id); !staged.empty()) {
        // DM1-c: toggling the mod back does not undo an install that waits for the restart.
        const auto existing =
            std::ranges::find_if(m_restartRequired, [&](const auto& entry) { return entry.first == id; });
        const auto reason = fmt::format("v{} installed; restart the game to run it", staged);
        if (existing != m_restartRequired.end() && existing->second != reason) {
            existing->second = reason;
            ++m_generation;
        }
        return;
    }
    if (std::erase_if(m_restartRequired, [&](const auto& entry) { return entry.first == id; }) != 0) {
        ++m_generation;
        Log.info("{}: no restart needed any more", id);
    }
}

bool ModLoader::code_change_needs_restart(const LoadedMod& mod) const {
#if ALEKS2_NATIVE_MOD_LOADER
    // aleks2 (DM1-b): once the boot is over, code is neither started nor stopped until the next start --
    // nothing is unloaded on Switch, and hooks are not proven to come off cleanly.
    return m_startupComplete && (mod.native != nullptr || mod.nativeStatus != NativeModStatus::None);
#else
    (void)mod;
    return false;
#endif
}

void ModLoader::settle_update_journal() {
#if ALEKS2_NATIVE_MOD_LOADER
#if defined(__SWITCH__)
    {
        // DM1-b: Dawn's worker stacks, as dawn-worker-stack.patch set them up (0 threads: none started yet).
        namespace w = aleks2::dawn_worker;
        Log.info("dawn workers: {} thread(s) with a {} KiB stack (default {} KiB); {} fell back to the default "
                 "(last pthread_create error {})",
            w::threads.load(), w::stackSize.load() / 1024, w::stackDefault.load() / 1024, w::fallbacks.load(),
            w::lastError.load());
    }
#endif
    std::error_code error;
    const auto userDir = data::canonical_path(user_mods_dir(), error);
    if (error) {
        return;
    }
    const auto outcome = [this](std::string_view id) {
        const auto* mod = find_mod(id);
        if (mod == nullptr || mod->loadFailed) {
            return journal::Outcome::Failed;
        }
        return mod->active ? journal::Outcome::Running : journal::Outcome::NotStarted;
    };
    for (const auto& action : journal::settle_after_boot(userDir, outcome, journal_replace)) {
        Log.info("update journal: {}: {}", action.id, action.message);
        if (action.restored) {
            require_restart(action.id, action.message);
        }
    }
#endif
}

bool ModLoader::can_uninstall(const LoadedMod& mod) const {
    return mod.hasUserPackage;
}

bool ModLoader::can_update(const LoadedMod& mod) const {
    return mod.searchDirIndex != 0 || (!mod.fromDirectory && can_uninstall(mod));
}

void ModLoader::report_failure(LoadedMod& mod, ModResult code, std::string_view message) {
    if (m_loaderThread == std::thread::id{} || std::this_thread::get_id() == m_loaderThread) {
        fail_mod(mod, code, message);
        return;
    }
    std::lock_guard lock{m_threadFailuresMutex};
    m_threadFailures.push_back({mod.metadata.id, code, std::string(message)});
}

void ModLoader::notify_mod_failure(LoadedMod& mod, bool firstFailure) {
    if (firstFailure) {
        m_pendingFailures.push_back(mod.metadata.name);
    }
#if ALEKS2_NATIVE_MOD_LOADER
    // aleks2 (H37-c5): a mod that had started and then failed after the boot window is quarantined
    // on disk now, before any teardown: the next boot does not load it, even if the game dies on
    // the way down (boot_guard.hpp). A failure while the mod initialises is reported by
    // activate_mod, so mod.initialized keeps the two apart.
    if (firstFailure && m_startupComplete && mod.initialized) {
        if (auto* bootGuard = switchelf::boot_guard()) {
            bootGuard->record_runtime_failure(
                {mod.metadata.id, guard::Category::RuntimeFailure, "runtime", mod.failureReason});
        }
    }
#endif
    // Startup failures are handled inline by activate_mod
    if (!m_startupComplete) {
        return;
    }
    m_pendingRequests.emplace_back(LifecycleRequest{
        .modId = mod.metadata.id,
        .action = LifecycleAction::Disable,
    });
}

void ModLoader::flush_toasts() {
    if (m_pendingFailures.empty()) {
        return;
    }

    const auto names = std::exchange(m_pendingFailures, {});

    // Skip displaying toasts if the mods window is currently open
    if (const auto* window = dynamic_cast<const ui::ModsWindow*>(ui::top_document())) {
        if (window->visible()) {
            return;
        }
    }

    ui::Toast toast{.type = "warning", .duration = std::chrono::seconds{5}};
    if (names.size() == 1) {
        toast.title = "Mod failed";
        toast.content =
            fmt::format("<div><b>{}</b> failed and was disabled.</div><div>Check Mods for "
                        "more information.</div>",
                ui::escape(names.front()));
    } else {
        toast.title = "Mods failed";
        toast.content = fmt::format("<div><b>{} mods</b> failed and were disabled.</div><div>Check "
                                    "Mods for more information.</div>",
            names.size());
    }
    ui::push_toast(std::move(toast));
}

std::vector<LoadedMod*> ModLoader::collect_lifecycle_set(LoadedMod& target) const {
    std::vector included{&target};
    std::vector pending{&target};
    while (!pending.empty()) {
        auto* current = pending.back();
        pending.pop_back();
        for (const auto& edge : current->dependents) {
            auto* dependent = edge.mod;
            if (!dependent->active && !dependent->suspendedByProvider) {
                continue;
            }
            if (std::ranges::find(included, dependent) != included.end()) {
                continue;
            }
            included.push_back(dependent);
            pending.push_back(dependent);
        }
    }

    std::vector<LoadedMod*> ordered;
    ordered.reserve(included.size());
    for (auto& mod : mods()) {
        if (std::ranges::find(included, &mod) != included.end()) {
            ordered.push_back(&mod);
        }
    }
    return ordered;
}

bool ModLoader::reload_bundle(LoadedMod& mod) {
    log::write(mod.metadata.id, LOG_LEVEL_INFO, "reloading from {}",
        data::abbreviated_path_string(mod.modPath));
#if ALEKS2_NATIVE_MOD_LOADER
    if (auto* bootGuard = switchelf::boot_guard()) {
        bootGuard->release(mod.metadata.id);  // a reload is the player asking to try again
    }
#endif

    std::shared_ptr<ModBundle> newBundle;
    LoadedManifest newManifest;
    try {
        std::error_code ec;
        newBundle = load_bundle(mod.modPath, fs::is_directory(mod.modPath, ec));
        newManifest = load_manifest(mod.modPath, *newBundle);
    } catch (const std::exception& e) {
        fail_mod(mod, MOD_ERROR, fmt::format("Reload failed: {}", e.what()));
        return false;
    }

    if (newManifest.metadata.id != mod.metadata.id) {
        fail_mod(mod, MOD_CONFLICT,
            fmt::format(
                "Mod ID changed on reload ('{}'); restart required", newManifest.metadata.id));
        return false;
    }

    mod.metadata = std::move(newManifest.metadata);
    mod.runtime = std::move(newManifest.runtime);
    // In-flight readers of the old bundle keep it alive through their shared_ptr.
    mod.bundle = std::move(newBundle);
    mod.fileIdentity = file_identity(mod.modPath);
    mod.loadFailed = false;
    mod.failureReason.clear();

    ModManifestInfo newInfo;
    if (mod.runtime.has_value()) {
        const auto& runtime = *mod.runtime;
        newInfo.imports.push_back({runtime.id, runtime.major, runtime.minMinor, true});
    }
    if (!load_native_if_present(mod)) {
        return false;
    }
    if (mod.native) {
        newInfo = build_manifest_info(mod.native->parsed);
    } else {
        ++mod.cacheGeneration;
    }

    if (newInfo != mod.manifestInfo) {
        // The reload changes the mod's imports/exports; rebuild the dependency graph so edges,
        // init/tick/shutdown order and cascade sets reflect the new manifest.
        log::write(mod.metadata.id, LOG_LEVEL_INFO,
            "changed its service imports/exports; rebuilding mod dependency graph");
        mod.manifestInfo = std::move(newInfo);
        loader::sort_mods(m_mods);
    }

    return true;
}

void ModLoader::resume_lifecycle_set(const std::vector<LoadedMod*>& affected) {
    for (auto* mod : affected) {
        if (mod->active || mod->loadFailed || !mod->cvarIsEnabled->getValue()) {
            continue;
        }
        if (!ensure_native_loaded(*mod)) {
            continue;
        }
        if (mod->native && !mod->servicesRegistered) {
            if (register_static_service_exports(*mod)) {
                mod->servicesRegistered = true;
            } else {
                log::write(mod->metadata.id, LOG_LEVEL_ERROR, "failed to register service exports");
                deactivate_mod(*mod);
            }
        }
    }

    for (auto* mod : affected) {
        if (mod->active || mod->loadFailed || !mod->cvarIsEnabled->getValue()) {
            continue;
        }
        if (!required_deps_active(*mod)) {
            mod->suspendedByProvider = true;
            log::write(
                mod->metadata.id, LOG_LEVEL_INFO, "suspended: a required provider is disabled");
            continue;
        }
        mod->suspendedByProvider = false;
        activate_mod(*mod);
    }

    for (auto* mod : affected) {
        if (!mod->active && mod->servicesRegistered) {
            svc::remove_services_for_provider(*mod);
            mod->servicesRegistered = false;
        }
    }
}

void ModLoader::apply_lifecycle_change(
    LoadedMod& target, const bool reload, const PackageCandidate* replacement) {
    auto affected = collect_lifecycle_set(target);

    // Dependents first (reverse init order), like shutdown.
    for (auto* mod : affected | std::views::reverse) {
        const bool needsTeardown =
            mod->active ||
            (mod == &target && (mod->initialized || (reload && mod->native != nullptr)));
        if (!needsTeardown) {
            continue;
        }
        const bool wasActive = mod->active;
        log::write(mod->metadata.id, LOG_LEVEL_INFO, "deactivating mod");
        deactivate_mod(*mod);
        if (mod != &target && wasActive) {
            // Provisional; cleared below if the mod comes straight back up.
            mod->suspendedByProvider = true;
        }
    }

    if (replacement != nullptr) {
        target.modPath = replacement->path;
        target.searchDirIndex = replacement->searchDirIndex;
        target.fromDirectory = replacement->fromDirectory;
        target.nativeInPlace =
            replacement->fromDirectory && m_searchDirs[replacement->searchDirIndex].inPlaceNative;
        std::stable_sort(m_mods.begin(), m_mods.end(),
            [](const auto& a, const auto& b) { return a->searchDirIndex > b->searchDirIndex; });
        loader::sort_mods(m_mods);
    }

    if (reload) {
        // On failure the target is failed and stays down; dependents get resume attempts below
        // and suspend against the failed provider where required.
        reload_bundle(target);

        // The reload may have rebuilt the dependency graph and reordered m_mods; refresh the
        // set's iteration order so reactivation still runs providers first.
        std::vector<LoadedMod*> reordered;
        reordered.reserve(affected.size());
        for (auto& mod : mods()) {
            if (std::ranges::find(affected, &mod) != affected.end()) {
                reordered.push_back(&mod);
            }
        }
        affected = std::move(reordered);
    }

    resume_lifecycle_set(affected);
}

void ModLoader::on_enabled_changed(LoadedMod& mod) {
    svc::config_mark_dirty();
    if (mod.loadFailed) {
        if (!mod.cvarIsEnabled->getValue()) {
            mod.loadFailed = false;
            mod.failureReason.clear();
#if ALEKS2_NATIVE_MOD_LOADER
            // Turning it off acknowledges the crash: it is tried again when next loaded.
            if (auto* bootGuard = switchelf::boot_guard()) {
                bootGuard->release(mod.metadata.id);
            }
#endif
        }
        return;
    }
    if (mod.suspendedByProvider) {
        if (!mod.cvarIsEnabled->getValue()) {
            mod.suspendedByProvider = false;
        }
        return;
    }
    if (code_change_needs_restart(mod)) {
        // aleks2 (DM1-b): the setting is saved now; the code starts or stops at the next start.
        const bool wanted = mod.cvarIsEnabled->getValue();
        if (wanted == (mod.active || mod.enabledApplied)) {
            clear_restart(mod.metadata.id);
        } else {
            require_restart(mod.metadata.id,
                wanted ? "enabled; its code starts after a restart" : "disabled; its code stops after a restart");
        }
        return;
    }
    m_pendingRequests.push_back(LifecycleRequest{
        .modId = mod.metadata.id,
        .action =
            mod.cvarIsEnabled->getValue() ? LifecycleAction::Enable : LifecycleAction::Disable,
    });
}

void ModLoader::forget_mod(LoadedMod& mod) {
    auto affected = collect_lifecycle_set(mod);
    std::vector<LoadedMod*> blocked;
    std::vector<LoadedMod*> pending{&mod};
    while (!pending.empty()) {
        auto* provider = pending.back();
        pending.pop_back();
        for (const auto& edge : provider->dependents) {
            if (!edge.required || edge.mod == nullptr ||
                std::ranges::find(blocked, edge.mod) != blocked.end())
            {
                continue;
            }
            blocked.push_back(edge.mod);
            pending.push_back(edge.mod);
        }
    }

    for (auto* affectedMod : affected | std::views::reverse) {
        const bool wasActive = affectedMod->active;
        if (affectedMod->active || affectedMod->initialized || affectedMod->native != nullptr) {
            log::write(affectedMod->metadata.id, LOG_LEVEL_INFO, "deactivating mod");
            deactivate_mod(*affectedMod);
        }
        if (affectedMod != &mod && wasActive) {
            affectedMod->suspendedByProvider = true;
        }
    }

    const std::string modId = mod.metadata.id;
    auto* removedMod = &mod;
    if (mod.enabledSubscription != 0) {
        config::unsubscribe(mod.enabledSubscription);
        mod.enabledSubscription = 0;
    }
    unregister(*mod.cvarIsEnabled);

    std::vector<LoadedMod*> remaining;
    remaining.reserve(affected.size());
    for (auto* affectedMod : affected) {
        if (affectedMod != &mod) {
            remaining.push_back(affectedMod);
        }
    }
    std::erase_if(
        m_mods, [removedMod](const auto& candidate) { return candidate.get() == removedMod; });
    loader::sort_mods(m_mods);

    std::vector<LoadedMod*> resumable;
    for (auto* affectedMod : remaining) {
        const bool needsRemovedProvider = std::ranges::find(blocked, affectedMod) != blocked.end();
        affectedMod->suspendedByProvider = needsRemovedProvider;
        if (needsRemovedProvider) {
            log::write(affectedMod->metadata.id, LOG_LEVEL_INFO,
                "suspended: required provider '{}' was removed", modId);
        } else {
            resumable.push_back(affectedMod);
        }
    }
    resume_lifecycle_set(resumable);

    ++m_generation;
    log::write(modId, LOG_LEVEL_INFO, "forgot removed package");
}

ModLoader::OperationResult ModLoader::load_runtime_mod(const fs::path& requestedPath) {
    const auto path = data::absolute_path(requestedPath).lexically_normal();
    std::error_code error;
    const auto status = fs::status(path, error);
    if (error || !fs::exists(status)) {
        return {
            .success = false,
            .message = error ? fmt::format("Could not inspect the package: {}", error.message()) :
                               "The package was not found",
        };
    }

    const bool fromDir = fs::is_directory(status);
    std::unique_ptr<ModBundle> bundle;
    std::optional<ModMetadata> metadata;
    try {
        bundle = load_bundle(path, fromDir);
        metadata = load_manifest(path, *bundle).metadata;
    } catch (const std::exception& exception) {
        return {
            .success = false,
            .message = fmt::format("Invalid mod package: {}", exception.what()),
        };
    }
    if (const auto* duplicate = find_mod(metadata->id)) {
        return {
            .success = false,
            .message = fmt::format("A mod with this ID is already loaded from {}",
                data::abbreviated_path_string(duplicate->modPath)),
        };
    }
    auto* mod = try_load_mod(path, fromDir, 0, std::move(bundle));
    if (mod == nullptr) {
        return {
            .success = false,
            .message = "The mod could not be loaded",
        };
    }

    mod->enabledSubscription = Register(
        *mod->cvarIsEnabled, [this, mod](const bool&, const bool&) { on_enabled_changed(*mod); });
    loader::sort_mods(m_mods);
    if (!mod->cvarIsEnabled->getValue()) {
        mod->active = false;
        mod->suspendedByProvider = false;
        log::write(mod->metadata.id, LOG_LEVEL_INFO, "installed disabled by config");
    } else if (!mod->loadFailed) {
        mod->active = false;
        apply_lifecycle_change(*mod, false);
    }
    ++m_generation;
    log::write(mod->metadata.id, LOG_LEVEL_INFO, "installed at runtime");
    return runtime_result(*mod);
}

ModLoader::OperationResult ModLoader::reload_runtime_mod(
    LoadedMod& mod, const PackageCandidate* replacement) {
    if (mod.nativeInPlace && replacement == nullptr) {
        return {
            .success = false,
            .message = "An in-place native library cannot be reloaded",
            .mod = &mod,
        };
    }
    apply_lifecycle_change(mod, true, replacement);
    ++m_generation;
    return runtime_result(mod);
}

ModLoader::OperationResult ModLoader::uninstall_runtime_mod(LoadedMod& mod) {
    std::vector<PackageCandidate> packages;
    try {
        packages = scan_packages(m_searchDirs);
    } catch (const std::exception& exception) {
        return {.success = false, .message = exception.what()};
    }
    record_package_sources(mod, packages);
    if (!can_uninstall(mod)) {
        return {.success = false, .message = "No installed package to remove"};
    }

    std::string removalError;
    std::erase_if(packages, [&](const auto& package) {
        if (package.metadata.id != mod.metadata.id || package.searchDirIndex != 0 ||
            package.fromDirectory || package.symlink)
        {
            return false;
        }
        std::error_code error;
        // V1-rc12: the mod's own .dusk is open (its ModBundleZip reads from it while the mod is listed), and Horizon
        // does not delete an open file: a tester's uninstalled mods were still in mods/ and came back at the next
        // start. Close it first; reopen it if the file stays.
        const bool ownBundle = !mod.active && mod.bundle != nullptr && package.path == mod.modPath;
        if (ownBundle) {
            mod.bundle.reset();
        }
        fs::remove(package.path, error);
        {
            // aleks2 (V1-rc19): an update of it waiting for the next start goes too, or it would come back.
            std::error_code pendingError;
            fs::remove(fs::path{package.path.string() + ".pending"}, pendingError);
        }
        if (ownBundle && error) {
            mod.bundle = load_bundle(mod.modPath, false);
        }
        if (error) {
            removalError = fmt::format("Could not remove {}: {}",
                data::abbreviated_path_string(package.path), error.message());
        }
        return !error;
    });

    // V1-rc12: an uncommitted update of this mod leaves a journal entry and the previous package in <mods>/.backup; the
    // next start sees the installed package gone and puts the backup back (update_journal.hpp): the uninstalled mod
    // returned after a restart (tester, rc9). Drop both once the package is gone.
    if (removalError.empty()) {
        const auto userDir = user_mods_dir();
        if (!userDir.empty()) {
            for (const auto& entry : journal::read_all(userDir)) {
                if (entry.id == mod.metadata.id && !entry.backup.empty()) {
                    std::error_code ec;
                    fs::remove(entry.backup, ec);
                }
            }
            journal::remove(userDir, mod.metadata.id);
        }
    }

    OperationResult result;
    if (const auto* selected = select_package(packages, mod.metadata.id)) {
        if (selected->path != mod.modPath) {
            result = reload_runtime_mod(mod, selected);
        } else {
            result.mod = &mod;
            ++m_generation;
        }
        record_package_sources(mod, packages);
    } else {
        forget_mod(mod);
    }
    if (!removalError.empty()) {
        result.success = false;
        result.message = std::move(removalError);
    }
    return result;
}

ModLoader::OperationResult ModLoader::runtime_result(LoadedMod& mod) {
    if (mod.loadFailed) {
        return {
            .success = false,
            .message = mod.failureReason.empty() ? "Mod failed to activate" : mod.failureReason,
            .mod = &mod,
        };
    }
    if (mod.cvarIsEnabled->getValue() && !mod.active) {
        return {
            .success = false,
            .message = "A required provider is unavailable",
            .mod = &mod,
        };
    }
    if (!mod.cvarIsEnabled->getValue()) {
        return {
            .message = "Installed, disabled by config",
            .mod = &mod,
        };
    }
    return {.mod = &mod};
}

ModLoader::OperationResult ModLoader::install_staged(
    const fs::path& requestedPath, const std::optional<UpdatePrecondition>& update) {
    if (m_searchDirs.empty()) {
        return {
            .success = false,
            .message = "No writable mods directory is configured",
        };
    }
    std::error_code error;
    const auto userDir = data::canonical_path(m_searchDirs.front().path, error);
    if (error) {
        return {
            .success = false,
            .message =
                fmt::format("Could not resolve the user mods directory: {}", error.message()),
        };
    }
    const auto stagingDir = userDir / ".staging";
    const auto path = data::canonical_path(requestedPath, error);
    const bool stagedName = path.extension() == ".part" && path.stem().extension() == ".dusk";
    if (error || path.parent_path() != stagingDir || !stagedName ||
        !fs::is_regular_file(path, error))
    {
        Log.error("refusing staged install from {}", data::abbreviated_path_string(requestedPath));
        return {
            .success = false,
            .message = "The package is not in the mod staging directory",
        };
    }

    ModMetadata metadata;
    std::string validationError;
    if (!inspect_mod_bundle(path, metadata, validationError)) {
        return {
            .success = false,
            .message = fmt::format("Invalid mod package: {}", validationError),
        };
    }

    if (update) {
        if (metadata.id != update->modId || metadata.version != update->targetVersion) {
            return {.success = false, .message = "The update package identity changed"};
        }
        if (auto reason = updates::validate(*update); !reason.empty()) {
            return {.success = false, .message = std::move(reason)};
        }
    }

    const auto destination = userDir / fmt::format("{}.dusk", utils::safe_filename(metadata.id));
    auto* installed = find_mod(metadata.id);
    if (installed != nullptr && !can_update(*installed)) {
        return {
            .success = false,
            .message = "Cannot install mod over a development directory",
        };
    }

    for (const auto& mod : mods()) {
        if (mod.metadata.id != metadata.id && dusk::mods::same_path(mod.modPath, destination, error)) {
            // V1: say which mod and what to do (uninstall it), not only that the name is taken.
            Log.error("install of {} refused: {} is {}'s package", metadata.id, destination.filename().string(),
                mod.metadata.id);
            return {
                .success = false,
                .message = fmt::format("{} is already used by another mod ({}). Uninstall that mod first, then "
                                       "install this one again.",
                    destination.filename().string(), mod.metadata.name.empty() ? mod.metadata.id : mod.metadata.name),
            };
        }
    }
    error.clear();

    if (!borealis::update::parse_version(metadata.version)) {
        return {.success = false, .message = "The package version is invalid"};
    }
    std::vector<PackageCandidate> packages;
    try {
        packages = scan_packages(m_searchDirs);
    } catch (const std::exception& exception) {
        return {.success = false, .message = exception.what()};
    }
    if (const auto* selected = select_package(packages, metadata.id);
        selected && compare_package_versions(metadata.version, selected->metadata.version) < 0)
    {
        return {
            .success = false,
            .message = fmt::format(
                "A newer version ({}) is already installed", selected->metadata.version),
        };
    }
    // aleks2 (H38): the same classification for the package being installed as the scan gives an installed one, so the
    // hot path and the next start choose alike; an update with native code but no switch-arm64 library never replaces
    // a Switch copy that works (the file stays, nothing is swapped).
    PackageLibraries incomingLibraries;
    try {
        if (auto bundle = load_bundle(path, false)) {
            incomingLibraries = classify_package_libraries(*bundle);
        }
    } catch (const std::exception& e) {
        Log.warn("could not list the libraries of {}: {}", data::abbreviated_path_string(path), e.what());
    }
    {
        // V1-rc5: only a copy at least as new as the incoming package can refuse it. A newer catalog version of a
        // mod this build ships a Switch package of (Twilit Essentials 2.8.4 over our 2.5.0) installs, and the older
        // Switch copy is set aside below so the scan does not keep preferring it.
        std::vector<PackageLibraries> existing;
        for (const auto& package : packages) {
            if (package.metadata.id == metadata.id &&
                compare_package_versions(package.metadata.version, metadata.version) >= 0)
            {
                existing.push_back(package.libraries);
            }
        }
        if (refuses_non_switch_update(incomingLibraries, existing, prefers_switch_packages())) {
            Log.warn("refusing {} v{}: no switch-arm64 library and a Switch copy is installed", metadata.id,
                metadata.version);
            return {.success = false, .message = std::string{k_nonSwitchUpdateMessage}};
        }
    }

#if ALEKS2_NATIVE_MOD_LOADER
    // aleks2 (H37): on Switch a package is installed only if its native code would load on this
    // build -- the Dry Run of its library, nothing run -- so the browser never says "installed"
    // for a mod that cannot start. A package with code for other platforms only is refused with
    // that reason.
    try {
        if (auto bundle = load_bundle(path, false)) {
            if (auto why = switchelf::check_package(*bundle, metadata.id)) {
                return {.success = false, .message = "Not installed: " + *why};
            }
        }
    } catch (const std::exception& e) {
        return {.success = false, .message = fmt::format("Invalid mod package: {}", e.what())};
    }
    // An update keeps the version it replaces, to put back if the new one does not load.
    fs::path backup;
    if (installed != nullptr && fs::is_regular_file(destination, error)) {
        backup = userDir / ".backup" / fmt::format("{}.dusk", utils::safe_filename(metadata.id));
        fs::create_directories(backup.parent_path(), error);
        if (error || !file_copy::copy_file(destination, backup, error)) {  // aleks2 (rc18): file_copy.hpp
            return {.success = false,
                .message = fmt::format("Could not back up the installed version: {}", error.message())};
        }
    }
    error.clear();
    // aleks2 (DM1-b): a package with native code is only put in place; it runs after a restart, and
    // the journal undoes it if that start fails (update_journal.hpp). Asset and Luau packages keep
    // the live path below.
    if (m_startupComplete &&
        (incomingLibraries.native() || (installed != nullptr && code_change_needs_restart(*installed))))
    {
        const bool journaled = installed != nullptr && !backup.empty();
        if (journaled) {
            std::string journalError;
            if (!journal::write(userDir,
                    {.id = metadata.id, .fromVersion = installed->metadata.version, .toVersion = metadata.version,
                        .destination = destination, .backup = backup},
                    journalError))
            {
                return {.success = false,
                    .message = fmt::format("Not installed: could not record the update ({})", journalError)};
            }
        }
        const auto deferred = install_package(path, destination, metadata.id);
        if (!deferred.replaced) {
            if (journaled) {
                journal::remove(userDir, metadata.id);
            }
            return {.success = false, .message = deferred.error};
        }
        // V1-rc5: older Switch copies of this mod under other file names (randomizer.dusk, twilit_essentials.dusk from
        // the release) would still be chosen over a newer Linux package; they move to mods/.replaced/, where the
        // player can take one back.
        if (!incomingLibraries.switchArm64) {
            for (const auto& package : packages) {
                if (package.metadata.id != metadata.id || !package.libraries.switchArm64 || package.searchDirIndex != 0 ||
                    package.fromDirectory || package.symlink || dusk::mods::same_path(package.path, destination, error))
                {
                    error.clear();
                    continue;
                }
                const auto aside = userDir / ".replaced" / package.path.filename();
                fs::create_directories(aside.parent_path(), error);
                fs::rename(package.path, aside, error);
                if (error) {
                    Log.warn("{}: could not set aside the older Switch package {}: {}", metadata.id,
                        data::abbreviated_path_string(package.path), error.message());
                    error.clear();
                } else {
                    Log.info("{}: the older Switch package v{} moved to {} (the catalog's v{} replaces it)", metadata.id,
                        package.metadata.version, data::abbreviated_path_string(aside), metadata.version);
                }
            }
        }
        Log.info("{} v{} installed for the next start ({}{}); the running code is unchanged", metadata.id,
            metadata.version,
            incomingLibraries.switchArm64      ? "switch-arm64" :
            incomingLibraries.linuxAarch64     ? "linux-aarch64 compatibility" :
                                                 "no Switch library",
            journaled ? fmt::format(", replaces v{}", installed->metadata.version) : std::string{});
        auto message = fmt::format("v{} installed; restart the game to run it", metadata.version);
        // DM1-c: remembered so the browser and the updates list stop offering what is already on the card.
        if (const auto staged = std::ranges::find_if(
                m_stagedVersions, [&](const auto& entry) { return entry.first == metadata.id; });
            staged != m_stagedVersions.end())
        {
            staged->second = metadata.version;
        } else {
            m_stagedVersions.emplace_back(metadata.id, metadata.version);
        }
        require_restart(metadata.id, message);
        return {.success = true, .message = std::move(message)};
    }
#endif
    const auto packageResult = install_package(path, destination, metadata.id);
    if (!packageResult.replaced) {
        return {.success = false, .message = packageResult.error};
    }
    if (packageResult.pending) {
        // aleks2 (V1-rc19): the installed package was open; the new one is put in place at the next start.
        std::string message = "updated; it applies after a restart";
        require_restart(metadata.id, message);
        return {.success = true, .message = std::move(message)};
    }
    std::erase_if(packages, [&](const auto& package) {
        if (package.metadata.id != metadata.id || package.searchDirIndex != 0 ||
            package.fromDirectory)
        {
            return false;
        }
        std::error_code statusError;
        return dusk::mods::same_path(package.path, destination, statusError) ||
               !fs::exists(package.path, statusError);
    });
    packages.push_back({.path = destination, .metadata = metadata, .libraries = incomingLibraries});
    const auto* selected = select_package(packages, metadata.id);
    auto result = installed != nullptr ? reload_runtime_mod(*installed, selected) :
                                         load_runtime_mod(selected->path);
    if (result.mod != nullptr) {
        record_package_sources(*result.mod, packages);
    }

    if (result.success && !packageResult.error.empty()) {
        result.success = false;
        result.message = packageResult.error;
    }
#if ALEKS2_NATIVE_MOD_LOADER
    if (!result.success && !backup.empty() && installed != nullptr) {
        // The update did not load: the previous version goes back, and is loaded again.
        std::string restoreError;
        const auto restoring = userDir / ".staging" / fmt::format("{}-restore.dusk.part", utils::safe_filename(metadata.id));
        fs::create_directories(restoring.parent_path(), error);
        if (file_copy::copy_file(backup, restoring, error) &&  // aleks2 (rc18): file_copy.hpp
            borealis::io::atomic_replace(restoring, destination, restoreError))
        {
            const std::string why = result.message.empty() ? "it did not load" : result.message;
            // The file is back; whether the previous version runs again is the reload's result,
            // which can fail too (H37-c2: it was ignored, and the message said "restored" anyway).
            ModMetadata previous;
            if (!inspect_mod_bundle(destination, previous, restoreError)) {
                result.message = fmt::format("Update not applied ({}); the previous version was put back but "
                                             "cannot be read ({})", why, restoreError);
            } else {
                packages.back().metadata = previous;
                packages.back().libraries = {};
                try {
                    if (auto restoredBundle = load_bundle(destination, false)) {
                        packages.back().libraries = classify_package_libraries(*restoredBundle);
                    }
                } catch (const std::exception&) {
                }
                const auto restored = reload_runtime_mod(*installed, select_package(packages, metadata.id));
                if (restored.mod != nullptr) {
                    record_package_sources(*restored.mod, packages);
                }
                result.message = restored.success ?
                    fmt::format("Update not applied ({}); the previous version was restored", why) :
                    fmt::format("Update not applied ({}); the previous version was put back but did not load "
                                "either ({})", why, restored.message);
            }
        } else {
            result.message += fmt::format(" -- and the previous version could not be restored ({}); it is "
                                          "kept at {}",
                restoreError.empty() ? error.message() : restoreError, data::abbreviated_path_string(backup));
        }
    }
#endif
    return result;
}

void ModLoader::apply_pending_requests() {
    // Images retired by the previous tick have had a full frame to unwind off the stack.
    drain_retired_natives();

    if (m_pendingRequests.empty()) {
        return;
    }

    const auto requests = std::exchange(m_pendingRequests, {});
    std::vector<LifecycleRequest> coalesced;
    for (const auto& request : requests) {
        if (const auto* install = std::get_if<InstallRequest>(&request)) {
            auto result = install_staged(install->stagedPath, install->update);
            if (result.success && result.mod != nullptr) {
                const auto& metadata = result.mod->metadata;
                const std::string iconRml =
                    metadata.iconPath.empty() ?
                        std::string{} :
                        fmt::format(R"(<img src="{}" />)",
                            ui::escape(ui::mod_image_source(*result.mod, metadata.iconPath)));
                ui::push_toast({
                    .type = "mod-installed",
                    .title = "Mod installed",
                    .content = fmt::format(
                        R"(<row><mod-icon>{}</mod-icon><mod-info><mod-name><b>{}</b><small class="version">v{}</small></mod-name><small>{}</small></mod-info></row>)",
                        iconRml, ui::escape(metadata.name), ui::escape(metadata.version),
                        ui::escape(metadata.author)),
                    .duration = std::chrono::seconds{4},
                });
            } else if (!result.success && result.mod == nullptr) {
                ui::push_toast({
                    .type = "warning",
                    .title = "Mod install failed",
                    .content =
                        result.message.empty() ? "The loader rejected the package" : result.message,
                    .duration = std::chrono::seconds{6},
                });
            }
            if (!result.success && !m_searchDirs.empty()) {
                // request_install owns only files under the configured staging directory.
                std::error_code error;
                const auto userDir = data::canonical_path(m_searchDirs.front().path, error);
                if (!error) {
                    const auto stagedPath = data::canonical_path(install->stagedPath, error);
                    if (!error && stagedPath.parent_path() == userDir / ".staging") {
                        fs::remove(stagedPath, error);
                    }
                }
            }
            complete_operation(install->operation, result.success, std::move(result.message));
            continue;
        }
        if (const auto* reload = std::get_if<ReloadRequest>(&request)) {
            auto* mod = find_mod(reload->modId);
            if (mod == nullptr) {
                complete_operation(reload->operation, false, "The mod is no longer installed");
                continue;
            }
            if (code_change_needs_restart(*mod)) {
                complete_operation(reload->operation, false,
                    "A mod with native code is reloaded by restarting the game");
                continue;
            }
            auto result = reload_runtime_mod(*mod);
            complete_operation(reload->operation, result.success, std::move(result.message));
            continue;
        }
        if (const auto* uninstall = std::get_if<UninstallRequest>(&request)) {
            auto* mod = find_mod(uninstall->modId);
            if (mod == nullptr) {
                complete_operation(uninstall->operation);
                continue;
            }
            if (code_change_needs_restart(*mod) && (mod->active || mod->initialized)) {
                complete_operation(uninstall->operation, false,
                    "Its native code is running: turn the mod off, restart, then uninstall it");
                continue;
            }
            const auto removedName = mod->metadata.name;
            const auto removedId = mod->metadata.id;
            auto result = uninstall_runtime_mod(*mod);
            if (result.success) {
                queue::remove_by_mod_id(removedId);
                ui::push_toast({
                    .title = result.mod != nullptr ? "User update removed" : "Mod uninstalled",
                    .content = removedName,
                    .duration = std::chrono::seconds{2},
                });
            }
            complete_operation(uninstall->operation, result.success, std::move(result.message));
            continue;
        }

        const auto& lifecycle = std::get<LifecycleRequest>(request);
        const auto existing =
            std::ranges::find(coalesced, lifecycle.modId, &LifecycleRequest::modId);
        if (existing != coalesced.end()) {
            complete_operation(
                existing->operation, false, "Superseded by a newer lifecycle request");
            *existing = lifecycle;
        } else {
            coalesced.push_back(lifecycle);
        }
    }

    for (const auto& request : coalesced) {
        auto* mod = find_mod(request.modId);
        if (mod == nullptr) {
            Log.warn("lifecycle request for unknown mod '{}'", request.modId);
            complete_operation(request.operation, false, "The mod is no longer installed");
            continue;
        }
        if (request.action == LifecycleAction::Enable && mod->enabledApplied) {
            continue;
        }
        if (request.action == LifecycleAction::Disable && !mod->enabledApplied && !mod->active) {
            continue;
        }

        if (request.action == LifecycleAction::Reactivate && code_change_needs_restart(*mod)) {
#if ALEKS2_NATIVE_MOD_LOADER
            if (auto* bootGuard = switchelf::boot_guard()) {
                bootGuard->release(mod->metadata.id);
            }
#endif
            if (!mod->cvarIsEnabled->getValue()) {
                mod->cvarIsEnabled->setValue(true);
            }
            require_restart(mod->metadata.id, "it is tried again after a restart");
            complete_operation(request.operation);
            continue;
        }
        if (request.action == LifecycleAction::Reactivate) {
            mod->loadFailed = false;
            mod->failureReason.clear();
            mod->suspendedByProvider = false;
            if (!mod->cvarIsEnabled->getValue()) {
                mod->cvarIsEnabled->setValue(true);
            }
        }
        apply_lifecycle_change(*mod, false);

        if (request.action == LifecycleAction::Reactivate) {
            std::string error;
            if (mod->loadFailed) {
                error =
                    mod->failureReason.empty() ? "The mod failed to activate" : mod->failureReason;
            } else if (mod->cvarIsEnabled->getValue() && !mod->active) {
                error = "A required provider is unavailable";
            }
            complete_operation(request.operation, error.empty(), std::move(error));
        }
    }

    svc::modules_lifecycle_applied();

    auto active = std::ranges::count_if(mods(), [](const LoadedMod& m) { return m.active; });
    Log.info("{}/{} mod(s) active", active, m_mods.size());
#if defined(__SWITCH__) && DUSK_HAS_FUNCHOOK
    // aleks2 (H37): how many kernel code memory objects the hooks took (code_arena.h).
    char arenas[512];
    funchook_switch_describe(arenas, sizeof arenas);
    Log.info("{}", arenas);
#endif
}

void ModLoader::tick() {
    {
        std::vector<ThreadFailure> failures;
        {
            std::lock_guard lock{m_threadFailuresMutex};
            failures.swap(m_threadFailures);
        }
        for (const auto& failure : failures) {
            if (auto* mod = find_mod(failure.modId)) {
                fail_mod(*mod, failure.code, failure.message);
            }
        }
    }
    svc::modules_frame_begin();
    apply_pending_requests();
#if ALEKS2_NATIVE_MOD_LOADER
    // aleks2 (H37): ten seconds of frames after the mods started, the boot is over: a crash
    // after that is not a boot loop (boot_guard.hpp).
    const double nowSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (m_startupComplete && m_startupCompleteAt == 0.0) {
        m_startupCompleteAt = nowSeconds;
    }
    if (m_startupComplete && m_bootTicks < kBootTicks && m_bootTicks >= 60 &&
        nowSeconds - m_startupCompleteAt >= kBootSeconds)
    {
        m_bootTicks = kBootTicks - 1;  // ends below, as after 600 frames
    }
    if (m_startupComplete && m_bootTicks < kBootTicks && ++m_bootTicks == kBootTicks) {
        if (auto* bootGuard = switchelf::boot_guard()) {
            bootGuard->end_boot();
        }
        settle_update_journal();
    }
#endif

    for (auto& mod : mods()) {
        if (!mod.active || (!mod.native && !mod.runtime.has_value())) {
            continue;
        }
        try {
            ModError error = MOD_ERROR_INIT;
            const bool delegated = !mod.native;
            const auto updateStart = std::chrono::steady_clock::now();
            const auto result = delegated ?
                                    mod.runtime->service->update(
                                        mod.runtime->providerContext, mod.context.get(), &error) :
                                    mod.native->fn_update(&error);
            svc::cpu_profile_add(mod.context.get(),
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - updateStart)
                                          .count()));
            if (result != MOD_OK) {
                fail_mod(mod, result,
                    lifecycle_error_message(
                        delegated ? "runtime update" : "mod_update", result, error));
            }
        } catch (const std::exception& e) {
            fail_mod(mod, MOD_ERROR, fmt::format("Exception in mod update: {}", e.what()));
        } catch (...) {
            fail_mod(mod, MOD_ERROR, "Unknown exception in mod_update");
        }
    }

    svc::modules_frame_end();
    flush_toasts();

    // V1-rc6: every 900 frames (~30 s), the mods' share of the game thread (hook callbacks + mod_update).
    static uint32_t s_profileFrames = 0;
    if (s_profileFrames == 0) {
        svc::cpu_profile_set_thread();
    }
    if (++s_profileFrames >= 900) {
        auto entries = svc::cpu_profile_take();
        std::ranges::sort(entries, std::greater{}, &svc::CpuProfileEntry::nanoseconds);
        std::string line;
        uint64_t total = 0;
        for (const auto& entry : entries) {
            total += entry.nanoseconds;
            const char* id = "?";
            for (const auto& mod : mods()) {
                if (mod.context.get() == entry.modContext) {
                    id = mod.metadata.id.c_str();
                    break;
                }
            }
            if (line.size() < 400) {
                line += fmt::format("{}{} {:.2f}", line.empty() ? "" : ", ", id,
                    static_cast<double>(entry.nanoseconds) / 1e6 / s_profileFrames);
            }
        }
        Log.info("mods cpu per frame (ms, game thread): total {:.2f} | {}",
            static_cast<double>(total) / 1e6 / s_profileFrames, line.empty() ? "none" : line);
        // V1-rc9: the hook dispatch (ours) against the mods' callbacks in it (theirs); timing sampled 1 in 64.
        const auto hooks = svc::hook_stats_take(s_profileFrames);
        if (hooks.targets != 0) {
            Log.info("hooks per frame (game thread): {:.0f} calls on {} targets, dispatch {:.2f} ms (callbacks "
                     "{:.2f} ms, dispatcher {:.2f} ms) | top: {}",
                hooks.callsPerFrame, hooks.targets, hooks.dispatchMsPerFrame, hooks.callbacksMsPerFrame,
                hooks.dispatchMsPerFrame - hooks.callbacksMsPerFrame, hooks.top);
            if (!hooks.topCallbacks.empty()) {
                Log.info("hooks: top target's callbacks (ms/frame): {}", hooks.topCallbacks);  // V1-rc16
            }
        }
        s_profileFrames = 1;
    }
}

void ModLoader::shutdown() {
#if defined(__SWITCH__) && ALEKS2_NATIVE_MOD_LOADER
    {
        // DM1-b: again at exit -- Dawn starts its workers lazily, often after the boot log line.
        namespace w = aleks2::dawn_worker;
        Log.info("dawn workers at exit: {} thread(s) with a {} KiB stack (default {} KiB); {} fell back",
            w::threads.load(), w::stackSize.load() / 1024, w::stackDefault.load() / 1024, w::fallbacks.load());
    }
#endif
    // Reverse initialization order, so importers shut down before their service providers.
    for (auto& mod : mods() | std::views::reverse) {
        deactivate_mod(mod);
        if (mod.enabledSubscription != 0) {
            config::unsubscribe(mod.enabledSubscription);
            mod.enabledSubscription = 0;
        }
        unregister(*mod.cvarIsEnabled);
    }

    m_mods.clear();
    drain_retired_natives();
    svc::modules_shutdown();
    Log.info("all mods unloaded");
}

}  // namespace dusk::mods
