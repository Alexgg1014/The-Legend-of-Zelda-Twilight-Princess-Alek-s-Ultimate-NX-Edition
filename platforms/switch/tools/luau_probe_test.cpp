// H36: host test for the Luau probe package and the Luau runtime that runs it.
//
// What is real here, compiled from this tree: package discovery and manifest validation
// (mods/loader/packages.cpp, manifest.cpp, bundle_zip.cpp, bundle_disk.cpp), the zip
// reader (dusk/archive.cpp over miniz 3.0.2), the Switch borealis::io
// (platforms/switch/source/borealis_io_switch.cpp), the resource service
// (mods/svc/resource.cpp) and the whole Luau runtime mod (mods/luau_runtime) on Luau 0.734 and LuaBridge3 twilit-v1. The
// .dusk files are made by platforms/switch/tools/make_dusk.py, the same packer the
// Switch build stages the probe with.
//
// What stands in for the host: the log, host, UI and config services are recording
// doubles (so the test can see every line, every notice and every write attempt), and
// SDL's file streams are plain stdio; those and the few host functions the code above calls
// into are in luau_probe_test_shims.cpp, each saying what it stands for. The runtime is wired the
// way the loader wires a native mod: its import records are filled from its own metadata
// and its runtime service is taken from its export record.
//
// Run by platforms/switch/tools/test-luau-probe.sh, which also builds the packages.
#include "dusk/mod_loader.hpp"
#include "dusk/mods/loader/loader.hpp"
#include "dusk/mods/loader/manifest.hpp"
#include "dusk/mods/loader/native_module.hpp"
#include "dusk/mods/loader/packages.hpp"
#include "dusk/mods/svc/registry.hpp"

#include "mods/api.h"
#include "mods/runtime.h"
#include "mods/svc/config.h"
#include "mods/svc/host.h"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace dusk::mods;

// The runtime's exports (DEFINE_MOD in mods/luau_runtime/src/runtime.cpp).
extern "C" ModContext* mod_ctx;

// From luau_probe_test_shims.cpp: every line the loader's own logging produced.
std::vector<std::string>& loader_log_lines();

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

// --- recording service doubles ------------------------------------------------------------

struct Recorded {
    std::vector<std::string> log;      // "<level> <mod id>: <message>"
    std::vector<std::string> toasts;   // title|body
    std::vector<std::string> failures; // HostService::fail messages
    int configCalls = 0;               // any ConfigService entry point
    int dataDirCalls = 0;              // HostService::data_dir (creates a directory)
};
Recorded g_rec;

const char* level_name(LogLevel level) {
    switch (level) {
    case LOG_LEVEL_TRACE: return "trace";
    case LOG_LEVEL_DEBUG: return "debug";
    case LOG_LEVEL_INFO: return "info";
    case LOG_LEVEL_WARN: return "warn";
    case LOG_LEVEL_ERROR: return "error";
    }
    return "?";
}

const char* ctx_id(ModContext* ctx) {
    return ctx != nullptr && ctx->mod != nullptr ? ctx->mod->metadata.id.c_str() : "<host>";
}

void log_write(ModContext* ctx, LogLevel level, const char* message) {
    g_rec.log.push_back(std::string(level_name(level)) + " " + ctx_id(ctx) + ": " + message);
}
void log_trace(ModContext* c, const char* m) { log_write(c, LOG_LEVEL_TRACE, m); }
void log_debug(ModContext* c, const char* m) { log_write(c, LOG_LEVEL_DEBUG, m); }
void log_info(ModContext* c, const char* m) { log_write(c, LOG_LEVEL_INFO, m); }
void log_warn(ModContext* c, const char* m) { log_write(c, LOG_LEVEL_WARN, m); }
void log_error(ModContext* c, const char* m) { log_write(c, LOG_LEVEL_ERROR, m); }

const LogService kLog{
    .header = SERVICE_HEADER(LogService, LOG_SERVICE_MAJOR, LOG_SERVICE_MINOR),
    .write = log_write,
    .trace = log_trace,
    .debug = log_debug,
    .info = log_info,
    .warn = log_warn,
    .error = log_error,
};

