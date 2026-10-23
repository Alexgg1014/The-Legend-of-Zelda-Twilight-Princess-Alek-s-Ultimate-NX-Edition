// DM3: the sockets layer for Linux-built modules, really executed. Built for aarch64-linux and run under
// qemu-aarch64 by test-dm3.sh.
//
//   elf_dm3_exec_test DIR [HTTP_HOST HTTP_PORT [DNS_NAME]]
//
// 1. Translation units: the BSD sockaddr path (sa_len, 1-byte family) is checked byte by byte with an explicit
//    layout, since this glibc host's own sockaddr is glibc's; the Linux-style truncation of an output address;
//    the bit-group and option maps on this host (identity) and their refusals.
// 2. dm3_net_lab.so (dm3_net_lab_module.c) runs twice: natively (dlopen, glibc underneath) and mapped by the real
//    loader with the production Linux shim tables (switch_native_loader.cpp's order: DM2-d, DM3, dl, loader,
//    TLS), every socket call going through glibc_net.cpp. Both must pass every check, and both must report the
//    same checks, except those only the shimmed run makes (fork/execvp/waitpid failing as on the console).
#include "dusk/mods/loader/elf_dl.hpp"
#include "dusk/mods/loader/elf_loader.hpp"
#include "dusk/mods/loader/elf_runtime_policy.hpp"
#include "dusk/mods/loader/elf_tls.hpp"
#include "dusk/mods/loader/glibc_abi.hpp"
#include "dusk/mods/loader/glibc_net.hpp"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <poll.h>
#include <cstdlib>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace dusk::mods::elf;

extern "C" void __register_frame(void*);

namespace {
int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using RunFn = int(char*, size_t, int, const char*, int, const char*);

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
    for (const auto& s : glibc::net::shims()) table.push_back(s);
    for (const auto& s : dl::shims()) table.push_back(s);
    for (const auto& s : shims()) table.push_back(s);
    for (const auto& s : tls::shims()) table.push_back(s);
    for (const auto& s : tls::once_shims()) table.push_back(s);
    return table;
}

GameLookup this_process() {
    return [](std::string_view name) -> std::optional<uint64_t> {
        void* p = dlsym(RTLD_DEFAULT, std::string(name).c_str());
        return p ? std::optional<uint64_t>{reinterpret_cast<uintptr_t>(p)} : std::nullopt;
    };
}

RunFn* load_shimmed(const std::string& path, std::vector<std::string>& shimmedImports) {
    static std::vector<uint8_t> file;
    file = read_file(path);
    const auto parsed = parse(file);
    if (!parsed.image) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), parsed.diagnostic.message.c_str());
        return nullptr;
    }
    const auto& image = *parsed.image;
    const Resolver inner = make_resolver(PackageAbi::Linux, this_process(), linux_shims(), image_runtime(),
        tls::host_variables(tls::once_variables()));
    const Resolver resolver = [&inner, &shimmedImports](const ImportQuery& q, std::string& why) -> std::optional<Resolved> {
        auto r = inner(q, why);
        if (r && r->source == Source::Shim) {
            shimmedImports.emplace_back(q.name);
        }
        return r;
    };
    const auto p = plan(image, resolver, make_needed_policy(PackageAbi::Linux), true);
    if (!p.ok()) {
        for (const auto& pr : p.problems) std::fprintf(stderr, "%s: %s\n", path.c_str(), pr.message.c_str());
        return nullptr;
    }
    auto* memory = new PosixMemory;
    auto mapped = map(image, p, *memory, &tls::runtime());
    if (!mapped.loaded) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), mapped.diagnostic.message.c_str());
        return nullptr;
    }
    static Loaded loaded;
    loaded = std::move(*mapped.loaded);
    dl::add({path, loaded.runtimeBase - image.base, image.phdrTable, image.phnum, loaded.tlsModule});
    if (loaded.ehFrame != 0) __register_frame(reinterpret_cast<void*>(static_cast<uintptr_t>(loaded.ehFrame)));
    run_constructors(loaded);
    return reinterpret_cast<RunFn*>(static_cast<uintptr_t>(loaded.find("dm3_net_lab_run")));
}

