#!/usr/bin/env bash
# H38-c2: the image runtime table (src/dusk/mods/loader/elf_image_runtime.c) names the hidden toolchain functions a
# mod may import, each at the image's own function. H38-c1's gate found Twilit Essentials 2.5.0 needs libm's fmod and
# fmodf, which the NRO links but keeps local (hidden) like sqrt and sqrtf. This checks, without a link:
#   1. the source registers exactly the expected names, each entry pointing at the function of its own name, with
#      sqrt, sqrtf, fmod and fmodf as "libm" and none registered twice;
#   2. with an AArch64 compiler (devkitA64, $AARCH64_CC, or aarch64-linux-gnu-gcc), the file compiles with -Werror and
#      the object really takes the address of each of those functions (undefined symbols fmod, fmodf, sqrt, sqrtf).
# What reads the table out of a linked ELF (check_symbol_manifest.py, image_surface.py) is test-symbol-manifest.sh and
# the gate; this is the small check that a name was not forgotten.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
src="$root/src/dusk/mods/loader/elf_image_runtime.c"
py=python3; command -v "$py" >/dev/null 2>&1 || py=python
out="${1:-${TMPDIR:-/tmp}/dusk-image-runtime-table-test}"
rm -rf "$out"
mkdir -p "$out"

"$py" - "$src" <<'PYEOF'
import re, sys
text = open(sys.argv[1], encoding="utf-8").read()
entries = re.findall(r'\{"(\w+)",\s*\(const void\*\)&(\w+),\s*"(\w+)"\}', text)
names = [e[0] for e in entries]
expected = {
    "_Unwind_Resume": "libgcc_eh",
    "__eqtf2": "libgcc", "__gttf2": "libgcc", "__netf2": "libgcc", "__trunctfdf2": "libgcc",
    "__udivti3": "libgcc", "__unordtf2": "libgcc",
    "sqrt": "libm", "sqrtf": "libm", "fmod": "libm", "fmodf": "libm",
}
assert len(names) == len(set(names)), f"a name is registered twice: {names}"
for name, symbol, library in entries:
    assert name == symbol, f"{name} points at {symbol}"
got = {name: library for name, _symbol, library in entries}
assert got == expected, f"table differs: missing {sorted(set(expected) - set(got))}, extra {sorted(set(got) - set(expected))}, " \
    f"libraries {[(n, got[n], expected[n]) for n in got if n in expected and got[n] != expected[n]]}"
for name in ("fmod", "fmodf"):
    assert got[name] == "libm", name
print(f"image runtime table source: {len(entries)} entries, fmod and fmodf registered as libm at their own functions: ok")
PYEOF

cc=""
for candidate in "${AARCH64_CC:-}" aarch64-none-elf-gcc aarch64-linux-gnu-gcc "${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin/aarch64-none-elf-gcc"; do
  [ -n "$candidate" ] || continue
  if command -v "$candidate" >/dev/null 2>&1; then cc="$candidate"; break; fi
done
if [ -z "$cc" ]; then
  echo "image runtime table object: SKIPPED (no AArch64 compiler: set AARCH64_CC, DEVKITPRO or install aarch64-linux-gnu-gcc)"
  exit 0
fi
nm="${cc%gcc}nm"; command -v "$nm" >/dev/null 2>&1 || nm="${cc%gcc}gcc-nm"
"$cc" -O2 -fPIE -Wall -Wextra -Werror -c "$src" -o "$out/elf_image_runtime.o"
undefined="$("$nm" -u "$out/elf_image_runtime.o")"
for symbol in sqrt sqrtf fmod fmodf _Unwind_Resume; do
  echo "$undefined" | grep -qw "$symbol" || { echo "FAIL: the object does not take the address of $symbol"; exit 1; }
done
echo "image runtime table object ($cc): sqrt, sqrtf, fmod, fmodf and _Unwind_Resume are referenced by address: ok"
