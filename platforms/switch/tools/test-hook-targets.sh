#!/usr/bin/env bash
# H37-c2: every by-name hook target of the official H37 packages is a whole function in the code
# the Switch build generates -- present, long enough for funchook's patch, reached, and never
# inlined into a caller.
#
#   test-hook-targets.sh [OUT_DIR]
#
# H37-c1's audit of the final ELF found five Randomizer targets missing from the symbol
# manifest: b_bq_end, lure_heart, uki_catch, d_a_npc_ks.cpp#action_check (static functions with
# one caller) and daWindStone_c::chkEveOccur (small, two callers), all inlined by -O3 until no
# copy was left. Measured here too: dComIfGp_setNextStage, which the manifest did find, had been
# folded into the short overload and into dComIfGs_gameStart, so the hook would have missed those
# stage changes. All six are DUSK_NOINLINE now.
#
# 1. The game units that define the targets are compiled for AArch64 with the Switch build's
#    code generation, read from the build files themselves: DUSK_SWITCH_OPT_FLAGS
#    (cmake/SwitchOptions.cmake), -g1, the hook-friendly options CMakeLists.txt adds for Switch
#    code mods, ALEKS2_NO_INLINE_FILES with their options, devkitA64's -march/-mtune.
#    The units are compiled with -g2 where the build has -g1: the same code (checked below on
#    the control), and DWARF that records every inlined copy.
# 2. hook_inline_audit.py links each unit into an image of its own and checks every hook line
#    of the official packages in platforms/switch/mod_required_symbols.txt: symgen names it as
#    the post-link manifest will, image_surface.hook_targets accepts it as check_symbol_manifest.py
#    does in the game, the unit's DWARF has no inlined copy of it, and the unit compiled again
#    with inlining off makes no more direct calls to it. A target defined in none of the units
#    fails: the unit list below must grow with the mods' targets.
# 3. The control: a unit with the same flags, a static function with one caller, a small member
#    function with two, and the same with DUSK_NOINLINE's attribute. The audit must find the first
#    two inlined and the last two whole -- what makes the check worth anything; and the control
#    compiled at -g1 and at -g2 must disassemble identically.
#
# Needs aarch64-linux-gnu-g++, python3, symgen (SYMGEN, or downloaded as test-symbol-manifest.sh
# does) and the third-party headers some game units include (SDL3, RmlUi, borealis, Tracy, fmt):
# H37_DEPS_INCLUDES, a colon-separated list of include directories (and Dear ImGui).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-hook-targets}"
rm -rf "$out"
mkdir -p "$out/units"
out="$(cd "$out" && pwd)"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
command -v aarch64-linux-gnu-g++ >/dev/null || { echo "aarch64-linux-gnu-g++ is required"; exit 2; }
symgen="${SYMGEN:?set SYMGEN to the symgen executable cmake/SymbolManifest.cmake pins}"
[ -n "${H37_DEPS_INCLUDES:-}" ] || { echo "set H37_DEPS_INCLUDES (SDL3, RmlUi, borealis, Tracy, fmt, imgui include dirs)"; exit 2; }

