#!/usr/bin/env bash
# H37: the Switch symbol manifest and the game's export surface, on real AArch64 links.
#
#   test-symbol-manifest.sh [OUT_DIR]
#
# Links small AArch64 "games" the way the Switch build links the NRO (static PIE, so libgcc,
# libgcc_eh and libm are inside the image with their hidden symbols local, --build-id=sha1, -g1,
# --dynamic-list for the export surface, and src/dusk/mods/loader/elf_image_runtime.c), with GNU,
# SysV and both hash styles, then:
#   1. symgen (the release cmake/SymbolManifest.cmake pins, checked against its SHA-256) makes
#      the manifest, run from the source root as the build runs it, so file-local statics get
#      their "path#name" names;
#   2. check_symbol_manifest.py on the fixture requirements: a compatible mod, an import that
#      exists but is not exported (the allowlist), an exported name nothing defines, an
#      ambiguous static, a missing hook target -- each must be reported as such, --strict must
#      fail, and a manifest from another link must be refused as stale. H37-c2: imports of
#      hidden libgcc/libgcc_eh/libm functions resolve through the image runtime table read out
#      of the ELF, at the address of the image's own function; and a hook target nothing
#      reaches, one with no room for funchook's patch before the next function and one that
#      branches back into it are refused, each with its reason (a one-instruction function
#      followed by alignment padding is accepted);
#   3. image_symbols_test (host, ASan/UBSan) resolves every export of every game and mod
#      library through the image's own hash tables, including a game with 3000 exports;
#   4. if H37_MODS_DIR has the H37 packages, gen_required_symbols.py on them must reproduce the
#      committed platforms/switch/mod_required_symbols.txt line for line.
#
# Needs aarch64-linux-gnu-g++, python3 and a host C++ compiler; downloads symgen unless SYMGEN
# is set.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-symbol-manifest}"
rm -rf "$out"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
fixtures="$here/symbol_manifest_fixtures"
cxx="${CXX:-g++}"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
command -v aarch64-linux-gnu-g++ >/dev/null || { echo "aarch64-linux-gnu-g++ is required"; exit 2; }

failures=0
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }

symgen="${SYMGEN:-}"
if [ -z "$symgen" ]; then
  version="$(sed -n 's/^set(_SYMGEN_VERSION "\(.*\)")/\1/p' "$root/cmake/SymbolManifest.cmake")"
  want="$(grep -A1 '"symgen-linux-x86_64"' "$root/cmake/SymbolManifest.cmake" | sed -n 's/.*SHA256=\([0-9a-f]*\).*/\1/p')"
  symgen="$out/symgen"
  curl -sSfL -o "$symgen" "https://github.com/encounter/symgen/releases/download/v$version/symgen-linux-x86_64"
  got="$(sha256sum "$symgen" | cut -d' ' -f1)"
  [ "$got" = "$want" ] || { echo "symgen download has SHA-256 $got, cmake/SymbolManifest.cmake pins $want"; exit 1; }
  chmod +x "$symgen"
fi

# --- the games --------------------------------------------------------------------------------
# The image runtime table, as the Switch build compiles it (C, into the game image).
aarch64-linux-gnu-gcc -O2 -fPIE -c "$root/src/dusk/mods/loader/elf_image_runtime.c" -o "$out/elf_image_runtime.o"
link() {  # link OUT HASH_STYLE extra sources...
  local elf="$1" style="$2"
  shift 2
  # Relative source paths, compiled from the fixture root, like CMake's paths under the source
  # root: symgen names file-local statics by their path relative to where it runs.
  (cd "$fixtures" && aarch64-linux-gnu-g++ -std=c++20 -O2 -g1 -fPIE -static-pie "-Wl,--hash-style=$style" \
    -Wl,--build-id=sha1 "-Wl,--dynamic-list=$fixtures/dynamic_list.txt" -o "$elf" "$@" "$out/elf_image_runtime.o")
}
for style in gnu sysv both; do
  link "$out/game-$style.elf" "$style" src/d/d_menu_ring.cpp src/f/f_game.cpp src/f/f_loop.S
done
# A different link of the same sources (one more function), for the stale-manifest case.
printf 'int one_more_function(int v) { return v; }\n' > "$out/extra.cpp"
link "$out/game-other.elf" gnu src/d/d_menu_ring.cpp src/f/f_game.cpp src/f/f_loop.S "$out/extra.cpp"
# H37-c2: a table entry that is not the image's own function of that name (a stand-in named
# "sqrt"), and an image without the table at all: the check must refuse both outright.
cat > "$out/bad_runtime.c" <<'CEOF'
#include "elf_image_runtime.h"
double not_sqrt(double v) { return v * 0.5; }
const struct Aleks2ImageRuntimeEntry aleks2_image_runtime[] = {{"sqrt", (const void*)&not_sqrt, "libm"}};
const size_t aleks2_image_runtime_count = 1;
CEOF
aarch64-linux-gnu-gcc -O2 -fPIE -I"$root/src/dusk/mods/loader" -c "$out/bad_runtime.c" -o "$out/bad_runtime.o"
(cd "$fixtures" && aarch64-linux-gnu-g++ -std=c++20 -O2 -g1 -fPIE -static-pie -Wl,--build-id=sha1 \
  "-Wl,--dynamic-list=$fixtures/dynamic_list.txt" -o "$out/game-badtable.elf" src/d/d_menu_ring.cpp src/f/f_game.cpp \
  src/f/f_loop.S "$out/bad_runtime.o")
