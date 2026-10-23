#!/usr/bin/env bash
# H37: builds the native mod packages the dynamic loader is audited and tested against, from
# their real sources and with the mod SDK (add_mod), for linux-aarch64 -- the platform
# published mods ship -- with Ubuntu's aarch64-linux-gnu GCC. Also writes, per library, an
# independent relocation oracle made with binutils' readelf, for test-elf-loader.sh.
#
#   build-h37-test-mods.sh OUT_DIR
#
# H37-c6: Randomizer is the exception. Its linux-aarch64 build (Ubuntu GCC 13 headers) crashed the game inside the
# game's libstdc++ (devkitA64, GCC 16) the first time it opened an std::ofstream: the two disagree about ios_base and
# locale. With devkitA64 on PATH (or $DEVKITPRO) randomizer.dusk is built with aarch64-none-elf-g++
# (platforms/switch/cmake/SwitchArm64Mod.toolchain.cmake) and holds lib/switch-arm64/mod.so ONLY -- the loader prefers
# it, and the candidate gate refuses any other Randomizer. Without devkitA64 a linux-aarch64 randomizer.dusk is still
# built, for the host tests, with a warning: do not ship it. check_switch_module.py checks the result; if SYMGEN is set,
# mod_required_symbols.txt with Randomizer's lines regenerated from the new library is written to OUT_DIR (see below).
#
# Needs aarch64-linux-gnu-g++/readelf, cmake, ninja, git (yaml-cpp, base64pp and
# battery-embed are fetched by the Randomizer's own CMakeLists) and, as in
# test-luau-probe.sh: FMT_SRC (fmt 12.1.0), LUAU_SRC (luau 0.734), LUABRIDGE_SRC (LuaBridge3
# twilit-v1); COSMETICS_SRC / RANDOMIZER_SRC if those submodules are not checked out.
# Mods whose sources are missing are skipped with a note. H37_TEST_MODS limits the build to
# some of them (names as below, space-separated).
#
# H37-c2: the output is reproducible -- the same bytes from any two output directories:
#  * every compile maps the variable absolute paths away (-ffile-prefix-map): OUT_DIR becomes
#    /h37-mods, the repository /dusklight, and the external sources /deps/<name>. Before, the
#    build directory was in each library's DWARF (.debug_line_str), and through it in the GNU
#    build id, so /out and /out2 gave two different mod.so;
#  * each package is the canonical one make_dusk.py packs (entries stored, sorted, fixed times,
#    normalised permissions, text files LF) from the SDK's own package, unpacked: the SDK's zip
#    carries the time it was made. Only the canonical .dusk -- and its mod.so, extracted from it --
#    is left in OUT_DIR; the SDK's stays in its build directory;
#  * SHA256SUMS lists every .dusk and .so in OUT_DIR.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:?usage: build-h37-test-mods.sh OUT_DIR}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
wanted() { [ -z "${H37_TEST_MODS:-}" ] || [[ " $H37_TEST_MODS " == *" $1 "* ]]; }

# --- which toolchains this run needs --------------------------------------------------------------
devkit_bin=""
if command -v aarch64-none-elf-g++ >/dev/null 2>&1; then
  devkit_bin="$(dirname "$(command -v aarch64-none-elf-g++)")"
elif [ -x "${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin/aarch64-none-elf-g++" ]; then
  devkit_bin="${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin"
