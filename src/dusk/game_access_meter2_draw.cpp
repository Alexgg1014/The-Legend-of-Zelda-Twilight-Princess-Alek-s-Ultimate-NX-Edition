// Definitions for the dMeter2Draw_c access view (see dusk/game_access.h).
//
// This code used to live in src/d/d_meter2_draw.cpp, where it was ours inside
// a file upstream owns. Moving it here changes nothing it does: the same
// methods run on the same object, reached through a data-free subclass.

#include "dusk/game_access.h"

#include "d/dolzel.h" // IWYU pragma: keep
#include "d/d_meter2_draw.h"
#include "JSystem/J2DGraph/J2DAnmLoader.h"
#include "JSystem/J2DGraph/J2DGrafContext.h"
#include "JSystem/J2DGraph/J2DScreen.h"
#include "JSystem/J2DGraph/J2DTextBox.h"
#include "JSystem/JKernel/JKRExpHeap.h"
#include "d/actor/d_a_alink.h"
#include "d/d_item.h"
#include "d/d_item_data.h"
#include "d/d_kantera_icon_meter.h"
#include "d/d_meter_HIO.h"
#include "d/d_meter2_info.h"
#include "d/d_msg_class.h"
#include "d/d_msg_object.h"
#include "d/d_pane_class.h"
#include <cstring>
#include "dusk/game_clock.h"
#include "dusk/interp/frame_interpolation.h"
#include "dusk/interp/user_interface.h"
#include "dusk/settings.h"
#include "dusk/dualscreen.h"
#include "dusk/ui/icon_provider.hpp"
#include "dusk/version.hpp"
#include "d/d_meter2.h"

// Shared with src/d/d_meter2_draw.cpp, which owns both.
extern bool g_meter2ItemUsable[4];

// First textured picture in a pane subtree. Prefers color textures over
// intensity ones (formats 0-3): button subtrees start with greyscale drop
// shadows that would otherwise win.
J2DPicture* dusk_meter2FindPictureWithTexture(J2DPane* i_pane, bool i_colorOnly) {
    if (i_pane == NULL) {
        return NULL;
    }
    if (i_pane->getKind() == MULTI_CHAR('PIC1') || i_pane->getKind() == MULTI_CHAR('PIC2')) {
        J2DPicture* pic = (J2DPicture*)i_pane;
        if (pic->getTexture(0) != NULL &&
            (!i_colorOnly || pic->getTexture(0)->getFormat() > 3))
        {
            return pic;
        }
    }
    for (J2DPane* child = i_pane->getFirstChildPane(); child != NULL;
         child = child->getNextChildPane())
    {
        J2DPicture* pic = dusk_meter2FindPictureWithTexture(child, i_colorOnly);
        if (pic != NULL) {
            return pic;
        }
    }
    return NULL;
}

using dusk::ga::dMeter2DrawAccess;

namespace {
// aleks2: a pane is only a J2DPicture when its kind says so. Several HUD
// nodes the companion reads (the heart bases h_ba_nXX, for one) are plain
// container panes whose picture is a CHILD; casting the container and calling
// draw() through its vtable ran off the end of J2DPane's table into
// J2DAnmTransformFull::getTransform on the 2.0 layout (H1 crash, Cinematic
// hearts). Resolve to the pane itself if it is a picture, else the first
// textured picture below it, else NULL.
static J2DPicture* asPicture(J2DPane* i_pane) {
    if (i_pane == NULL) {
        return NULL;
    }
    if (i_pane->getKind() == MULTI_CHAR('PIC1') || i_pane->getKind() == MULTI_CHAR('PIC2')) {
        return (J2DPicture*)i_pane;
    }
    J2DPicture* pic = dusk_meter2FindPictureWithTexture(i_pane, true);
    if (pic == NULL) {
        pic = dusk_meter2FindPictureWithTexture(i_pane, false);
    }
    return pic;
}

static J2DPicture* findBestPicture(CPaneMgr* i_mgr) {
    if (i_mgr == NULL || i_mgr->getPanePtr() == NULL) {
        return NULL;
    }
    J2DPicture* pic = dusk_meter2FindPictureWithTexture(i_mgr->getPanePtr(), true);
    if (pic == NULL) {
        pic = dusk_meter2FindPictureWithTexture(i_mgr->getPanePtr(), false);
    }
    return pic;
}

}  // namespace

