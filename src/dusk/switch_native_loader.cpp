// aleks2 (H37): see switch_native_loader.hpp.
#include "dusk/switch_native_loader.hpp"
#include "dusk/dev_mode.hpp"
#include "dusk/hash.hpp"

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

#include "own_process_handle.h"

#include <chrono>
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

#if defined(ALEKS2_MOD_TLS)
#include "dusk/mods/loader/elf_tls.hpp"
#include "dusk/mods/loader/elf_dl.hpp"
#include "dusk/mods/loader/glibc_abi.hpp"
#include "dusk/mods/loader/glibc_net.hpp"
#include "dusk/benchmark.hpp"
#include "dusk/switch_net.h"
#include "dusk/switch_net_selftest.hpp"
#include "dusk/switch_tls_selftest.hpp"
#endif

namespace dusk::switchelf {
namespace fs = std::filesystem;
namespace elf = mods::elf;
namespace guard = mods::guard;

namespace {
#if defined(ALEKS2_MOD_TLS)
constexpr bool kModTls = true;

// DM3: what only the console knows, for the Linux sockets layer (glibc_net.hpp).
void install_net_platform() {
    static const bool s_installed = [] {
        elf::glibc::net::Platform p;
        p.lanListenAllowed = [] { return switchnet::lan_listen_allowed(); };
        p.interfaces = [] {
            std::vector<elf::glibc::net::Interface> list;
            switchnet::IpConfig ip;
            if (switchnet::ip_config(ip)) {
                list.push_back({"wlan0", ip.address, ip.netmask, false});
            }
            return list;
        };
        p.describe = [](std::string& sysname, std::string& release, std::string& version, std::string& machine) {
            sysname = "Horizon";
            release = switchnet::system_release();
            version = "Dusklight " + benchmark::build_id();
            machine = "aarch64";
        };
        p.log = [](int level, const std::string& line) {
            constexpr borealis::Log kNetLog{"aleks2::native"};
            if (level > 0) {
                kNetLog.warn("{}", line);
            } else {
                kNetLog.info("{}", line);
            }
        };
        elf::glibc::net::set_platform(p);
        // Sockets do not survive sleep: shut every one a Linux module opened, so its loop sees an error.
        switchnet::add_resume_listener([] {
            const size_t n = elf::glibc::net::shutdown_module_sockets();
            if (n != 0) {
                constexpr borealis::Log kNetLog{"aleks2::native"};
                kNetLog.info("resumed from sleep: shut down {} socket(s) opened by Linux mods", n);
            }
        });
        return true;
    }();
    (void)s_installed;
}

// The Linux shims, the TLS runtime's, and std::call_once's (elf_tls_once.cpp): the same tables the
// host tests run (platforms/switch/tools/test-elf-tls.sh).
std::vector<elf::Shim> linux_tls_shims() {
    install_net_platform();
    // DM2-d first: the general glibc ABI layer (errno, clocks, open/fcntl flags, struct tm) and
    // dl_iterate_phdr. Their names are not in the tables below; Shim::linuxOnly keeps them off switch-arm64.
    auto table = elf::glibc::shims();
    // DM3: the sockets layer (sockaddr, addrinfo, msghdr, SO_*/MSG_*/AI_*/EAI_*, emulated pipe/eventfd/epoll,
    // getifaddrs, uname, fork/exec failing as glibc does).
    for (const auto& s : elf::glibc::net::shims()) {
        table.push_back(s);
    }
    for (const auto& s : elf::dl::shims()) {
        table.push_back(s);
    }
    for (const auto& s : elf::shims()) {
        table.push_back(s);
    }
    for (const auto& s : elf::tls::shims()) {
        table.push_back(s);
    }
    for (const auto& s : elf::tls::once_shims()) {
        table.push_back(s);
    }
    return table;
}

// DM2-d: the game image as dl_iterate_phdr reports the executable, from libnx's linker-script symbols
// (switch.ld): text from __start__ to __rodata_start (R-X), rodata (with .eh_frame_hdr and .eh_frame) to
// __data_start (R), data and bss to __end__ (RW), and PT_GNU_EH_FRAME over .eh_frame_hdr. Link-time vaddrs
// are offsets from __start__, so the load bias is __start__ itself. Name "" as glibc gives the executable.
extern "C" char __start__[], __rodata_start[], __data_start[], __end__[], __eh_frame_hdr_start[],
    __eh_frame_hdr_end[];

elf::dl::Object game_image_object() {
    struct Phdr {
        uint32_t type, flags;
        uint64_t offset, vaddr, paddr, filesz, memsz, align;
    };
    static_assert(sizeof(Phdr) == 56);
    const auto at = [](const char* p) { return static_cast<uint64_t>(p - __start__); };
    const Phdr phdrs[] = {
        {1, 5, 0, 0, 0, at(__rodata_start), at(__rodata_start), 0x1000},
        {1, 4, at(__rodata_start), at(__rodata_start), at(__rodata_start), at(__data_start) - at(__rodata_start),
            at(__data_start) - at(__rodata_start), 0x1000},
        {1, 6, at(__data_start), at(__data_start), at(__data_start), at(__end__) - at(__data_start),
            at(__end__) - at(__data_start), 0x1000},
        {0x6474E550, 4, at(__eh_frame_hdr_start), at(__eh_frame_hdr_start), at(__eh_frame_hdr_start),
            at(__eh_frame_hdr_end) - at(__eh_frame_hdr_start), at(__eh_frame_hdr_end) - at(__eh_frame_hdr_start), 4},
    };
    elf::dl::Object o;
    o.name = "";
    o.bias = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(__start__));
    o.phdrTable.assign(reinterpret_cast<const uint8_t*>(phdrs), reinterpret_cast<const uint8_t*>(phdrs) + sizeof phdrs);
    o.phnum = static_cast<uint16_t>(std::size(phdrs));
    return o;
}

elf::GameLookup tls_imports() {
    return elf::tls::host_variables(elf::tls::once_variables());
}

// A fatal TLS error (out of memory or no key association inside a module's TLS access) stops the
// game; say why in the log first.
void tls_fatal(const char* what) {
    constexpr borealis::Log kTlsLog{"aleks2::native"};
    kTlsLog.error("native mod TLS: {} -- stopping", what);
    std::fflush(nullptr);
}
#else
constexpr bool kModTls = false;
#endif
}  // namespace

