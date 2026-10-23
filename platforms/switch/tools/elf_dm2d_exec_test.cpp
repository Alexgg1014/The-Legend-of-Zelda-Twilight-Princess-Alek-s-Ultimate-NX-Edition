// DM2-d: the glibc ABI layer and the synthetic dl_iterate_phdr, really executed. Built for aarch64-linux and
// run under qemu-aarch64 by test-dm2d.sh.
//
//   elf_dm2d_exec_test DIR [TWILIT_SO]
//
// 1. The glibc ABI table: compiled here against aarch64 glibc, every entry's glibc column must equal glibc's own
//    value (the native column). The shims run too (identity translations on glibc): open/fcntl flags, a clock,
//    localtime_r/gmtime_r with tm_gmtoff/tm_zone as glibc fills them, strerror, a failing open's errno.
// 2. dl_iterate_phdr: dm2d_dl.so and dm2d_unwind.so (dm2d_dl_module.cpp, the second with -static-libgcc) are
//    mapped with the real loader and the production shim tables, registered as switch_native_loader.cpp does,
//    and each finds itself (Twilit's search), reports its TLS module and this thread's block, fingerprints its
//    code, throws and catches; dm2d_unwind.so walks 8+ of its own frames with ITS libgcc_eh, which finds FDEs only
//    through dl_iterate_phdr.
// 3. TWILIT_SO (Twilit Essentials 2.8.4's lib/linux-aarch64/mod.so) is mapped -- not run -- and its own
//    dl_iterate_phdr callback (0x114298, measured in research/dm2d) is called through the shim with its own
//    marker (0x111c38): it must find its executable PT_LOAD.
#include "dusk/mods/loader/elf_dl.hpp"
#include "dusk/mods/loader/elf_loader.hpp"
#include "dusk/mods/loader/elf_runtime_policy.hpp"
#include "dusk/mods/loader/elf_tls.hpp"
#include "dusk/mods/loader/glibc_abi.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

using namespace dusk::mods::elf;

extern "C" void __register_frame(void*);
extern "C" {
int aleks2_glibc_open(const char*, int, ...);
int aleks2_glibc_fcntl(int, int, ...);
int aleks2_glibc_clock_gettime(int, struct timespec*);
void* aleks2_glibc_localtime_r(const time_t*, void*);
void* aleks2_glibc_gmtime_r(const time_t*, void*);
char* aleks2_glibc_strerror(int);
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

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

struct PosixMemory : MemoryBackend {
    uint8_t* base = nullptr;
    bool reserve(uint64_t sz, uint64_t align, uint8_t*& staging, uint64_t& runtimeBase, std::string& error) override {
        void* raw = mmap(nullptr, sz + align, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED) {
            error = "mmap failed";
            return false;
        }
        const auto aligned = (reinterpret_cast<uintptr_t>(raw) + align - 1) & ~(align - 1);
        base = reinterpret_cast<uint8_t*>(aligned);
        staging = base;
        runtimeBase = aligned;
        return true;
    }
    bool publish(const std::vector<Protection>& ranges, std::string& error) override {
        for (const auto& r : ranges) {
            if (r.perm & kPermX) {
                __builtin___clear_cache(reinterpret_cast<char*>(base + r.offset), reinterpret_cast<char*>(base + r.offset + r.size));
            }
        }
        for (const auto& r : ranges) {
            const int prot = (r.perm & kPermR ? PROT_READ : 0) | (r.perm & kPermW ? PROT_WRITE : 0) | (r.perm & kPermX ? PROT_EXEC : 0);
            if (mprotect(base + r.offset, r.size, prot) != 0) {
                error = "mprotect failed";
                return false;
            }
        }
        return true;
    }
};

// The production Linux table, in switch_native_loader.cpp's order.
std::vector<Shim> linux_shims() {
    auto table = glibc::shims();
    for (const auto& s : dl::shims()) table.push_back(s);
    for (const auto& s : shims()) table.push_back(s);
    for (const auto& s : tls::shims()) table.push_back(s);
    for (const auto& s : tls::once_shims()) table.push_back(s);
    return table;
}

struct Mapped {
    std::vector<uint8_t> file;
    Loaded loaded;
    template <class F>
    F* fn(const char* name) const {
        return reinterpret_cast<F*>(static_cast<uintptr_t>(loaded.find(name)));
    }
};

// game: how imports resolve (this process's libraries, or a stand-in address for a module that is only mapped).
// mapOnly: every import resolves to game's address with no policy at all -- the module is only mapped and one
// of its functions called, so what its imports would be on Switch does not matter here.
bool load(const std::string& path, Mapped& m, bool run, GameLookup game, bool mapOnly = false) {
    m.file = read_file(path);
    const auto parsed = parse(m.file);
    if (!parsed.image) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), parsed.diagnostic.message.c_str());
        return false;
    }
    const auto& image = *parsed.image;
    const Resolver resolver = mapOnly ? Resolver([game](const ImportQuery& q, std::string&) -> std::optional<Resolved> {
        return Resolved{*game(q.name), Source::Runtime};
    })
                                      : make_resolver(PackageAbi::Linux, std::move(game), linux_shims(), image_runtime(),
                                            tls::host_variables(tls::once_variables()));
    const auto p = plan(image, resolver, make_needed_policy(PackageAbi::Linux), true);
    if (!p.ok()) {
        for (const auto& pr : p.problems) std::fprintf(stderr, "%s: %s\n", path.c_str(), pr.message.c_str());
        return false;
    }
    auto* memory = new PosixMemory;
    auto mapped = map(image, p, *memory, &tls::runtime());
    if (!mapped.loaded) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), mapped.diagnostic.message.c_str());
        return false;
    }
    m.loaded = std::move(*mapped.loaded);
    dl::add({path, m.loaded.runtimeBase - image.base, image.phdrTable, image.phnum, m.loaded.tlsModule});
    if (run) {
        if (m.loaded.ehFrame != 0) __register_frame(reinterpret_cast<void*>(static_cast<uintptr_t>(m.loaded.ehFrame)));
        run_constructors(m.loaded);
    }
    return true;
}

