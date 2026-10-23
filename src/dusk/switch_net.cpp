// aleks2: bring the network up on Switch.
//
// The port shipped a stub that answered "no HTTP backend on Switch" to
// everything, so the online mod browser, mod updates and the guide downloader
// had nothing to talk to. devkitPro's libcurl is a portlib and borealis already
// has a curl backend, so the code was all there -- see
// platforms/switch/cmake/CurlHttp.cmake.
//
// That libcurl does its own TLS through Horizon's ssl service (it references
// sslInitialize, sslCreateContext and nifmInitialize directly), so no TLS
// library has to be built or shipped. What it does NOT do is bring up the
// BSD sockets service, and libnx does not do it by default either: without
// socketInitialize every connect fails before a packet is sent.
//
// libnx runs __appInit before the C++ constructors, so doing it from a static
// initialiser here is early enough for anything that later asks for a URL, and
// avoids threading a call through upstream's startup.
//
// DM3: mods now open sockets of their own (the net and websocket services, and Linux mods' own libc calls), so
// the defaults change:
//   - BSD sessions: libnx's default is 3. Each blocking call in flight holds one (the net service's poll
//     thread, its resolver, curl, a WebSocket worker, a mod's own poll loop), so 3 is short: 4 by default (see kDefaultSessions).
//   - Everything is overridable at run time from sdmc:/switch/dusklight/net-lab.ini (one key=value per line:
//     bsd_sessions, tcp_tx_buf, tcp_rx_buf, tcp_tx_buf_max, tcp_rx_buf_max, udp_tx_buf, udp_rx_buf,
//     sb_efficiency), and the log says what was used and what it cost (process memory before and after).
//   - nifm answers "is there a connection" and "what is my IPv4 configuration" for the services and getifaddrs.
//   - Sockets do not survive sleep: on resume the listeners (net/websocket services, the Linux sockets layer)
//     close what they hold, and mods see a disconnect.
#ifdef __SWITCH__
#include <switch.h>

#include "dusk/switch_net.h"
#include "dusk/dev_mode.hpp"

#include "dusk/config.hpp"
#include "dusk/config_var.hpp"
#include "dusk/logging.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dusk::switchnet {
namespace {

// 4, not more: Ryujinx 1.9.3 hangs in the loader with 8 (DM3-c1, measured; 3 and 4 run). On the console more can be
// tried with net-lab.ini (bsd_sessions = 6 or 8) without a new build.
constexpr u32 kDefaultSessions = 4;

struct Startup {
    Result result = 0;
    bool started = false;
    bool fellBack = false;
    bool fromIni = false;
    SocketInitConfig config{};
    u64 usedBefore = 0, usedAfter = 0;
    size_t transferMemory = 0;