namespace {

constexpr borealis::Log Log{"aleks2::native"};

fs::path s_modsDir;
std::unique_ptr<guard::BootGuard> s_guard;
guard::BootGuard::Startup s_startup;

config::ConfigVar<s32>& mode_var() {
    // Registered on first use; the config system restores a persisted value registered late.
    // DM1-c: Execute by default (was Dry Run through DM1-b); start() keeps Dry Run on an older install.
    static config::ConfigVar<s32> s_mode{"aleks2.nativeLoaderMode", static_cast<s32>(LoaderMode::Execute)};
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

namespace {
// Loader-only lab mode (m_Do_main.cpp): Execute for this session, without touching the saved setting.
bool s_forceExecute = false;
}  // namespace

void force_execute_this_session() {
    s_forceExecute = true;
}

LoaderMode loader_mode() {
    if (s_forceExecute || !dev_mode()) {
        return LoaderMode::Execute;  // V1-rc5: a player has no Loader Mode; only developer mode (dev.txt) has one
    }
    const s32 value = mode_var().getValue();
    // An unknown value (a hand-edited config) is the safe one, not the default.
    return value >= 0 && value <= 2 ? static_cast<LoaderMode>(value) : LoaderMode::DryRun;
}

// V1-rc4: which release's choice the saved Loader Mode is. Lab builds (DM1-b..V1-c5) saved Dry Run, or kept it
// for an older install; testers updating to the release then had every native mod "not run because Loader
// Mode is Dry Run". The menu writes this marker when the player picks a mode, so a Dry Run without it is an old
// lab value and is moved to Execute once. Bump kModeChoice for a release that must ask again.
constexpr s32 kModeChoice = 1;

config::ConfigVar<s32>& choice_var() {
    static config::ConfigVar<s32> s_choice{"aleks2.nativeLoaderModeChoice", 0};
    static const bool s_registered = [] {
        config::Register(s_choice);
        return true;
    }();
    (void)s_registered;
    return s_choice;
}

void set_loader_mode(LoaderMode mode) {
    mode_var().setValue(static_cast<s32>(mode));
    choice_var().setValue(kModeChoice);  // the player's own choice in this release: not migrated
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
    } else if (s_startup.stoppedOnce) {
        Log.warn("the previous boot stopped while '{}' was loading (closed, or a crash); it is tried once more and "
                 "disabled only if that happens again",
            s_startup.stoppedOnce->modId);
    } else if (s_startup.unattributedCrash) {
        Log.warn("the previous boot did not finish starting; no mod was running new code at the time");
    }
    if (s_startup.withoutMods) {
        Log.warn("starting without mods: {}", s_startup.withoutModsReason);
    }
    // DM1-c: the default became Execute. A mode the player never chose is not in the config, so it
    // follows the default; the boot counter (in the guard directory since H37) says whether this card
    // already ran a loader whose default was Dry Run. The first boot pins the new default, so a later
    // boot of the same install is not mistaken for an old one.
    // V1-rc4: Dry Run is a diagnostic now, never a default. A mode never chosen is Execute; a Dry Run the player
    // did not pick in this release (choice_var) is a lab build's and becomes Execute, once, said in the log.
    auto& mode = mode_var();
    if (mode.getLayer() == config::ConfigVarLayer::Default) {
        mode.setValue(static_cast<s32>(LoaderMode::Execute));
        config::save();
        Log.info("loader mode not chosen yet: Execute");
    } else if (mode.getValue() == static_cast<s32>(LoaderMode::DryRun) && choice_var().getValue() != kModeChoice) {
        mode.setValue(static_cast<s32>(LoaderMode::Execute));
        choice_var().setValue(kModeChoice);
        config::save();
        Log.warn("loader mode: Dry Run (saved by an earlier lab build) migrated to Execute; Dry Run stays available "
                 "as a diagnostic in Settings > Aleks > Native Mods: Loader Mode");
    }
    Log.info("native mod loader: {} (boot {})", loader_mode_name(loader_mode()), s_startup.boot);
#if defined(ALEKS2_MOD_TLS)
    Log.info("dynamic TLS for linux-aarch64 modules: on (TLS-lab)");
    elf::tls::set_fatal_handler(&tls_fatal);
    // DM2-d: dl_iterate_phdr lists the game first, then every module as it maps.
    elf::dl::set_tls_block_lookup(&elf::tls::current_block);
    elf::dl::set_main(game_image_object());
    if (dev_mode()) {  // V1-rc5: a test library left on a player's card is never run
        run_tls_selftest(modsDirectory);  // only if the tester put tls-selftest/tls_selftest.so on the card
        run_net_selftest(modsDirectory);  // DM3: only if the tester put net-selftest/net_lab.so on the card
    }
#endif
    // DM1-c: hbloader's process handle on the console; the IPC fallback means an emulator started the NRO.
    if (loader_mode() == LoaderMode::Execute) {
        Log.info("own process handle: {}", aleks2_own_process_handle() == 0 ? "none" :
                                           aleks2_own_process_handle_from_ipc() ? "IPC fallback (no hbloader)" :
                                                                                  "hbloader");
    }
    return s_startup;
}

#if defined(ALEKS2_MOD_TLS)
mods::elf::Resolver linux_tls_resolver(std::vector<mods::elf::tls::HostVariable> extra) {
    const auto* game = game_symbols();
    auto variables = elf::tls::once_variables();
    variables.insert(variables.end(), extra.begin(), extra.end());
    return elf::make_resolver(elf::PackageAbi::Linux,
        [game](std::string_view name) -> std::optional<uint64_t> {
            const uintptr_t address = game != nullptr ? game->find(name) : 0;
            return address != 0 ? std::optional<uint64_t>{address} : std::nullopt;
        },
        linux_tls_shims(), elf::image_runtime(), elf::tls::host_variables(std::move(variables)));
}
#endif

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
    bool tls = false;  // TLS-lab: planned with dynamic TLS (a Linux module, ALEKS2_MOD_TLS)
    std::string why;
    guard::Category category = guard::Category::Validation;
    // rc18: where a mod's load time goes (one line per mod at boot, after "mapped at").
    double parseSeconds = 0, planSeconds = 0, reportSeconds = 0, servicesSeconds = 0;
    bool ok() const { return why.empty(); }
};

