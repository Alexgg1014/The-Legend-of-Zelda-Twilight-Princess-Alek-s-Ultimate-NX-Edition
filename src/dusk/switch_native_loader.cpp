// aleks2 (H37): see switch_native_loader.hpp.
#include "dusk/switch_native_loader.hpp"

#include "dusk/config.hpp"
#include "dusk/config_var.hpp"
#include "dusk/mods/loader/elf_loader.hpp"
#include "dusk/mods/loader/elf_runtime_policy.hpp"
#include "dusk/mods/loader/image_symbols.hpp"
#include "dusk/switch_elf_memory.hpp"
#include "dusk/mods/loader/loader.hpp"
#include "dusk/mods/loader/mod_services.hpp"
#include "dusk/mods/svc/registry.hpp"

#include <borealis/log.hpp>

#include <cstdio>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

// libnx's crt0: the first byte of the image, and its MOD0 header, which says where the
// image's own dynamic section is (crt0 relocates the image through it too).
extern "C" char _start[];
extern "C" const struct NxMod0 {
    uint32_t magic;
    int32_t dynOffset;
} __nx_mod0;
// libgcc: makes a module's unwind tables known, so C++ exceptions can cross its frames.
extern "C" void __register_frame(void* begin);

namespace dusk::switchelf {
namespace fs = std::filesystem;
namespace elf = mods::elf;
namespace guard = mods::guard;

namespace {

constexpr borealis::Log Log{"aleks2::native"};

fs::path s_modsDir;
std::unique_ptr<guard::BootGuard> s_guard;
guard::BootGuard::Startup s_startup;

config::ConfigVar<s32>& mode_var() {
    // Registered on first use; the config system restores a persisted value registered late.
    static config::ConfigVar<s32> s_mode{"aleks2.nativeLoaderMode", static_cast<s32>(LoaderMode::DryRun)};
    static const bool s_registered = [] {
        config::Register(s_mode);
        return true;
    }();
    (void)s_registered;
    return s_mode;
}

// The game image's exports: its own .dynsym, through its own hash tables. Only what the build's
// --dynamic-list exported is there (ModAbiExports.cmake), which is the allowlist.
const elf::ImageSymbols* game_symbols() {
    static const std::optional<elf::ImageSymbols> s_symbols = [] {
        std::string error;
        const auto* dynamic = reinterpret_cast<const char*>(&__nx_mod0) + __nx_mod0.dynOffset;
        auto table = elf::ImageSymbols::from_dynamic(reinterpret_cast<uintptr_t>(_start), dynamic, error);
        if (!table) {
            Log.error("the game image's exports cannot be read: {}", error);
        } else {
            Log.info("game image exports {} symbol(s) to native mods", table->size());
        }
        return table;
    }();
    return s_symbols ? &*s_symbols : nullptr;
}

std::vector<uint8_t> read_file(const fs::path& path) {
    std::vector<uint8_t> data;
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (f == nullptr) {
        return data;
    }
    uint8_t chunk[64 * 1024];
    for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) {
        data.insert(data.end(), chunk, chunk + n);
    }
    std::fclose(f);
    return data;
}

void write_report(std::string_view modId, const std::string& json) {
    if (s_modsDir.empty()) {
        return;
    }
    std::error_code ec;
    const fs::path dir = s_modsDir / ".reports";
    fs::create_directories(dir, ec);
    const fs::path path = dir / (std::string(modId) + ".json");
    if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
        std::fwrite(json.data(), 1, json.size(), f);
        std::fputc('\n', f);
        std::fclose(f);
    }
}

// A Linux-built package names glibc's libraries or carries glibc symbol versions; one built for
// Switch against newlib has neither.
elf::PackageAbi package_abi(const elf::Image& image) {
    if (!image.needed.empty()) {
        return elf::PackageAbi::Linux;
    }
    for (const auto& symbol : image.symbols) {
        if (!symbol.version.empty()) {
            return elf::PackageAbi::Linux;
        }
    }
    return elf::PackageAbi::Switch;
}

[[noreturn]] void refuse(std::string_view modId, guard::Category category, const std::string& message,
    bool incident = true) {
    Log.error("{}: {}", modId, message);
    if (incident && s_guard) {
        s_guard->report({std::string(modId), category, std::string(guard::stage_code(guard::Stage::Load)), message});
    }
    throw LoadError(category, message);
}

class ElfModule final : public Module {
public:
    ElfModule(std::unique_ptr<CodeMemory> memory, elf::Loaded loaded)
        : m_memory(std::move(memory)), m_loaded(std::move(loaded)) {}

