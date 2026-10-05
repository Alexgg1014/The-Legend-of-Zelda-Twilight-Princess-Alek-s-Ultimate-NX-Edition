#include "d/actor/d_a_alink.h"
#include "d/d_camera.h"
#include "mods/service.hpp"
#include "mods/svc/hook.h"
#include "mods/svc/hook.hpp"
#include <mods/svc/ui.h>

DEFINE_MOD();
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(ConfigService, svc_config);

DEFINE_HOOK(&daAlink_c::procMoveInit, LinkProcMoveInit);
DEFINE_HOOK(&daAlink_c::setDoubleAnime, LinkSetDoubleAnime);
DEFINE_HOOK(&daAlink_c::checkNormalAction, LinkCheckCutAction);
DEFINE_HOOK(&daAlink_c::decideCommonDoStatus, LinkDecideCommonDoStatus);

UiElementHandle statusText1 = 0;
UiElementHandle statusText2 = 0;
UiElementHandle statusText3 = 0;
ConfigVarHandle var1 = 0;
ConfigVarHandle var2 = 0;
ConfigVarHandle var3 = 0;

bool running = false;
bool holdingA = false;

extern "C" {

HookAction link_proc_move_init_pre(ModContext* ctx, void* args, void* retval, void*) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (!running && link && link->mProcID == daAlink_c::daAlink_PROC::PROC_FRONT_ROLL) {
        if (mDoCPd_c::getHoldA(0) != 0 && !link->checkEventRun() && !link->checkBootsOrArmorHeavy())
        {
            if (link->mEquipItem != 0xFF) {
                link->allUnequip(0);
            }

            link->setSwordVoiceSe(Z2SE_AL_V_THROW_IB);
            running = true;
            holdingA = true;

            // Set here to prevent roll briefly appearing when in non-toggle mode
            link->setDoStatus(BUTTON_STATUS_NONE);

            dCamera_c* camera = dCam_getBody();
            if (camera) {
                camera->mCamParam.mManualMode = 0;
            }
        }
    }
    return HOOK_CONTINUE;
}

HookAction link_set_double_anime_pre(ModContext* ctx, void* args, void*, void*) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link && running) {
        daAlink_c::daAlink_ANM& linkAnm = mods::arg_ref<daAlink_c::daAlink_ANM>(args, 5);

        if (linkAnm == link->ANM_RUN) {
            int64_t speed = 100;
            svc_config->get_int(mod_ctx, var1, &speed);
            f32& anmSpeed = mods::arg_ref<f32>(args, 3);
            anmSpeed = 2.0f * (speed / 100.0f);
            linkAnm = link->ANM_RUN_B;
            link->mNormalSpeed = 35.0f * (speed / 100.0f);
        }
    }
    return HOOK_CONTINUE;
}

