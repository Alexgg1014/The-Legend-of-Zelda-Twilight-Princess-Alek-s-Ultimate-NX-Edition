// aleks2 (DM3): the console self-test of the sockets layer for Linux mods (src/dusk/mods/loader/glibc_net.cpp).
//
// Runs only if <mods directory>/../net-selftest/net_lab.so exists: the lab module the host tests build
// (platforms/switch/tools/dm3_net_lab_module.c, aarch64-linux-gnu). It is mapped with the real loader and the
// production Linux resolver, exactly as a catalog mod's library is, and its checks run at loader start, before
// any mod (a few seconds; longer if target.txt names a server that does not answer): UDP and TCP over loopback, non-blocking connect,
// socket options, pipe/eventfd/epoll/select, getaddrinfo/getnameinfo, getifaddrs, uname, fork failing, and,
// if net-selftest/target.txt says "HOST PORT [DNS_NAME]", an HTTP GET to that server (a PC on the LAN running
// `python -m http.server PORT`) and the resolution of DNS_NAME.
// Writes net-selftest/result.txt and the game log; the last line is "NET SELFTEST RESULT: PASS|FAIL|SKIPPED".
// What only the console can answer is here: Horizon's BSD sockaddr (sa_len) and addrinfo layout under the
// translation, loopback on bsd:u, the resolver, nifm's view for getifaddrs.
#include "dusk/switch_net_selftest.hpp"

#include "dusk/mods/loader/elf_loader.hpp"
#include "dusk/mods/loader/elf_tls.hpp"
#include "dusk/mods/loader/glibc_net.hpp"
#include "dusk/switch_elf_memory.hpp"
#include "dusk/switch_native_loader.hpp"

#include <borealis/log.hpp>
#include <switch.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" void __register_frame(void* begin);

namespace dusk::switchelf {
namespace fs = std::filesystem;
namespace elf = mods::elf;

namespace {
constexpr borealis::Log Log{"aleks2::net-selftest"};

using RunFn = int(char*, size_t, int, const char*, int, const char*);

void run(fs::path dir, std::vector<uint8_t> file) {
    const fs::path resultPath = dir / "result.txt";
    std::FILE* out = std::fopen(resultPath.string().c_str(), "w");
    const auto line = [&](const std::string& text) {
        Log.info("{}", text);
        if (out != nullptr) {
            std::fprintf(out, "%s\n", text.c_str());
            std::fflush(out);
        }
    };
    line("DM3 net self-test: " + (dir / "net_lab.so").string());
    char host[128] = "";
    char name[128] = "";
    int port = 0;
    if (std::FILE* t = std::fopen((dir / "target.txt").string().c_str(), "r")) {
        if (std::fscanf(t, "%127s %d %127s", host, &port, name) < 2) {
            host[0] = 0;
            port = 0;
        }
        std::fclose(t);
    }
    line(port > 0 ? std::string{"HTTP target "} + host + ":" + std::to_string(port) + (name[0] ? std::string{", DNS name "} + name : "")
                  : "no target.txt: loopback, resolver and interfaces only");
    const auto parsed = elf::parse(file);
    std::string why;
    RunFn* fn = nullptr;
    if (!parsed.image) {
        why = "parse: " + parsed.diagnostic.message;
    } else {
        const auto plan = elf::plan(*parsed.image, linux_tls_resolver(), elf::make_needed_policy(elf::PackageAbi::Linux), true);
        if (!plan.ok()) {
            why = "plan: " + plan.problems.front().message;
        } else {
            auto* memory = new CodeMemory;  // kept for the life of the process, as a mod's mapping is
            auto mapped = elf::map(*parsed.image, plan, *memory, &elf::tls::runtime());
            if (!mapped.loaded) {
                why = "map: " + mapped.diagnostic.message;
            } else {
                static elf::Loaded s_loaded;
                s_loaded = std::move(*mapped.loaded);
                if (s_loaded.ehFrame != 0) {
                    __register_frame(reinterpret_cast<void*>(static_cast<uintptr_t>(s_loaded.ehFrame)));
                }
                elf::run_constructors(s_loaded);
                fn = reinterpret_cast<RunFn*>(static_cast<uintptr_t>(s_loaded.find("dm3_net_lab_run")));
                if (fn == nullptr) {
                    why = "the module has no dm3_net_lab_run";
                }
            }
        }
    }
    if (fn == nullptr) {
        line("FAIL could not load the lab module: " + why);
        line("NET SELFTEST RESULT: FAIL");
        if (out != nullptr) std::fclose(out);
        return;
    }
    std::vector<char> report(64 * 1024);
    const int fails = fn(report.data(), report.size(), 3, host, port, name);
    // One log line per report line.
    const char* p = report.data();
    while (*p != 0) {
        const char* e = std::strchr(p, '\n');
        const std::string l = e != nullptr ? std::string{p, e} : std::string{p};
        line(l);
        if (e == nullptr) break;
        p = e + 1;
    }
    const auto c = elf::glibc::net::counters();
    line("layer: " + std::to_string(c.sockets) + " socket(s) still open, " + std::to_string(c.getaddrinfoCalls) +
         " getaddrinfo call(s), " + std::to_string(c.lanRebinds) + " LAN rebind(s)");
    line(std::string{"NET SELFTEST RESULT: "} + (fails == 0 ? "PASS" : "FAIL"));
    if (out != nullptr) std::fclose(out);
}

}  // namespace

void run_net_selftest(const fs::path& modsDirectory) {
    const fs::path dir = modsDirectory.parent_path() / "net-selftest";
    const fs::path library = dir / "net_lab.so";
    std::error_code ec;
    if (!fs::exists(library, ec)) {
        return;
    }
    if (const auto why = unavailable_reason()) {
        if (std::FILE* out = std::fopen((dir / "result.txt").string().c_str(), "w")) {
            std::fprintf(out, "SKIP: native code cannot be mapped on this launch: %s\nNET SELFTEST RESULT: SKIPPED\n", why->c_str());
            std::fclose(out);
        }
        Log.warn("net self-test skipped: {}", *why);
        return;
    }
    std::vector<uint8_t> file;
    if (std::FILE* f = std::fopen(library.string().c_str(), "rb")) {
        uint8_t chunk[64 * 1024];
        for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) file.insert(file.end(), chunk, chunk + n);
        std::fclose(f);
    }
    // On this thread, before any mod loads: a background thread could still be writing the log while a short
    // session (loader-only mode) is already exiting. It only runs when the tester put net_lab.so on the card;
    // without a reachable target it takes a few seconds.
    Log.info("net self-test: {} bytes", file.size());
    run(dir, std::move(file));
}

}  // namespace dusk::switchelf