    // Nothing is unmapped: the module may still be on a stack or in a callback table. The
    // CodeMemory object owns no mapping it would undo.
    ~ElfModule() override { (void)m_memory.release(); }

    void* lookup(std::string_view name) const override {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(m_loaded.find(name)));
    }

    bool run_constructors(std::string& error) override {
        if (m_constructed) {
            return true;
        }
        m_constructed = true;  // once per mapping, whatever happens
        try {
            elf::run_constructors(m_loaded);
        } catch (const std::exception& e) {
            error = std::string("a constructor threw: ") + e.what();
            return false;
        } catch (...) {
            error = "a constructor threw";
            return false;
        }
        return true;
    }

private:
    std::unique_ptr<CodeMemory> m_memory;
    elf::Loaded m_loaded;
    bool m_constructed = false;
};

}  // namespace

LoaderMode loader_mode() {
    const s32 value = mode_var().getValue();
    return value >= 0 && value <= 2 ? static_cast<LoaderMode>(value) : LoaderMode::DryRun;
}

void set_loader_mode(LoaderMode mode) {
    mode_var().setValue(static_cast<s32>(mode));
    config::save();
}

std::string_view loader_mode_name(LoaderMode mode) {
    switch (mode) {
    case LoaderMode::Off:
        return "Off";
    case LoaderMode::DryRun:
        return "Dry Run";
    case LoaderMode::Execute:
        return "Execute";
    }
    return "?";
}

guard::BootGuard::Startup start(const fs::path& modsDirectory) {
    s_modsDir = modsDirectory;
    s_guard = std::make_unique<guard::BootGuard>(modsDirectory / ".guard");
    s_startup = s_guard->begin_boot();
    if (s_startup.crashed) {
        Log.error("the previous boot crashed in '{}' ({} step); it is disabled until you enable it again",
            s_startup.crashed->modId, s_startup.crashed->stage);
    } else if (s_startup.unattributedCrash) {
        Log.warn("the previous boot did not finish starting; no mod was running new code at the time");
    }
    if (s_startup.withoutMods) {
        Log.warn("starting without mods: {}", s_startup.withoutModsReason);
    }
    Log.info("native mod loader: {} (boot {})", loader_mode_name(loader_mode()), s_startup.boot);
    return s_startup;
}

guard::BootGuard* boot_guard() {
    return s_guard.get();
}

const guard::BootGuard::Startup& startup() {
    return s_startup;
}

size_t exported_symbol_count() {
    const auto* symbols = game_symbols();
    return symbols != nullptr ? symbols->size() : 0;
}

namespace {

// Validation and planning against this build: everything Dry Run does. On success `parsed` holds
// the image and `plan` its plan; otherwise `why` and `category` say what refuses it.
struct Checked {
    elf::ParseResult parsed;
    std::optional<elf::Plan> plan;
    elf::PackageAbi abi = elf::PackageAbi::Switch;
    std::string why;
    guard::Category category = guard::Category::Validation;
    bool ok() const { return why.empty(); }
};

Checked check(std::span<const uint8_t> file, std::string_view modId, const char* modeName) {
    Checked c;
    c.parsed = elf::parse(file);
    if (!c.parsed.image) {
        write_report(modId, elf::report_json(modId, modeName, c.parsed, nullptr));
        c.category = guard::category_for_load_error(elf::error_code(c.parsed.diagnostic.error));
        c.why = "not loadable: " + c.parsed.diagnostic.message;
        return c;
    }
    const auto* game = game_symbols();
    if (game == nullptr) {
        c.why = "this build's exported symbols cannot be read";
        return c;
    }
    c.abi = package_abi(*c.parsed.image);
    c.plan = elf::plan(*c.parsed.image,
        elf::make_resolver(c.abi,
            [game](std::string_view name) -> std::optional<uint64_t> {
                const uintptr_t address = game->find(name);
                return address != 0 ? std::optional<uint64_t>{address} : std::nullopt;
            }),
        elf::make_needed_policy(c.abi));
    std::string report = elf::report_json(modId, modeName, c.parsed, &*c.plan);
    if (!c.plan->ok()) {
        write_report(modId, report);
        const auto& first = c.plan->problems.front();
        c.category = guard::category_for_load_error(elf::error_code(first.error));
        c.why = first.message;
        if (c.plan->problems.size() > 1) {
            c.why += " (and " + std::to_string(c.plan->problems.size() - 1) + " more problem(s); see the report)";
        }
        return c;
    }

    // aleks2 (H37-c4): the game's services the mod imports, judged against this build, from the
    // file alone -- nothing mapped, no mod code run. Dry Run used to say "compatible" and Execute
    // then failed the mod at activation with "Mod requires a service that is unavailable".
    const auto scan = elf::scan_service_imports(file);
    if (!scan.ok()) {
        write_report(modId, report);
        c.why = "cannot read its service imports: " + scan.error;
        return c;
    }
    const auto problems = elf::check_services(scan.imports, [](std::string_view id) {
        std::vector<elf::HostService> known;
        for (const auto& version : mods::svc::host_service_versions(id)) {
            known.push_back({version.major, version.minor, version.available});
        }
        return known;
    });
    write_report(modId, elf::annotate_report(std::move(report), scan.imports, problems));
    for (const auto& problem : problems) {
        if (!problem.blocking) {
            Log.info("{}: optional service not available: {}", modId, problem.message);
        }
    }
    c.why = elf::first_blocking(problems);
    return c;
}

const char* abi_name(elf::PackageAbi abi) {
    return abi == elf::PackageAbi::Linux ? "linux-aarch64, compatibility mode" : "switch-arm64";
}

}  // namespace

