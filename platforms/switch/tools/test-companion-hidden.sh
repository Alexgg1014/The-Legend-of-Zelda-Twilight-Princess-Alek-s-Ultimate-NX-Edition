#!/usr/bin/env bash
# aleks2 (1.0.3): the companion's slot queries are hidden from mods' by-name lookups while the companion is off
# (companion_hidden.hpp), and only there: svc_hook->resolve checks the setting first; hooks and imports do not.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-companion-hidden-test"
mkdir -p "$out"
flags=(-std=c++20 -Wall -Wextra -O1 -g -I"$root/src")
if [ "${SANITIZE:-1}" = "1" ] && echo 'int main(){}' | g++ -x c++ -fsanitize=address,undefined - -o /dev/null 2>/dev/null; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
g++ "${flags[@]}" "$here/companion_hidden_test.cpp" -o "$out/companion_hidden_test"
"$out/companion_hidden_test"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
"$py" - "$root/src/dusk/mods/svc/hook.cpp" "$root/src/dusk/mods/loader/loader.cpp" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8").read().replace("\r\n", "\n")
loader = open(sys.argv[2], encoding="utf-8").read().replace("\r\n", "\n")
start = src.index("ModResult hook_resolve(")
body = src[start:src.index("\n}\n", start)]
guard = body.find("if (companion_hidden_blocks(context, symbol))")
manifest = body.find("manifest::resolve(")
snapshot = src[src.index("void companion_hidden_snapshot("):]
checks = [
    (0 <= guard < manifest, "hook_resolve asks companion_hidden_blocks before the manifest"),
    (guard >= 0 and "return MOD_UNAVAILABLE;" in body[guard:manifest], "a hidden name answers MOD_UNAVAILABLE"),
    (len(re.findall(r"companion_hidden::hide\(", src)) == 1, "the decision is made in one place"),
    ("std::call_once" in snapshot, "the setting is taken once"),
    (re.search(r"bool ModLoader::activate_mod\(LoadedMod& mod\) \{\s*(//[^\n]*\n\s*)*"
               r"svc::companion_hidden_snapshot\(getSettings\(\)\.game\.dualScreen\.getValue\(\)\);", loader)
     is not None, "the loader passes the companion setting when the first mod is activated (startup)"),
    ("getSettings" not in src, "hook.cpp itself reads no setting (host tests compile it alone)"),
    ("companion screen is off at startup" in snapshot, "the startup line is logged"),
]
fail = 0
for ok, what in checks:
    print(("ok   " if ok else "FAIL ") + what)
    fail += not ok
sys.exit(1 if fail else 0)
PY