(cd "$fixtures" && aarch64-linux-gnu-g++ -std=c++20 -O2 -g1 -fPIE -static-pie -Wl,--build-id=sha1 \
  "-Wl,--dynamic-list=$fixtures/dynamic_list.txt" -o "$out/game-notable.elf" src/d/d_menu_ring.cpp src/f/f_game.cpp \
  src/f/f_loop.S)
# A game with 3000 exported functions, for long hash chains and a full bloom filter.
"$py" - "$out" <<'PYEOF'
import pathlib, sys
out = pathlib.Path(sys.argv[1])
src = ["int main() { return 0; }"]
names = []
for i in range(3000):
    src.append(f'extern "C" int big_game_function_{i}(int v) {{ return v + {i}; }}')
    names.append(f"big_game_function_{i};")
(out / "big.cpp").write_text("\n".join(src) + "\n")
(out / "big.list").write_text("{\n" + "\n".join(names) + "\n};\n")
PYEOF
(cd "$out" && aarch64-linux-gnu-g++ -O1 -fPIE -pie -Wl,--build-id=sha1 -Wl,--dynamic-list=big.list -o big.elf big.cpp)

# --- symgen and the checker -------------------------------------------------------------------
"$symgen" -C "$fixtures" manifest --binary "$out/game-gnu.elf" -o "$out/game.symdb.raw" --no-compress >/dev/null 2>&1
"$symgen" -C "$fixtures" manifest --binary "$out/game-gnu.elf" -o "$out/game.symdb" >/dev/null 2>&1
[ -s "$out/game.symdb" ] || fail "symgen wrote no compressed manifest"

set +e
"$py" "$here/check_symbol_manifest.py" --elf "$out/game-gnu.elf" --symdb "$out/game.symdb.raw" \
  --required "$fixtures/required.txt" --report "$out/report.json" > "$out/check.txt" 2>&1
lenient=$?
"$py" "$here/check_symbol_manifest.py" --elf "$out/game-gnu.elf" --symdb "$out/game.symdb.raw" \
  --required "$fixtures/required.txt" --report "$out/report-strict.json" --strict > /dev/null 2>&1
strict=$?
"$py" "$here/check_symbol_manifest.py" --elf "$out/game-other.elf" --symdb "$out/game.symdb.raw" \
  --required "$fixtures/required.txt" --report "$out/report-stale.json" > "$out/stale.txt" 2>&1
stale=$?
"$py" "$here/check_symbol_manifest.py" --elf "$out/game-gnu.elf" --symdb "$out/game.symdb" \
  --required "$fixtures/required.txt" --report "$out/report-compressed.json" > "$out/compressed.txt" 2>&1
compressed=$?
for bad in badtable notable; do
  "$symgen" -C "$fixtures" manifest --binary "$out/game-$bad.elf" -o "$out/$bad.symdb.raw" --no-compress >/dev/null 2>&1
  "$py" "$here/check_symbol_manifest.py" --elf "$out/game-$bad.elf" --symdb "$out/$bad.symdb.raw" \
    --required "$fixtures/required.txt" --report "$out/report-$bad.json" --require-image-runtime > "$out/$bad.txt" 2>&1
  eval "${bad}=\$?"
done
set -e
[ "$badtable" -eq 2 ] && grep -q "image runtime sqrt: .* is not where the image's own sqrt starts" "$out/badtable.txt" ||
  fail "a table entry that is not the image's own function was not refused: $(cat "$out/badtable.txt")"
[ "$notable" -eq 2 ] && grep -q "no aleks2_image_runtime table" "$out/notable.txt" ||
  fail "an image without the runtime table was not refused: $(cat "$out/notable.txt")"
# Without --require-image-runtime (a Switch build without the native loader links no table) the
# same image is checked on its exports alone: the hidden-runtime imports are then missing.
"$py" "$here/check_symbol_manifest.py" --elf "$out/game-notable.elf" --symdb "$out/notable.symdb.raw" \
  --required "$fixtures/required.txt" --report "$out/report-notable-lenient.json" > "$out/notable-lenient.txt" 2>&1 ||
  fail "an image without the runtime table failed the check without --require-image-runtime"
grep -q "missing import: _Unwind_Resume (libgcc)" "$out/notable-lenient.txt" ||
  fail "without the table, _Unwind_Resume was not reported missing"
cat "$out/check.txt"
[ "$lenient" -eq 0 ] || fail "without --strict an incompatible mod must not fail the build (exit $lenient)"
[ "$strict" -eq 1 ] || fail "with --strict an incompatible mod must fail the build (exit $strict)"
[ "$stale" -eq 2 ] && grep -q "stale symbol manifest" "$out/stale.txt" ||
  fail "a manifest from another link was not refused as stale: $(cat "$out/stale.txt")"