    Startup() {
        config = *socketGetDefaultInitConfig();
        config.num_bsd_sessions = kDefaultSessions;
        read_ini();
        svcGetInfo(&usedBefore, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        result = socketInitialize(&config);
        if (R_FAILED(result)) {
            // Never worse than before DM3: libnx's own defaults.
            fellBack = true;
            result = socketInitializeDefault();
        }
        started = R_SUCCEEDED(result);
        svcGetInfo(&usedAfter, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        // libnx's bsdInitialize: sb_efficiency * (tcp tx max-or-size + tcp rx max-or-size + udp tx + udp rx),
        // page-aligned (bsd.c, _bsdGetTransferMemSizeForConfig).
        const SocketInitConfig& c = fellBack ? *socketGetDefaultInitConfig() : config;
        const size_t tcpTx = c.tcp_tx_buf_max_size != 0 ? c.tcp_tx_buf_max_size : c.tcp_tx_buf_size;
        const size_t tcpRx = c.tcp_rx_buf_max_size != 0 ? c.tcp_rx_buf_max_size : c.tcp_rx_buf_size;
        transferMemory = (c.sb_efficiency * (tcpTx + tcpRx + c.udp_tx_buf_size + c.udp_rx_buf_size) + 0xfff) & ~size_t{0xfff};
    }

    ~Startup() {
        if (started) {
            socketExit();
        }
    }

    // Not newlib's stdio: this runs from a static initialiser, and fopen there hung the process before main
    // (DM3-c1, Ryujinx: right after the file system answered). libnx's file system calls and a hand parser.
    void read_ini() {
        FsFileSystem* sd = fsdevGetDeviceFileSystem("sdmc");
        if (sd == nullptr) {
            return;
        }
        if (!dusk::dev_mode()) {
            return;  // V1-rc5: net-lab.ini is a developer-mode file
        }
        char path[FS_MAX_PATH] = "/switch/dusklight/net-lab.ini";
        FsFile file;
        if (R_FAILED(fsFsOpenFile(sd, path, FsOpenMode_Read, &file))) {
            return;
        }
        char text[1024];
        u64 got = 0;
        const Result r = fsFileRead(&file, 0, text, sizeof text - 1, FsReadOption_None, &got);
        fsFileClose(&file);
        if (R_FAILED(r)) {
            return;
        }
        text[got] = 0;
        fromIni = true;
        for (char* line = text; line != nullptr && *line != 0;) {
            char* next = std::strchr(line, '\n');
            if (next != nullptr) {
                *next++ = 0;
            }
            parse_line(line);
            line = next;
        }
    }

    // "key = value" (decimal or 0x hex); '#' starts a comment line.
    void parse_line(const char* p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == 0) {
            return;
        }
        char key[64];
        size_t k = 0;
        while ((*p >= 'a' && *p <= 'z') || *p == '_' || (*p >= '0' && *p <= '9')) {
            if (k + 1 < sizeof key) key[k++] = *p;
            p++;
        }
        key[k] = 0;
        while (*p == ' ' || *p == '\t' || *p == '=') p++;
        unsigned long value = 0;
        bool digits = false;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            for (p += 2;; p++, digits = true) {
                const char c = *p;
                const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (d < 0) break;
                value = value * 16 + static_cast<unsigned>(d);
            }
        } else {
            for (; *p >= '0' && *p <= '9'; p++, digits = true) value = value * 10 + static_cast<unsigned>(*p - '0');
        }
        if (!digits) {
            return;
        }
        const auto set = [&](const char* name, u32& field, unsigned long lo, unsigned long hi) {
            if (std::strcmp(key, name) == 0 && value >= lo && value <= hi) {
                field = static_cast<u32>(value);
            }
        };
        set("bsd_sessions", config.num_bsd_sessions, 1, 32);
        set("tcp_tx_buf", config.tcp_tx_buf_size, 0x1000, 0x400000);
        set("tcp_rx_buf", config.tcp_rx_buf_size, 0x1000, 0x400000);
        set("tcp_tx_buf_max", config.tcp_tx_buf_max_size, 0, 0x400000);
        set("tcp_rx_buf_max", config.tcp_rx_buf_max_size, 0, 0x400000);
        set("udp_tx_buf", config.udp_tx_buf_size, 0x400, 0x100000);
        set("udp_rx_buf", config.udp_rx_buf_size, 0x400, 0x100000);
        set("sb_efficiency", config.sb_efficiency, 1, 8);
    }
};

// Not a pointer on purpose: the destructor closing the service on exit is the
// tidy behaviour, and there is nothing else in this file to order against.
Startup s_startup;

struct Nifm {
    std::once_flag once;
    bool ok = false;

    bool ready() {
        std::call_once(once, [this] { ok = R_SUCCEEDED(nifmInitialize(NifmServiceType_User)); });
        return ok;
    }
};

Nifm& nifm() {
    static Nifm* s = new Nifm;
    return *s;
}

struct Resume {
    std::mutex mutex;
    std::vector<std::pair<int, std::function<void()>>> listeners;
    int nextId = 1;
    uint32_t count = 0;
    AppletHookCookie cookie{};
    bool hooked = false;
};

Resume& resume_state() {
    static Resume* s = new Resume;
    return *s;
}

void on_applet(AppletHookType type, void*) {
    if (type != AppletHookType_OnResume) {
        return;
    }
    auto& r = resume_state();
    std::vector<std::function<void()>> copy;
    {
        std::lock_guard lock{r.mutex};
        r.count++;
        for (const auto& [_, l] : r.listeners) {
            copy.push_back(l);
        }
    }
    DuskLog.info("Network: resumed from sleep (#{}); {} listener(s) close what they held", r.count, copy.size());
    for (const auto& l : copy) {
        l();
    }
}

config::ConfigVar<bool>& lan_var() {
    // Registered on first use, as the loader mode is (switch_native_loader.cpp).
    static config::ConfigVar<bool> s_var{"aleks2.modsListenOnLan", false};
    static const bool s_registered = [] {
        config::Register(s_var);
        return true;
    }();
    (void)s_registered;
    return s_var;
}

std::string ip_text(uint32_t networkOrder) {
    const auto* b = reinterpret_cast<const uint8_t*>(&networkOrder);
    return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." + std::to_string(b[3]);
}

}  // namespace