using StageClock = std::chrono::steady_clock;
double seconds_since(StageClock::time_point t) {
    return std::chrono::duration<double>(StageClock::now() - t).count();
}

Checked check(std::span<const uint8_t> file, std::string_view modId, const char* modeName) {
    Checked c;
    auto stage = StageClock::now();
    c.parsed = elf::parse(file);
    c.parseSeconds = seconds_since(stage);
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
    const auto lookup = [game](std::string_view name) -> std::optional<uint64_t> {
        const uintptr_t address = game->find(name);
        return address != 0 ? std::optional<uint64_t>{address} : std::nullopt;
    };
    // TLS-lab: only a Linux-built module gets dynamic TLS. Its descriptors are resolved against
    // TPIDR_EL0, which is what glibc's toolchain reads; a switch-arm64 module (-mtp=soft) would read
    // libnx's thread pointer instead, so it stays refused.
    c.tls = kModTls && c.abi == elf::PackageAbi::Linux;
#if defined(ALEKS2_MOD_LINUX_STREAMS)
    // DM2-b: the glibc-table ctype<char> in every locale a Linux module can reach, once, before any of its imports
    // is bound or its code runs (linux_ctype.cpp).
    if (c.abi == elf::PackageAbi::Linux) {
        static const std::string s_check = [] {
            std::string verdict = elf::prepare_linux_runtime();
            Log.info("{}", verdict);
            return verdict;
        }();
    }
#endif
    stage = StageClock::now();
#if defined(ALEKS2_MOD_TLS)
    c.plan = c.tls ? elf::plan(*c.parsed.image,
                         elf::make_resolver(c.abi, lookup, linux_tls_shims(), elf::image_runtime(), tls_imports()),
                         elf::make_needed_policy(c.abi), true) :
                     elf::plan(*c.parsed.image, elf::make_resolver(c.abi, lookup), elf::make_needed_policy(c.abi));
#else
    c.plan = elf::plan(*c.parsed.image, elf::make_resolver(c.abi, lookup), elf::make_needed_policy(c.abi));
#endif
    c.planSeconds = seconds_since(stage);
    stage = StageClock::now();
    std::string report = elf::report_json(modId, modeName, c.parsed, &*c.plan);
    c.reportSeconds = seconds_since(stage);
    if (!c.plan->ok()) {
        write_report(modId, report);
        const auto& first = c.plan->problems.front();
        c.category = guard::category_for_load_error(elf::error_code(first.error));
        c.why = first.message;
        if (first.error == elf::Error::TlsUnsupported && c.abi == elf::PackageAbi::Switch && kModTls) {
            // TLS-lab: -mtls-dialect=trad fixes devkitA64's TLSDESC code, but this loader still gives
            // TLS to linux-aarch64 modules only.
            c.why += " (dynamic TLS is provided to linux-aarch64 modules only; a switch-arm64 module may not use TLS)";
        }
        if (c.plan->problems.size() > 1) {
            c.why += " (and " + std::to_string(c.plan->problems.size() - 1) + " more problem(s); see the report)";
        }
        return c;
    }

    // aleks2 (H37-c4): the game's services the mod imports, judged against this build, from the
    // file alone -- nothing mapped, no mod code run. Dry Run used to say "compatible" and Execute
    // then failed the mod at activation with "Mod requires a service that is unavailable".
    stage = StageClock::now();
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
    c.servicesSeconds = seconds_since(stage);
    stage = StageClock::now();
    write_report(modId, elf::annotate_report(std::move(report), scan.imports, problems));
    c.reportSeconds += seconds_since(stage);
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
    auto stage = StageClock::now();
    const auto file = read_file(library);
    const double readSeconds = seconds_since(stage);
    if (file.empty()) {
        refuse(modId, guard::Category::Validation, "cannot read " + library.string());
    }
    stage = StageClock::now();
    const auto checked = check(file, modId, mode == LoaderMode::DryRun ? "dry_run" : "execute");
    const double checkSeconds = seconds_since(stage);
    if (!checked.ok()) {
        refuse(modId, checked.category, checked.why);
    }
    if (mode == LoaderMode::DryRun) {
        refuse(modId, guard::Category::Validation,
            std::string("compatible (") + abi_name(checked.abi) +
                "); not run because Loader Mode is Dry Run (a diagnostic). To play with it: Settings > Aleks > "
                "Native Mods: Loader Mode > Execute, then Restart now",
            false);
    }

    if (const auto why = unavailable_reason()) {
        refuse(modId, guard::Category::Validation, "cannot map native code on this launch: " + *why);
    }
    stage = StageClock::now();
    auto memory = std::make_unique<CodeMemory>();
#if defined(ALEKS2_MOD_TLS)
    if (checked.tls) {
        elf::tls::set_fatal_handler(&tls_fatal);
    }
    auto mapped = elf::map(*checked.parsed.image, *checked.plan, *memory, checked.tls ? &elf::tls::runtime() : nullptr);
#else
    auto mapped = elf::map(*checked.parsed.image, *checked.plan, *memory);
#endif
    if (!mapped.loaded) {
        write_report(modId, elf::report_json(modId, "execute", checked.parsed, &*checked.plan, &mapped.diagnostic));
        refuse(modId, guard::category_for_load_error(elf::error_code(mapped.diagnostic.error)),
            mapped.diagnostic.message);
    }
#if defined(ALEKS2_MOD_TLS)
    if (checked.abi == elf::PackageAbi::Linux) {
        const auto& image = *checked.parsed.image;
        elf::dl::add({library.string(), mapped.loaded->runtimeBase - image.base, image.phdrTable, image.phnum,
            mapped.loaded->tlsModule});
    }
#endif
    if (mapped.loaded->ehFrame != 0) {
        __register_frame(reinterpret_cast<void*>(static_cast<uintptr_t>(mapped.loaded->ehFrame)));
    }
    const double mapSeconds = seconds_since(stage);
    stage = StageClock::now();
    // V1-rc5: the library's sha256 too, so that a log says which build of a mod ran (a tester's crash came from
    // an old package that maps to exactly the same size as the new one).
    hash::Sha256 sha;
    sha.update(std::span<const uint8_t>{file.data(), file.size()});
    Log.info("{}: mapped at {:#x} ({} bytes, {}, mod.so sha256 {}), {} constructor(s) deferred to activation{}",
        modId, mapped.loaded->runtimeBase, mapped.loaded->size, abi_name(checked.abi), sha.finish().substr(0, 16),
        mapped.loaded->constructors.size(),
        mapped.loaded->counterReadsRedirected != 0
            ? fmt::format("; {} cntvct_el0 read(s) redirected to cntpct_el0", mapped.loaded->counterReadsRedirected)
            : std::string{});
    // rc18: the load split by stage (the boot timing's "load" per mod is this plus the extraction before it).
    Log.info("{}: load stages: read {:.2f} s ({} bytes), parse {:.2f} s, plan (imports and relocations) {:.2f} s, "
             "report {:.2f} s, services {:.2f} s, map {:.2f} s, sha256 {:.2f} s; check total {:.2f} s",
        modId, readSeconds, file.size(), checked.parseSeconds, checked.planSeconds, checked.reportSeconds,
        checked.servicesSeconds, mapSeconds, seconds_since(stage), checkSeconds);
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

// V1-rc11: called from DuskMain once game_main has returned (the mods are unloaded by then).
extern "C" size_t aleks2_close_mod_sockets(void) {
#if defined(ALEKS2_MOD_TLS)
    return dusk::mods::elf::glibc::net::close_module_sockets();
#else
    return 0;  // no Linux sockets layer in this build
#endif
}