const ResTIMG* dMeter2DrawAccess::getActionLabelTimg(int i_which) {
    CPaneMgr* mgr = i_which == 0 ? mpBTextA : mpBTextB;
    if (mgr == NULL || mgr->getPanePtr() == NULL || !mgr->getPanePtr()->isVisible()) {
        return NULL;
    }
    J2DPicture* pic = asPicture(mgr->getPanePtr());
    if (pic == NULL || pic->getTexture(0) == NULL) {
        return NULL;
    }
    return pic->getTexture(0)->getTexInfo();
}
// Whole button pane subtrees (0=A 1=B 2=X 3=Y), for layer-composited
// rendering on the companion.
J2DPane* dMeter2DrawAccess::getButtonPane(int i_which) {
    CPaneMgr* mgr = NULL;
    switch (i_which) {
    case 0:
        mgr = mpButtonA;
        break;
    case 1:
        mgr = mpButtonB;
        break;
    case 2:
        mgr = mpButtonXY[0];
        break;
    case 3:
        mgr = mpButtonXY[1];
        break;
    case 4:
        mgr = mpButtonXY[2];
        break;
    }
    return mgr != NULL ? mgr->getPanePtr() : NULL;
}
// Face button base pictures for the companion cluster (0=A 1=B 2=X 3=Y).
// Returned as pictures so the caller can reuse their tint colors: the button
// graphics are intensity textures colored by the pane's black/white TEV.
J2DPicture* dMeter2DrawAccess::getButtonBasePicture(int i_which) {
    CPaneMgr* mgr = NULL;
    switch (i_which) {
    case 0:
        mgr = mpButtonA;
        break;
    case 1:
        mgr = mpButtonB;
        break;
    case 2:
        mgr = mpButtonXY[0];
        break;
    case 3:
        mgr = mpButtonXY[1];
        break;
    case 4:
        mgr = mpButtonXY[2];
        break;
    }
    return findBestPicture(mgr);
}
// Current A action word ("Speak", "Blow", ...); the mpAText panes are real
// text boxes (unlike b_text_a, which is the "A" glyph picture). NULL/empty
// when no contextual action.
const char* dMeter2DrawAccess::getActionTextA() {
    if (mpAText[0] == NULL || mpAText[0]->getPanePtr() == NULL ||
        !mpAText[0]->getPanePtr()->isVisible())
    {
        return NULL;
    }
    return (const char*)((J2DTextBox*)mpAText[0]->getPanePtr())->getStringPtr();
}
// B-button action word ("Attack", "Dig", ...); NULL when the game hides it.
const char* dMeter2DrawAccess::getActionTextB() {
    if (mpTextB == NULL || mpTextB->getPanePtr() == NULL ||
        !mpTextB->getPanePtr()->isVisible() || mpBText[0] == NULL ||
        mpBText[0]->getPanePtr() == NULL)
    {
        return NULL;
    }
    return (const char*)((J2DTextBox*)mpBText[0]->getPanePtr())->getStringPtr();
}
// X/Y-button action words ("Sense", "Dig", ... — wolf form); NULL when the
// game hides them. i_no: 0 = X, 1 = Y.
const char* dMeter2DrawAccess::getActionTextXY(int i_no) {
    if (i_no < 0 || i_no > 1 || mpTextXY[i_no] == NULL ||
        mpTextXY[i_no]->getPanePtr() == NULL || mpXYText[0][i_no] == NULL ||
        mpXYText[0][i_no]->getPanePtr() == NULL)
    {
        return NULL;
    }
#if TARGET_PC
    // ONLY Functional hides these panes by hand (its cluster stays visible,
    // so the words would otherwise draw on the main screen). There the flag
    // is ours, not the game's, so it cannot gate — the caller checks wolf
    // form instead. Every other mode keeps the game's own flag authoritative.
    if (!dusk::dualscreen::mainHudRestored() && !mpTextXY[i_no]->getPanePtr()->isVisible()) {
        return NULL;
    }
#else
    if (!mpTextXY[i_no]->getPanePtr()->isVisible()) {
        return NULL;
    }
#endif
    return (const char*)((J2DTextBox*)mpXYText[0][i_no]->getPanePtr())->getStringPtr();
}
// Midna (Z) button pane subtree regardless of the prompt's active state
// (the companion dims it instead of hiding).
J2DPane* dMeter2DrawAccess::getMidnaButtonPaneRaw() {
    if (mpButtonMidona == NULL || mpButtonMidona->getPanePtr() == NULL) {
        return NULL;
    }
    // Functional hides the MAIN-screen copy of this pane (the partition owns
    // its visibility there, and the companion Z corner is the caller) — that
    // deliberate hide must not read as "prompt gone".
    if (!dusk::dualscreen::mainHudRestored() && !mpButtonMidona->getPanePtr()->isVisible()) {
        return NULL;
    }
    return mpButtonMidona->getPanePtr();
}
// A tear-of-light picture from the vessel layout.
J2DPicture* dMeter2DrawAccess::getLightDropPicture() {
    return findBestPicture(mpLightDropParent);
}
// D-pad label strings from the cross HUD textboxes (localized by the game).
const char* dMeter2DrawAccess::getDpadLabel(int i_no) {
    if (mpScreen == NULL) {
        return NULL;
    }
    J2DPane* pane = mpScreen->search(i_no == 0 ? MULTI_CHAR('cont_ju0') : MULTI_CHAR('cont_ju5'));
    if (pane == NULL) {
        return NULL;
    }
    return (const char*)((J2DTextBox*)pane)->getStringPtr();
}
// Midna prompt pulse, drawn at companion coordinates (frame state shared
// with the main HUD, which skips its own draw in dual-screen mode).
void dMeter2DrawAccess::drawMidnaPikariAt(f32 i_posX, f32 i_posY) {
    if (field_0x738 <= 0.0f) {
        return;
    }
    drawPikari(i_posX, i_posY, &field_0x738, g_drawHIO.mMidnaIconPikariScale,
               g_drawHIO.mMidnaIconPikariFrontOuter, g_drawHIO.mMidnaIconPikariFrontInner,
               g_drawHIO.mMidnaIconPikariBackOuter, g_drawHIO.mMidnaIconPikariBackInner,
               g_drawHIO.mMidnaIconPikariAnimSpeed, 3);
}
// D-pad cross HUD pane subtree (up = item wheel, right = map).
J2DPane* dMeter2DrawAccess::getButtonCrossPane() {
    return mpButtonCrossParent != NULL ? mpButtonCrossParent->getPanePtr() : NULL;
}
// Vessel of Light pane subtree for full companion compositing.
J2DPane* dMeter2DrawAccess::getLightDropPane() {
    return mpLightDropParent != NULL ? mpLightDropParent->getPanePtr() : NULL;
}
// Whether the X (0) / Y (1) item can currently be used. Reads the per-frame
// snapshot taken in setButtonIconAlpha — the live dMeter2Info bits are reset
// before the companion's draw pass runs, so they can't be read directly.
bool dMeter2DrawAccess::isItemUsable(int i_xy) {
    if (i_xy < 0 || i_xy > 3) {
        return false;
    }
    return g_meter2ItemUsable[i_xy];
}
bool dMeter2DrawAccess::isOxygenActive() {
    // The game fades the bar in and out through mMeterAlphaRate; riding that
    // keeps the companion in step with the main screen's own timing instead
    // of popping the moment Link enters water.
    return mMeterAlphaRate[2] > 0.0f && dComIfGp_getMaxOxygen() > 0;
}
J2DPane* dMeter2DrawAccess::getVesselTearPane(int i_idx) {
    if (i_idx < 0 || i_idx >= 16 || mpSIParts[i_idx][1] == NULL) {
        return NULL;
    }
    return mpSIParts[i_idx][1]->getPanePtr();
}
// Companion twin of the main-screen vessel tear glow (skipped there in
// dual-screen mode): same animation state machine, but each tear's pikari
// draws at the caller-mapped companion position, sized by i_sizeScale.
// Positions below -9000 mark tears the caller could not map.
void dMeter2DrawAccess::drawVesselPikariForCompanion(const f32* i_tearX, const f32* i_tearY,
                                                 f32 i_sizeScale) {
    if (mpLightDropParent == NULL || mpLightDropParent->getAlphaRate() == 0.0f) {
        return;
    }

    f32 dropScale = g_drawHIO.mLightDrop.mPikariScaleNormal;
    f32 dropSpeed = g_drawHIO.mLightDrop.mDropPikariAnimSpeed;

    if (field_0x756 >= 0) {
        dropSpeed = g_drawHIO.mLightDrop.mDropPikariAnimSpeed_Completed;
        int completeEnd = g_drawHIO.mLightDrop.mPikariInterval * 15;
        dropScale = g_drawHIO.mLightDrop.mPikariScaleComplete;
        // aleks2: same state machine as the main-screen path above, in 2.0's
        // game-clock form (field_0x756 is an f32 advanced by original_frames()).
        {
            if (field_0x756 <= completeEnd) {
                int phase = static_cast<int>(field_0x756) % g_drawHIO.mLightDrop.mPikariInterval;
                int tearIdx = static_cast<int>(field_0x756) / g_drawHIO.mLightDrop.mPikariInterval;

                if (phase == 0 && field_0x62c[tearIdx] == 0.0f) {
                    field_0x62c[tearIdx] = 18.0f;
                }
                field_0x756 += dusk::game_clock::original_frames();
            } else {
                int holdStart = completeEnd + 1;

                if (field_0x756 < holdStart + 1) {
                    if (field_0x62c[15] == 0.0f) {
                        field_0x756 += dusk::game_clock::original_frames();
                    }
                } else if (field_0x756 >= g_drawHIO.mLightDrop.field_0x54 + holdStart) {
                    for (int i = 0; i < 16; i++) {
                        field_0x62c[i] = 18.0f - dropSpeed;
                        field_0x66c[i] = 18.0f - g_drawHIO.mLightDrop.mPikariLoopAnimSpeed;
                    }
                    field_0x756 = -1;
                } else {
                    field_0x756 += dusk::game_clock::original_frames();
                }
            }
        }
    }

    for (int i = 0; i < 16; i++) {
        if (i_tearX[i] < -9000.0f) {
            continue;
        }
        if (field_0x66c[i] > 0.0f) {
            drawPikari(i_tearX[i], i_tearY[i], &g_drawHIO.mLightDrop.mPikariLoopBackStopFrame,
                       g_drawHIO.mLightDrop.mPikariLoopBackScale * i_sizeScale,
                       g_drawHIO.mLightDrop.mPikariLoopFrontOuter[1],
                       g_drawHIO.mLightDrop.mPikariLoopFrontInner[1],
                       g_drawHIO.mLightDrop.mPikariLoopBackOuter[1],
                       g_drawHIO.mLightDrop.mPikariLoopBackInner[1], 0.0f, 3);
            drawPikari(i_tearX[i], i_tearY[i], &field_0x66c[i],
                       g_drawHIO.mLightDrop.mPikariLoopScale * i_sizeScale,
                       g_drawHIO.mLightDrop.mPikariLoopFrontOuter[0],
                       g_drawHIO.mLightDrop.mPikariLoopFrontInner[0],
                       g_drawHIO.mLightDrop.mPikariLoopBackOuter[0],
                       g_drawHIO.mLightDrop.mPikariLoopBackInner[0],
                       g_drawHIO.mLightDrop.mPikariLoopAnimSpeed, 3);
        }

        if (g_drawHIO.mLightDrop.mAnimDebug &&
            dComIfGp_getNeedLightDropNum() !=
                dComIfGs_getLightDropNum(dComIfGp_getStartStageDarkArea()))
        {
            field_0x66c[i] = 0.0f;
        }

        if (field_0x62c[i] > 0.0f) {
            drawPikari(i_tearX[i], i_tearY[i], &field_0x62c[i], dropScale * i_sizeScale,
                       g_drawHIO.mLightDrop.mDropPikariFrontOuter,
                       g_drawHIO.mLightDrop.mDropPikariFrontInner,
                       g_drawHIO.mLightDrop.mDropPikariBackOuter,
                       g_drawHIO.mLightDrop.mDropPikariBackInner, dropSpeed, field_0x75f);
        }
    }
}
// Re-apply the vessel's per-drop fill tints/alphas from the live tear count
// at full opacity — the game's own fade leaves it invisible between pickups,
// but the companion shows it for the whole quest.
void dMeter2DrawAccess::refreshVesselForCompanion() {
    if (mpLightDropParent == NULL) {
        return;
    }
    mpLightDropParent->setAlphaRate(g_drawHIO.mParentAlpha);
    for (int i = 0; i < 2; i++) {
        if (mpSIParent[i] != NULL) {
            mpSIParent[i]->setAlphaRate(1.0f);
        }
    }
    drawLightDrop(dComIfGs_getLightDropNum(dComIfGp_getStartStageDarkArea()),
                  dComIfGp_getNeedLightDropNum(), g_drawHIO.mLightDrop.mVesselPosX,
                  g_drawHIO.mLightDrop.mVesselPosY, g_drawHIO.mLightDrop.mVesselScale,
                  g_drawHIO.mLightDrop.mVesselAlpha[0], 0);
}
void dMeter2DrawAccess::pushVesselStateForCompanion(f32* o_alpha, f32* o_x, f32* o_y,
    f32* o_scale) {
    CPaneMgr* panes[VESSEL_ALPHA_SAVE_COUNT];
    panes[0] = mpLightDropParent;
    for (int i = 0; i < 2; i++) {
        panes[1 + i] = mpSIParent[i];
    }
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 2; j++) {
            panes[3 + i * 2 + j] = mpSIParts[i][j + 1];
        }
    }
    for (int k = 0; k < VESSEL_ALPHA_SAVE_COUNT; k++) {
        if (panes[k] != NULL) {
            o_alpha[k] = panes[k]->getAlphaRate();
            o_x[k] = panes[k]->getPosX();
            o_y[k] = panes[k]->getPosY();
        } else {
            o_alpha[k] = 0.0f;
            o_x[k] = 0.0f;
            o_y[k] = 0.0f;
        }
    }
    o_scale[0] = panes[0] != NULL ? panes[0]->getScaleX() : 1.0f;
    o_scale[1] = panes[0] != NULL ? panes[0]->getScaleY() : 1.0f;
    // Canonical corner layout (positions, scale, textures, alphas).
    refreshVesselForCompanion();
}
void dMeter2DrawAccess::popVesselStateForCompanion(const f32* i_alpha, const f32* i_x,
    const f32* i_y, const f32* i_scale) {
    CPaneMgr* panes[VESSEL_ALPHA_SAVE_COUNT];
    panes[0] = mpLightDropParent;
    for (int i = 0; i < 2; i++) {
        panes[1 + i] = mpSIParent[i];
    }
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 2; j++) {
            panes[3 + i * 2 + j] = mpSIParts[i][j + 1];
        }
    }
    for (int k = 0; k < VESSEL_ALPHA_SAVE_COUNT; k++) {
        if (panes[k] != NULL) {
            panes[k]->setAlphaRate(i_alpha[k]);
            panes[k]->getPanePtr()->move(i_x[k], i_y[k]);
        }
    }
    if (panes[0] != NULL) {
        panes[0]->scale(i_scale[0], i_scale[1]);
    }
}
// The live kantera (lantern oil) meters, repositionable via setPos.
dKantera_icon_c* dMeter2DrawAccess::getKanteraMeter(int i_no) {
    if (i_no < 0 || i_no >= 2) {
        return NULL;
    }
    return mpKanteraMeter[i_no];
}
// Dual-screen: the status panes are hidden on the main screen and composited
// on the companion, which should never inherit the main HUD's idle fade —
// pin them fully opaque every frame (runs after the game's own fade logic).
void dMeter2DrawAccess::forceCompanionAlpha() {
    const f32 full = g_drawHIO.mParentAlpha;
    const f32 btn = g_drawHIO.mParentAlpha * g_drawHIO.mMainHUDButtonsAlpha;
    // Pin only what the companion actually composites. In Cinematic that is
    // everything below, and pinning is invisible because those panes are
    // hidden on the main screen. Functional moves hearts, the A/B/Z cluster
    // and the d-pad back to the main screen, where the game's own fades have
    // to play out — pinning them there would freeze the HUD fully opaque
    // through cutscenes. The panes that stay on the companion (rupee/key
    // readouts, X/Y) are hidden on main in both modes, so they are always
    // safe to pin.
    const bool restored = dusk::dualscreen::mainHudRestored();
    if (!restored && mpLifeParent != NULL && mpLifeParent->getAlphaRate() != full) {
        mpLifeParent->setAlphaRate(full);
        setAlphaLifeChange(true);
    }
    if (mpRupeeParent[0] != NULL) {
        mpRupeeParent[0]->setAlphaRate(full);
    }
    if (mpKeyParent != NULL) {
        mpKeyParent->setAlphaRate(full);
    }
    if (!restored && mpButtonParent != NULL && mpButtonParent->getAlphaRate() != btn) {
        // Pinning the parent propagates init alphas to ALL children — but the
        // Midna prompt is alpha-gated by game state; preserve its own alpha.
        u8 midnaAlpha = 0;
        const bool hasMidna = mpButtonMidona != NULL && mpButtonMidona->getPanePtr() != NULL;
        if (hasMidna) {
            midnaAlpha = mpButtonMidona->getPanePtr()->getAlpha();
        }
        mpButtonParent->setAlphaRate(btn);
        if (hasMidna) {
            mpButtonMidona->setAlpha(midnaAlpha);
        }
    }
    if (!restored && mpButtonCrossParent != NULL) {
        mpButtonCrossParent->setAlphaRate(btn);
    }
    // X and Y stay on the companion in both modes.
    if (mpButtonXY[0] != NULL) {
        mpButtonXY[0]->setAlphaRate(btn);
    }
    if (mpButtonXY[1] != NULL) {
        mpButtonXY[1]->setAlphaRate(btn);
    }
    // Z rides with the cluster, so Functional leaves it to the main screen.
    if (!restored && mpButtonXY[2] != NULL) {
        mpButtonXY[2]->setAlphaRate(btn);
    }
}
bool dMeter2DrawAccess::isButtonClusterVisible() {
    // The companion cluster is always shown (user preference): cutscenes,
    // grass blowing, dialogue, the item wheel and pause screens all hide or
    // fade the main HUD buttons, but the second screen keeps them.
    return mpButtonParent != NULL;
}
// Counter pane subtrees (icon + live digits): 0 = rupees, 1 = small keys.
J2DPane* dMeter2DrawAccess::getCounterPane(int i_which) {
    CPaneMgr* mgr = i_which == 0 ? mpRupeeParent[0] : mpKeyParent;
    return mgr != NULL ? mgr->getPanePtr() : NULL;
}
// Collects the pictures composing heart i_no as currently displayed (empty
// base + full fill, or base + quarter texture for the partial heart), letting
// the companion dashboard draw the original assets at any size. Returns the
// number of pictures written to o_pics (up to 2); 0 when the heart is hidden.
int dMeter2DrawAccess::getHeartPictures(int i_no, J2DPicture** o_pics) {
    if (i_no < 0 || i_no >= 20 || mpLifeParts[i_no] == NULL ||
        mpLifeParts[i_no]->getPanePtr() == NULL ||
        !mpLifeParts[i_no]->getPanePtr()->isVisible())
    {
        return 0;
    }
    int count = 0;
    if (mpHeartBase[i_no] != NULL) {
        // h_ba_nXX is a container pane; the base picture is its child.
        if (J2DPicture* pic = asPicture(mpHeartBase[i_no]->getPanePtr())) {
            o_pics[count++] = pic;
        }
    }
    if (mpLifeTexture[i_no][1] != NULL && mpLifeTexture[i_no][1]->getPanePtr() != NULL &&
        mpLifeTexture[i_no][1]->getPanePtr()->isVisible())
    {
        // Full heart.
        if (J2DPicture* pic = asPicture(mpLifeTexture[i_no][1]->getPanePtr())) {
            o_pics[count++] = pic;
        }
    } else if (mpBigHeart != NULL && mpBigHeart->getPanePtr() != NULL &&
               mpBigHeart->getPanePtr()->isVisible() && mpScreen != NULL)
    {
        // The heart drawLife parks the shared quarter pane on (also used for
        // the last full heart, showing bigh_00): pick whichever quarter
        // texture the game left visible.
        const s16 life = dComIfGs_getLife();
        s16 partialHeart = life / 4;
        if (life % 4 == 0) {
            partialHeart--;
        }
        if (i_no == partialHeart) {
            static u64 const tag_bigh[] = {MULTI_CHAR('bigh_00'), MULTI_CHAR('bigh_01'),
                MULTI_CHAR('bigh_02'), MULTI_CHAR('bigh_03')};
            for (u64 tag : tag_bigh) {
                J2DPane* quarterPane = mpScreen->search(tag);
                if (quarterPane != NULL && quarterPane->isVisible()) {
                    if (J2DPicture* pic = asPicture(quarterPane)) {
                        o_pics[count++] = pic;
                    }
                    break;
                }
            }
        }
    }
    return count;
}
f32 dMeter2DrawAccess::getLightDropAlpha() {
    return mpLightDropParent != NULL ? mpLightDropParent->getAlphaRate() : 0.0f;
}
void dMeter2DrawAccess::dualScreenSyncPaneVisibility(bool i_dualScreenHud, bool i_mainHudRestored,
    bool i_lowLifeHearts)
{
    // Track the resolved state rather than a bool: switching Cinematic <->
    // Functional changes which panes are hidden, so a mode change has to
    // re-sync as well, or panes stay stuck from the previous mode.
    enum { STATE_MAIN, STATE_CINEMATIC, STATE_FUNCTIONAL };
    const int state =
        !i_dualScreenHud ? STATE_MAIN : (i_mainHudRestored ? STATE_FUNCTIONAL : STATE_CINEMATIC);
    static int sPrevState = STATE_MAIN;
    if (state == STATE_MAIN && sPrevState == STATE_MAIN) {
        return;
    }

    // On the companion whenever dual-screen is on, in either mode. The X/Y
    // item icons are separate panes from the X/Y buttons, so hide them too —
    // in Functional their parent cluster is visible and would otherwise keep
    // drawing the icons for buttons that are not there. The d-pad joins
    // them: the companion's tab strip and pages replace what it toggled, so
    // it has no job on the main screen in either mode.
    CPaneMgr* companionPanes[] = {mpMagicParent, mpRupeeKeyParent, mpButtonXY[0], mpButtonXY[1],
        mpItemXY[0], mpItemXY[1], mpButtonCrossParent};
    for (CPaneMgr* pane : companionPanes) {
        if (pane != NULL) {
            if (state == STATE_MAIN) {
                pane->show();
            } else {
                pane->hide();
            }
        }
    }

    // Cinematic-only: Functional moves these back to the main screen. The
    // hearts alone come back even in Cinematic while the low-health pop-in
    // holds — a warning belongs on the screen the eyes are on. (The
    // companion composite draws these panes with explicit rects, so showing
    // them here affects only the main screen.)
    CPaneMgr* cinematicOnlyPanes[] = {mpLifeParent, mpLightDropParent, mpButtonParent};
    for (CPaneMgr* pane : cinematicOnlyPanes) {
        if (pane != NULL) {
            const bool show =
                state != STATE_CINEMATIC || (pane == mpLifeParent && i_lowLifeHearts);
            if (show) {
                pane->show();
            } else {
                pane->hide();
            }
        }
    }

    // FUNCTIONAL-ONLY: the Z/Midna prompt pane is a child of the button
    // cluster, which Functional keeps on the main screen for A/B — but the
    // companion's bottom-left Z corner already shows the same portrait, so
    // the main-screen copy hides rather than duplicating it. The game
    // drives this prompt via pane ALPHA only (never visibility), so an
    // unconditional show() on the way out cannot resurrect a stale state,
    // and the companion's composite ignores the root's own flag
    // (collectPaneLayers) — only getMidnaButtonPaneRaw needed to learn
    // about the partition hide.
    if (mpButtonMidona != NULL) {
        if (state == STATE_FUNCTIONAL) {
            mpButtonMidona->hide();
        } else {
            mpButtonMidona->show();
        }
    }
    // Same dedup for the Z button glyph itself (zbtn_n, also a child of the
    // cluster): the companion's bottom-left Z corner is its Functional home.
    // Like the Midna prompt, the game drives it via alpha only, and no
    // Functional composite reads this pane.
    if (mpButtonXY[2] != NULL) {
        if (state == STATE_FUNCTIONAL) {
            mpButtonXY[2]->hide();
        } else {
            mpButtonXY[2]->show();
        }
    }

    // FUNCTIONAL-ONLY: the X/Y action words ("Sense"/"Dig"). Cinematic hides
    // their parent cluster, which already stops them drawing, so touching
    // them there would only destroy the visibility flag getActionTextXY
    // relies on — that is what made the words show in human form. Functional
    // keeps the cluster visible for A/B/Z, so these must be hidden by hand.
    // On the way OUT of Functional nothing is shown here: an unconditional
    // show() would resurrect stale words in human form (drawButtonXY only
    // re-asserts visibility on a status change). moveButtonXY forces that
    // redraw on the partition change instead, so the game state decides.
    if (state == STATE_FUNCTIONAL) {
        CPaneMgr* functionalOnlyPanes[] = {mpTextXY[0], mpTextXY[1]};
        for (CPaneMgr* pane : functionalOnlyPanes) {
            if (pane != NULL) {
                pane->hide();
            }
        }
    }

    sPrevState = state;
}
