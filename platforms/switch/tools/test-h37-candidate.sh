#!/usr/bin/env bash
# H37-c2: check_h37_candidate.py, the strict check before an H37 candidate is delivered, against
# the four official packages themselves.
#
#   test-h37-candidate.sh [OUT_DIR]
#
# The packages are the real ones (build-h37-test-mods.sh, or H37_MODS_DIR): loader_lab, Cosmetics,
# Roll and Sprint, Randomizer. The game is a stand-in linked like the NRO (static PIE, the image
# runtime table, a --dynamic-list, -g1, a GNU build id) that provides exactly what the packages ask
# for, read from the packages by gen_required_symbols.py: every import exported -- except the
# hidden libgcc/libgcc_eh/libm functions, which only the image runtime table provides, as on the
# console -- and every by-name hook target a function something calls, Randomizer's file-local
# "src/d/actor/d_a_npc_ks.cpp#action_check" included (a second action_check elsewhere makes the
# bare name ambiguous, as in the game). Then:
#
#   1. that game, its NRO and a clean commit: READY, all four packages compatible;
#   2. one import missing (Roll and Sprint's dCam_getBody): NOT READY, that package named;
#   3. one hook target nothing calls (Randomizer's uki_catch): NOT READY, "intercepts nothing";
#   4. one hook target inlined away (Randomizer's b_bq_end absent): NOT READY;
#   5. the image runtime table not linked: NOT READY, the hidden-runtime imports missing;
#   6. an NRO from another link, a manifest from another link, a tracked file changed, a package
#      missing: NOT READY each time, with its reason.
#
#   7. (H37-c6) the same Randomizer library packed as lib/linux-aarch64/mod.so: NOT READY, "must ship
#      lib/switch-arm64/mod.so" -- the READY case above needs the switch-arm64 Randomizer
#      build-h37-test-mods.sh makes with devkitA64.
#
# Needs aarch64-linux-gnu-g++, python3, git and symgen (SYMGEN).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-h37-candidate}"
rm -rf "$out"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
symgen="${SYMGEN:?set SYMGEN to the symgen executable cmake/SymbolManifest.cmake pins}"
mods="${H37_MODS_DIR:-$out/mods}"
if [ -z "${H37_MODS_DIR:-}" ]; then
  "$here/build-h37-test-mods.sh" "$mods"
fi
failures=0
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }

# --- the stand-in game, generated from what the packages require -------------------------------
game="$out/game"
mkdir -p "$game/src/d/actor" "$game/src/d/other"
"$py" - "$here" "$symgen" "$mods" "$game" "$root/src/dusk/mods/loader/elf_image_runtime.c" <<'PYEOF'
import pathlib, re, subprocess, sys
sys.path.insert(0, sys.argv[1])
import gen_required_symbols
symgen, mods, game, runtime_c = sys.argv[2], pathlib.Path(sys.argv[3]), pathlib.Path(sys.argv[4]), sys.argv[5]
official = {"dev.aleks2.loader_lab": "loader_lab", "dev.twilitrealm.cosmetics": "cosmetics",
            "com.melonspeedruns.rollandsprint": "rollandsprint", "dev.twilitrealm.randomizer": "randomizer"}
hidden = set(re.findall(r'\{"([^"]+)", \(const void\*\)', open(runtime_c).read()))
imports, hooks = set(), set()
for mod_id, stem in official.items():
    lines, denied = gen_required_symbols.requirements(mod_id, mods / f"{stem}.dusk", symgen)
    assert not denied, denied
    for _m, kind, _g, name in lines:
        (imports if kind == "import" else hooks).add(name)
exported = sorted(imports - hidden)
# symgen shows a mangled function by its name without parameters: a plain hook name that an
# exported mangled name displays as is that function, not another one.
demangled = subprocess.run(["c++filt"], input="\n".join(exported), capture_output=True, text=True).stdout.split("\n")
display = {d.split("(")[0]: m for m, d in zip(exported, demangled) if m.startswith("_Z")}
asm = ["    .text"]
calls = []
def function(label, local=False):
    if not local:
        asm.append(f"    .globl {label}")
    asm.extend([f"    .type {label}, %function", f"{label}:", "    nop", "    nop", "    nop", "    ret",
                f"    .size {label}, . - {label}"])
for name in exported:
    function(name)
