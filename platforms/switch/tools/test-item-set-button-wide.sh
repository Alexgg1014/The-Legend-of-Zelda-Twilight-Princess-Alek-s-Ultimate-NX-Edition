#!/usr/bin/env bash
# aleks2 (1.1.3): checkItemSetButtonWide must read checkItemSetButton's answers as upstream's callers do -- only 2
# means "on no button". A mod's hook may answer something else (Twilit Essentials: 3 for its Z slot and for what
# Quick Access keeps equipped); upstream reads that as "set". Before 1.1.3 the port read it as none, so the per-frame
# Iron Boots check in daAlink_c::execute called setHeavyBoots(0) every frame with the boots on Twilit's Z, and right
# after a Z press Twilit's guard let it through: boots off, then procBootsEquip put them back on.
#
# Compiles the real checkItemSetButtonWide body and the real per-frame boots check, both taken from d_a_alink.cpp,
# against small stubs, and compares every answer with upstream's rule. Needs a host C++ compiler ($CXX or c++/g++).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
source_file="${1:-$root/src/d/actor/d_a_alink.cpp}"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
cxx="${CXX:-}"
if [ -z "$cxx" ]; then
    for c in c++ g++ clang++; do
        if command -v "$c" >/dev/null 2>&1; then cxx="$c"; break; fi
    done
fi
[ -n "$cxx" ] || { echo "item-set-button-wide: no host C++ compiler" >&2; exit 2; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$py" - "$source_file" "$work/extracted.inc" <<'EOF'
import sys
text = open(sys.argv[1], encoding="utf-8", errors="replace").read().replace("\r\n", "\n")

def block(start):
    open_at = text.index("{", start)
    depth, i = 0, open_at
    while True:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        if depth == 0:
            return text[start:i + 1]
        i += 1

wide = block(text.index("int daAlink_c::checkItemSetButtonWide(int i_itemNo)"))
# the per-frame check in execute(): the only `if (checkEquipHeavyBoots()) {` whose body asks for the boots' button
anchor = text.index("int itemButton = checkItemSetButtonWide(dItemNo_HVY_BOOTS_e);")
boots = block(text.rindex("if (checkEquipHeavyBoots()) {", 0, anchor))
with open(sys.argv[2], "w", encoding="utf-8") as out:
    out.write(wide + "\n\n")
    out.write("void daAlink_c::bootsFrameCheck() {\n" + boots + "\n}\n")
EOF

cat > "$work/test.cpp" <<'EOF'
#include <cstdio>
typedef unsigned char u8;
typedef unsigned int u32;

namespace dusk { namespace dualscreen {
static bool g_companion = false;
bool hudOnCompanion() { return g_companion; }
} }

enum { dItemNo_HVY_BOOTS_e = 0x45, dItemNo_NONE_e = 0xFF };
static u8 g_select[4];
static int g_hookAnswer;          // what checkItemSetButton answers (upstream's own, or a mod hook's)
static int g_unequipCalls;
static int g_directUse;

u8 dComIfGp_getSelectItem(int i) { return g_select[i]; }
bool dComIfGp_checkPlayerStatus1(int, u32) { return false; }
void dMeter2Info_onDirectUseItem(int i) { g_directUse = i; }

struct daAlink_c {
    static const int ITEM_BTN_COUNT = 4;
    bool equipped = true;
    int checkItemSetButton(int) { return g_hookAnswer; }
    bool checkGroupItem(int a, int b) { return a == b; }
    u32 checkEquipHeavyBoots() const { return equipped; }
    bool checkNotHeavyBootsStage() { return false; }
    bool checkHookshotRoofLv7Boss() { return false; }
    int setHeavyBoots(int on) { if (on == 0) { g_unequipCalls++; } return 0; }
    int checkItemSetButtonWide(int i_itemNo);
    void bootsFrameCheck();
};

#include "extracted.inc"

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}

static void reset(int answer, bool companion) {
    g_hookAnswer = answer;
    dusk::dualscreen::g_companion = companion;
    for (u8& s : g_select) { s = dItemNo_NONE_e; }
    g_unequipCalls = 0;
    g_directUse = -1;
}

int main() {
    daAlink_c link;
    const int none = daAlink_c::ITEM_BTN_COUNT;
    char what[160];

    reset(0, false); check(link.checkItemSetButtonWide(dItemNo_HVY_BOOTS_e) == 0, "X (0) stays X");
    reset(1, false); check(link.checkItemSetButtonWide(dItemNo_HVY_BOOTS_e) == 1, "Y (1) stays Y");
    reset(2, false); check(link.checkItemSetButtonWide(dItemNo_HVY_BOOTS_e) == none, "2 with the companion off is none");
    reset(2, false); g_select[2] = dItemNo_HVY_BOOTS_e;
    check(link.checkItemSetButtonWide(dItemNo_HVY_BOOTS_e) == none, "2, companion off: select item 2 is a mod's, not a slot");
    reset(2, true); g_select[2] = dItemNo_HVY_BOOTS_e;
    check(link.checkItemSetButtonWide(dItemNo_HVY_BOOTS_e) == 2, "2, companion on, item in slot I: 2");
    reset(2, true); g_select[3] = dItemNo_HVY_BOOTS_e;
    check(link.checkItemSetButtonWide(dItemNo_HVY_BOOTS_e) == 3, "2, companion on, item in slot II: 3");
    reset(3, false); check(link.checkItemSetButtonWide(dItemNo_HVY_BOOTS_e) == 3, "a hook's 3 (Twilit's Z) stays set, companion off");
    reset(3, true); check(link.checkItemSetButtonWide(dItemNo_HVY_BOOTS_e) != none, "a hook's 3 stays set, companion on");
    reset(4, false); check(link.checkItemSetButtonWide(dItemNo_HVY_BOOTS_e) != none, "a hook's 4 is not read as none");

    // The per-frame Iron Boots check: upstream unequips exactly when checkItemSetButton answers 2.
    for (int answer = 0; answer <= 4; answer++) {
        reset(answer, false);
        link.equipped = true;
        link.bootsFrameCheck();
        const bool upstreamUnequips = answer == 2;
        std::snprintf(what, sizeof what, "boots worn, checkItemSetButton -> %d: %s, as upstream", answer,
                      upstreamUnequips ? "taken off" : "kept on");
        check((g_unequipCalls != 0) == upstreamUnequips, what);
    }

    std::printf("item-set-button-wide: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
EOF

"$cxx" -std=c++17 -O0 -o "$work/test" "$work/test.cpp"
"$work/test"