std::set<std::string> checks_of(const std::string& report) {
    std::set<std::string> names;
    std::istringstream in(report);
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("PASS ", 0) == 0 || line.rfind("FAIL ", 0) == 0) {
            std::string name = line.substr(5);
            name = name.substr(0, name.find(": ("));
            if (line[0] == 'F') {
                name = name.substr(0, name.find(':'));
            }
            names.insert(name);
        }
    }
    return names;
}

void translation_checks() {
    std::printf("== translation\n");
    // glibc sockaddr_in {02 00, port 0x1f90 (8080) big-endian, 192.168.1.20} -> BSD {16, 2, ...}.
    const uint8_t glibcIn[16] = {2, 0, 0x1f, 0x90, 192, 168, 1, 20};
    uint8_t bsd[128]{};
    CHECK(glibc::net::sockaddr_from_glibc<true>(glibcIn, 16, bsd, sizeof bsd) == 16);
    CHECK(bsd[0] == 16 && bsd[1] == glibc::net::af_from_glibc(2) && std::memcmp(bsd + 2, glibcIn + 2, 14) == 0);
    // Linux accepts a longer length (sizeof sockaddr_storage); BSD gets the family's own length.
    uint8_t storage[128]{};
    std::memcpy(storage, glibcIn, 16);
    CHECK(glibc::net::sockaddr_from_glibc<true>(storage, 128, bsd, sizeof bsd) == 16 && bsd[0] == 16);
    // A short sockaddr_in is refused (EINVAL in the shim).
    CHECK(glibc::net::sockaddr_from_glibc<true>(glibcIn, 8, bsd, sizeof bsd) == 0);
    // glibc AF_INET6 (10) -> sa_len 28, the native family byte.
    uint8_t glibc6[28]{10, 0, 0, 80};
    glibc6[27] = 1;
    CHECK(glibc::net::sockaddr_from_glibc<true>(glibc6, 28, bsd, sizeof bsd) == 28);
    CHECK(bsd[0] == 28 && bsd[1] == static_cast<uint8_t>(glibc::net::af_from_glibc(10)) && bsd[27] == 1);
    // Back: BSD -> glibc, the family as two bytes, full and truncated.
    uint8_t back[128];
    std::memset(back, 0xee, sizeof back);
    uint32_t len = sizeof back;
    const uint8_t bsdIn[16] = {16, static_cast<uint8_t>(glibc::net::af_from_glibc(2)), 0x1f, 0x90, 10, 0, 0, 7};
    glibc::net::sockaddr_to_glibc<true>(bsdIn, 16, back, &len);
    CHECK(len == 16 && back[0] == 2 && back[1] == 0 && back[2] == 0x1f && back[7] == 7 && back[16] == 0xee);
    std::memset(back, 0xee, sizeof back);
    len = 4;
    glibc::net::sockaddr_to_glibc<true>(bsdIn, 16, back, &len);
    CHECK(len == 16 && back[0] == 2 && back[3] == 0x90 && back[4] == 0xee);
    // Horizon may report sa_len shorter than the family's size: the module still gets a whole sockaddr_in.
    len = sizeof back;
    glibc::net::sockaddr_to_glibc<true>(bsdIn, 8, back, &len);
    CHECK(len == 16);
    // The glibc (non-BSD) path is the identity on this host.
    CHECK(glibc::net::sockaddr_from_glibc<false>(glibcIn, 16, bsd, sizeof bsd) == 16 && std::memcmp(bsd, glibcIn, 16) == 0);
    // Bit groups and options on this host: the table maps glibc to glibc.
    CHECK(glibc::net::bits_from_glibc("msg", 0x40 | 0x4000) == (0x40 | 0x4000));
    CHECK(glibc::net::bits_to_glibc("poll", POLLIN | POLLOUT) == (POLLIN | POLLOUT));
    int dropped = 0;
    CHECK(glibc::net::bits_from_glibc("msg", 0x40000000 | 0x40, &dropped) == (0x40000000 | 0x40) && dropped == 0);
    const auto reuse = glibc::net::option_from_glibc(1, 2);
    CHECK(reuse.level == SOL_SOCKET && reuse.name == SO_REUSEADDR);
    CHECK(glibc::net::option_from_glibc(1, 0x7fff).level == -1);
    CHECK(glibc::net::option_from_glibc(12345, 1).level == -1);
    CHECK(glibc::net::eai_to_glibc(0) == 0);
    std::printf("  BSD sockaddr in/out (sa_len, family byte, truncation, short sa_len), bit groups, options\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s DIR [HTTP_HOST HTTP_PORT [DNS_NAME]]\n", argv[0]);
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::string dir = argv[1];
    const char* host = argc >= 4 ? argv[2] : "";
    const int port = argc >= 4 ? std::atoi(argv[3]) : 0;
    const char* name = argc >= 5 ? argv[4] : "";
    dl::set_tls_block_lookup(&tls::current_block);
    glibc::net::Platform platform;
    platform.log = [](int level, const std::string& line) { std::printf("  [layer %s] %s\n", level ? "warn" : "info", line.c_str()); };
    glibc::net::set_platform(platform);

    translation_checks();

    std::vector<char> nativeReport(64 * 1024), shimReport(64 * 1024);
    std::printf("== dm3_net_lab.so natively (dlopen, glibc)\n");
    void* h = dlopen((dir + "/dm3_net_lab.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    CHECK(h != nullptr);
    int nativeFails = -1;
    if (h != nullptr) {
        auto* run = reinterpret_cast<RunFn*>(dlsym(h, "dm3_net_lab_run"));
        nativeFails = run(nativeReport.data(), nativeReport.size(), 0, host, port, name);
        std::fputs(nativeReport.data(), stdout);
    }
    CHECK(nativeFails == 0);

    std::printf("== dm3_net_lab.so mapped by the loader, every socket call through glibc_net.cpp\n");
    std::vector<std::string> shimmed;
    RunFn* run = load_shimmed(dir + "/dm3_net_lab.so", shimmed);
    CHECK(run != nullptr);
    int shimFails = -1;
    if (run != nullptr) {
        shimFails = run(shimReport.data(), shimReport.size(), 1, host, port, name);
        std::fputs(shimReport.data(), stdout);
        std::string list;
        for (const auto& s : shimmed) list += " " + s;
        std::printf("  imports bound to shims (%zu):%s\n", shimmed.size(), list.c_str());
        for (const char* must : {"socket", "bind", "connect", "accept4", "recvfrom", "sendto", "setsockopt", "getsockopt",
                 "getsockname", "poll", "ioctl", "getaddrinfo", "freeaddrinfo", "getnameinfo", "getifaddrs", "pipe2",
                 "eventfd", "epoll_wait", "select", "fork", "uname", "read", "write", "close"}) {
            bool found = false;
            for (const auto& s : shimmed) found = found || s == must;
            if (!found) {
                std::fprintf(stderr, "  not shimmed: %s\n", must);
            }
            CHECK(found);
        }
    }
    CHECK(shimFails == 0);

    // Same checks, except the console-only ones.
    auto a = checks_of(nativeReport.data());
    auto b = checks_of(shimReport.data());
    std::set<std::string> extra;
    for (const auto& n : b) {
        if (!a.count(n)) extra.insert(n);
    }
    for (const auto& n : a) {
        if (!b.count(n)) {
            std::fprintf(stderr, "  only in the native run: %s\n", n.c_str());
            ++g_failures;
        }
    }
    for (const auto& n : extra) {
        const bool consoleOnly = n.rfind("fork", 0) == 0 || n.rfind("execvp", 0) == 0 || n.rfind("waitpid", 0) == 0 ||
                                 n.rfind("uname", 0) == 0;
        if (!consoleOnly) {
            std::fprintf(stderr, "  only in the shimmed run: %s\n", n.c_str());
            ++g_failures;
        }
    }
    const auto c = glibc::net::counters();
    std::printf("  native %zu checks, shimmed %zu (+%zu console-only); layer counters: %zu open socket(s), %zu emulated, "
                "%llu getaddrinfo call(s)\n",
        a.size(), b.size(), extra.size(), c.sockets, c.emulated, static_cast<unsigned long long>(c.getaddrinfoCalls));
    CHECK(c.sockets == 0);  // the module closed everything it opened: the registry agrees

    if (g_failures) {
        std::fprintf(stderr, "elf_dm3_exec_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("elf_dm3_exec_test: all checks passed\n");
    return 0;
}
