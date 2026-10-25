#!/usr/bin/env bash
# aleks2 (1.0.2): by-name hook targets that differ only in [abi:...] tags resolve, in the game
# (src/dusk/mods/abi_tags.hpp, used by manifest::resolve) and in the gates (check_symbol_manifest.lookup), alike.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-abi-tags-test"
mkdir -p "$out"
flags=(-std=c++20 -Wall -Wextra -O1 -g -I"$root/src")
if [ "${SANITIZE:-1}" = "1" ] && echo 'int main(){}' | g++ -x c++ -fsanitize=address,undefined - -o /dev/null 2>/dev/null; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
g++ "${flags[@]}" "$here/abi_tags_test.cpp" -o "$out/abi_tags_test"
"$out/abi_tags_test"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
"$py" - "$here" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import check_symbol_manifest as c
m = {
    "dusk::ui::midna_icon_source[abi:cxx11]": (1, 0x100),
    "name_of[abi:cxx11]": (1, 0x300), "name_of[abi:other]": (1, 0x310),
    "alias[abi:cxx11]": (1, 0x400), "alias[abi:cxx11][abi:x]": (1, 0x400),
    "untagged_only": (1, 0x500),
}
cases = [
    ("dusk::ui::midna_icon_source", (1, 0x100)),
    ("dusk::ui::midna_icon_source[abi:cxx11]", (1, 0x100)),
    ("name_of", "ambiguous"),
    ("alias", (1, 0x400)),
    ("untagged_only[abi:cxx11]", (1, 0x500)),
    ("missing", None),
]
fail = 0
for name, want in cases:
    got = c.lookup(m, name)
    if want == "ambiguous":
        ok = got is not None and got[0] & c.FLAG_DUP_NAME
    else:
        ok = got == want
    print(("ok  " if ok else "FAIL") + f" lookup {name!r} -> {got}")
    fail += not ok
print("abi-tags (gates):", "FAIL" if fail else "ok")
sys.exit(1 if fail else 0)
PY
