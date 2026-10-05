#pragma once

// Access views onto game classes.
//
// The companion needs members and helpers the decomp's own classes do not
// declare. Adding them to the game's headers works, and is what this fork did
// first, but it puts our code inside files upstream owns: every Dusklight
// release then has to be merged through them, and anything that changes a
// class's layout breaks separately-compiled mods, which address members by
// absolute offset.
//
// A derived class that adds NO data members and NO virtual functions has the
// same layout and the same size as its base. So the declarations live here
// instead, on "access" subclasses, and a caller reaches them with
//
//     ga::cast(meter)->getHeartPictures(i, pics);
//
// which is a static_cast and nothing else: no conversion, no copy, no cost.
// The object really is the base type; this is a view over it.
//
// The technique is taken from SiggeMcKvack/dusklight-dualscreen-companion
// (CC0), which had to solve the same problem from the outside, as a mod.
//
// Rules for anything added here:
//   * methods only -- never a data member, never a virtual;
//   * the base class must expose what the method touches (the decomp's HUD
//     classes are public throughout);
//   * definitions go in src/dusk/game_access_*.cpp, not in the game's sources.

#include "d/d_menu_collect.h"
#include "d/d_menu_dmap.h"
#include "d/d_menu_fmap.h"
#include "d/d_menu_fmap2D.h"
#include "d/d_menu_window.h"
#include "d/d_meter2.h"
#include "d/d_meter2_draw.h"
#include "d/d_meter_map.h"

class dKantera_icon_c;
class J2DPane;
class J2DPicture;
struct ResTIMG;

namespace dusk::ga {

// --- dMeter2Draw_c: the HUD the companion mirrors ---------------------------
struct dMeter2DrawAccess : dMeter2Draw_c {
    // Panes and textures the companion re-draws on the second screen.
    const ResTIMG* getActionLabelTimg(int i_which);  // 0 = A, 1 = B
    J2DPicture* getButtonBasePicture(int i_which);   // 0 = A, 1 = B, 2 = X, 3 = Y
    J2DPane* getButtonPane(int i_which);             // 0 = A, 1 = B, 2 = X, 3 = Y
    J2DPane* getCounterPane(int i_which);            // 0 = rupees, 1 = keys
    J2DPane* getButtonCrossPane();
    J2DPane* getMidnaButtonPaneRaw();
    J2DPicture* getLightDropPicture();
    J2DPane* getLightDropPane();
    f32 getLightDropAlpha();
    int getHeartPictures(int i_no, J2DPicture** o_pics);
    dKantera_icon_c* getKanteraMeter(int i_no);

    // Live labels.
    const char* getActionTextA();
    const char* getActionTextB();
    const char* getActionTextXY(int i_no);
    const char* getDpadLabel(int i_no);

    // State the dashboard reflects.
    bool isItemUsable(int i_xy);  // X = 0, Y = 1
    bool isOxygenActive();
    bool isButtonClusterVisible();

    // Vessel of light: the companion composites it, so the live pane state is
    // saved, rewritten to the canonical corner layout and restored.
    static const int VESSEL_ALPHA_SAVE_COUNT = 35;
    J2DPane* getVesselTearPane(int i_idx);
    void drawVesselPikariForCompanion(const f32* i_tearX, const f32* i_tearY, f32 i_sizeScale);
    void refreshVesselForCompanion();
    void pushVesselStateForCompanion(f32* o_alpha, f32* o_x, f32* o_y, f32* o_scale);
    void popVesselStateForCompanion(
        const f32* i_alpha, const f32* i_x, const f32* i_y, const f32* i_scale);