path_hooks = []
for name in sorted(hooks):
    if "#" in name:
        path_hooks.append(name)
        continue
    if name.startswith("_Z"):
        label = name
    elif name in display:
        label = display[name]
    elif "::" in name:   # Class::method, as symgen displays _ZN<len>Class<len>methodEv
        label = "_ZN" + "".join(f"{len(p)}{p}" for p in name.split("::")) + "Ev"
    else:
        label = name
    if label not in exported:
        function(label, local=True)
    calls.append(label)
# Something calls every hook target, as the game does.
asm += ["    .globl game_calls_hooks", "    .type game_calls_hooks, %function", "game_calls_hooks:",
        "    stp x29, x30, [sp, #-16]!"]
asm += [f"    bl {label}  // hook:{label}" for label in calls]
asm += ["    ldp x29, x30, [sp], #16", "    ret", "    .size game_calls_hooks, . - game_calls_hooks"]
(game / "game.S").write_text("\n".join(asm) + "\n")
(game / "dynamic_list.txt").write_text("{\n" + "".join(f"  {n};\n" for n in exported) + "};\n")
assert path_hooks == ["src/d/actor/d_a_npc_ks.cpp#action_check"], path_hooks
(game / "src/d/actor/d_a_npc_ks.cpp").write_text(
    "static __attribute__((noinline)) int action_check(int* v) { asm volatile(\"\"); return v[0] * 3 + 1; }\n"
    "int npc_ks_execute(int* v) { return action_check(v); }\n")
(game / "src/d/other/d_a_other.cpp").write_text(
    "static __attribute__((noinline)) int action_check(int* v) { asm volatile(\"\"); return v[0] - 1; }\n"
    "int other_execute(int* v) { return action_check(v); }\n")
(game / "main.cpp").write_text(
    'extern "C" void game_calls_hooks();\nint npc_ks_execute(int*);\nint other_execute(int*);\n'
    "int main(int argc, char**) { int v[1] = {argc}; if (argc > 5) game_calls_hooks();\n"
    "  return npc_ks_execute(v) + other_execute(v); }\n")
# What libgcc_eh and libm need of a C library, where the stand-in does not export it already.
stubs = {"abort": "void abort(void) { for (;;) {} }",
         "_dl_find_object": "int _dl_find_object(void* pc, void* result) { (void)pc; (void)result; return -1; }"}
(game / "runtime_stubs.c").write_text('#ifdef __cplusplus\nextern "C" {\n#endif\n__thread int errno;\n' + "".join(
    body + "\n" for name, body in stubs.items() if name not in exported) + "#ifdef __cplusplus\n}\n#endif\n")
print(f"stand-in game: {len(exported)} exports, {len(hooks)} hook targets, "
      f"{len(imports & hidden)} hidden-runtime imports left to the table")
PYEOF

aarch64-linux-gnu-gcc -O2 -fPIE -c "$root/src/dusk/mods/loader/elf_image_runtime.c" -o "$game/elf_image_runtime.o"
# Linked like the NRO, but never run: no C library (the stand-in defines memcpy & co. as stubs),
# only the toolchain runtime the image runtime table names -- libgcc, libgcc_eh, libm -- and the
# three things those need of a C library (runtime_stubs.c).
link_game() {  # OUT GAME_S DYNAMIC_LIST [extra objects...]
  local elf="$1" asm="$2" list="$3"
  shift 3
  (cd "$game" && aarch64-linux-gnu-g++ -std=c++20 -O2 -g1 -fPIE -static-pie -nostdlib -nostartfiles -Wl,-e,main \
    -Wl,--build-id=sha1 "-Wl,--dynamic-list=$list" -o "$elf" main.cpp src/d/actor/d_a_npc_ks.cpp \
    src/d/other/d_a_other.cpp runtime_stubs.c "$asm" "$@" -lgcc_eh -lgcc -lm)
}
manifest() {  # ELF RAW
  "$symgen" -C "$game" manifest --binary "$1" -o "$2" --no-compress > /dev/null 2>&1
}
fake_nro() {  # ELF NRO: what check_h37_candidate reads of an NRO -- its magic and build id
  "$py" - "$here" "$1" "$2" <<'PYEOF'
import sys
sys.path.insert(0, sys.argv[1])
import check_symbol_manifest, elf_audit
elf = elf_audit.Elf(open(sys.argv[2], "rb").read(), "game")
build_id = check_symbol_manifest.elf_build_id(elf)
nro = bytearray(0x80)
nro[0x10:0x14] = b"NRO0"
nro[0x40:0x40 + len(build_id)] = build_id
open(sys.argv[3], "wb").write(bytes(nro))
PYEOF
}
link_game "$out/game.elf" game.S dynamic_list.txt "$game/elf_image_runtime.o"
manifest "$out/game.elf" "$out/game.symdb.raw"
fake_nro "$out/game.elf" "$out/game.nro"