HookAction link_check_cut_action_pre(ModContext* ctx, void* args, void* retval, void*) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (link && running && link->swordSwingTrigger()) {
        u16 equipSword = dComIfGs_getSelectEquipSword();

        bool allowNoSword = false;
        svc_config->get_bool(mod_ctx, var2, &allowNoSword);
        if (!allowNoSword && equipSword == 0xFF) {
            return HOOK_CONTINUE;
        }

        if (!link->checkSwordEquipAnime() && equipSword != 0xFF) {
            if (link->checkWoodSwordEquip()) {
                link->seStartSwordCut(Z2SE_AL_ITEM_TAKEOUT_FAST);
            } else {
                link->seStartSwordCut(Z2SE_AL_SWORD_PULLOUT);
            }
        }

        link->mEquipItem = 0x103;
        link->setItemModel();
        link->procCutJumpInit(FALSE);
        running = false;

        if (retval != nullptr) {
            *static_cast<int*>(retval) = 1;
        }

        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

void link_decide_common_do_status_post(ModContext* ctx, void* args, void* retval, void*) {
    daAlink_c* link = daAlink_getAlinkActorClass();

    bool toggle = false;
    svc_config->get_bool(mod_ctx, var3, &toggle);

    // BUTTON_STATUS_UNK_121 is Roll
    if (dComIfGp_getDoStatus() == BUTTON_STATUS_UNK_121)
    {
        if (!toggle)
        {
            if (running)
            {
                link->setDoStatus(BUTTON_STATUS_NONE);
            }
        }
        else
        {
            if (running)
            {
                link->setDoStatus(BUTTON_STATUS_CANCEL);
            }
            else if (holdingA)
            {
                link->setDoStatus(BUTTON_STATUS_NONE);
            }
        }
    }
}

ModResult build(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, panel, "Settings");

    UiControlDesc control1 = UI_CONTROL_DESC_INIT;
    control1.kind = UI_CONTROL_NUMBER;
    control1.label = "Speed Multiplier (%)";
    control1.help_rml = "The % of speed at which Link sprints at. Default is 100%.";
    control1.binding = UI_BINDING_CONFIG_VAR;
    control1.config_var = var1;  // from svc_config->register_var
    control1.min = 80;
    control1.max = 200;
    control1.step = 5;
    svc_ui->pane_add_control(mod_ctx, panel, &control1, &statusText1);

    UiControlDesc control2 = UI_CONTROL_DESC_INIT;
    control2.kind = UI_CONTROL_TOGGLE;
    control2.label = "Allow Jump Slash Without Sword";
    control2.help_rml = "Allow Jump Slashing even if you don't have a sword equipped.";
    control2.binding = UI_BINDING_CONFIG_VAR;
    control2.config_var = var2;  // from svc_config->register_var
    svc_ui->pane_add_control(mod_ctx, panel, &control2, &statusText2);

    UiControlDesc control3 = UI_CONTROL_DESC_INIT;
    control3.kind = UI_CONTROL_TOGGLE;
    control3.label = "Toggle Sprint";
    control3.help_rml = "Shown in the help pane while focused.";
    control3.binding = UI_BINDING_CONFIG_VAR;
    control3.config_var = var3;  // from svc_config->register_var
    svc_ui->pane_add_control(mod_ctx, panel, &control3, &statusText3);

    return MOD_OK;
}

ModResult update(ModContext*, void*, ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_initialize(ModError*) {
    ConfigVarDesc desc1 = CONFIG_VAR_DESC_INIT;
    desc1.name = "speedMultiplier";
    desc1.type = CONFIG_VAR_INT;
    desc1.default_int = 100;
    svc_config->register_var(mod_ctx, &desc1, &var1);

    ConfigVarDesc desc2 = CONFIG_VAR_DESC_INIT;
    desc2.name = "allowWithoutSword";
    desc2.type = CONFIG_VAR_BOOL;
    desc2.default_bool = false;
    svc_config->register_var(mod_ctx, &desc2, &var2);

    ConfigVarDesc desc3 = CONFIG_VAR_DESC_INIT;
    desc3.name = "toggleSprint";
    desc3.type = CONFIG_VAR_BOOL;
    desc3.default_bool = false;
    svc_config->register_var(mod_ctx, &desc3, &var3);

    mods::hook::add_pre<LinkProcMoveInit>(link_proc_move_init_pre);
    mods::hook::add_pre<LinkSetDoubleAnime>(link_set_double_anime_pre);
    mods::hook::add_pre<LinkCheckCutAction>(link_check_cut_action_pre);
    mods::hook::add_post<LinkDecideCommonDoStatus>(link_decide_common_do_status_post);

    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build;
    panel.update = update;
    svc_ui->register_mods_panel(mod_ctx, &panel);

    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    daAlink_c* link = daAlink_getAlinkActorClass();
    if (mDoCPd_c::getHoldA(0) == 0)
    {
        holdingA = false;
    }

    if (link)
    {
        bool toggle = false;
        svc_config->get_bool(mod_ctx, var3, &toggle);

        if (!toggle)
        {
            if (running && mDoCPd_c::getHoldA(0) == 0 || link->checkEventRun() ||
                link->mProcID != daAlink_c::daAlink_PROC::PROC_MOVE)
            {
                running = false;
            }
        }
        else
        {
            if (running && !holdingA && mDoCPd_c::getHoldA(0) != 0 || link->checkEventRun() ||
                link->mProcID != daAlink_c::daAlink_PROC::PROC_MOVE || mDoCPd_c::getStickValue(0) == 0)
            {
                running = false;
                // If running is false but this is true then we want to avoid rolling
                holdingA = true;
            }
        }
    }

    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    return MOD_OK;
}
}