    void drawMidnaPikariAt(f32 i_posX, f32 i_posY);
    void forceCompanionAlpha();
    // Hides on the main screen whatever the companion has taken over.
    void dualScreenSyncPaneVisibility(
        bool i_dualScreenHud, bool i_mainHudRestored, bool i_lowLifeHearts);
};

// --- The menu/meter containers the companion reads through ------------------
struct dMw_cAccess : dMw_c {
    dMenu_Fmap_c* getMenuFmap() { return mpMenuFmap; }
    dMenu_Dmap_c* getMenuDmap() { return mpMenuDmap; }
    dMenu_Collect_c* getMenuCollect() { return mpMenuCollect; }
};

struct dMeter2Access : dMeter2_c {
    // The contextual control panel drawables, rendered on the companion
    // instead of the main view.
    dMeterSub_c* getSubContentsDlst() { return mpSubContents; }
    dMeterString_c* getSubSubContentsDlst() { return mpSubSubContents; }
};

struct dMeterMapAccess : dMeterMap_c {
    dMap_c* getDMap() { return mMap; }
};

struct dMenu_FmapAccess : dMenu_Fmap_c {
    dMenu_Fmap2DTop_c* getDraw2DTop() { return mpDraw2DTop; }
    // True while the map is armed in portal-warp mode (the Z toggle).
    bool isWarpMapMode() const { return mIsWarpMap; }
};

// --- Map, dungeon-map and collection screens --------------------------------
struct dMenu_Collect2DAccess : dMenu_Collect2D_c {
    // Category icon panes for the companion: 0 = fish journal, 1 = hidden
    // skills scroll, 2 = letters, 3-7 = scent icons (medicine, children,
    // fish, youth's/Ilia, poe).
    J2DPane* getIconPane(int i_which) {
        if (mpScreen == NULL) {
            return NULL;
        }
        static const u64 tags[8] = {
            MULTI_CHAR('fish_3_n'), MULTI_CHAR('maki_5_n'), MULTI_CHAR('lett_4_n'),
            MULTI_CHAR('wolf_med'), MULTI_CHAR('wolf_chi'), MULTI_CHAR('wolf_fis'),
            MULTI_CHAR('wolf_iri'), MULTI_CHAR('wolf_pou'),
        };
        if (i_which < 0 || i_which >= 8) {
            return NULL;
        }
        return mpScreen->search(tags[i_which]);
    }
};

struct dMenu_CollectAccess : dMenu_Collect_c {
    dMenu_Collect2D_c* getCollect2D() { return mpCollect2D; }
};

struct dMenu_DmapAccess : dMenu_Dmap_c {
    dMenu_DmapBg_c* getDrawBg() { return mpDrawBg; }
};

struct dMenu_DmapBgAccess : dMenu_DmapBg_c {
    const char* getButtonLabel(int i_which);  // 0 = A, 1 = B; empty if hidden
};

struct dMenu_Fmap2DTopAccess : dMenu_Fmap2DTop_c {
    const char* getButtonLabel(int i_which);  // 0 = A, 1 = B; NULL if hidden
};

inline dMeter2DrawAccess* cast(dMeter2Draw_c* p) {
    return static_cast<dMeter2DrawAccess*>(p);
}
inline dMw_cAccess* cast(dMw_c* p) {
    return static_cast<dMw_cAccess*>(p);
}
inline dMeter2Access* cast(dMeter2_c* p) {
    return static_cast<dMeter2Access*>(p);
}
inline dMeterMapAccess* cast(dMeterMap_c* p) {
    return static_cast<dMeterMapAccess*>(p);
}
inline dMenu_FmapAccess* cast(dMenu_Fmap_c* p) {
    return static_cast<dMenu_FmapAccess*>(p);
}
inline dMenu_Collect2DAccess* cast(dMenu_Collect2D_c* p) {
    return static_cast<dMenu_Collect2DAccess*>(p);
}
inline dMenu_CollectAccess* cast(dMenu_Collect_c* p) {
    return static_cast<dMenu_CollectAccess*>(p);
}
inline dMenu_DmapAccess* cast(dMenu_Dmap_c* p) {
    return static_cast<dMenu_DmapAccess*>(p);
}
inline dMenu_DmapBgAccess* cast(dMenu_DmapBg_c* p) {
    return static_cast<dMenu_DmapBgAccess*>(p);
}
inline dMenu_Fmap2DTopAccess* cast(dMenu_Fmap2DTop_c* p) {
    return static_cast<dMenu_Fmap2DTopAccess*>(p);
}

}  // namespace dusk::ga
