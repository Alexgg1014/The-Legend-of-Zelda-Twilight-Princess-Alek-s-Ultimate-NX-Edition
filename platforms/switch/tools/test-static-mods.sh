#!/usr/bin/env bash
# H36: host test of the static-mod build for Switch, without building Switch.
#
#  1. The proposed H36 set (CMakePresets.json, switch-devkitA64-release-h36): Cosmetics, the
#     Luau runtime and Roll and Sprint; no template_mod, no flow_demo, no randomizer.
#  2. StaticModSet.cmake in CMake script mode: the selection check, the built-in table, and
#     every selection or table it must refuse.
#  3. The real mods compiled and sealed exactly as StaticMods.cmake does (ar, ld -r
#     --whole-archive, seal_mod.py), then static_mod_link.py over them: the H36 set links
#     together and yields the names it needs; combinations that would share SDK template
#     code are refused; damaged inputs fail cleanly.
#  4. Every line of static_mod_symbols.txt compiled against its game header.
#  5. Both generated tables compiled, linked and read through switch_builtin_mods.hpp,
#     and the table's per-mod names matched against the sealed objects.
#  6. hook_call_sites.py on a small linked program: direct callers, no caller, missing, vtable.
#
# The mods are compiled for AArch64 with aarch64-linux-gnu-g++ when it is installed (the
# Switch's ABI for member pointers and its object format), otherwise for the host. Override
# with STATIC_MODS_CROSS=<prefix> (e.g. aarch64-none-elf-) or STATIC_MODS_CROSS= for the host.
# Cosmetics is a submodule: COSMETICS_SRC if mods/cosmetics is not checked out. The Luau
# runtime is included when its sources are found (LUAU_SRC/LUABRIDGE_SRC or a build's _deps,
# as in test-luau-probe.sh). fmt: FMT_SRC or a build's _deps.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-static-mods-test"
rm -rf "$out"
mkdir -p "$out"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
cmake="${CMAKE:-cmake}"
fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok   $*"; }

if [ -z "${STATIC_MODS_CROSS+x}" ]; then
  if command -v aarch64-linux-gnu-g++ >/dev/null 2>&1; then STATIC_MODS_CROSS=aarch64-linux-gnu-; else STATIC_MODS_CROSS=; fi