fi
unsafe_linux_randomizer=0
[ "${H37_ALLOW_UNSAFE_LINUX_RANDOMIZER_FOR_HOST_TESTS:-}" = 1 ] && unsafe_linux_randomizer=1
need_linux=0
for m in loader_lab template_mod rollandsprint cosmetics luau_runtime; do wanted "$m" && need_linux=1; done
# DM2-a: the hook_stress lab mod, only when named in H37_TEST_MODS (never part of the default set).
stress_wanted=0
[[ " ${H37_TEST_MODS:-} " == *" hook_stress "* ]] && stress_wanted=1 && need_linux=1
# DM2-b: the iostream_lab lab mod, the same way.
iolab_wanted=0
[[ " ${H37_TEST_MODS:-} " == *" iostream_lab "* ]] && iolab_wanted=1 && need_linux=1
# Cross-save: the Cloud Save mod (mods/cross_save), the same way.
crosssave_wanted=0
[[ " ${H37_TEST_MODS:-} " == *" cross_save "* ]] && crosssave_wanted=1 && need_linux=1
rando_mode=none  # none | switch | linux-host-test
if wanted randomizer; then
  if [ -n "$devkit_bin" ]; then
    rando_mode=switch
  elif [ "$unsafe_linux_randomizer" = 1 ]; then
    rando_mode=linux-host-test
    need_linux=1
  else
    echo "build-h37-test-mods.sh: randomizer needs devkitA64 (aarch64-none-elf-g++ on PATH or under \$DEVKITPRO/devkitA64/bin)." >&2
    echo "  Its linux-aarch64 build crashes the game inside the game's libstdc++ (iostream layout); nothing was built." >&2
    echo "  Build the other packages without it with H37_TEST_MODS=\"loader_lab template_mod rollandsprint cosmetics\"," >&2
    echo "  or set H37_ALLOW_UNSAFE_LINUX_RANDOMIZER_FOR_HOST_TESTS=1 for a host-test-only randomizer-linux-host-test.dusk." >&2
    exit 2
  fi
fi
if [ "$need_linux" = 1 ] && ! command -v aarch64-linux-gnu-g++ >/dev/null; then
  echo "aarch64-linux-gnu-g++ is required" >&2; exit 2
fi
# The switch-arm64 flow passes its path maps as one cmake string and runs the Windows toolchain: no spaces anywhere.
no_spaces() {  # no_spaces LABEL PATH
  if [[ "$2" == *" "* ]]; then
    echo "build-h37-test-mods.sh: the switch-arm64 build does not support a space in $1: '$2'" >&2
    echo "  Map the repository to a drive letter without spaces first (Windows cmd, once per session; adapt the path):" >&2
    echo "    subst X: \"D:\\path\\to\\My Projects\\<repository folder>\"" >&2
    echo "  then in Git Bash:  cd /x && H37_TEST_MODS=randomizer platforms/switch/tools/build-h37-test-mods.sh /x/h37-out" >&2
    echo "  (OUT_DIR must be spaceless too, e.g. inside the substituted drive). Undo with: subst X: /d" >&2
    exit 2
  fi
}
if [ "$rando_mode" = switch ]; then
  no_spaces "the repository path" "$root"
  no_spaces "OUT_DIR" "$out"
  for dep in FMT_SRC RANDOMIZER_SRC; do [ -z "${!dep:-}" ] || no_spaces "$dep" "${!dep}"; done
fi

toolchain="$out/aarch64-linux.cmake"
if [ "$need_linux" = 1 ]; then
cat > "$toolchain" <<'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
fi
built_sos=()  # libraries made by THIS run: their relocation oracle is made here, with the readelf of their toolchain
# Path maps, most general first: GCC applies the last one that matches, and OUT_DIR may be inside
# the repository.
maps=("-ffile-prefix-map=$root=/dusklight")
for dep in FMT_SRC LUAU_SRC LUABRIDGE_SRC COSMETICS_SRC RANDOMIZER_SRC; do
  if [ -n "${!dep:-}" ] && [ -d "${!dep}" ]; then
    maps+=("-ffile-prefix-map=$(cd "${!dep}" && pwd)=/deps/$(echo "${dep%_SRC}" | tr 'A-Z' 'a-z')")
  fi
done
maps+=("-ffile-prefix-map=$out=/h37-mods")
# The maps travel in CMAKE_<LANG>_FLAGS, a space-separated string.
for m in "${maps[@]}"; do
  [[ "$m" != *" "* ]] || { echo "build-h37-test-mods.sh: paths with spaces are not supported: $m" >&2; exit 2; }