# A clean commit to stand for the sources (the check reads commit and dirty from git).
repo="$out/repo"
mkdir -p "$repo/src"
echo "int x;" > "$repo/src/file.cpp"
git -C "$repo" init -q && git -C "$repo" add -A &&
  git -C "$repo" -c user.email=t@t -c user.name=t commit -q -m candidate

candidate() {  # NAME ELF SYMDB NRO PACKAGES [expect: ready|not]
  local name="$1" elf="$2" symdb="$3" nro="$4" packages="$5" expect="$6"
  set +e
  "$py" "$here/check_h37_candidate.py" --elf "$elf" --symdb "$symdb" --nro "$nro" --packages "$packages" \
    --symgen "$symgen" --source-root "$repo" --sdk-include "$root/sdk/include" --tag H37-test --report "$out/$name.json" > "$out/$name.txt" 2>&1
  local rc=$?
  set -e
  if [ "$expect" = ready ] && [ "$rc" -ne 0 ]; then
    fail "$name: expected READY (exit $rc)"; cat "$out/$name.txt"
  elif [ "$expect" = not ] && [ "$rc" -ne 1 ]; then
    fail "$name: expected NOT READY (exit $rc)"; cat "$out/$name.txt"
  fi
}

# 1. Everything the packages need: ready.
candidate ready "$out/game.elf" "$out/game.symdb.raw" "$out/game.nro" "$mods" ready
cat "$out/ready.txt"
"$py" - "$out/ready.json" <<'PYEOF' || failures=$((failures + 1))
import json, sys
r = json.load(open(sys.argv[1]))
bad = 0
want_runtime = {"dev.aleks2.loader_lab": 1, "dev.twilitrealm.cosmetics": 7, "com.melonspeedruns.rollandsprint": 0,
                "dev.twilitrealm.randomizer": 9}
for mod_id, n in want_runtime.items():
    p = r["packages"][mod_id]
    if not p["compatible"] or len(p["imports"]["image_runtime"]) != n:
        print(f"FAIL: {mod_id}: compatible={p['compatible']}, image runtime imports {p['imports']['image_runtime']}")
        bad += 1
# H37-c4: the game's services each package imports are judged too (host_services.py), and a package
# that needs the file service -- Randomizer, 15 services -- is satisfied on this Switch build.
for mod_id, p in r["packages"].items():
    s = p["services"]
    if any(sp["blocking"] for sp in s["problems"]):
        print(f"FAIL: {mod_id}: blocking service problems {s['problems']}")
        bad += 1
if r["packages"]["dev.twilitrealm.randomizer"]["services"]["required"] != 15:
    print(f"FAIL: randomizer services {r['packages']['dev.twilitrealm.randomizer']['services']}")
    bad += 1
rando = r["packages"]["dev.twilitrealm.randomizer"]["hooks"]
for target in ("b_bq_end", "daWindStone_c::chkEveOccur", "lure_heart", "src/d/actor/d_a_npc_ks.cpp#action_check",
               "uki_catch"):
    if target not in r["hook_targets"] or r["hook_targets"][target]["problems"]:
        print(f"FAIL: {target}: {r['hook_targets'].get(target)}")
        bad += 1
if rando["found"] != rando["required"] or r["dirty"] != 0 or not r["ready"]:
    print(f"FAIL: randomizer hooks {rando['found']}/{rando['required']}, dirty {r['dirty']}, ready {r['ready']}")
    bad += 1
sys.exit(1 if bad else 0)
PYEOF

# 2-4. One requirement unmet at a time.
sed 's/^  _Z12dCam_getBodyv;$//' "$game/dynamic_list.txt" > "$game/dynamic_list.missing.txt"
link_game "$out/game-missing-import.elf" game.S dynamic_list.missing.txt "$game/elf_image_runtime.o"
grep -v "// hook:uki_catch$" "$game/game.S" > "$game/game-unreached.S"
sed -e '/^    \.type b_bq_end, %function$/,/^    \.size b_bq_end, \. - b_bq_end$/d' -e '/\/\/ hook:b_bq_end$/d' \
  "$game/game.S" > "$game/game-absent.S"