void host_fail(ModContext*, ModResult, const char* message) {
    g_rec.failures.push_back(message != nullptr ? message : "");
}
const char* host_mod_id(ModContext* ctx) { return ctx->mod->metadata.id.c_str(); }
const char* host_mod_name(ModContext* ctx) { return ctx->mod->metadata.name.c_str(); }
const char* host_mod_version(ModContext* ctx) { return ctx->mod->metadata.version.c_str(); }
const char* host_mod_dir(ModContext* ctx) { return ctx->mod->dirUtf8.c_str(); }
ModResult host_data_dir(ModContext*, const char** out) {
    ++g_rec.dataDirCalls;
    *out = nullptr;
    return MOD_UNAVAILABLE;
}

const HostService kHost{
    .header = SERVICE_HEADER(HostService, HOST_SERVICE_MAJOR, HOST_SERVICE_MINOR),
    .version = "h36-host-test",
    .fail = host_fail,
    .mod_id = host_mod_id,
    .mod_name = host_mod_name,
    .mod_version = host_mod_version,
    .mod_dir = host_mod_dir,
    .data_dir = host_data_dir,
};

ModResult ui_push_toast(ModContext*, const UiToastDesc* desc) {
    g_rec.toasts.push_back(std::string(desc->title_rml ? desc->title_rml : "") + "|" +
                           (desc->body_rml ? desc->body_rml : ""));
    return MOD_OK;
}

UiService make_ui() {
    UiService ui{};
    ui.header = SERVICE_HEADER(UiService, UI_SERVICE_MAJOR, UI_SERVICE_MINOR);
    ui.push_toast = ui_push_toast;
    return ui;
}
const UiService kUi = make_ui();

// Every entry point counts: the probe must not touch configuration at all.
ModResult config_register(ModContext*, const ConfigVarDesc*, ConfigVarHandle*) {
    ++g_rec.configCalls;
    return MOD_ERROR;
}
ConfigService make_config() {
    ConfigService config{};
    config.header = SERVICE_HEADER(ConfigService, CONFIG_SERVICE_MAJOR, CONFIG_SERVICE_MINOR);
    config.register_var = config_register;
    return config;
}
const ConfigService kConfig = make_config();

// --- wiring the runtime like the loader does ----------------------------------------------

const ModRuntimeService* g_runtime = nullptr;
ModContext g_runtimeContext{};
LoadedMod g_runtimeMod;

const void* service_for(const char* id) {
    const std::string_view name{id};
    if (name == LOG_SERVICE_ID) return &kLog;
    if (name == HOST_SERVICE_ID) return &kHost;
    if (name == RESOURCE_SERVICE_ID) return svc::g_resourceModule.service;
    if (name == UI_SERVICE_ID) return &kUi;
    if (name == CONFIG_SERVICE_ID) return &kConfig;
    return nullptr;  // overlay, texture, audio_res: optional, absent here
}

bool wire_runtime() {
    const auto* cursor = static_cast<const uint8_t*>(mod_meta.records_begin);
    const auto* end = static_cast<const uint8_t*>(mod_meta.records_end);
    int imports = 0;
    while (cursor + 8 <= end) {
        uint64_t first = 0;
        std::memcpy(&first, cursor, sizeof(first));
        if (first == 0) {
            cursor += 8;
            continue;
        }
        const auto* rec = reinterpret_cast<const ModMetaRecord*>(cursor);
        if (rec->kind == MOD_META_IMPORT) {
            const auto* import = reinterpret_cast<const ModMetaImport*>(cursor);
            *static_cast<const void**>(import->slot) = service_for(import->service_id.chars);
            ++imports;
        } else if (rec->kind == MOD_META_EXPORT) {
            const auto* exported = reinterpret_cast<const ModMetaExport*>(cursor);
            if (std::string_view{exported->service_id.chars} == "dev.twilitrealm.luau") {
                g_runtime = static_cast<const ModRuntimeService*>(exported->service);
            }
        }
        cursor += rec->size;
    }
    g_runtimeMod.metadata.id = "dev.twilitrealm.luau";
    g_runtimeContext.mod = &g_runtimeMod;
    mod_ctx = &g_runtimeContext;
    ModError error = MOD_ERROR_INIT;
    return imports >= 5 && g_runtime != nullptr && mod_initialize(&error) == MOD_OK;
}

// --- a discovered package, as the loader would hold it -------------------------------------

struct Subject {
    LoadedMod mod;
    ModContext context{};
};

std::unique_ptr<Subject> open_package(const fs::path& path) {
    auto subject = std::make_unique<Subject>();
    const bool fromDirectory = fs::is_directory(path);
    std::shared_ptr<ModBundle> bundle = load_bundle(path, fromDirectory);
    const auto manifest = load_manifest(path, *bundle);
    subject->mod.metadata = manifest.metadata;
    subject->mod.bundle = std::move(bundle);
    subject->mod.dir = path;
    subject->mod.dirUtf8 = path.string();
    subject->context.mod = &subject->mod;
    return subject;
}

