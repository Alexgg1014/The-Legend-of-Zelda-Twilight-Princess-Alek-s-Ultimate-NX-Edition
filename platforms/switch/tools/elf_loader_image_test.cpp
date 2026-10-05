// H37-c2: the loader in an image built like the NRO, running a module's exceptions through the
// image's own unwinder. Built for aarch64-linux as a static PIE and run under qemu-aarch64 by
// test-elf-loader.sh.
//
// The NRO links libgcc, libgcc_eh and libm statically; their functions are hidden there, so the
// link makes them local and they never reach .dynsym. This image is linked the same way (static
// PIE, an explicit --dynamic-list, src/dusk/mods/loader/elf_image_runtime.c), and it resolves a
// module's imports exactly as switch_native_loader.cpp does: ImageSymbols over the image's own
// dynamic section, then make_resolver with the loader's shims and the image runtime table.
//
//  1. Without the image runtime table, _Unwind_Resume is missing: H37-c1's Dry Run, reproduced.
//  2. With it, _Unwind_Resume resolves to this image's own function (the hidden one the image
//     itself unwinds with), not to a copy.
//  3. The module (elf_image_test_module.cpp) is mapped, its .eh_frame registered with this
//     image's unwinder, its constructor run once; then exceptions go through its frames: thrown
//     by the image through a module cleanup (_Unwind_Resume) to the image's handler, thrown and
//     caught inside the module, thrown by the module to the image.
//  4. Every library given after the module (the H37 packages) gets a Dry Run against this image:
//     each import of a libgcc/libgcc_eh/libm function must resolve through the image runtime
//     table to the address the table holds. (Its other imports are glibc's here, not newlib's:
//     not this test's question.)
#include "dusk/mods/loader/elf_loader.hpp"
#include "dusk/mods/loader/elf_runtime_policy.hpp"
#include "dusk/mods/loader/image_symbols.hpp"

#include <elf.h>
#include <sys/mman.h>
#include <unwind.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace dusk::mods::elf;

extern "C" const char __ehdr_start[];
extern "C" void __register_frame(void*);

// The image's side of the boundary: the module imports this.
extern "C" __attribute__((noinline)) void game_throw(int value) {
    throw value;
}

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

std::vector<uint8_t> read(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// The dynamic section as the linker wrote it. glibc's static-PIE start-up relocates the d_ptr
// values of the loaded copy in place; libnx's crt0 does not, and ImageSymbols expects them
// unrelocated, so the test reads them from its own file.
std::vector<Elf64_Dyn> g_dynamic;

std::optional<ImageSymbols> image_symbols() {
    const auto self = read("/proc/self/exe");
    const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(self.data());
    for (int i = 0; i < eh->e_phnum; ++i) {
        const auto* ph = reinterpret_cast<const Elf64_Phdr*>(self.data() + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type == PT_DYNAMIC) {
            g_dynamic.resize(ph->p_filesz / sizeof(Elf64_Dyn));
            std::memcpy(g_dynamic.data(), self.data() + ph->p_offset, g_dynamic.size() * sizeof(Elf64_Dyn));
        }
    }
    std::string error;
    auto symbols = ImageSymbols::from_dynamic(reinterpret_cast<uintptr_t>(__ehdr_start), g_dynamic.data(), error);
    if (!symbols) {
        std::fprintf(stderr, "image symbols: %s\n", error.c_str());
    }
    return symbols;
}

// What switch_native_loader.cpp's check() passes to plan().
Resolver console_resolver(const ImageSymbols& game, PackageAbi abi, bool withImageRuntime) {
    auto lookup = [&game](std::string_view name) -> std::optional<uint64_t> {
        const uintptr_t address = game.find(name);
        return address != 0 ? std::optional<uint64_t>{address} : std::nullopt;
    };
    return withImageRuntime ? make_resolver(abi, lookup) : make_resolver(abi, lookup, shims(), {});
}

const ImportDecision* find_import(const Plan& plan, std::string_view name) {
    for (const auto& d : plan.imports) {
        if (d.name == name) {
            return &d;
        }
    }
    return nullptr;
}

struct PosixMemory : MemoryBackend {
    uint8_t* base = nullptr;
    bool reserve(uint64_t size, uint64_t align, uint8_t*& staging, uint64_t& runtimeBase, std::string& error) override {
        void* raw = mmap(nullptr, size + align, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED) {
            error = "mmap failed";
            return false;
        }
        base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(raw) + align - 1) & ~(align - 1));
        staging = base;
        runtimeBase = reinterpret_cast<uintptr_t>(base);
        return true;
    }
    bool publish(const std::vector<Protection>& ranges, std::string& error) override {
        for (const auto& r : ranges) {
            if (r.perm & kPermX) {
                __builtin___clear_cache(reinterpret_cast<char*>(base + r.offset),
                    reinterpret_cast<char*>(base + r.offset + r.size));
            }
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

uint64_t table_address(std::string_view name) {
    for (const auto& r : image_runtime()) {
        if (r.name == name) {
            return r.address;
        }
    }
    return 0;
}

void module_test(const ImageSymbols& game, const std::string& path) {
    const auto file = read(path);
    const auto parsed = parse(file);
    CHECK(parsed.image.has_value());
    if (!parsed.image) {
        return;
    }
    // 1. H37-c1: no table, no _Unwind_Resume.
    const auto before = plan(*parsed.image, console_resolver(game, PackageAbi::Linux, false),
        make_needed_policy(PackageAbi::Linux));
    const auto* unresolved = find_import(before, "_Unwind_Resume");
    CHECK(!before.ok() && unresolved != nullptr && unresolved->source == Source::Missing);
    std::printf("without the image runtime table: %s\n",
        before.ok() ? "loads (unexpected)" : before.problems.front().message.c_str());

    // 2. With it: the image's own unwinder.
    const auto p = plan(*parsed.image, console_resolver(game, PackageAbi::Linux, true),
        make_needed_policy(PackageAbi::Linux));
    for (const auto& pr : p.problems) {
        std::fprintf(stderr, "plan: %s: %s\n", error_code(pr.error), pr.message.c_str());
    }
    CHECK(p.ok());
    const auto* resume = find_import(p, "_Unwind_Resume");
    const auto own = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&_Unwind_Resume));
    CHECK(resume != nullptr && resume->source == Source::Runtime && resume->address == own);
    CHECK(game.find("_Unwind_Resume") == 0);  // hidden in this image, as in the NRO
    std::printf("with it: _Unwind_Resume -> %#lx, the image's own (%s)\n",
        static_cast<unsigned long>(resume ? resume->address : 0), resume ? resume->why.c_str() : "?");
    if (!p.ok()) {
        return;
    }

    // 3. Mapped, registered, constructed; then the exceptions.
    PosixMemory memory;
    auto mapped = map(*parsed.image, p, memory);
    CHECK(mapped.loaded.has_value());
    if (!mapped.loaded) {
        std::fprintf(stderr, "map: %s\n", mapped.diagnostic.message.c_str());
        return;
    }
    auto& m = *mapped.loaded;
    CHECK(m.ehFrame != 0);
    __register_frame(reinterpret_cast<void*>(m.ehFrame));
    auto constructed = reinterpret_cast<int (*)()>(m.find("mod_constructed"));
    auto cleanups = reinterpret_cast<int (*)()>(m.find("mod_cleanups"));
    auto passThrough = reinterpret_cast<void (*)(int)>(m.find("mod_pass_through"));
    auto modCatch = reinterpret_cast<int (*)(int)>(m.find("mod_catch"));
    auto modThrow = reinterpret_cast<void (*)(int)>(m.find("mod_throw"));
    CHECK(constructed && cleanups && passThrough && modCatch && modThrow);
    if (!(constructed && cleanups && passThrough && modCatch && modThrow)) {
        return;
    }
    CHECK(constructed() == 0);  // nothing runs before the loader runs the constructors
    run_constructors(m);
    CHECK(constructed() == 42);

    int caught = 0;
    try {
        passThrough(7);
    } catch (int value) {
        caught = value;
    }
    CHECK(caught == 7);          // the image's throw reached the image's handler...
    CHECK(cleanups() == 1);      // ...through the module's cleanup and _Unwind_Resume
    CHECK(modCatch(5) == 6);     // inside the module
    CHECK(cleanups() == 2);
    caught = 0;
    try {
        modThrow(9);
    } catch (int value) {
        caught = value;
    }
    CHECK(caught == 9);          // module to image, two module frames unwound
    CHECK(cleanups() == 4);
    for (int i = 0; i < 1000; ++i) {  // and again, on the same registration
        try {
            passThrough(i);
        } catch (int value) {
            caught = value;
        }
    }
    CHECK(caught == 999 && cleanups() == 1004);
    CHECK(constructed() == 42);  // constructors ran once
    std::printf("module: constructor ran once; exceptions image->module->image, inside the module, "
                "module->image: %d cleanups, all handled\n", cleanups());
}