fi
X="$STATIC_MODS_CROSS"
echo "mods compiled with ${X}g++"
shopt -s nullglob
deps=("$root"/build/*/_deps)
first_with() {  # first_with marker candidate...
  local marker="$1" c
  shift
  for c in "$@"; do if [ -n "$c" ] && [ -e "$c/$marker" ]; then echo "$c"; return 0; fi; done
  return 1
}
fmt="$(first_with include/fmt/format.h "${FMT_SRC:-}" "${deps[@]/%//fmt-src}")" || fail "fmt 12 not found (FMT_SRC)"
cosmetics="$(first_with mod.json "$root/mods/cosmetics" "${COSMETICS_SRC:-}")" || cosmetics=""
luau="$(first_with VM/src/lvm.h "${LUAU_SRC:-}" "${deps[@]/%//luau-src}")" || luau=""
luabridge="$(first_with Source/LuaBridge/LuaBridge.h "${LUABRIDGE_SRC:-}" "${deps[@]/%//luabridge3-src}")" || luabridge=""

# --- 1. the proposed H36 set ------------------------------------------------------------
"$py" - "$root" <<'PYEOF'
import json, pathlib, sys
root = pathlib.Path(sys.argv[1])
presets = json.loads((root / "CMakePresets.json").read_text())
configure = {p["name"]: p for p in presets["configurePresets"]}
build = {p["name"]: p for p in presets["buildPresets"]}
h36 = configure["switch-devkitA64-release-h36"]
assert h36["inherits"] == ["switch-devkitA64-release"], h36
mods = h36["cacheVariables"]["ALEKS2_STATIC_MODS"].split(";")
assert mods == ["cosmetics", "luau_runtime", "rollandsprint"], mods
for absent in ("template_mod", "flow_demo", "randomizer"):
    assert absent not in mods, absent
assert build["switch-devkitA64-release-h36"]["configurePreset"] == "switch-devkitA64-release-h36"
assert set(build["switch-devkitA64-release-h36"]["targets"]) == {"dusklight", "dusklight_nro"}
# The other presets carry no mod set of their own: the H36 set is opt-in.
for name in ("switch-devkitA64-release", "switch-devkitA64-debug"):
    assert "ALEKS2_STATIC_MODS" not in configure[name].get("cacheVariables", {}), name
doc = (root / "docs/switch-h36-mod-platform.md").read_text()
assert "--preset switch-devkitA64-release-h36" in doc
assert '"cosmetics;luau_runtime;rollandsprint"' in doc
print("ok   H36 preset: cosmetics;luau_runtime;rollandsprint, without template_mod/flow_demo/randomizer")
PYEOF

# --- 2. StaticModSet.cmake in script mode --------------------------------------------------
casefile="$here/static_mods_test.cmake"
run_case() { "$cmake" "$@" -P "$casefile" > "$out/case.log" 2>&1; }
expect_refusal() {  # expect_refusal "<message part>" -D...
  local message="$1"
  shift
  if run_case "$@"; then fail "accepted: $*"; fi
  grep -qF -- "$message" "$out/case.log" || { cat "$out/case.log" >&2; fail "no '$message' for: $*"; }
}
mods_dir="$out/mods"
mkdir -p "$mods_dir/real_mod" "$mods_dir/empty_submodule"
touch "$mods_dir/real_mod/CMakeLists.txt"
for mod in luau_runtime rollandsprint; do
  mkdir -p "$mods_dir/$mod" && cp "$root/mods/$mod/CMakeLists.txt" "$mods_dir/$mod/"
done
mkdir -p "$mods_dir/cosmetics" && touch "$mods_dir/cosmetics/CMakeLists.txt"
run_case -DCASE=select -DMODS_DIR="$mods_dir" "-DSELECTION=cosmetics;luau_runtime;rollandsprint" \
  || { cat "$out/case.log"; fail "H36 selection refused"; }
if [ -f "$root/mods/cosmetics/CMakeLists.txt" ]; then
  run_case -DCASE=select -DMODS_DIR="$root/mods" "-DSELECTION=cosmetics;luau_runtime;rollandsprint" \
    || { cat "$out/case.log"; fail "H36 selection refused against mods/"; }
fi
expect_refusal "listed twice" -DCASE=select -DMODS_DIR="$mods_dir" "-DSELECTION=real_mod;real_mod"
expect_refusal "empty entry" -DCASE=select -DMODS_DIR="$mods_dir" "-DSELECTION=real_mod;;rollandsprint"
expect_refusal "no such mod 'mods/template_mods'" -DCASE=select -DMODS_DIR="$mods_dir" "-DSELECTION=template_mods"
expect_refusal "is not a directory name" -DCASE=select -DMODS_DIR="$mods_dir" "-DSELECTION=../mods/real_mod"
expect_refusal "git submodule update --init mods/empty_submodule" \
  -DCASE=select -DMODS_DIR="$mods_dir" "-DSELECTION=empty_submodule"
pass "selection: H36 set accepted; duplicate, empty, unknown, path and unchecked-out submodule refused"

# The real targets and ids, read from the mods the way StaticMods.cmake reads them.
targets=()
ids=()
for dir in "$cosmetics" "$root/mods/luau_runtime" "$root/mods/rollandsprint"; do
  [ -n "$dir" ] || continue
  targets+=("$(sed -n 's/^add_mod(\([A-Za-z0-9_]*\).*/\1/p' "$dir/CMakeLists.txt" | head -1)")
  ids+=("$("$py" -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$dir/mod.json")")
done
[ -n "$cosmetics" ] || echo "note: cosmetics is not checked out; it is left out of the table and link checks"
join() { local IFS=';'; echo "$*"; }
run_case -DCASE=table "-DTARGETS=$(join "${targets[@]}")" "-DIDS=$(join "${ids[@]}")" -DOUT="$out/table.cpp" \
  || { cat "$out/case.log"; fail "table refused"; }
for target in "${targets[@]}"; do
  grep -qF "&${target}__mod_meta," "$out/table.cpp" || fail "table lacks $target"
done
grep -qF '{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},' "$out/table.cpp" || fail "table not terminated"
run_case -DCASE=table_empty -DOUT="$out/table_empty.cpp" || fail "empty table refused"
expect_refusal "1 id(s)" -DCASE=table_mismatch
expect_refusal "share the id 'dev.x.same'" -DCASE=table_duplicate_id
expect_refusal "cannot prefix a C symbol" -DCASE=table_bad_target
pass "built-in table: ${#targets[@]} row(s) from the real mods; mismatched, duplicate-id and bad-target tables refused"

# --- 3. the real mods, compiled and sealed ------------------------------------------------
game=(-std=gnu++20 -O2 -fPIE -fsigned-char -w -DTARGET_PC=1 -DWIDESCREEN_SUPPORT=1 -DAVOID_UB=1
      -DVERSION=0 -DMTX_USE_PS=1 -DPARTIAL_DEBUG=1 -DDUSK_MOD_FEATURE_GAME=1
      -I"$root/include" -I"$root/assets/GZ2E01" -I"$root/libs/JSystem/include"
      -I"$root/extern/aurora/include/dolphin" -I"$root/extern/aurora/include" -I"$root/sdk/include"
      -include global.h)
fmtf=(-DDUSK_MOD_FEATURE_FMT=1 -DFMT_HEADER_ONLY=1 -I"$fmt/include")
objs="$out/objs"
mkdir -p "$objs"
seal() {  # seal <target> <object dir>: what StaticMods.cmake does to one mod
  local target="$1" dir="$2"
  "${X}ar" rcs "$objs/$target.a" "$dir"/*.o
  "${X}ld" -r --whole-archive "$objs/$target.a" -o "$objs/$target.merged.o"
  "$py" "$here/seal_mod.py" --objcopy "${X}objcopy" --nm "${X}nm" --name "$target" \
      "$objs/$target.merged.o" "$objs/$target.sealed.o" > /dev/null
}
compile_mod() {  # compile_mod <target> <flags...> -- <sources...>
  local target="$1" dir="$objs/$1"
  shift
  local flags=() src
  while [ "$1" != "--" ]; do flags+=("$1"); shift; done
  shift
  mkdir -p "$dir"
  local pids=() n=0
  for src in "$@"; do
    "${X}g++" "${flags[@]}" -c "$src" -o "$dir/$n.o" &
    pids+=($!)
    n=$((n + 1))
  done
  local pid
  for pid in "${pids[@]}"; do wait "$pid" || fail "compiling $target"; done
  seal "$target" "$dir"
}
compile_mod roll_and_sprint "${game[@]}" -- "$root"/mods/rollandsprint/src/*.cpp "$root/sdk/src/game_feature.cpp"
set_args=("roll_and_sprint=$objs/roll_and_sprint.sealed.o")
if [ -n "$cosmetics" ]; then
  compile_mod cosmetics "${game[@]}" "${fmtf[@]}" -- "$cosmetics"/src/*.cpp "$root/sdk/src/game_feature.cpp"
  set_args=("cosmetics=$objs/cosmetics.sealed.o" "${set_args[@]}")
fi
if [ -n "$luau" ] && [ -n "$luabridge" ]; then
  compile_mod luau_runtime -std=c++20 -O2 -fPIE -w -I"$root/sdk/include" -I"$luau/Common/include" \
      -I"$luau/Ast/include" -I"$luau/Bytecode/include" -I"$luau/Compiler/include" -I"$luau/VM/include" \
      -I"$luabridge/Source" -DLUABRIDGE_SAFE_STACK_CHECKS=0 \
      -include "$root/mods/luau_runtime/src/lua_bridge_helpers.hpp" \
      -include "$root/mods/luau_runtime/src/lua_helpers.hpp" -- \
      "$root"/mods/luau_runtime/src/*.cpp "$root"/mods/luau_runtime/src/services/*.cpp \
      "$root"/mods/luau_runtime/src/services/audio_res/*.cpp
  set_args=("${set_args[0]}" "luau_runtime=$objs/luau_runtime.sealed.o" "${set_args[@]:1}")
else
  echo "note: Luau sources not found; luau_runtime is left out of the link checks"
fi
fixtures="$here/static_mods_fixtures"
compile_mod colliding "${game[@]}" "${fmtf[@]}" -- "$fixtures/colliding_mod.cpp"
compile_mod colliding_again "${game[@]}" "${fmtf[@]}" -- "$fixtures/colliding_mod.cpp"
compile_mod distinct "${game[@]}" -- "$fixtures/distinct_mod.cpp"
pass "sealed: ${set_args[*]%%=*} and three fixtures"

link() { "$py" "$here/static_mod_link.py" "$@" > "$out/link.log" 2>&1; }
aliases="$root/platforms/switch/static_mod_symbols.txt"

# The H36 set links together.
link --aliases "$aliases" --output "$out/symbols.cpp" --targets "$out/hook_targets.txt" "${set_args[@]}" \
  || { cat "$out/link.log"; fail "the H36 set was refused"; }
grep -q "no shared template code" "$out/link.log" || fail "no verdict for the H36 set"
"$py" - "$out" "$cosmetics" <<'PYEOF'
import pathlib, re, sys
out = pathlib.Path(sys.argv[1])
have_cosmetics = bool(sys.argv[2])
names = re.findall(r'^\s*\{"([^"]+)", &aleks2_builtin_symbol_\d+, (true|false)\},$',
                   (out / "symbols.cpp").read_text(), re.M)
expected = {("_ZTV10COutFont_c", "false"), ("_ZTV15dMeterHakusha_c", "false"),
            ("dMenu_DmapBg_c::buttonIconScreenInit", "true")} if have_cosmetics else set()
assert set(names) == expected and len(names) == len(expected), names
rows = [l.split("\t") for l in (out / "hook_targets.txt").read_text().splitlines()
        if l and not l.startswith("#")]
by_mod = {}
for mod, kind, symbol, display in rows:
    by_mod.setdefault(mod, []).append((kind, symbol, display))
assert len(by_mod["roll_and_sprint"]) == 4, by_mod["roll_and_sprint"]
assert {s for _, s, _ in by_mod["roll_and_sprint"]} == {
    "_ZN9daAlink_c12procMoveInitEv", "_ZN9daAlink_c14setDoubleAnimeEfffNS_11daAlink_ANMES0_if",
    "_ZN9daAlink_c17checkNormalActionEv", "_ZN9daAlink_c20decideCommonDoStatusEv"}
assert "luau_runtime" not in by_mod  # the runtime hooks nothing
if have_cosmetics:
    kinds = [k for k, _, _ in by_mod["cosmetics"]]
    assert len(kinds) == 28 and kinds.count("vtable") == 2 and kinds.count("name") == 1, kinds
    assert ("name", "_ZN14dMenu_DmapBg_c20buttonIconScreenInitEv",
            "dMenu_DmapBg_c::buttonIconScreenInit") in by_mod["cosmetics"]
print(f"ok   H36 set: no shared template code; {len(names)} name(s) for by-name resolution; "
      f"{len(rows)} hook target(s)")
PYEOF

# Combinations that must be refused: the same hook target, under the same alias, as Roll and
# Sprint; and one mod twice under two names.
if link --output "$out/x.cpp" "roll_and_sprint=$objs/roll_and_sprint.sealed.o" "colliding=$objs/colliding.sealed.o"; then
  fail "a mod sharing Roll and Sprint's hook template was accepted"
fi
grep -q "cannot be compiled into one build together" "$out/link.log" || fail "no refusal message"
grep -q "procMoveInit.*trampoline.*defined by colliding, roll_and_sprint" "$out/link.log" \
  || { cat "$out/link.log"; fail "the refusal does not name the trampoline and both mods"; }
grep -q "roll_and_sprint uses roll_and_sprint__mod_ctx" "$out/link.log" || fail "the refusal does not name the context"
if link --output "$out/x.cpp" "colliding=$objs/colliding.sealed.o" "colliding_again=$objs/colliding_again.sealed.o"; then
  fail "the same mod twice was accepted"
fi
pass "refused: a hook shared with Roll and Sprint (names the trampoline and both contexts); a mod twice"

# The fixture that hooks by name and a virtual member next to the H36 set: accepted, its C name
# in the table, the vtable it shares with Cosmetics listed once, its unmapped name warned about.
link --aliases "$aliases" --output "$out/symbols_all.cpp" "${set_args[@]}" "distinct=$objs/distinct.sealed.o" \
  || { cat "$out/link.log"; fail "the distinct fixture was refused"; }
grep -q "warning: distinct: no link name for 'fixture_c::unmapped'" "$out/link.log" || fail "no warning for an unmapped name"
grep -q '{"fixture_c_function", &aleks2_builtin_symbol_[0-9]*, true},' "$out/symbols_all.cpp" || fail "C name missing"
[ "$(grep -c '_ZTV15dMeterHakusha_c' "$out/symbols_all.cpp")" = 2 ] || fail "vtable not deduplicated"
pass "accepted beside the set: by-name and virtual hooks of another mod; unmapped name warned about"

# Damaged inputs fail with a message, not a traceback.
head -c 300 "$objs/roll_and_sprint.sealed.o" > "$out/truncated.o"
if link --output "$out/x.cpp" "broken=$out/truncated.o"; then fail "a truncated object was accepted"; fi
grep -q "^static_mod_link: .*malformed ELF object" "$out/link.log" || { cat "$out/link.log"; fail "truncated object"; }
! grep -q Traceback "$out/link.log" || fail "traceback on a truncated object"
printf 'Only::two _ZN4Only3twoEv\n' > "$out/bad_aliases.txt"
if link --aliases "$out/bad_aliases.txt" --output "$out/x.cpp" "${set_args[@]}"; then fail "a bad alias file was accepted"; fi
grep -q "expected '<display name> <mangled name> <header>'" "$out/link.log" || fail "bad alias file message"
if link --output "$out/x.cpp" "no_such_mod=$out/does-not-exist.o"; then fail "a missing object was accepted"; fi
pass "damaged: truncated object, malformed alias file and missing object fail cleanly"

# --- 4. the alias file against the game headers ----------------------------------------------
"$py" - "$aliases" "$here" > "$out/aliases.tsv" <<'PYEOF'
import pathlib, sys
sys.path.insert(0, sys.argv[2])
import static_mod_link
for display, mangled, header in static_mod_link.load_alias_entries(pathlib.Path(sys.argv[1])):
    print(f"{display}\t{mangled}\t{header}")
PYEOF
n=0
while IFS=$'\t' read -r display mangled header; do
  printf '#include "%s"\nvoid* h36_alias_reference() { auto p = &%s; return reinterpret_cast<void*&>(p); }\n' \
      "$header" "$display" > "$out/alias_$n.cpp"
  "${X}g++" "${game[@]}" -fno-access-control -c "$out/alias_$n.cpp" -o "$out/alias_$n.o" \
    || fail "cannot reference $display through $header"
  "${X}nm" -u "$out/alias_$n.o" | grep -qw "$mangled" \
    || { "${X}nm" -u "$out/alias_$n.o" | grep -i "$(echo "$display" | sed 's/.*:://')" >&2; fail "$display is not $mangled"; }
  n=$((n + 1))
done < "$out/aliases.tsv"
[ "$n" -gt 0 ] || fail "no alias entries"
pass "alias file: $n entr(y/ies) match what the compiler emits for the game header"

# --- 5. the generated tables, compiled and read --------------------------------------------
{
  echo '// Generated by test-static-mods.sh: one stand-in per export the built-in table names.'
  echo '#include <mods/api.h>'
  echo 'extern "C" {'
  for target in "${targets[@]}"; do
    echo "extern const ModMeta ${target}__mod_meta{sizeof(ModMeta), nullptr, nullptr};"
    echo "ModContext* ${target}__mod_ctx = nullptr;"
    for fn in initialize update shutdown; do echo "ModResult ${target}__mod_${fn}(ModError*) { return MOD_OK; }"; done
  done
  echo '}'
} > "$out/table_defs.cpp"
# Only the H36 set and the distinct fixture are in symbols_all.cpp, which the test expects.
cxx="${CXX:-g++}"
san=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
echo 'int main(){}' | "$cxx" -x c++ "${san[@]}" - -o "$out/san" 2>/dev/null || san=()
if [ -n "$cosmetics" ]; then
  "$cxx" -std=c++20 -Wall -Wextra -g "${san[@]}" -I"$root/src" -I"$root/sdk/include" \
      "$here/static_mods_table_test.cpp" "$out/table.cpp" "$out/symbols_all.cpp" "$out/table_defs.cpp" \
      -o "$out/static_mods_table_test"
  "$out/static_mods_table_test" "${ids[@]}"
else
  echo "note: the symbol-table run needs cosmetics (its names are the ones it checks); skipped"
fi
# The table's names are exactly what the seal produced: a relocatable link of the table with
# the sealed mods leaves no per-mod export undefined.
"${X}g++" -std=c++20 -fPIE -w -I"$root/src" -I"$root/sdk/include" -c "$out/table.cpp" -o "$out/table.o"
sealed=()
for target in "${targets[@]}"; do sealed+=("$objs/$target.sealed.o"); done
if [ ${#sealed[@]} -eq ${#targets[@]} ] && [ -f "${sealed[0]}" ] && \
   { [ -n "$luau" ] && [ -n "$luabridge" ] || ! printf '%s\n' "${targets[@]}" | grep -qx luau_runtime; }; then
  "${X}ld" -r "$out/table.o" "${sealed[@]}" -o "$out/table_linked.o"
  if "${X}nm" -u "$out/table_linked.o" | grep -E '__mod_(meta|ctx|initialize|update|shutdown)$'; then
    fail "the table names an export the seal did not produce"
  fi
  pass "table names match the sealed exports of ${targets[*]}"
else
  echo "note: Luau sources not found; the table-to-seal match covers the mods that were sealed"
  for target in "${targets[@]}"; do
    [ -f "$objs/$target.sealed.o" ] || continue
    "${X}nm" "$objs/$target.sealed.o" | grep -q " ${target}__mod_meta$" || fail "$target seal"
  done
fi

# --- 6. hook_call_sites.py on a linked program -----------------------------------------------
cat > "$out/calls.cpp" <<'EOF'
struct Base { virtual int v(); };
int Base::v() { return 1; }
__attribute__((noinline)) int called(int x) { return x + 1; }
__attribute__((noinline)) int uncalled(int x) { return x * 2; }
int (*volatile keep)(int) = &uncalled;
int main(int argc, char**) { Base b; Base* p = &b; return called(argc) + called(argc + 1) + keep(1) + p->v(); }
EOF
calls_ok=0
for tool in "" "$X"; do
  if "${tool}g++" -O0 "$out/calls.cpp" -o "$out/calls${tool:+-cross}" 2>/dev/null || \
     "${tool}g++" -O0 -static "$out/calls.cpp" -o "$out/calls${tool:+-cross}" 2>/dev/null; then
    printf 't\tdirect\t_Z6calledi\tcalled\nt\tdirect\t_Z8uncalledi\tuncalled\nt\tdirect\t_Z7missingv\tmissing\nt\tvtable\t_ZTV4Base\tBase::v\n' > "$out/calls_targets.txt"
    if "$py" "$here/hook_call_sites.py" --objdump "${tool}objdump" --nm "${tool}nm" \
        "$out/calls_targets.txt" "$out/calls${tool:+-cross}" > "$out/calls.log"; then
      fail "hook_call_sites.py passed a target with no caller"
    fi
    grep -q "called .* 2 direct call(s)" "$out/calls.log" || { cat "$out/calls.log"; fail "direct calls (${tool:-host})"; }
    grep -q "uncalled .*NO DIRECT CALLER" "$out/calls.log" || fail "uncalled (${tool:-host})"
    grep -q "missing .*MISSING" "$out/calls.log" || fail "missing (${tool:-host})"
    grep -q "Base::v .*ok (virtual, via vtable)" "$out/calls.log" || fail "vtable (${tool:-host})"
    grep -q "4 hook target(s), 2 with nothing to catch or missing" "$out/calls.log" || fail "summary"
    printf 't\tdirect\t_Z6calledi\tcalled\nt\tvtable\t_ZTV4Base\tBase::v\n' > "$out/calls_ok.txt"
    "$py" "$here/hook_call_sites.py" --objdump "${tool}objdump" --nm "${tool}nm" \
        "$out/calls_ok.txt" "$out/calls${tool:+-cross}" > /dev/null || fail "hook_call_sites.py refused good targets"
    calls_ok=$((calls_ok + 1))
  fi
  [ -n "$X" ] || break
done
[ "$calls_ok" -gt 0 ] || fail "could not link the call-site program"
pass "hook_call_sites.py: direct callers counted, no caller and missing reported, vtable accepted ($calls_ok toolchain(s))"

echo "test-static-mods: all checks passed"
