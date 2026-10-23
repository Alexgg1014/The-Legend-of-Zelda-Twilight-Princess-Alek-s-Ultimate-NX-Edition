// DM3: the sockets layer under AddressSanitizer/UBSan on the x86-64 host. test-dm3.sh compiles
// dm3_net_lab_module.c for x86-64, renames every socket call in the object to its glibc_net.cpp shim
// (objcopy --redefine-sym, epoll_* excepted: x86-64 packs struct epoll_event, aarch64 does not), and links it
// with glibc_net.cpp/glibc_abi.cpp here. The platform hooks are set as the console sets them (uname describes
// Horizon on aarch64), so the shimmed-only checks run too.
//
//   dm3_asan WORKDIR [HTTP_HOST HTTP_PORT [DNS_NAME]]
#include "dusk/mods/loader/glibc_net.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" int dm3_net_lab_run(char* report, size_t size, int flags, const char* host, int port, const char* name);

int main(int argc, char** argv) {
    using namespace dusk::mods::elf::glibc::net;
    Platform p;
    p.describe = [](std::string& sysname, std::string& release, std::string& version, std::string& machine) {
        sysname = "Horizon";
        release = "HOS 99.0.0 (host test)";
        version = "Dusklight DM3 host";
        machine = "aarch64";
    };
    p.log = [](int, const std::string& line) { std::printf("  [layer] %s\n", line.c_str()); };
    set_platform(p);
    std::vector<char> report(64 * 1024);
    const int fails = dm3_net_lab_run(report.data(), report.size(), 3, argc >= 4 ? argv[2] : "",
        argc >= 4 ? std::atoi(argv[3]) : 0, argc >= 5 ? argv[4] : "");
    std::fputs(report.data(), stdout);
    const auto c = counters();
    std::printf("dm3_asan: %d failure(s); %zu socket(s) left open\n", fails, c.sockets);
    return fails == 0 && c.sockets == 0 ? 0 : 1;
}
