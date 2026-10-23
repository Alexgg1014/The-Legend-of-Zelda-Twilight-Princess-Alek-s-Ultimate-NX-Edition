// DM2-b lab mod: runs lab::run (lab.hpp) in mod_initialize, against the game's libstdc++ and the loader's
// glibc-layout file streams, and logs one line per check plus a summary:
//   [iostream-lab] RESULT: N passed, M failed (failures...)
// The scratch file goes to sdmc:/switch/dusklight/ and is removed at the end. It hooks nothing, so it is
// safe with the game running; it stays a lab mod (not for the catalogue).
#include "lab.hpp"

#include "mods/service.hpp"
#include "mods/svc/log.h"

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);

namespace {
void say(const char* line) {
    svc_log->info(mod_ctx, line);
}
}  // namespace

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    lab::Report report;
    report.say = say;
    try {
        lab::run("sdmc:/switch/dusklight/", report);
    } catch (const std::exception& e) {
        report.check(false, e.what());
    } catch (...) {
        report.check(false, "unknown exception");
    }
    char line[512];
    std::snprintf(line, sizeof line, "[iostream-lab] RESULT: %d passed, %d failed%s%s", report.passed, report.failed,
                  report.failed ? " -- " : "", report.failures.c_str());
    say(line);
    return MOD_OK;
}
MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}
MOD_EXPORT ModResult mod_shutdown(ModError*) {
    return MOD_OK;
}
}
