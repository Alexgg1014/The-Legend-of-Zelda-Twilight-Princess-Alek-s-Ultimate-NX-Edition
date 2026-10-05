// aleks2: dusk_switch_log() was Aleks' raw line logger (its old port had no
// borealis file logging). The companion profiler and a few diagnostics still
// call it; route it into the normal dusk log so it lands in
// sdmc:/switch/dusklight/cache/logs/ with everything else.
#include "dusk/logging.h"

#include <cstring>
#include <string_view>

extern "C" void dusk_switch_log(const char* msg) {
    if (msg == nullptr) {
        return;
    }
    std::string_view line{msg};
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    DuskLog.info("{}", line);
}
