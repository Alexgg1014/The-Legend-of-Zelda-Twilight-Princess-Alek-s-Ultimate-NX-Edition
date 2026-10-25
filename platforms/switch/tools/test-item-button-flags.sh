#!/usr/bin/env bash
# aleks2 (1.0.1): Link's select-item index -> button flag table (daAlink_c::itemButtonFlag, TARGET_PC) must keep
# upstream's meaning, (1 << index), for the indices upstream has (0..2), because mods are built against it: Twilit
# Essentials keeps its Z item in select item 2 and expects BTN_Z (GitHub issue #3: with BTN_S1 alone the item worked
# once). The companion's two slot buttons must stay in the table too (BTN_S1 on index 2, BTN_S2 on index 3).
# Reads the enum and the table from the sources; no compiler needed.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
"$py" - "$root/include/d/actor/d_a_alink.h" "$root/src/d/actor/d_a_alink.cpp" <<'EOF'
import re, sys
header, source = (open(p, encoding="utf-8", errors="replace").read() for p in sys.argv[1:3])
# the TARGET_PC enum: the first daAlink_ITEM_BTN after "#if TARGET_PC"
pc = header[header.index("#if TARGET_PC", header.index("BTN_S1") - 4000):]
enum = pc[pc.index("enum daAlink_ITEM_BTN"):]
enum = enum[:enum.index("};")]
values = {m.group(1): 1 << int(m.group(2)) for m in re.finditer(r"(BTN_\w+)\s*=\s*\(1\s*<<\s*(\d+)\)", enum)}
table = re.search(r"static const u16 flags\[ITEM_BTN_COUNT\]\s*=\s*\{([^}]*)\}", source).group(1)
flags = []
for entry in table.split(","):
    v = 0
    for name in entry.split("|"):
        v |= values[name.strip()]
    flags.append(v)
fail = 0
for i in range(3):
    ok = bool(flags[i] & (1 << i))
    print(f"index {i}: flag 0x{flags[i]:03x} {'has' if ok else 'LACKS'} upstream's 1<<{i} = 0x{1 << i:02x}")
    fail += not ok
for i, name in ((2, "BTN_S1"), (3, "BTN_S2")):
    ok = bool(flags[i] & values[name])
    print(f"index {i}: {'has' if ok else 'LACKS'} the companion's {name}")
    fail += not ok
print("item-button-flags:", "FAIL" if fail else "ok")
sys.exit(1 if fail else 0)
EOF