// 4. The real packages: their hidden-runtime imports through the table.
void package_dry_run(const ImageSymbols& game, const std::string& path) {
    const auto file = read(path);
    const auto parsed = parse(file);
    if (!parsed.image) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), parsed.diagnostic.message.c_str());
        ++g_failures;
        return;
    }
    const auto p = plan(*parsed.image, console_resolver(game, PackageAbi::Linux, true),
        make_needed_policy(PackageAbi::Linux));
    int through = 0;
    for (const auto& r : image_runtime()) {
        if (const auto* d = find_import(p, r.name)) {
            CHECK(d->source == Source::Runtime && d->address == table_address(r.name) && d->address != 0);
            through += d->source == Source::Runtime;
        }
    }
    std::printf("%-60s %d import(s) of hidden libgcc/libgcc_eh/libm functions, all through the table\n",
        path.c_str(), through);
}

}  // namespace

// The control: the same module, its .eh_frame not registered. The image's unwinder then knows
// no frame of the module, and the first exception through one ends the process (std::terminate)
// -- what a module bound to an unwinder other than the image's own would get too.
[[noreturn]] void unregistered(const ImageSymbols& game, const std::string& path) {
    const auto file = read(path);
    const auto parsed = parse(file);
    const auto p = plan(*parsed.image, console_resolver(game, PackageAbi::Linux, true),
        make_needed_policy(PackageAbi::Linux));
    PosixMemory memory;
    auto mapped = map(*parsed.image, p, memory);
    run_constructors(*mapped.loaded);
    auto passThrough = reinterpret_cast<void (*)(int)>(mapped.loaded->find("mod_pass_through"));
    try {
        passThrough(1);
    } catch (int) {
        std::printf("caught without registration (unexpected)\n");
    }
    std::exit(0);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s [--unregistered] MODULE.so [PACKAGE.so...]\n", argv[0]);
        return 2;
    }
    if (std::string_view(argv[1]) == "--unregistered" && argc == 3) {
        if (auto game = image_symbols()) {
            unregistered(*game, argv[2]);
        }
        return 2;
    }
    auto game = image_symbols();
    CHECK(game.has_value());
    if (game) {
        CHECK(!image_runtime().empty());
        module_test(*game, argv[1]);
        for (int i = 2; i < argc; ++i) {
            package_dry_run(*game, argv[i]);
        }
    }
    if (g_failures) {
        std::fprintf(stderr, "elf_loader_image_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("elf_loader_image_test: all checks passed\n");
    return 0;
}