official=(com.melonspeedruns.rollandsprint dev.aleks2.loader_lab dev.twilitrealm.cosmetics dev.twilitrealm.randomizer)
units=(
  src/d/actor/d_a_alink.cpp src/d/actor/d_a_b_bq.cpp src/d/actor/d_a_demo_item.cpp src/d/actor/d_a_e_md.cpp
  src/d/actor/d_a_e_mk.cpp src/d/actor/d_a_kytag08.cpp src/d/actor/d_a_mg_rod.cpp src/d/actor/d_a_npc_ks.cpp
  src/d/actor/d_a_obj_gb.cpp src/d/actor/d_a_obj_wind_stone.cpp src/d/d_com_inf_game.cpp src/d/d_menu_dmap.cpp
  src/d/d_menu_ring.cpp src/d/d_save.cpp src/d/d_stage.cpp
)
# H37-c5: the units of the 34 catalog-mod hook targets the symbol manifest lacked (Twilit Essentials 4, ALBW 25,
# Dawnlight 5); see c5_hooks below.
units+=(
  src/d/d_menu_collect.cpp src/d/d_meter2_draw.cpp src/dusk/autosave.cpp
  src/d/actor/d_a_b_gnd.cpp src/d/actor/d_a_e_bs.cpp src/d/actor/d_a_e_cr.cpp
  src/d/actor/d_a_e_db.cpp src/d/actor/d_a_e_fm.cpp src/d/actor/d_a_e_fs.cpp src/d/actor/d_a_e_fz.cpp
  src/d/actor/d_a_e_gb.cpp src/d/actor/d_a_e_gi.cpp src/d/actor/d_a_e_hb.cpp src/d/actor/d_a_e_kk.cpp
  src/d/actor/d_a_e_kr.cpp src/d/actor/d_a_e_mf.cpp src/d/actor/d_a_e_mm.cpp src/d/actor/d_a_e_po.cpp
  src/d/actor/d_a_e_rdb.cpp src/d/actor/d_a_e_s1.cpp src/d/actor/d_a_e_sf.cpp src/d/actor/d_a_e_th.cpp
  src/d/actor/d_a_e_tt.cpp
)
# DM1-c: the units of the 12 local targets catalog mods hook by name (DUSK_NOINLINE since DM1-c); see dm1c_required.
units+=(src/m_Do/m_Do_graphic.cpp src/d/actor/d_a_ni.cpp src/d/actor/d_a_b_ob.cpp src/d/actor/d_a_e_sg.cpp)
# DM1-d: and the units of Dawnlight's local targets (d_a_e_mk.cpp is already above).
units+=(src/d/actor/d_a_b_gm.cpp src/d/actor/d_a_e_hzelda.cpp src/d/actor/d_a_e_kg.cpp src/d/actor/d_a_e_mk_bo.cpp)
c5_mods=(com.dusklight.twilit_essentials dev.bezide.dawnlight dev.albt.albw)
c5_hooks=(
  dMenu_Collect2D_c::setHeartPiece canAutoSave src/d/actor/d_a_b_gnd.cpp#damage_check src/d/actor/d_a_e_th.cpp#damage_check
  daE_KK_c::setActionMode daE_TT_c::setActionMode daE_GI_c::setActionMode daE_FZ_c::setActionMode
  src/d/actor/d_a_e_mf.cpp#small_damage e_po_damage e_cr_s_damage src/d/actor/d_a_e_s1.cpp#e_s1_damage
  src/d/actor/d_a_e_bs.cpp#e_bs_damage src/d/actor/d_a_e_fs.cpp#e_fs_damage src/d/actor/d_a_e_sf.cpp#damage_check
  src/d/actor/d_a_e_sf.cpp#e_sf_s_damage src/d/actor/d_a_e_kr.cpp#e_kr_damage src/d/actor/d_a_e_rdb.cpp#e_rdb_damage
  e_mm_damage b_bq_damage b_bq_wait e_fm_down e_fm_normal e_fm_fight_run e_fm_damage_run e_fm_stop e_fm_n_fight
  e_fm_f_fight e_fm_fire e_hb_s_damage e_gb_attack_1 e_gb_attack_2 e_db_s_damage dMeter2Draw_c::drawOxygen
)

