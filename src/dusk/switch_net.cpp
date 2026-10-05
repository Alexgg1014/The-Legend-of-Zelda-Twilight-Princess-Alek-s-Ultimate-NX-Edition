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
// socketInitializeDefault every connect fails before a packet is sent.
//
// libnx runs __appInit before the C++ constructors, so doing it from a static
// initialiser here is early enough for anything that later asks for a URL, and
// avoids threading a call through upstream's startup.
#ifdef __SWITCH__
#include <switch.h>

#include "dusk/logging.h"

namespace dusk::switchnet {
namespace {

struct Startup {
    Result result = 0;
    bool started = false;

    Startup() {
        result = socketInitializeDefault();
        started = R_SUCCEEDED(result);
    }

    ~Startup() {
        if (started) {
            socketExit();
        }
    }
};

// Not a pointer on purpose: the destructor closing the service on exit is the
// tidy behaviour, and there is nothing else in this file to order against.
Startup s_startup;

}  // namespace

bool available() noexcept {
    return s_startup.started;
}

void log_status() {
    if (s_startup.started) {
        DuskLog.info("Network: BSD sockets up; HTTPS through Horizon's ssl service");
    } else {
        // Worth a warning rather than silence: everything online then fails with
        // a connection error that looks like the site is down.
        DuskLog.warn("Network: socketInitializeDefault failed (0x{:x}); "
                     "the mod browser and updates will not reach anything",
            s_startup.result);
    }
}

}  // namespace dusk::switchnet
#endif
