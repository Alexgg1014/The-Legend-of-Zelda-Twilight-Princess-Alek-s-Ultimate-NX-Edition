// aleks2: NativeModule over the built-in mod table. Replaces
// mods/loader/native_module.cpp on Switch, which is why that file is excluded
// from this build and stays byte-identical to upstream. See
// switch_builtin_mods.hpp for why any of this is necessary.
#include "dusk/switch_builtin_mods.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "dusk/logging.h"
#include "dusk/mods/loader/native_module.hpp"
#if ALEKS2_NATIVE_MOD_LOADER
#include "dusk/switch_native_loader.hpp"
#endif

namespace dusk::mods::loader {

const BuiltinMod* builtin_mod(const std::string_view id) {
    if (id.empty()) {
        return nullptr;
    }
    for (const BuiltinMod* mod = detail::kBuiltinMods; mod->id != nullptr; mod++) {
        if (id == mod->id) {
            return mod;
        }
    }
    return nullptr;
}

const BuiltinSymbol* builtin_symbol(const std::string_view name) {
    return detail::find_builtin_symbol(detail::kBuiltinSymbols, name);
}

std::filesystem::path builtin_mod_path(const std::string_view id) {
    // Deliberately not a real location: a built-in mod has no file, and the
    // path exists so logs and the mod browser can say where the code came from.
    // NativeModule reads the id back out of the filename.
    return std::filesystem::path{"<built into dusklight.nro>"} / std::filesystem::path{std::string{id}};
}

namespace {
// aleks2 (H37): what `handle` points at -- a mod compiled into the NRO, or one loaded from the
// card by switch_native_loader.cpp.
struct SwitchModule {
    const BuiltinMod* builtin = nullptr;
#if ALEKS2_NATIVE_MOD_LOADER
    std::unique_ptr<switchelf::Module> loaded;
#endif
};
}  // namespace

NativeModule::NativeModule() noexcept : handle(nullptr) {}

NativeModule::NativeModule(NativeModule&& other) noexcept {
    handle = other.handle;
    other.handle = nullptr;
}

NativeModule& NativeModule::operator=(NativeModule&& other) noexcept {
    if (this != &other) {
        delete static_cast<SwitchModule*>(handle);
        handle = other.handle;
        other.handle = nullptr;
    }
    return *this;
}

NativeModule::NativeModule(const std::filesystem::path& path) : handle(nullptr) {
    const std::string id = path.filename().string();
    const BuiltinMod* mod = builtin_mod(id);
    if (mod == nullptr) {
#if ALEKS2_NATIVE_MOD_LOADER
        // Extracted by natives.cpp to <cache>/<mod id>/g<N>/mod.so.
        const std::string modId = path.parent_path().parent_path().filename().string();
        auto module = std::make_unique<SwitchModule>();
        module->loaded = switchelf::load(path, modId);  // throws a runtime_error with the reason
        handle = module.release();
        return;
#else
        // Either a mod that ships a library for another platform, or one built
        // for a Switch build that did not include it. Both are the same to the
        // player, so say what can be done about it.
        throw std::runtime_error(
            "code mods on Switch have to be compiled into the build; '" + id +
            "' is not part of this one");
#endif
    }

    // aleks2: loud on purpose. A built-in mod runs inside the game's own
    // process, so when one goes wrong it takes everything with it and the log
    // is all that is left -- and the log used to stop at "found <mod>", which
    // said nothing about which of them, or how far it got.
    DuskLog.info("Built-in mod '{}': meta={:p} ctx={:p} init={:p} update={:p} shutdown={:p}", id,
        mod->meta, static_cast<const void*>(mod->context), static_cast<const void*>(mod->initialize),
        static_cast<const void*>(mod->update), static_cast<const void*>(mod->shutdown));
    if (mod->meta == nullptr || mod->context == nullptr || mod->initialize == nullptr ||
        mod->update == nullptr || mod->shutdown == nullptr)
    {
        throw std::runtime_error("'" + id + "' is compiled in but one of its entry points is null");
    }
    handle = new SwitchModule{mod};
}

// A loaded module's mapping outlives this object on purpose (switch_native_loader.cpp).
NativeModule::~NativeModule() {
    delete static_cast<SwitchModule*>(handle);
}

bool NativeModule::RunDeferredInitializers(std::string& error) {
#if ALEKS2_NATIVE_MOD_LOADER
    if (auto* module = static_cast<SwitchModule*>(handle); module != nullptr && module->loaded) {
        return module->loaded->run_constructors(error);
    }
#endif
    (void)error;
    return true;  // a built-in mod's constructors ran with the game's
}

void* NativeModule::LookupSymbol(const char* name) const {
    if (handle == nullptr || name == nullptr) {
        return nullptr;
    }
    const auto* module = static_cast<const SwitchModule*>(handle);
#if ALEKS2_NATIVE_MOD_LOADER
    if (module->loaded) {
        return module->loaded->lookup(name);
    }
#endif
    const auto* mod = module->builtin;
    const std::string_view symbol{name};
    if (symbol == "mod_meta") {
        return const_cast<void*>(mod->meta);
    }
    if (symbol == "mod_ctx") {
        return mod->context;
    }
    if (symbol == "mod_initialize") {
        return mod->initialize;
    }
    if (symbol == "mod_update") {
        return mod->update;
    }
    if (symbol == "mod_shutdown") {
        return mod->shutdown;
    }
    // Upstream looks up nothing else; a miss is reported as a missing export.
    return nullptr;
}

}  // namespace dusk::mods::loader
