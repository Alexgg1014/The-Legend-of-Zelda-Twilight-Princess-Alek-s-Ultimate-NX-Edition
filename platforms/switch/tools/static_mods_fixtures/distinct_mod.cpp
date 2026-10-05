// H36 host-test fixture (platforms/switch/tools/test-static-mods.sh), never built into
// anything: a mod that hooks a target nobody else in the H36 set hooks, by name and by a
// virtual member, and logs through the C service only. It must link beside Roll and Sprint
// and Cosmetics, and its by-name requests must reach the generated table.
#include "d/actor/d_a_alink.h"
#include "d/d_meter_hakusha.h"
#include "mods/service.hpp"
#include "mods/svc/hook.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.h"

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);

DEFINE_HOOK(&daAlink_c::execute, FixtureLinkExecute);
DEFINE_HOOK(&dMeterHakusha_c::_execute, FixtureHakushaExecute);
DEFINE_HOOK_SYMBOL("fixture_c_function", void(), FixtureCFunction);
DEFINE_HOOK_SYMBOL("fixture_c::unmapped", void(), FixtureUnmapped);

static HookAction on_execute(ModContext*, void*, void*, void*) {
    return HOOK_CONTINUE;
}

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    svc_log->info(mod_ctx, "distinct fixture");
    mods::hook::add_pre<FixtureCFunction>(on_execute);
    mods::hook::add_pre<FixtureUnmapped>(on_execute);
    mods::hook::add_pre<FixtureHakushaExecute>(on_execute);
    return mods::hook::add_pre<FixtureLinkExecute>(on_execute);
}
MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}
MOD_EXPORT ModResult mod_shutdown(ModError*) {
    return MOD_OK;
}
}
