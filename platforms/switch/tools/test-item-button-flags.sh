#!/usr/bin/env bash
# aleks2 (1.0.1, 1.0.3): Link's item buttons keep upstream's meaning for everything a mod can see, because mods are
# built against upstream's headers. Reads the enum, the tables and the functions from the sources; no compiler needed.
#  1. upstream's u8 masks are the real ones, at upstream's places: mItemTrigger 0x2F8D, mItemButton 0x2F8E,
#     mUseButtonFlags 0x2FAE (mods read them through doTrigger()/doButton() and write them to inject presses); only
#     the companion's slot II bit (BTN_S2 = 1 << 8) is kept apart, and nothing ORs it into a u8 mask.
#  2. checkItemSetButton is upstream's: X and Y only, 2 = on no button. The port's callers use checkItemSetButtonWide.
#  3. The "which button fires which select item" loops see X, Y and the companion's slots only (index 2 = BTN_S1, not
#     Z: Z must not use what a mod keeps in select item 2). itemButton()/itemTrigger() keep upstream's 1 << index for
#     the selected item 2 (BTN_Z, plus slot I): Twilit Essentials' Z item (GitHub issue #3).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
"$py" - "$root/include/d/actor/d_a_alink.h" "$root/src/d/actor/d_a_alink.cpp" "$root/src/d/actor/d_a_alink_wolf.inc" <<'EOF'
import re, sys
header, source, wolf = (open(p, encoding="utf-8", errors="replace").read().replace("\r\n", "\n") for p in sys.argv[1:4])
fail = 0
def check(ok, what):
    global fail
    print(("ok   " if ok else "FAIL ") + what)
    fail += not ok

def body(text, signature):
    start = text.index(signature)
    open_at = text.index("{", start)
    depth, i = 0, open_at
    while True:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        if depth == 0:
            return text[open_at:i + 1]
        i += 1

# the TARGET_PC enum: the daAlink_ITEM_BTN that has BTN_S1
enum = header[header.rindex("enum daAlink_ITEM_BTN", 0, header.index("BTN_S1")):]
enum = enum[:enum.index("};")]
values = {m.group(1): 1 << int(m.group(2)) for m in re.finditer(r"(BTN_\w+)\s*=\s*\(1\s*<<\s*(\d+)\)", enum)}

# 1. storage
for offset, name in (("0x02F8D", "mItemTrigger"), ("0x02F8E", "mItemButton"), ("0x02FAE", "mUseButtonFlags")):
    decls = re.findall(r"/\*\s*" + offset + r"\s*\*/\s*(\w+)\s+(\w+)\s*;", header)
    check(decls == [("u8", name)], f"{offset}: u8 {name}, on every platform (found {decls})")
check(not re.search(r"\bu16\s+m(ItemTrigger|ItemButton|UseButtonFlags)\b", header),
      "no u16 copy of the masks anywhere else in the class")
for text, where in ((source, "d_a_alink.cpp"), (wolf, "d_a_alink_wolf.inc"), (header, "d_a_alink.h")):
    bad = re.findall(r"m(?:ItemTrigger|ItemButton|UseButtonFlags)\s*\|=\s*[^;]*BTN_S2", text)
    check(not bad, f"{where}: no BTN_S2 ORed into a u8 mask {bad}")

# 2. checkItemSetButton
plain = body(source, "int daAlink_c::checkItemSetButton(int i_itemNo)")
check(re.search(r"for \(u8 i = 0; i < 2; i\+\+\)", plain) is not None and re.search(r"return 2;", plain) is not None,
      "checkItemSetButton loops X and Y and returns 2 for none")
wide = body(source, "int daAlink_c::checkItemSetButtonWide(int i_itemNo)")
check("checkItemSetButton(i_itemNo)" in wide and "return ITEM_BTN_COUNT;" in wide,
      "checkItemSetButtonWide starts from checkItemSetButton (mod hooks still decide X/Y)")
rest = source.replace(plain, "").replace(wide, "")
callers = re.findall(r"\bcheckItemSetButton\(", rest)
defs = re.findall(r"daAlink_c::checkItemSetButton\(", rest)
check(len(callers) == len(defs), f"every port caller uses checkItemSetButtonWide ({len(callers) - len(defs)} plain calls)")

# 3. loops and the selected item
table = re.search(r"static const u16 flags\[ITEM_BTN_COUNT\]\s*=\s*\{([^}]*)\}", source).group(1)
names = [entry.strip() for entry in table.split(",")]
check(names == ["BTN_X", "BTN_Y", "BTN_S1", "BTN_S2"], f"loop table is X, Y, S1, S2 (found {names})")
select = body(source, "u16 daAlink_c::selectItemButtonFlag() const")
m = re.search(r"mSelectItemId == 2 \? static_cast<u16>\(([^)]+)\)", select)
got = 0
if m:
    for name in m.group(1).split("|"):
        got |= values[name.strip()]
check(got == values["BTN_Z"] | values["BTN_S1"], f"selected item 2 answers to BTN_Z | BTN_S1 (0x{got:03x})")
for i, name in ((0, "BTN_X"), (1, "BTN_Y")):
    check(values[name] == 1 << i, f"{name} is upstream's 1 << {i}")
check(values["BTN_Z"] == 1 << 2, "BTN_Z is upstream's 1 << 2")
print("item-button-flags:", "FAIL" if fail else "ok")
sys.exit(1 if fail else 0)
EOF
