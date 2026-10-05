// Definitions for the menu access views (see dusk/game_access.h).
// Moved out of src/d/d_menu_dmap.cpp and src/d/d_menu_fmap2D.cpp.

#include "dusk/game_access.h"

#include "d/d_msg_object.h"
#include "d/d_pane_class.h"
#include "JSystem/J2DGraph/J2DScreen.h"
#include "JSystem/J2DGraph/J2DTextBox.h"

using dusk::ga::dMenu_DmapBgAccess;
using dusk::ga::dMenu_Fmap2DTopAccess;

// Live A/B prompt for the companion cluster; the dungeon map empties the
// string when hidden, so empty means no prompt.
const char* dMenu_DmapBgAccess::getButtonLabel(int i_which) {
    if (mButtonScreen == NULL) {
        return NULL;
    }
    J2DPane* pane = mButtonScreen->search(
        i_which == 0 ? MULTI_CHAR('font_at') : MULTI_CHAR('font_bt'));
    if (pane == NULL) {
        return NULL;
    }
    return (const char*)((J2DTextBox*)pane)->getStringPtr();
}

// Live A/B prompt for the companion cluster; NULL when the map screen has
// the prompt hidden (mAlphaButton* at ALPHA_MIN).
const char* dMenu_Fmap2DTopAccess::getButtonLabel(int i_which) {
    if (mpTitleScreen == NULL) {
        return NULL;
    }
    if ((i_which == 0 ? mAlphaButtonA : mAlphaButtonB) == ALPHA_MIN) {
        return NULL;
    }
    J2DPane* pane = mpTitleScreen->search(
        i_which == 0 ? MULTI_CHAR('font_at1') : MULTI_CHAR('font_bt1'));
    if (pane == NULL) {
        return NULL;
    }
    return (const char*)((J2DTextBox*)pane)->getStringPtr();
}