bool available() noexcept {
    return s_startup.started;
}

void log_status() {
    if (s_startup.started) {
        const SocketInitConfig& c = s_startup.config;
        DuskLog.info("Network: BSD sockets up; HTTPS through Horizon's ssl service; {} BSD sessions{}{} "
                     "(tcp {}/{} KiB, max {}/{} KiB; udp {}/{} KiB; sb_efficiency {}); transfer memory {} KiB; "
                     "process memory {} -> {} KiB",
            s_startup.fellBack ? socketGetDefaultInitConfig()->num_bsd_sessions : c.num_bsd_sessions,
            s_startup.fromIni ? " (net-lab.ini)" : "", s_startup.fellBack ? " (libnx defaults: the requested config failed)" : "",
            c.tcp_tx_buf_size / 1024, c.tcp_rx_buf_size / 1024, c.tcp_tx_buf_max_size / 1024,
            c.tcp_rx_buf_max_size / 1024, c.udp_tx_buf_size / 1024, c.udp_rx_buf_size / 1024, c.sb_efficiency,
            s_startup.transferMemory / 1024, s_startup.usedBefore / 1024, s_startup.usedAfter / 1024);
        IpConfig ip;
        DuskLog.info("Network: link {}{}", link_name(link()),
            ip_config(ip) ? ", IPv4 " + ip_text(ip.address) + " mask " + ip_text(ip.netmask) : std::string{});
        if (lan_listen_allowed()) {
            DuskLog.warn("Network: mods may listen on the local network (Settings > Aleks)");
        }
    } else {
        // Worth a warning rather than silence: everything online then fails
        // with a connection error that looks like the site is down.
        DuskLog.warn("Network: socketInitialize failed (0x{:x}); "
                     "the mod browser and updates will not reach anything",
            s_startup.result);
    }
    auto& r = resume_state();
    std::lock_guard lock{r.mutex};
    if (!r.hooked) {
        // Here rather than in the static initialiser: the applet service is certainly up, and the hook runs
        // from appletMainLoop (SDL's event pump) on the main thread.
        appletHook(&r.cookie, on_applet, nullptr);
        r.hooked = true;
    }
}

Link link() {
    if (!s_startup.started || !nifm().ready()) {
        return Link::Unknown;
    }
    NifmInternetConnectionType type{};
    u32 strength = 0;
    NifmInternetConnectionStatus status{};
    if (R_FAILED(nifmGetInternetConnectionStatus(&type, &strength, &status))) {
        return Link::Offline;  // fails when there is no network at all (airplane mode, no Wi-Fi)
    }
    return status == NifmInternetConnectionStatus_Connected ? Link::Connected : Link::Connecting;
}

const char* link_name(Link l) {
    switch (l) {
    case Link::Connected: return "connected";
    case Link::Connecting: return "connecting";
    case Link::Offline: return "offline";
    default: return "unknown";
    }
}

bool ip_config(IpConfig& out) {
    if (!nifm().ready()) {
        return false;
    }
    if (R_FAILED(nifmGetCurrentIpConfigInfo(&out.address, &out.netmask, &out.gateway, &out.dns1, &out.dns2))) {
        return false;
    }
    return out.address != 0;
}

std::string system_release() {
    const u32 v = hosversionGet();
    std::string s = "HOS " + std::to_string(HOSVER_MAJOR(v)) + "." + std::to_string(HOSVER_MINOR(v)) + "." +
                    std::to_string(HOSVER_MICRO(v));
    if (hosversionIsAtmosphere()) {
        s += " (Atmosphere)";
    }
    return s;
}

int add_resume_listener(std::function<void()> listener) {
    auto& r = resume_state();
    std::lock_guard lock{r.mutex};
    const int id = r.nextId++;
    r.listeners.emplace_back(id, std::move(listener));
    return id;
}

void remove_resume_listener(int id) {
    auto& r = resume_state();
    std::lock_guard lock{r.mutex};
    std::erase_if(r.listeners, [id](const auto& p) { return p.first == id; });
}

uint32_t resume_count() {
    auto& r = resume_state();
    std::lock_guard lock{r.mutex};
    return r.count;
}

bool lan_listen_allowed() {
    return lan_var().getValue();
}

void set_lan_listen_allowed(bool allowed) {
    lan_var().setValue(allowed);
    config::save();
    DuskLog.info("Network: mods {} listen on the local network", allowed ? "may now" : "may no longer");
}

}  // namespace dusk::switchnet
#endif