GameLookup this_process() {
    return [](std::string_view name) -> std::optional<uint64_t> {
        void* p = dlsym(RTLD_DEFAULT, std::string(name).c_str());
        return p ? std::optional<uint64_t>{reinterpret_cast<uintptr_t>(p)} : std::nullopt;
    };
}

[[noreturn]] void stub() {
    std::fprintf(stderr, "a mapped-only module called an import\n");
    std::abort();
}

void glibc_abi_checks(const std::string& dir) {
    std::printf("== glibc ABI table against aarch64 glibc itself\n");
    int entries = 0, mismatched = 0;
    for (const auto& e : glibc::table()) {
        ++entries;
        if (e.glibc != e.native) {
            std::fprintf(stderr, "  %s %s: table says glibc %lld, glibc's header says %lld\n", e.group, e.name, e.glibc, e.native);
            ++mismatched;
        }
    }
    CHECK(entries > 150 && mismatched == 0);
    std::printf("  %d entries, %d mismatched\n", entries, mismatched);

    std::printf("== the shims (identity translations on glibc)\n");
    const std::string path = dir + "/dm2d-open-test.txt";
    unlink(path.c_str());
    const int fd = aleks2_glibc_open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
    CHECK(fd >= 0 && write(fd, "x", 1) == 1);
    CHECK(aleks2_glibc_fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
    CHECK((aleks2_glibc_fcntl(fd, F_GETFL) & O_NONBLOCK) != 0);
    CHECK(aleks2_glibc_fcntl(fd, 12345) == -1 && errno == EINVAL);  // a command glibc does not have
    close(fd);
    errno = 0;
    CHECK(aleks2_glibc_open((dir + "/no/such/dir/x").c_str(), O_RDONLY) == -1 && errno == ENOENT);
    timespec a{}, b{};
    CHECK(aleks2_glibc_clock_gettime(1 /* glibc CLOCK_MONOTONIC */, &a) == 0 && aleks2_glibc_clock_gettime(1, &b) == 0);
    CHECK(b.tv_sec > a.tv_sec || (b.tv_sec == a.tv_sec && b.tv_nsec >= a.tv_nsec));
    CHECK(aleks2_glibc_clock_gettime(10 /* no such glibc clock */, &a) == -1 && errno == EINVAL);
    const time_t t = 1700000000;
    struct tm mine{}, theirs{};
    CHECK(aleks2_glibc_localtime_r(&t, &mine) == &mine && localtime_r(&t, &theirs) != nullptr);
    CHECK(std::memcmp(&mine, &theirs, 9 * sizeof(int)) == 0 && mine.tm_gmtoff == theirs.tm_gmtoff);
    CHECK(mine.tm_zone != nullptr && theirs.tm_zone != nullptr && std::strcmp(mine.tm_zone, theirs.tm_zone) == 0);
    CHECK(aleks2_glibc_gmtime_r(&t, &mine) == &mine && mine.tm_gmtoff == 0 && mine.tm_hour == 22);
    CHECK(std::strcmp(aleks2_glibc_strerror(ENOENT), std::strerror(ENOENT)) == 0);
    unlink(path.c_str());
    std::printf("  open/fcntl flags, failing open errno, clocks, localtime_r/gmtime_r (tm_gmtoff, tm_zone), strerror\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s DIR [TWILIT_SO]\n", argv[0]);
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::string dir = argv[1];
    dl::set_tls_block_lookup(&tls::current_block);

    glibc_abi_checks(dir);

    std::printf("== dl_iterate_phdr over modules mapped by the loader\n");
    Mapped walk, unwind;
    CHECK(load(dir + "/dm2d_dl.so", walk, true, this_process()));
    CHECK(load(dir + "/dm2d_unwind.so", unwind, true, this_process()));
    CHECK(dl::object_count() == 2);
    for (Mapped* m : {&walk, &unwind}) {
        if (m->loaded.runtimeBase == 0) continue;
        uintptr_t start = 0, size = 0;
        int objects = 0, tlsOk = 0;
        size_t modid = 0, infoSize = 0;
        const int found = m->fn<int(uintptr_t*, uintptr_t*, int*, size_t*, int*, size_t*)>("dm2d_walk")(
            &start, &size, &objects, &modid, &tlsOk, &infoSize);
        CHECK(found == 1 && size > 0 && infoSize == 64);
        CHECK(modid == m->loaded.tlsModule && modid != 0 && tlsOk == 1);
        CHECK(m->fn<uint64_t()>("dm2d_fingerprint")() != 0);
        CHECK(m->fn<int()>("dm2d_throw_catch")() == 1);
        std::printf("  %s: found itself (%d object(s) walked), exec segment %#lx+%#lx, TLS module %zu with this "
                    "thread's block, fingerprint, throw/catch\n",
            m == &walk ? "dm2d_dl.so" : "dm2d_unwind.so", objects, (unsigned long)start, (unsigned long)size, modid);
    }
    if (unwind.loaded.runtimeBase != 0) {
        const int depth = unwind.fn<int()>("dm2d_backtrace_depth")();
        std::printf("  dm2d_unwind.so: its own libgcc_eh unwound %d frame(s) (needs >= 10)\n", depth);
        CHECK(depth >= 10);
    }

    if (argc >= 3) {
        std::printf("== Twilit Essentials 2.8.4's own dl_iterate_phdr callback\n");
        Mapped twilit;
        const auto everything = [](std::string_view) -> std::optional<uint64_t> {
            return reinterpret_cast<uintptr_t>(&stub);
        };
        CHECK(load(argv[2], twilit, false, everything, true));
        if (twilit.loaded.runtimeBase != 0) {
            const auto parsed = parse(twilit.file);
            const uint64_t bias = twilit.loaded.runtimeBase - parsed.image->base;
            uint64_t data[3] = {bias + 0x111c38, 0, 0};
            const auto callback = reinterpret_cast<int (*)(void*, size_t, void*)>(static_cast<uintptr_t>(bias + 0x114298));
            const int r = aleks2_dl_iterate_phdr(callback, data);
            // Its executable PT_LOAD, from the file.
            uint64_t execVaddr = 0, execMem = 0;
            for (const auto& s : parsed.image->loads) {
                if ((s.perm & kPermX) && execMem == 0) {
                    execVaddr = s.vaddr;
                    execMem = s.memsz;
                }
            }
            std::printf("  callback returned %d: segment %#lx+%#lx (expected %#lx+%#lx)\n", r, (unsigned long)data[1],
                (unsigned long)data[2], (unsigned long)(bias + execVaddr), (unsigned long)execMem);
            CHECK(r == 1 && data[1] == bias + execVaddr && data[2] == execMem);
        }
    }

    if (g_failures) {
        std::fprintf(stderr, "elf_dm2d_exec_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("elf_dm2d_exec_test: all checks passed\n");
    return 0;
}