std::unique_ptr<Module> load(const fs::path& library, std::string_view modId) {
    const LoaderMode mode = loader_mode();
    if (mode == LoaderMode::Off) {
        refuse(modId, guard::Category::Validation,
            "native mods are off (Diagnostics > Native Mods > Loader Mode)", false);
    }
    const auto file = read_file(library);
    if (file.empty()) {
        refuse(modId, guard::Category::Validation, "cannot read " + library.string());
    }
    const auto checked = check(file, modId, mode == LoaderMode::DryRun ? "dry_run" : "execute");
    if (!checked.ok()) {
        refuse(modId, checked.category, checked.why);
    }
    if (mode == LoaderMode::DryRun) {
        refuse(modId, guard::Category::Validation,
            std::string("compatible (") + abi_name(checked.abi) + "); not run because Loader Mode is Dry Run",
            false);
    }

    if (const auto why = unavailable_reason()) {
        refuse(modId, guard::Category::Validation, "cannot map native code on this launch: " + *why);
    }
    auto memory = std::make_unique<CodeMemory>();
    auto mapped = elf::map(*checked.parsed.image, *checked.plan, *memory);
    if (!mapped.loaded) {
        write_report(modId, elf::report_json(modId, "execute", checked.parsed, &*checked.plan, &mapped.diagnostic));
        refuse(modId, guard::category_for_load_error(elf::error_code(mapped.diagnostic.error)),
            mapped.diagnostic.message);
    }
    if (mapped.loaded->ehFrame != 0) {
        __register_frame(reinterpret_cast<void*>(static_cast<uintptr_t>(mapped.loaded->ehFrame)));
    }
    Log.info("{}: mapped at {:#x} ({} bytes, {}), {} constructor(s) deferred to activation", modId,
        mapped.loaded->runtimeBase, mapped.loaded->size, abi_name(checked.abi),
        mapped.loaded->constructors.size());
    return std::make_unique<ElfModule>(std::move(memory), std::move(*mapped.loaded));
}

std::optional<std::string> check_package(mods::ModBundle& bundle, std::string_view modId) {
    std::vector<std::string> platforms;
    std::optional<std::string> library;
    for (const auto& name : bundle.getFileNames()) {
        if (!name.starts_with("lib/") || (!name.ends_with("/mod.so") && !name.ends_with("/mod.dll"))) {
            continue;
        }
        const std::string platform = name.substr(4, name.find('/', 4) - 4);
        platforms.push_back(platform);
        if (platform == "switch-arm64" || (platform == "linux-aarch64" && !library)) {
            library = name;
        }
    }
    if (platforms.empty()) {
        return std::nullopt;  // assets or a script: nothing native to check
    }
    if (!library) {
        std::string list;
        for (const auto& platform : platforms) {
            list += (list.empty() ? "" : ", ") + platform;
        }
        return "this mod's code is built only for " + list +
               "; it has no switch-arm64 or linux-aarch64 build, so it cannot run on Switch";
    }
    std::vector<uint8_t> file;
    try {
        file = bundle.readFile(*library);
    } catch (const std::exception& e) {
        return std::string("cannot read ") + *library + ": " + e.what();
    }
    const auto checked = check(file, modId, "install_check");
    if (!checked.ok()) {
        return "its native code cannot load on this build: " + checked.why;
    }
    Log.info("{}: package checked before install: compatible ({})", modId, abi_name(checked.abi));
    return std::nullopt;
}

}  // namespace dusk::switchelf