[ "$compressed" -eq 2 ] && grep -q "compressed" "$out/compressed.txt" ||
  fail "a compressed manifest was not refused with a reason"
"$py" - "$out/report.json" <<'PYEOF' || failures=$((failures + 1))
import json, sys
r = json.load(open(sys.argv[1]))
m = r["mods"]
want = {
    "dev.test.compatible": (True, [], [], [], []),
    "dev.test.unexported": (False, ["_Z15game_unexportedi (game)"], [], [], []),
    "dev.test.undefined": (False, ["name_that_nothing_defines (libc)"], [], [], []),
    "dev.test.ambiguous": (False, [], [], ["action_check"], []),
    "dev.test.missing_hook": (False, [], ["nowhere_in_the_game"], [], []),
    "dev.test.unreached": (False, [], [], [], ["ring_unreached: nothing calls it or takes its address: "
                                                "a hook there intercepts nothing"]),
    "dev.test.too_short": (False, [], [], [], ["short_target: only 4 bytes before the next function: too little "
                                                "for the 8-byte patch"]),
    "dev.test.back_jump": (False, [], [], [], ["loop_target: branches back into the patched instructions"]),
    "dev.test.image_runtime": (True, [], [], [], []),
}
bad = 0
for mod, expected in want.items():
    got = m[mod]
    actual = (got["compatible"], got["imports"]["missing"], got["hooks"]["missing"], got["hooks"]["ambiguous"],
              got["hooks"]["not_interceptable"])
    if actual != expected:
        print(f"FAIL: {mod}: got {actual}, want {expected}")
        bad += 1
# The hidden toolchain functions: found through the table, at the image's own definitions.
rt = m["dev.test.image_runtime"]["imports"]
if sorted(rt["image_runtime"]) != ["_Unwind_Resume", "__eqtf2", "__udivti3", "fmod", "fmodf", "sqrtf"]:
    print(f"FAIL: image runtime imports: {rt['image_runtime']}")
    bad += 1
table = {e["name"]: e for e in r["image_runtime"]}
if sorted(table) != sorted(["_Unwind_Resume", "__eqtf2", "__gttf2", "__netf2", "__trunctfdf2", "__udivti3",
                            "__unordtf2", "fmod", "fmodf", "sqrt", "sqrtf"]):
    print(f"FAIL: image runtime table read from the ELF: {sorted(table)}")
    bad += 1
# Reached hook targets say how.
ht = r["hook_targets"]
if ht["src/d/d_menu_ring.cpp#action_check"]["data_pointers"] < 1 or not ht["dMenu_Ring_c::_create"]["exported"]:
    print(f"FAIL: reach not recorded: {ht['src/d/d_menu_ring.cpp#action_check']}, {ht['dMenu_Ring_c::_create']}")
    bad += 1
if m["dev.test.compatible"]["hooks"]["found"] != 4:
    print("FAIL: the compatible mod's four hook names (path#static, qualified, mangled, a one-instruction "
          "function with padding) did not all resolve")
    bad += 1
if r["stale"] or r["elf_build_id"] != r["manifest_build_id"]:
    print("FAIL: identity not recorded")
    bad += 1
sys.exit(1 if bad else 0)
PYEOF

# --- the lookup the loader uses on Switch -----------------------------------------------------
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -D_GLIBCXX_ASSERTIONS -I"$root/src" "$here/image_symbols_test.cpp" \
  "$root/src/dusk/mods/loader/image_symbols.cpp" -o "$out/image_symbols_test"
libs=()
if [ -n "${H37_MODS_DIR:-}" ]; then libs=("$H37_MODS_DIR"/*.so); fi
"$out/image_symbols_test" "$out"/game-*.elf "$out/big.elf" "${libs[@]}" || failures=$((failures + 1))

# --- the committed requirements, regenerated ---------------------------------------------------
if [ -n "${H37_MODS_DIR:-}" ] && [ -f "$H37_MODS_DIR/randomizer.so" ]; then
  m="$H37_MODS_DIR"
  "$py" "$here/gen_required_symbols.py" --symgen "$symgen" --out "$out/mod_required_symbols.txt" \
    com.melonspeedruns.rollandsprint="$m/rollandsprint.so" dev.twilitrealm.luau="$m/luau_runtime.so" \
    dev.twilitrealm.cosmetics="$m/cosmetics.so" dev.twilitrealm.randomizer="$m/randomizer.so" \
    dev.aleks2.loader_lab="$m/loader_lab.so" > /dev/null
  if ! diff -u "$root/platforms/switch/mod_required_symbols.txt" "$out/mod_required_symbols.txt" > "$out/required.diff"; then
    fail "platforms/switch/mod_required_symbols.txt differs from what the packages require (see $out/required.diff)"
  else
    echo "mod_required_symbols.txt matches the packages in $m"
  fi
else
  echo "H37_MODS_DIR not set (or no randomizer.so): the committed requirements were not regenerated"
fi

if [ "$failures" -gt 0 ]; then
  echo "$failures check(s) failed"
  exit 1
fi
echo "test-symbol-manifest: all checks passed"
