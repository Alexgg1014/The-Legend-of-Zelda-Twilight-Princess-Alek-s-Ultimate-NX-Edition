#include "dusk/dev_mode.hpp"

#include <switch.h>

namespace dusk {

// Checked once. libnx's file system calls, not stdio: switch_net.cpp asks from a static initialiser (net-lab.ini),
// where newlib stdio is not allowed (DM3-c1: the TLS it adds hung the start). sdmc is mounted by __appInit
// before any static initialiser runs.
bool dev_mode() {
    static const bool s_on = [] {
        FsFileSystem* sd = fsdevGetDeviceFileSystem("sdmc");
        if (sd == nullptr) {
            return false;
        }
        char path[FS_MAX_PATH] = "/switch/dusklight/dev.txt";
        FsDirEntryType type{};
        return R_SUCCEEDED(fsFsGetEntryType(sd, path, &type)) && type == FsDirEntryType_File;
    }();
    return s_on;
}

}  // namespace dusk