# --- 1. the Switch build's code generation, from the build files ------------------------------
"$py" - "$root" > "$out/flags.txt" <<'PYEOF'
import re, sys, pathlib
root = pathlib.Path(sys.argv[1])
switch = (root / "cmake/SwitchOptions.cmake").read_text()
opt = re.search(r"set\(DUSK_SWITCH_OPT_FLAGS(.*?)\)", switch, re.S).group(1).split()
lists = (root / "CMakeLists.txt").read_text()
block = lists[lists.index("if (ALEKS2_SWITCH_CODE_MODS AND CMAKE_CXX_COMPILER_ID STREQUAL \"GNU\")"):]
block = block[:block.index("endif ()")]
abi = re.findall(r"\$<\$<COMPILE_LANGUAGE:CXX>:(-f[^>]+)>", block)
files = re.search(r"set\(ALEKS2_NO_INLINE_FILES ([^)]*)\)", block).group(1).split()
no_inline = re.search(r'COMPILE_OPTIONS\s*"([^"]*)"', block).group(1).split(";")
print(" ".join(opt + ["-g1"] + abi))  # -g1 as the build has it; the units get -g2 (see 1.)
print(" ".join(files))
print(" ".join(no_inline))
PYEOF
read -r -a codegen < <(sed -n 1p "$out/flags.txt" | sed 's/ -g1 / -g2 /')
read -r -a no_inline_files < <(sed -n 2p "$out/flags.txt")
read -r -a no_inline_flags < <(sed -n 3p "$out/flags.txt")
echo "code generation: ${codegen[*]}"
echo "no inlining at all in: ${no_inline_files[*]} (${no_inline_flags[*]})"
[ "${#no_inline_files[@]}" -gt 0 ] && [ "${#codegen[@]}" -gt 6 ] || { echo "could not read the build's flags"; exit 1; }

arch=(-march=armv8-a+crc+crypto -mtune=cortex-a57 -ftls-model=local-exec -fPIE)
defs=(-DTARGET_PC=1 -DWIDESCREEN_SUPPORT=1 -DAVOID_UB=1 -DVERSION=0 -DMTX_USE_PS=1 -DPARTIAL_DEBUG=1  # GameABIConfig
      -DDUSK_BUILDING_GAME=1 -DDUSK_CODE_MODS=1 -DALEKS2_NATIVE_MOD_LOADER=1 -DDUSK_HAS_FUNCHOOK=1 -DDUSK_HAS_PREPATCH=0
      -DNDEBUG=1 -DNDEBUG_DEFINED=1 -DDEBUG_DEFINED=0                                            # game_base
      -DDUSK_PLATFORM_SWITCH=1 -DTARGET_SWITCH=1)                                                 # SwitchOptions
incs=(-I"$root/include" -I"$root/assets/GZ2E01" -I"$root/libs/JSystem/include" -I"$root/extern/aurora/include/dolphin"
      -I"$root/extern/aurora/include" -I"$root/sdk/include" -I"$root/src" -I"$root/extern")
IFS=: read -r -a extra <<< "$H37_DEPS_INCLUDES"
for d in "${extra[@]}"; do incs+=(-I"$d"); done

compile() {  # SOURCE (relative to the root) OBJECT [extra flags...]
  local src="$1" obj="$2"
  shift 2
  # From the root, with the relative path: DWARF then names the file as symgen's -C root expects.
  (cd "$root" && aarch64-linux-gnu-g++ -std=gnu++20 -Wno-multichar "${arch[@]}" "${codegen[@]}" "$@" "${defs[@]}" \
    "${incs[@]}" -include include/dusk_pch.hpp -c "$src" -o "$obj")
}
link_unit() {  # OBJECT ELF: the unit alone, its undefined references left at zero
  aarch64-linux-gnu-ld -pie --unresolved-symbols=ignore-all --no-dynamic-linker -z notext -e 0 --build-id=sha1 \
    --defsym=__dso_handle=0 -o "$2" "$1"
}

mkdir -p "$out/reference"
inlining_off=(-fno-inline -fno-inline-functions -fno-inline-small-functions -fno-inline-functions-called-once
              -fno-early-inlining)