std::string activate(Subject& subject) {
    ModError error = MOD_ERROR_INIT;
    const ModResult result = g_runtime->activate(&g_runtimeContext, &subject.context, &error);
    return result == MOD_OK ? std::string{} : std::string("error: ") + error.message;
}

bool has_line(std::string_view needle) {
    return std::ranges::any_of(g_rec.log,
        [&](const std::string& line) { return line.find(needle) != std::string::npos; });
}

int count_lines(std::string_view needle) {
    return static_cast<int>(std::ranges::count_if(g_rec.log,
        [&](const std::string& line) { return line.find(needle) != std::string::npos; }));
}

std::map<std::string, std::string> snapshot(const fs::path& root) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            std::ifstream in(entry.path(), std::ios::binary);
            std::stringstream data;
            data << in.rdbuf();
            files[entry.path().lexically_relative(root).generic_string()] = data.str();
        }
    }
    return files;
}

void dump_log() {
    for (const auto& line : g_rec.log) {
        std::fprintf(stderr, "  | %s\n", line.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <work dir with the packages>\n", argv[0]);
        return 2;
    }
    const fs::path work = argv[1];
    const fs::path card = work / "card" / "mods";  // stands for sdmc:/switch/dusklight/mods

    CHECK(wire_runtime());
    if (g_runtime == nullptr) {
        std::fprintf(stderr, "the Luau runtime exported no runtime service\n");
        return 1;
    }

    // 1. Discovery on the card: the probe, the folders the Switch build stages for the
    //    built-in mods, a damaged package and a package with a broken mod.json.
    {
        const std::vector<ModSearchDir> dirs{{.path = card}};
        const auto packages = scan_packages(dirs);
        std::map<std::string, const PackageCandidate*> byId;
        for (const auto& package : packages) {
            byId[package.metadata.id] = &package;
        }
        CHECK(byId.contains("dev.aleks2.luau_probe"));
        CHECK(byId.contains("dev.twilitrealm.luau"));
        CHECK(byId.contains("com.melonspeedruns.rollandsprint"));
        // Cosmetics is a submodule; the script leaves its folder out when it is not there.
        const bool haveCosmetics = fs::exists(card / "cosmetics" / "mod.json");
        CHECK(!haveCosmetics || byId.contains("dev.twilitrealm.cosmetics"));
        if (byId.contains("dev.aleks2.luau_probe")) {
            const auto& probe = *byId["dev.aleks2.luau_probe"];
            CHECK(!probe.fromDirectory);
            CHECK(probe.path.filename() == "luau_probe.dusk");
            CHECK(probe.metadata.version == "1.0.0");
            CHECK(select_package(packages, "dev.aleks2.luau_probe") == &probe);
        }
        // The damaged ones are reported and skipped; nothing else is lost.
        CHECK(packages.size() == (haveCosmetics ? 4u : 3u));
        const auto& lines = loader_log_lines();
        CHECK(std::ranges::any_of(lines, [](const std::string& line) {
            return line.find("bad mod package") != std::string::npos &&
                   line.find("damaged.dusk") != std::string::npos;
        }));
        CHECK(std::ranges::any_of(lines, [](const std::string& line) {
            return line.find("bad mod package") != std::string::npos &&
                   line.find("bad_manifest.dusk") != std::string::npos;
        }));
        // The probe declares the Luau runtime, which is what routes it to that mod.
        auto bundle = load_bundle(card / "luau_probe.dusk", false);
        const auto manifest = load_manifest(card / "luau_probe.dusk", *bundle);
        CHECK(manifest.runtime.has_value());
        if (manifest.runtime) {
            CHECK(manifest.runtime->id == "dev.twilitrealm.luau");
            CHECK(manifest.runtime->major == 1 && manifest.runtime->minMinor == 0);
        }
    }

    // 2. A card without the package: nothing is found, nothing fails.
    {
        const std::vector<ModSearchDir> dirs{{.path = work / "empty-card"}};
        CHECK(scan_packages(dirs).empty());
        const std::vector<ModSearchDir> missing{{.path = work / "no-such-dir"}};
        CHECK(scan_packages(missing).empty());
    }

    const auto before = snapshot(work);

    // 3. The probe itself: loads, runs every step, shows its notice, counts updates and
    //    says goodbye on shutdown -- with nothing written anywhere.
    {
        auto probe = open_package(card / "luau_probe.dusk");
        g_rec = {};
        const std::string result = activate(*probe);
        CHECK(result.empty());
        CHECK(has_line("[h36-probe] start: dev.aleks2.luau_probe v1.0.0 from "));
        CHECK(has_line("luau_probe.dusk"));
        CHECK(has_line("[h36-probe] ok resource: probe.txt 132 bytes, first line "
                       "'H36 Luau probe resource, read from the package'"));
        CHECK(has_line("[h36-probe] ok require: probe_util.luau loaded through require"));
        CHECK(has_line("[h36-probe] ok compute: sum of squares 1..1000 = 333833500"));
        CHECK(has_line("[h36-probe] ok notice: shown"));
        CHECK(has_line("[h36-probe] success: 4 of 4 steps"));
        CHECK(!has_line("FAIL"));
        CHECK(g_rec.toasts.size() == 1);
        CHECK(g_rec.toasts.size() == 1 &&
              g_rec.toasts[0] == "Luau probe|Script loaded from the SD card");
        CHECK(g_rec.failures.empty());
        for (int frame = 0; frame < 600; ++frame) {
            ModError error = MOD_ERROR_INIT;
            CHECK(g_runtime->update(&g_runtimeContext, &probe->context, &error) == MOD_OK);
        }
        CHECK(count_lines("[h36-probe] update: 600 frames") == 1);
        // Turned off from the Mods window: the loader deactivates it.
        ModError error = MOD_ERROR_INIT;
        CHECK(g_runtime->deactivate(&g_runtimeContext, &probe->context, &error) == MOD_OK);
        CHECK(has_line("[h36-probe] shutdown after 600 update(s)"));
        // Off means off: no more updates reach it.
        CHECK(g_runtime->update(&g_runtimeContext, &probe->context, &error) == MOD_INVALID_ARGUMENT);
        // And back on, without rebuilding anything.
        g_rec = {};
        CHECK(activate(*probe).empty());
        CHECK(has_line("[h36-probe] success: 4 of 4 steps"));
        CHECK(g_runtime->deactivate(&g_runtimeContext, &probe->context, &error) == MOD_OK);
        CHECK(has_line("[h36-probe] shutdown after 0 update(s)"));
        CHECK(g_rec.configCalls == 0);
        CHECK(g_rec.dataDirCalls == 0);
        if (g_failures) {
            dump_log();
        }
    }
    CHECK(snapshot(work) == before);  // no file created, changed or removed

    // 4. Broken scripts fail the mod with the reason, and the runtime survives them.
    const struct {
        const char* package;
        const char* expected;
    } broken[] = {
        {"syntax_error.dusk", "main.luau:3:"},
        {"runtime_error.dusk", "probe exploded on purpose"},
        {"no_main.dusk", "module not found: res/main.luau"},
        {"missing_module.dusk", "unable to locate 'no_such_module.lua(u)'"},
        {"missing_resource.dusk", "[h36-probe] FAIL resource"},
    };
    for (const auto& test : broken) {
        g_rec = {};
        auto subject = open_package(work / "broken" / test.package);
        const std::string result = activate(*subject);
        const bool mentioned = result.find(test.expected) != std::string::npos || has_line(test.expected);
        if (result.empty() || !mentioned) {
            std::fprintf(stderr, "%s: expected a failure mentioning '%s', got '%s'\n",
                test.package, test.expected, result.c_str());
            dump_log();
            ++g_failures;
        }
        ModError error = MOD_ERROR_INIT;
        g_runtime->deactivate(&g_runtimeContext, &subject->context, &error);
    }

    // 5. After all of that the probe still runs: one bad script does not poison the runtime.
    {
        g_rec = {};
        auto probe = open_package(card / "luau_probe.dusk");
        CHECK(activate(*probe).empty());
        CHECK(has_line("[h36-probe] success: 4 of 4 steps"));
        ModError error = MOD_ERROR_INIT;
        g_runtime->deactivate(&g_runtimeContext, &probe->context, &error);
    }

    ModError error = MOD_ERROR_INIT;
    CHECK(mod_shutdown(&error) == MOD_OK);

    if (g_failures != 0) {
        std::fprintf(stderr, "luau_probe_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("luau_probe_test: all checks passed\n");
    return 0;
}