done
common=(-G Ninja "-DCMAKE_TOOLCHAIN_FILE=$toolchain" -DCMAKE_BUILD_TYPE=RelWithDebInfo
        "-DCMAKE_C_FLAGS=${maps[*]}" "-DCMAKE_CXX_FLAGS=${maps[*]}")
[ -n "${FMT_SRC:-}" ] && common+=("-DFETCHCONTENT_SOURCE_DIR_FMT=$FMT_SRC")

# The SDK's package, unpacked and packed again canonically; its library extracted from the result.
canonical() {  # canonical NAME SDK_DUSK [PLATFORM: linux-aarch64] [devkit: its readelf makes the oracle]
  local name="$1" sdk="$2" platform="${3:-linux-aarch64}" toolchain_kind="${4:-linux}"
  built_sos+=("$name.so:$toolchain_kind")
  rm -rf "$out/stage-$name"
  "$py" - "$sdk" "$out/stage-$name" <<'PYEOF2'
import pathlib, sys, zipfile
dest = pathlib.Path(sys.argv[2])
with zipfile.ZipFile(sys.argv[1]) as z:
    for info in z.infolist():
        if info.is_dir():
            continue
        target = dest / info.filename
        if dest.resolve() not in target.resolve().parents:
            raise SystemExit(f"{sys.argv[1]}: {info.filename} would land outside the package")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(z.read(info))
PYEOF2
  "$py" "$here/make_dusk.py" "$out/stage-$name" "$out/$name.dusk" > /dev/null
  rm -rf "$out/stage-$name"
  "$py" - "$out/$name.dusk" "$out/$name.so" "$platform" <<'PYEOF2'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    open(sys.argv[2], "wb").write(z.read(f"lib/{sys.argv[3]}/mod.so"))
PYEOF2
}

build() {  # build NAME SOURCE_DIR [cmake args...]
  local name="$1" src="$2"
  shift 2
  wanted "$name" || return 0
  if [ ! -f "$src/CMakeLists.txt" ]; then
    echo "note: $name sources not found ($src); skipped"
    return 0
  fi
  cmake -S "$src" -B "$out/build-$name" "${common[@]}" "$@" > "$out/build-$name.log" 2>&1 &&
    cmake --build "$out/build-$name" >> "$out/build-$name.log" 2>&1 ||
    { tail -20 "$out/build-$name.log" >&2; echo "building $name failed" >&2; return 1; }
  local dusk
  dusk="$(find "$out/build-$name/mods" -maxdepth 1 -name '*.dusk' | head -1)"
  if [ -n "$dusk" ]; then
    canonical "$name" "$dusk"
  else
    cp "$(find "$out/build-$name" -maxdepth 1 -name '*.so' | head -1)" "$out/$name.so"
    built_sos+=("$name.so:linux")
  fi
  echo "built $name"
}

build loader_lab "$root/platforms/switch/test-mods/loader_lab" "-DDUSK_DIR=$root"
[ "$stress_wanted" = 1 ] && build hook_stress "$root/platforms/switch/test-mods/hook_stress" "-DDUSK_DIR=$root"
[ "$iolab_wanted" = 1 ] && build iostream_lab "$root/platforms/switch/test-mods/iostream_lab" "-DDUSK_DIR=$root"
[ "$crosssave_wanted" = 1 ] && build cross_save "$root/mods/cross_save" "-DDUSK_DIR=$root"
build template_mod "$root/mods/template_mod"
build rollandsprint "$root/mods/rollandsprint"
build cosmetics "${COSMETICS_SRC:-$root/mods/cosmetics}" "-DDUSKLIGHT_DIR=$root"
if ! wanted luau_runtime; then
  :
