#!/usr/bin/env bash
# H38: the loader keeps a working switch-arm64 copy of a mod instead of a newer package that only has code for another
# platform (src/dusk/mods/loader/package_choice.*): selection, the refusal of such an update, uninstall, and zip ==
# directory classification, on the host with ASan/UBSan where available. Needs g++ and python3; nothing else.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
py=python3; command -v "$py" >/dev/null 2>&1 || py=python
out="${1:-${TMPDIR:-/tmp}/dusk-package-preference-test}"
rm -rf "$out"
mkdir -p "$out/fixtures"
"$py" - "$out/fixtures" <<'PYEOF'
import os, sys, zipfile
base = sys.argv[1]
shapes = {
    "switch_only": ["mod.json", "lib/switch-arm64/mod.so", "assets/a.txt"],
    "linux_only": ["mod.json", "lib/linux-aarch64/mod.so"],
    "both": ["mod.json", "lib/switch-arm64/mod.so", "lib/linux-aarch64/mod.so"],
    "other_only": ["mod.json", "lib/linux-x86_64/mod.so"],
    "assets_only": ["mod.json", "assets/a.txt", "scripts/main.lua"],
    # a library that is not at lib/<platform>/mod.so is not counted
    "misplaced": ["mod.json", "mod.so", "lib/switch-arm64/extra/mod.so", "lib/switch-arm64/readme.txt"],
    "windows_dll": ["mod.json", "lib/windows-amd64/mod.dll"],
}
for name, files in shapes.items():
    for f in files:
        path = os.path.join(base, name, f)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(b"x")
    # directory entries are in the zip too, as most zip tools write them
    with zipfile.ZipFile(os.path.join(base, name + ".dusk"), "w", zipfile.ZIP_STORED) as z:
        dirs = sorted({os.path.dirname(f) + "/" for f in files if os.path.dirname(f)})
        for d in dirs:
            z.writestr(d, b"")
        for f in files:
            z.writestr(f, b"x")
PYEOF
san=()
if echo 'int main(){}' | "${CXX:-g++}" -fsanitize=address,undefined -x c++ - -o "$out/san_probe" >/dev/null 2>&1 && "$out/san_probe" >/dev/null 2>&1; then
  san=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
"${CXX:-g++}" -std=c++20 -Wall -Wextra -Werror -O1 -g "${san[@]}" -I"$root/src" \
    "$here/package_preference_test.cpp" "$root/src/dusk/mods/loader/package_choice.cpp" -o "$out/package_preference_test"
"$out/package_preference_test" "$out/fixtures"
