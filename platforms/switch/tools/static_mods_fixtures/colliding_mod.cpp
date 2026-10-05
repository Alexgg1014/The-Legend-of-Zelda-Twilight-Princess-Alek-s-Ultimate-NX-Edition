// H36 host-test fixture (platforms/switch/tools/test-static-mods.sh), never built into
// anything: a mod that cannot share a static build with Roll and Sprint or Cosmetics.
// It hooks the target Roll and Sprint hooks, under the same alias name, and logs through
// mods::log with an int argument as Cosmetics does. Each of those is SDK template code the
// linker would keep once for every mod; static_mod_link.py has to refuse the set.
#include "d/actor/d_a_alink.h"
#include "mods/service.hpp"
#include "mods/svc/hook.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.h"
#include "mods/svc/log.hpp"

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);

DEFINE_HOOK(&daAlink_c::procMoveInit, LinkProcMoveInit);

static HookAction on_proc_move_init(ModContext*, void*, void*, void*) {
    return HOOK_CONTINUE;
}

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    mods::log::debug("colliding fixture {}", 1);
    return mods::hook::add_pre<LinkProcMoveInit>(on_proc_move_init);
}
MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}
MOD_EXPORT ModResult mod_shutdown(ModError*) {
    return MOD_OK;
}
}
