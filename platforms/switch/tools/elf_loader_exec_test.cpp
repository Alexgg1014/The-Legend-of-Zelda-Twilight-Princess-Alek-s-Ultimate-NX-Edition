// H37: the loader actually running AArch64 mod code. Built for aarch64-linux and run under
// qemu-aarch64 by test-elf-loader.sh.
//
// It maps real mod libraries (built by build-h37-test-mods.sh with the mod SDK) with
// src/dusk/mods/loader/elf_loader.cpp through a POSIX memory backend (mmap, then mprotect to
// the loader's final permissions), registers each module's .eh_frame, runs its constructors,
// wires its SDK services from its own metadata the way the game's loader does, and drives its
// lifecycle:
//
//   loader_lab     init/update/shutdown three times on one mapping: constructors, a vtable,
//                  libstdc++, a C++ exception thrown and caught inside the module
//   template_mod   the SDK's own template
//   luau_runtime   the whole Luau runtime mod (1.2 MB, ~3300 relocations, 29 constructors),
//                  activating a script through its runtime service: Luau compiles and runs it,
//                  including an error raised and caught inside Lua (a C++ throw in the module)
//
// What this proves and what it does not: it is Linux under qemu, so the module's imports are
// served by glibc/libstdc++ of this process rather than by the game's newlib -- it tests the
// loader's mapping, relocation, permissions, constructor order, unwind registration and
// symbol isolation on real AArch64 code, not the newlib compatibility of Linux-built mods.
#include "dusk/mods/loader/elf_loader.hpp"
#include "dusk/mods/loader/elf_runtime_policy.hpp"

#include "mods/api.h"
#include "mods/runtime.h"
#include "mods/svc/config.h"
#include "mods/svc/host.h"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"

#include <dlfcn.h>
#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace dusk::mods::elf;

extern "C" void __register_frame(void*);

struct ModContext {
    const char* id;
};

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

std::vector<std::string> g_log;

void log_write(ModContext* c, LogLevel, const char* m) {
    g_log.push_back(std::string(c ? c->id : "?") + ": " + m);
    std::printf("  | %s: %s\n", c ? c->id : "?", m);
}
void log_trace(ModContext* c, const char* m) { log_write(c, LOG_LEVEL_TRACE, m); }
void log_debug(ModContext* c, const char* m) { log_write(c, LOG_LEVEL_DEBUG, m); }
void log_info(ModContext* c, const char* m) { log_write(c, LOG_LEVEL_INFO, m); }
void log_warn(ModContext* c, const char* m) { log_write(c, LOG_LEVEL_WARN, m); }
void log_error(ModContext* c, const char* m) { log_write(c, LOG_LEVEL_ERROR, m); }
const LogService kLog{SERVICE_HEADER(LogService, LOG_SERVICE_MAJOR, LOG_SERVICE_MINOR), log_write, log_trace,
    log_debug, log_info, log_warn, log_error};

std::string g_script;
ModResult res_load(ModContext*, const char* path, ResourceBuffer* out) {
    if (std::string(path) != "main.luau") {
        return MOD_UNAVAILABLE;
    }
    out->data = std::malloc(g_script.size());
    std::memcpy(out->data, g_script.data(), g_script.size());
    out->size = g_script.size();
    return MOD_OK;
}
void res_free(ModContext*, ResourceBuffer* b) {
    std::free(b->data);
    b->data = nullptr;
}
bool res_file_exists(ModContext*, const char* p) { return std::string(p) == "main.luau"; }
bool res_dir_exists(ModContext*, const char*) { return false; }
const ResourceService kResource{SERVICE_HEADER(ResourceService, RESOURCE_SERVICE_MAJOR, RESOURCE_SERVICE_MINOR),
    res_load, res_free, res_file_exists, res_dir_exists};

const char* host_id(ModContext* c) { return c->id; }
const char* host_version(ModContext*) { return "1.0.0"; }
const char* host_dir(ModContext*) { return "sdmc:/switch/dusklight/mods/h37-script.dusk"; }
void host_fail(ModContext* c, ModResult, const char* m) { log_write(c, LOG_LEVEL_ERROR, m); }
HostService make_host() {
    HostService h{};
    h.header = SERVICE_HEADER(HostService, HOST_SERVICE_MAJOR, HOST_SERVICE_MINOR);
    h.version = "h37-exec-test";
    h.fail = host_fail;
    h.mod_id = host_id;
    h.mod_name = host_id;
    h.mod_version = host_version;
    h.mod_dir = host_dir;
    return h;
}
const HostService kHost = make_host();