pids=()
for src in "${units[@]}"; do
  name="$(basename "$src" .cpp)"
  flags=()
  for f in "${no_inline_files[@]}"; do [ "$f" = "$src" ] && flags=("${no_inline_flags[@]}"); done
  ( compile "$src" "$out/units/$name.o" "${flags[@]}" > "$out/units/$name.log" 2>&1 &&
    link_unit "$out/units/$name.o" "$out/units/$name.elf" >> "$out/units/$name.log" 2>&1 ) &
  pids+=($!)
  ( compile "$src" "$out/reference/$name.o" "${inlining_off[@]}" > "$out/reference/$name.log" 2>&1 &&
    link_unit "$out/reference/$name.o" "$out/reference/$name.elf" >> "$out/reference/$name.log" 2>&1 ) &
  pids+=($!)
done
status=0
for p in "${pids[@]}"; do wait "$p" || status=1; done
if [ "$status" -ne 0 ]; then
  grep -l -E "error|undefined" "$out"/units/*.log "$out"/reference/*.log | while read -r log; do
    echo "== $log"; grep -E "error" "$log" | head -5
  done
  exit 1
fi
echo "compiled ${#units[@]} game units, and again with inlining off"

# DM1-c (first, so the older blocks' known failures do not hide it): the local functions the 2026-10-14 catalog hooks by name that -O3 had inlined into every caller (DM1B-c1
# audit), and fish_move, which showed up once fish_normal was out of line. Not in mod_required_symbols.txt (that file
# is the official packages'), so the lines are written here.
dm1c_required=(
  "com.giorgiomendieta.dof_hq hook name drawDepth2"
  "com.krowten.object_yeet hook name ni_carry"
  "com.blueflaregames.zora_swim hook name src/d/actor/d_a_b_ob.cpp#core_damage_check"
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_b_ob.cpp#fish_damage_check"
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_b_ob.cpp#pl_check"
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_b_ob.cpp#fish_normal"
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_b_ob.cpp#fish_vacume"
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_b_ob.cpp#demo_camera"
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_b_ob.cpp#fish_move"
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_e_sg.cpp#action"
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_e_sg.cpp#e_sg_move"
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_e_sg.cpp#e_sg_search"
)
printf '%s\n' "${dm1c_required[@]}" > "$out/required-dm1c.txt"
"$py" "$here/hook_inline_audit.py" --symgen "$symgen" --readelf aarch64-linux-gnu-readelf --source-root "$root" \
  --required "$out/required-dm1c.txt" \
  --mods com.giorgiomendieta.dof_hq com.krowten.object_yeet com.blueflaregames.zora_swim com.blueflaregames.dark_morpheel \
  --reference-dir "$out/reference" --report "$out/hook-targets-dm1c.json" "$out"/units/*.elf

# DM1-d: every "<file>#<function>" a catalog mod names as a string (research/catalog-tools/string_hooks.py) for
# Dark Morpheel 1.0.0, Zora Swim 1.3.0 and Dawnlight 4.0.3, in these units -- all of them, not only the ones DM1-d
# marked: taking one function out of line can let -O3 inline another (fish_normal -> fish_move in DM1-c). The
# *_Execute entries are left out: they are reached through the actor profile table, which a unit alone cannot show.
dm1d_required=(
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_b_ob.cpp#anm_init"
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_b_ob.cpp#demo_camera"
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_b_ob.cpp#fish_damage_check"
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_b_ob.cpp#fish_move"
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_b_ob.cpp#fish_normal"
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_b_ob.cpp#fish_vacume"
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_b_ob.cpp#pl_check"
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_e_sg.cpp#action"
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_e_sg.cpp#e_sg_move"
  "com.blueflaregames.dark_morpheel hook name src/d/actor/d_a_e_sg.cpp#e_sg_search"
  "com.blueflaregames.zora_swim hook name src/d/actor/d_a_b_ob.cpp#core_damage_check"
  "dev.bezide.dawnlight hook name src/d/actor/d_a_b_gm.cpp#action"
  "dev.bezide.dawnlight hook name src/d/actor/d_a_b_ob.cpp#core_action"
  "dev.bezide.dawnlight hook name src/d/actor/d_a_b_ob.cpp#fish_move"
  "dev.bezide.dawnlight hook name src/d/actor/d_a_e_hzelda.cpp#action"
  "dev.bezide.dawnlight hook name src/d/actor/d_a_e_kg.cpp#action"
  "dev.bezide.dawnlight hook name src/d/actor/d_a_e_mk.cpp#action"
  "dev.bezide.dawnlight hook name src/d/actor/d_a_e_mk.cpp#e_mk_move"
  "dev.bezide.dawnlight hook name src/d/actor/d_a_e_mk_bo.cpp#action"
  "dev.bezide.dawnlight hook name src/d/actor/d_a_e_mk_bo.cpp#e_mk_bo_hasira"
  "dev.bezide.dawnlight hook name src/d/actor/d_a_e_mk_bo.cpp#e_mk_bo_r04"
  "dev.bezide.dawnlight hook name src/d/actor/d_a_e_mk_bo.cpp#e_mk_bo_shot"
)
printf '%s\n' "${dm1d_required[@]}" > "$out/required-dm1d.txt"
set +e
"$py" "$here/hook_inline_audit.py" --symgen "$symgen" --readelf aarch64-linux-gnu-readelf --source-root "$root" \
  --required "$out/required-dm1d.txt" \
  --mods com.blueflaregames.dark_morpheel com.blueflaregames.zora_swim dev.bezide.dawnlight \
  --reference-dir "$out/reference" --report "$out/hook-targets-dm1d.json" "$out"/units/*.elf
set -e
# One known reading to set aside, and only that one: anm_init is DUSK_NOINLINE, its DWARF has no inlined copy, and
# fish_normal calls it twice in the source and twice at -O3 -- the inlining-off reference has a third call because jump
# threading duplicated one there (measured on DM1-d: 21 calls against the reference's 22, every source call present).
"$py" - "$out/hook-targets-dm1d.json" <<'PYEOF'
import json, sys
report = json.load(open(sys.argv[1]))
waived = {"src/d/actor/d_a_b_ob.cpp#anm_init"}
bad = []
for name, verdict in sorted(report.items()):
    problems = verdict.get("problems", [])
    if name in waived and verdict.get("inlined_copies") == 0 and problems and all(
            "direct calls made with inlining off are gone" in problem for problem in problems):
        continue
    if problems:
        bad.append(f"{name}: {problems}")
print("dm1d hook targets: " + ("ok" if not bad else "FAIL\n  " + "\n  ".join(bad)))
sys.exit(1 if bad else 0)
PYEOF

# --- 2. the audit -------------------------------------------------------------------------------
"$py" "$here/hook_inline_audit.py" --symgen "$symgen" --readelf aarch64-linux-gnu-readelf --source-root "$root" \
  --required "$root/platforms/switch/mod_required_symbols.txt" --mods "${official[@]}" \
  --reference-dir "$out/reference" --report "$out/hook-targets.json" "$out"/units/*.elf

# H37-c5: the same for the 34 hook targets of the catalog mods (each was inlined into its only caller and dropped).
# dMeter2Draw_c::drawOxygen is a global function nothing in the Switch build calls; it is kept as a root
# (platforms/switch/mod_hook_roots.txt), which this unit-by-unit audit models as "reached from other units".
for hook in "${c5_hooks[@]}"; do
  grep -qE " hook name ${hook}\$" "$root/platforms/switch/mod_required_symbols.txt" ||
    { echo "FAIL: $hook is not a hook line in mod_required_symbols.txt"; exit 1; }
done
while read -r mod kind group name; do
  case " ${c5_mods[*]} " in *" $mod "*) ;; *) continue ;; esac
  for hook in "${c5_hooks[@]}"; do [ "$hook" = "$name" ] && echo "$mod hook name $name"; done
done < <(grep " hook name " "$root/platforms/switch/mod_required_symbols.txt") > "$out/required-c5.txt"
[ "$(wc -l < "$out/required-c5.txt")" -ge "${#c5_hooks[@]}" ] || { echo "FAIL: required-c5.txt is incomplete"; exit 1; }
"$py" "$here/hook_inline_audit.py" --symgen "$symgen" --readelf aarch64-linux-gnu-readelf --source-root "$root" \
  --required "$out/required-c5.txt" --mods "${c5_mods[@]}" \
  --reference-dir "$out/reference" --report "$out/hook-targets-c5.json" "$out"/units/*.elf

# --- 3. the control -----------------------------------------------------------------------------
mkdir -p "$out/control/src/aleks2"
cat > "$out/control/src/aleks2/control.cpp" <<'EOF'
// Same flags as the game units. Inlined away: once_static (one caller), Small::check (small,
// two callers). Kept whole: their DUSK_NOINLINE twins.
struct Small {
    int v;
    bool check();
    __attribute__((noinline)) bool check_kept();
    int a();
    int b();
};
static int once_static(int* p) { return p[0] * 3 + p[1]; }
static __attribute__((noinline)) int once_static_kept(int* p) { return p[0] * 3 + p[1]; }
bool Small::check() { return v > 3; }
bool Small::check_kept() { return v > 3; }
int Small::a() { return check() ? 1 : check_kept() ? 2 : 0; }
int Small::b() { return check() ? 3 : check_kept() ? 4 : 0; }
int control_entry(int* p) { return once_static(p) + once_static_kept(p); }
EOF
printf '%s\n' "dev.test.control hook name once_static" "dev.test.control hook name Small::check" \
  "dev.test.control hook name once_static_kept" "dev.test.control hook name Small::check_kept" > "$out/control/required.txt"
mkdir -p "$out/control/reference"
read -r -a codegen_g1 < <(sed -n 1p "$out/flags.txt")
(cd "$out/control" &&
  aarch64-linux-gnu-g++ -std=gnu++20 "${arch[@]}" "${codegen[@]}" -c src/aleks2/control.cpp -o control.o &&
  aarch64-linux-gnu-g++ -std=gnu++20 "${arch[@]}" "${codegen[@]}" "${inlining_off[@]}" -c src/aleks2/control.cpp \
    -o reference/control.o &&
  aarch64-linux-gnu-g++ -std=gnu++20 "${arch[@]}" "${codegen_g1[@]}" -c src/aleks2/control.cpp -o control-g1.o)
link_unit "$out/control/control.o" "$out/control/control.elf"
link_unit "$out/control/reference/control.o" "$out/control/reference/control.elf"
aarch64-linux-gnu-objdump -d "$out/control/control.o" | tail -n +3 > "$out/control/g2.dis"
aarch64-linux-gnu-objdump -d "$out/control/control-g1.o" | tail -n +3 > "$out/control/g1.dis"
cmp -s "$out/control/g1.dis" "$out/control/g2.dis" || { echo "FAIL: -g2 changed the generated code"; exit 1; }
echo "control: -g1 and -g2 generate the same code"
set +e
"$py" "$here/hook_inline_audit.py" --symgen "$symgen" --readelf aarch64-linux-gnu-readelf \
  --source-root "$out/control" --required "$out/control/required.txt" --report "$out/control/report.json" \
  --reference-dir "$out/control/reference" "$out/control/control.elf" > "$out/control/audit.txt"
control=$?
set -e
cat "$out/control/audit.txt"
"$py" - "$out/control/report.json" "$control" <<'PYEOF'
import json, sys
r = json.load(open(sys.argv[1]))
def bad(name, what):
    return any(what in p for p in r[name]["problems"])
ok = (int(sys.argv[2]) == 1
      and (bad("once_static", "not defined in any audited unit") or bad("once_static", "inlined"))
      and (bad("Small::check", "inlined") or bad("Small::check", "not defined"))
      and not r["once_static_kept"]["problems"] and not r["Small::check_kept"]["problems"])
print("control: inlined targets refused, noinline ones accepted" if ok else "FAIL: the control did not come out as expected")
sys.exit(0 if ok else 1)
PYEOF
echo "test-hook-targets: all checks passed"