elif [ -n "${LUAU_SRC:-}" ] && [ -n "${LUABRIDGE_SRC:-}" ]; then
  build luau_runtime "$root/mods/luau_runtime" "-DFETCHCONTENT_SOURCE_DIR_LUAU=$LUAU_SRC" \
    "-DFETCHCONTENT_SOURCE_DIR_LUABRIDGE3=$LUABRIDGE_SRC"
else
  echo "note: LUAU_SRC/LUABRIDGE_SRC not set; luau_runtime skipped"
fi
# Randomizer: a copy of the pinned sources with platforms/switch/patches/randomizer applied (no
# thread-local storage, joined threads, seed_hash checks, dependencies pinned by commit, the
# source path size from the mapped prefix, no use of a failed data directory), packed
# as a .dusk by its own add_mod(... BUNDLE).
# H37-c6: built with devkitA64 as lib/switch-arm64/mod.so when devkitA64 is there (see the top of this file).
rando="${RANDOMIZER_SRC:-$root/mods/randomizer}"
if ! wanted randomizer; then
  :
elif [ -f "$rando/CMakeLists.txt" ]; then
  rm -rf "$out/randomizer-src"
  mkdir -p "$out/randomizer-src"
  (cd "$rando" && git archive --format=tar HEAD) | tar -x -C "$out/randomizer-src"
  # Keep `git apply` anchored in the extracted source even if OUT_DIR is under another checkout.
  git -C "$out/randomizer-src" init -q
  for patch in "$root"/platforms/switch/patches/randomizer/*.patch; do
    git -C "$out/randomizer-src" apply "$patch" || { echo "patch $(basename "$patch") does not apply" >&2; exit 1; }
  done
  # Optional pinned source mirrors keep the package build independent of network availability.
  # The names containing a dash are FetchContent's exact cache-variable names.
  rando_deps=()
  [ -z "${YAML_CPP_SRC:-}" ] || rando_deps+=("-DFETCHCONTENT_SOURCE_DIR_YAML-CPP=$YAML_CPP_SRC")
  [ -z "${BASE64PP_SRC:-}" ] || rando_deps+=("-DFETCHCONTENT_SOURCE_DIR_BASE64PP=$BASE64PP_SRC")
  [ -z "${BATTERY_EMBED_SRC:-}" ] || rando_deps+=("-DFETCHCONTENT_SOURCE_DIR_BATTERY-EMBED=$BATTERY_EMBED_SRC")
  # Patch 0004: __FILENAME__'s offset is the length of the prefix __FILE__ has after the path map.
  if [ "$rando_mode" = switch ]; then
    PATH="$devkit_bin:$PATH" cmake -S "$out/randomizer-src" -B "$out/build-randomizer" -G Ninja \
      "-DCMAKE_TOOLCHAIN_FILE=$root/platforms/switch/cmake/SwitchArm64Mod.toolchain.cmake" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POSITION_INDEPENDENT_CODE=ON "-DDUSK_SWITCH_ARM64_EXTRA_FLAGS=${maps[*]}" \
      ${FMT_SRC:+"-DFETCHCONTENT_SOURCE_DIR_FMT=$FMT_SRC"} "-DDUSKLIGHT_DIR=$root" \
      "${rando_deps[@]}" \
      -DRANDOMIZER_SOURCE_PATH_PREFIX=/h37-mods/randomizer-src > "$out/build-randomizer.log" 2>&1 &&
      PATH="$devkit_bin:$PATH" cmake --build "$out/build-randomizer" --target randomizer_package >> "$out/build-randomizer.log" 2>&1 &&
      canonical randomizer "$out/build-randomizer/mods/randomizer.dusk" switch-arm64 devkit &&
      "$py" "$here/check_switch_module.py" "$out/randomizer.dusk" &&
      echo "built randomizer (switch-arm64, devkitA64 in $devkit_bin)" ||
      { tail -20 "$out/build-randomizer.log" >&2; echo "building randomizer for switch-arm64 failed" >&2; exit 1; }
    if [ -n "${SYMGEN:-}" ]; then
      # What the NRO must export for THIS Randomizer: devkitA64's libstdc++ may name things the Ubuntu one did not.
      cp "$root/platforms/switch/mod_required_symbols.txt" "$out/mod_required_symbols.txt"
      "$py" "$here/gen_required_symbols.py" --symgen "$SYMGEN" --merge --out "$out/mod_required_symbols.txt" \
        "dev.twilitrealm.randomizer=$out/randomizer.dusk" > /dev/null
      if diff -q "$root/platforms/switch/mod_required_symbols.txt" "$out/mod_required_symbols.txt" > /dev/null; then
        echo "mod_required_symbols.txt already has everything the switch-arm64 Randomizer needs"
      else
        diff -u "$root/platforms/switch/mod_required_symbols.txt" "$out/mod_required_symbols.txt" > "$out/mod_required_symbols.diff" || true
        echo "NOTE: the switch-arm64 Randomizer needs different exports than the committed list: $out/mod_required_symbols.diff" >&2
        echo "      copy $out/mod_required_symbols.txt over platforms/switch/mod_required_symbols.txt BEFORE building the NRO" >&2
      fi
    else
      echo "note: SYMGEN not set; mod_required_symbols.txt was not compared with the switch-arm64 Randomizer" >&2
    fi
  else
    echo "WARNING: H37_ALLOW_UNSAFE_LINUX_RANDOMIZER_FOR_HOST_TESTS=1 and no devkitA64: building" >&2
    echo "         randomizer-linux-host-test.dusk (linux-aarch64). Its iostreams crash inside the game's libstdc++:" >&2
    echo "         host tests only, never a deliverable; the candidate gate does not look at this name." >&2
    cmake -S "$out/randomizer-src" -B "$out/build-randomizer" "${common[@]}" "-DDUSKLIGHT_DIR=$root" \
      "${rando_deps[@]}" \
      -DRANDOMIZER_SOURCE_PATH_PREFIX=/h37-mods/randomizer-src > "$out/build-randomizer.log" 2>&1 &&
      cmake --build "$out/build-randomizer" --target randomizer_package >> "$out/build-randomizer.log" 2>&1 &&
      canonical randomizer-linux-host-test "$out/build-randomizer/mods/randomizer.dusk" &&
      echo "built randomizer-linux-host-test (linux-aarch64: host tests only)" ||
      { tail -20 "$out/build-randomizer.log" >&2; echo "building randomizer failed" >&2; exit 1; }
  fi
else
  echo "note: randomizer sources not found; skipped"
fi

# The relocation oracle: what binutils says each relocation is, for the loader test to check
# what the loader wrote. Columns: offset, type name, symbol name (or -). addend. Made for the libraries this run built,
# with the readelf of the toolchain that built them (aarch64-none-elf-readelf for switch-arm64: nothing from the Linux
# toolchain is needed then); the oracles of libraries an earlier run left in OUT_DIR stay as they are.
for entry in ${built_sos[@]+"${built_sos[@]}"}; do
  so="$out/${entry%%:*}"
  if [ "${entry##*:}" = devkit ]; then readelf="$devkit_bin/aarch64-none-elf-readelf"; else readelf=aarch64-linux-gnu-readelf; fi
  "$readelf" -rW "$so" | awk '
    /^ *[0-9a-f]+ +[0-9a-f]+ +R_AARCH64_/ {
      off = $1; type = $3; sym = "-"; add = "0";
      if (NF >= 7) { sym = $5; add = ($6 == "-" ? "-" : "") $7 } else if (NF == 4) { add = $4 }
      sub(/@.*/, "", sym);
      print off, type, sym, add
    }' > "${so%.so}.relocs"
done
# Every package and library in OUT_DIR, whichever run (and toolchain) made it.
(cd "$out" && sha256sum -- *.dusk *.so 2>/dev/null > SHA256SUMS) || true
echo "packages in $out (SHA256SUMS)"