const void* service_for(const char* id) {
    const std::string_view name{id};
    if (name == LOG_SERVICE_ID) return &kLog;
    if (name == RESOURCE_SERVICE_ID) return &kResource;
    if (name == HOST_SERVICE_ID) return &kHost;
    return nullptr;
}

// mmap + mprotect: the loader's Linux stand-in for Horizon's code memory.
struct PosixMemory : MemoryBackend {
    uint8_t* base = nullptr;
    uint64_t size = 0;
    bool reserve(uint64_t sz, uint64_t align, uint8_t*& staging, uint64_t& runtimeBase, std::string& error) override {
        void* raw = mmap(nullptr, sz + align, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED) {
            error = "mmap failed";
            return false;
        }
        const auto aligned = (reinterpret_cast<uintptr_t>(raw) + align - 1) & ~(align - 1);
        base = reinterpret_cast<uint8_t*>(aligned);
        size = sz;
        staging = base;
        runtimeBase = aligned;
        return true;
    }
    bool publish(const std::vector<Protection>& ranges, std::string& error) override {
        for (const auto& r : ranges) {
            if (r.perm & kPermX) {
                __builtin___clear_cache(reinterpret_cast<char*>(base + r.offset),
                    reinterpret_cast<char*>(base + r.offset + r.size));
            }
        }
        for (const auto& r : ranges) {
            const int prot = (r.perm & kPermR ? PROT_READ : 0) | (r.perm & kPermW ? PROT_WRITE : 0) |
                             (r.perm & kPermX ? PROT_EXEC : 0);
            if (mprotect(base + r.offset, r.size, prot) != 0) {
                error = "mprotect failed";
                return false;
            }
        }
        return true;
    }
};

struct Module {
    std::vector<uint8_t> file;
    Loaded loaded;
    ModContext context{};
    const ModMeta* meta = nullptr;
    ModResult (*init)(ModError*) = nullptr;
    ModResult (*update)(ModError*) = nullptr;
    ModResult (*shutdown)(ModError*) = nullptr;
    const void* runtimeService = nullptr;
};

bool load(const std::string& path, const char* id, Module& m) {
    std::ifstream in(path, std::ios::binary);
    m.file.assign(std::istreambuf_iterator<char>(in), {});
    const auto parsed = parse(m.file);
    if (!parsed.image) {
        std::fprintf(stderr, "%s: %s\n", id, parsed.diagnostic.message.c_str());
        return false;
    }
    // Imports: this process's own libraries play the game's part.
    const auto game = [](std::string_view name) -> std::optional<uint64_t> {
        void* p = dlsym(RTLD_DEFAULT, std::string(name).c_str());
        if (p == nullptr) return std::nullopt;
        return reinterpret_cast<uintptr_t>(p);
    };
    const auto p = plan(*parsed.image, make_resolver(PackageAbi::Linux, game), make_needed_policy(PackageAbi::Linux));
    if (!p.ok()) {
        for (const auto& pr : p.problems) {
            std::fprintf(stderr, "%s: %s: %s\n", id, error_code(pr.error), pr.message.c_str());
        }
        return false;
    }
    PosixMemory memory;
    auto mapped = map(*parsed.image, p, memory);
    if (!mapped.loaded) {
        std::fprintf(stderr, "%s: %s\n", id, mapped.diagnostic.message.c_str());
        return false;
    }
    m.loaded = std::move(*mapped.loaded);
    m.context.id = id;
    if (m.loaded.ehFrame != 0) {
        __register_frame(reinterpret_cast<void*>(m.loaded.ehFrame));
    }
    run_constructors(m.loaded);  // first code of the module to run
    m.meta = reinterpret_cast<const ModMeta*>(m.loaded.find("mod_meta"));
    auto** ctx = reinterpret_cast<ModContext**>(m.loaded.find("mod_ctx"));
    m.init = reinterpret_cast<ModResult (*)(ModError*)>(m.loaded.find("mod_initialize"));
    m.update = reinterpret_cast<ModResult (*)(ModError*)>(m.loaded.find("mod_update"));
    m.shutdown = reinterpret_cast<ModResult (*)(ModError*)>(m.loaded.find("mod_shutdown"));
    if (!m.meta || !ctx || !m.init || !m.update || !m.shutdown) {
        std::fprintf(stderr, "%s: exports missing after mapping\n", id);
        return false;
    }
    *ctx = &m.context;
    // Services, from the module's own metadata records, as natives.cpp wires them.
    const auto* cur = static_cast<const uint8_t*>(m.meta->records_begin);
    const auto* end = static_cast<const uint8_t*>(m.meta->records_end);
    while (cur + 8 <= end) {
        uint64_t first = 0;
        std::memcpy(&first, cur, 8);
        if (first == 0) {
            cur += 8;
            continue;
        }
        const auto* rec = reinterpret_cast<const ModMetaRecord*>(cur);
        if (rec->kind == MOD_META_IMPORT) {
            const auto* imp = reinterpret_cast<const ModMetaImport*>(cur);
            *static_cast<const void**>(imp->slot) = service_for(imp->service_id.chars);
        } else if (rec->kind == MOD_META_EXPORT) {
            m.runtimeService = reinterpret_cast<const ModMetaExport*>(cur)->service;
        }
        cur += rec->size;
    }
    std::printf("%s: mapped at %#lx, %zu constructor(s) run\n", id, static_cast<unsigned long>(m.loaded.runtimeBase),
        m.loaded.constructors.size());
    return true;
}