link_game "$out/game-unreached.elf" game-unreached.S dynamic_list.txt "$game/elf_image_runtime.o"
link_game "$out/game-absent.elf" game-absent.S dynamic_list.txt "$game/elf_image_runtime.o"
link_game "$out/game-notable.elf" game.S dynamic_list.txt
for case in missing-import unreached absent notable; do
  manifest "$out/game-$case.elf" "$out/game-$case.symdb.raw"
  fake_nro "$out/game-$case.elf" "$out/game-$case.nro"
  candidate "$case" "$out/game-$case.elf" "$out/game-$case.symdb.raw" "$out/game-$case.nro" "$mods" not
done
grep -q "com.melonspeedruns.rollandsprint: not compatible with this build: _Z12dCam_getBodyv (game)" \
  "$out/missing-import.txt" || fail "the missing import was not named: $(cat "$out/missing-import.txt")"
grep -q "dev.twilitrealm.randomizer: not compatible with this build: uki_catch: nothing calls it" \
  "$out/unreached.txt" || fail "the unreached hook target was not named: $(cat "$out/unreached.txt")"
grep -q "dev.twilitrealm.randomizer: not compatible with this build: b_bq_end" "$out/absent.txt" ||
  fail "the absent hook target was not named: $(cat "$out/absent.txt")"
grep -q "no image runtime table" "$out/notable.txt" && grep -q "_Unwind_Resume (libgcc)" "$out/notable.txt" ||
  fail "without the image runtime table the hidden imports were not reported: $(cat "$out/notable.txt")"
for mod in dev.aleks2.loader_lab dev.twilitrealm.cosmetics dev.twilitrealm.randomizer; do
  grep -q "$mod: INCOMPATIBLE" "$out/notable.txt" || fail "without the table $mod was not refused"
done

# 7. H37-c6: the official Randomizer must come from lib/switch-arm64.
mkdir -p "$out/linuxrando"
cp "$mods/loader_lab.dusk" "$mods/cosmetics.dusk" "$mods/rollandsprint.dusk" "$out/linuxrando/"
"$py" - "$mods/randomizer.dusk" "$out/linuxrando/randomizer.dusk" <<'PYEOF'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as src, zipfile.ZipFile(sys.argv[2], "w") as dst:
    for info in src.infolist():
        name = info.filename.replace("lib/switch-arm64/", "lib/linux-aarch64/")
        dst.writestr(name, src.read(info))
PYEOF
candidate linux-randomizer "$out/game.elf" "$out/game.symdb.raw" "$out/game.nro" "$out/linuxrando" not
grep -q "dev.twilitrealm.randomizer: .*must ship lib/switch-arm64/mod.so" "$out/linux-randomizer.txt" ||
  fail "a linux-aarch64 Randomizer was not refused: $(cat "$out/linux-randomizer.txt")"

# 6. Identity and inputs.
candidate other-nro "$out/game.elf" "$out/game.symdb.raw" "$out/game-unreached.nro" "$mods" not
grep -q "the NRO's build id .* is not the ELF's" "$out/other-nro.txt" || fail "an NRO from another link was accepted"
candidate stale "$out/game.elf" "$out/game-unreached.symdb.raw" "$out/game.nro" "$mods" not
grep -q "stale symbol manifest" "$out/stale.txt" || fail "a manifest from another link was accepted"
mkdir -p "$out/three"
cp "$mods/loader_lab.dusk" "$mods/cosmetics.dusk" "$mods/rollandsprint.dusk" "$out/three/"
candidate missing-package "$out/game.elf" "$out/game.symdb.raw" "$out/game.nro" "$out/three" not
grep -q "dev.twilitrealm.randomizer: .*randomizer.dusk is missing" "$out/missing-package.txt" ||
  fail "a missing package was not reported"
echo "int y;" >> "$repo/src/file.cpp"
candidate dirty "$out/game.elf" "$out/game.symdb.raw" "$out/game.nro" "$mods" not
grep -q "not a clean commit (dirty=1): src/file.cpp" "$out/dirty.txt" || fail "a changed tracked file was accepted"
for case in missing-import unreached absent notable other-nro stale missing-package dirty; do
  echo "$case: $(grep -c "problem:" "$out/$case.txt") problem(s); first: $(grep -m1 "problem:" "$out/$case.txt" | cut -c1-140)"
done

if [ "$failures" -gt 0 ]; then
  echo "$failures check(s) failed"
  exit 1
fi
echo "test-h37-candidate: all checks passed"