bool has(const std::string& needle) {
    for (const auto& l : g_log) {
        if (l.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s MODS_DIR\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    ModError err = MOD_ERROR_INIT;

    // Level 0: the lab module, three lifecycles on one mapping (re-enable never remaps).
    Module lab;
    CHECK(load(dir + "/loader_lab.so", "loader_lab", lab));
    if (lab.init) {
        for (int cycle = 1; cycle <= 3; ++cycle) {
            CHECK(lab.init(&err) == MOD_OK);
            for (int i = 0; i < 250; ++i) CHECK(lab.update(&err) == MOD_OK);
            CHECK(lab.shutdown(&err) == MOD_OK);
        }
        CHECK(has("[h37-lab] c constructor ran"));
        CHECK(has("[h37-lab] static object constructed"));
        CHECK(has("[h37-lab] sorted alpha,bravo,charlie,delta"));
        CHECK(has("[h37-lab] virtual calls: 10 sides"));
        CHECK(has("[h37-lab] caught: thrown inside the module"));
        CHECK(has("[h37-lab] update 200 (cycle 3)"));
        CHECK(has("[h37-lab] shutdown after 250 update(s), cycle 3"));
    }

    // The SDK template.
    Module tmpl;
    CHECK(load(dir + "/template_mod.so", "template_mod", tmpl));
    if (tmpl.init) {
        CHECK(tmpl.init(&err) == MOD_OK && tmpl.shutdown(&err) == MOD_OK);
        CHECK(has("template_mod: template_mod initialized") && has("template_mod: template_mod unloaded"));
    }

    // The Luau runtime, and a script through it.
    Module luau;
    std::ifstream probe(dir + "/luau_runtime.so");
    if (probe.good() && load(dir + "/luau_runtime.so", "dev.twilitrealm.luau", luau)) {
        CHECK(luau.init(&err) == MOD_OK);
        CHECK(luau.runtimeService != nullptr);
        g_script =
            "local log = require('dusklight.log')\n"
            "local squares = 0\n"
            "for i = 1, 100 do squares += i * i end\n"
            "local ok, message = pcall(function() error('raised in Lua') end)\n"
            "log.info('[h37-luau] dynamically loaded runtime: squares ' .. squares .. ', pcall caught: ' .. tostring(not ok))\n";
        ModContext script{"dev.aleks2.h37_script"};
        const auto* rt = static_cast<const ModRuntimeService*>(luau.runtimeService);
        ModError e = MOD_ERROR_INIT;
        const ModResult r = rt->activate(&luau.context, &script, &e);
        if (r != MOD_OK) std::fprintf(stderr, "activate: %s\n", e.message);
        CHECK(r == MOD_OK);
        CHECK(has("[h37-luau] dynamically loaded runtime: squares 338350, pcall caught: true"));
        CHECK(rt->deactivate(&luau.context, &script, &e) == MOD_OK);
        CHECK(luau.shutdown(&err) == MOD_OK);
    } else if (probe.good()) {
        CHECK(false && "luau_runtime did not load");
    }

    if (g_failures) {
        std::fprintf(stderr, "elf_loader_exec_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("elf_loader_exec_test: all checks passed\n");
    return 0;
}
