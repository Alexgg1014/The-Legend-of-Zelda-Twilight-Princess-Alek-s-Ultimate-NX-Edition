#!/usr/bin/env bash
# H36: builds and runs the host test for the Luau probe package (luau_probe_test.cpp): the
# real package discovery, zip reader, resource service and Luau runtime mod, on Luau 0.734
# and LuaBridge3 twilit-v1, against .dusk packages made by make_dusk.py. AddressSanitizer
# and UBSan when the compiler has them.
#
# Third-party sources are the ones the build pins. They are taken from an existing build's
# _deps (any build/*/_deps, desktop or Switch) or from these variables:
#   LUAU_SRC       luau 0.734            (luau-lang/luau, tag 0.734)
#   LUABRIDGE_SRC  LuaBridge3 twilit-v1  (TwilitRealm/LuaBridge3, tag twilit-v1)
#   MINIZ_SRC      miniz 3.0.2           (the release zip: miniz.c and miniz.h)
#   FMT_SRC        fmt 12.1.0
#   SDL3_INCLUDE   SDL3 headers (the directory that holds SDL3/)
#   BOREALIS_SRC   extern/borealis, if the submodule is not checked out
#   COSMETICS_SRC  mods/cosmetics, if the submodule is not checked out (optional)
# nlohmann/json comes from the system or a _deps copy.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
cxx="${CXX:-g++}"
cc="${CC:-gcc}"
out="${TMPDIR:-/tmp}/dusk-luau-probe-test"
mkdir -p "$out"

find_dep() {  # find_dep VAR marker candidate...
  local var="$1" marker="$2"
  shift 2
  if [ -n "${!var:-}" ]; then
    if [ -e "${!var}/$marker" ]; then echo "${!var}"; return 0; fi
    echo "$var=${!var} has no $marker" >&2
    return 1
  fi
  local candidate
  for candidate in "$@"; do
    if [ -e "$candidate/$marker" ]; then echo "$candidate"; return 0; fi
  done
  echo "missing $var: set it to a checkout that has $marker (see the top of this script)" >&2
  return 1
}
shopt -s nullglob
deps=("$root"/build/*/_deps)
luau="$(find_dep LUAU_SRC VM/src/lvm.h "${deps[@]/%//luau-src}")"
luabridge="$(find_dep LUABRIDGE_SRC Source/LuaBridge/LuaBridge.h "${deps[@]/%//luabridge3-src}")"
miniz="$(find_dep MINIZ_SRC miniz.h "${deps[@]/%//miniz-src}")"
fmt="$(find_dep FMT_SRC include/fmt/format.h "${deps[@]/%//fmt-src}")"
sdl3="$(find_dep SDL3_INCLUDE SDL3/SDL_iostream.h "${deps[@]/%//sdl3-src/include}" \
  "${deps[@]/%//sdl-src/include}" /usr/include /usr/local/include)"
borealis="$(find_dep BOREALIS_SRC include/borealis/io.hpp "$root/extern/borealis")"
json_inc=""
for candidate in /usr/include /usr/local/include "${deps[@]/%//nlohmann_json-src/include}" \
    "${deps[@]/%//json-src/include}"; do
  if [ -e "$candidate/nlohmann/json.hpp" ]; then json_inc="$candidate"; break; fi
done
if [ -z "$json_inc" ]; then echo "nlohmann/json.hpp not found" >&2; exit 1; fi
cosmetics="${COSMETICS_SRC:-$root/mods/cosmetics}"
if [ -n "${LUAU_SRC:-}" ] && [ -d "$luau/.git" ]; then
  echo "luau: $(git -C "$luau" describe --tags 2>/dev/null || git -C "$luau" rev-parse HEAD)"
fi
grep -q 'LUAU_FASTFLAG\|LUAU_FASTFLAGVARIABLE' "$luau/Common/include/Luau/Common.h"

# --- packages ---------------------------------------------------------------------------
work="$out/work"
rm -rf "$work"
mkdir -p "$work/card/mods" "$work/empty-card" "$work/broken"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
"$py" "$here/make_dusk.py" "$root/platforms/switch/test-mods/luau_probe" "$work/card/mods/luau_probe.dusk"
# The folders the Switch build stages for the built-in mods (mod.json and res/).
for mod in luau_runtime rollandsprint; do
  mkdir -p "$work/card/mods/$mod"
  cp "$root/mods/$mod/mod.json" "$work/card/mods/$mod/"
  if [ -d "$root/mods/$mod/res" ]; then cp -R "$root/mods/$mod/res" "$work/card/mods/$mod/"; fi
done
if [ -f "$cosmetics/mod.json" ]; then
  mkdir -p "$work/card/mods/cosmetics"
  cp "$cosmetics/mod.json" "$work/card/mods/cosmetics/"
  if [ -d "$cosmetics/res" ]; then cp -R "$cosmetics/res" "$work/card/mods/cosmetics/"; fi
else
  echo "cosmetics is not checked out; its staged folder is left out of the discovery check"
fi
"$py" - "$here" "$root" "$work" <<'PYEOF'
import json, pathlib, shutil, sys, zipfile
here, root, work = map(pathlib.Path, sys.argv[1:4])
sys.path.insert(0, str(here))
import make_dusk

probe = root / "platforms/switch/test-mods/luau_probe"
card = work / "card/mods"
# Damaged: the probe cut short, as a copy interrupted half way would leave it.
data = (card / "luau_probe.dusk").read_bytes()
(card / "damaged.dusk").write_bytes(data[: len(data) // 2])
# A well-formed zip whose mod.json names an invalid id.
with zipfile.ZipFile(card / "bad_manifest.dusk", "w") as z:
    z.writestr("mod.json", json.dumps({"id": "Not A Valid Id", "name": "x", "version": "1.0.0"}))
    z.writestr("res/main.luau", "print('never')\n")

manifest = json.loads((probe / "mod.json").read_text())
def package(name, files, mod_id):
    source = work / "src" / name
    if source.exists():
        shutil.rmtree(source)
    (source / "res").mkdir(parents=True)
    m = dict(manifest, id=mod_id, name=name)
    (source / "mod.json").write_text(json.dumps(m))
    for rel, text in files.items():
        path = source / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    make_dusk.pack(source, work / "broken" / f"{name}.dusk")

package("syntax_error", {"res/main.luau": "local log = require('dusklight.log')\nlog.info('a')\nlocal x = = 1\n"},
        "dev.aleks2.h36_syntax_error")
package("runtime_error", {"res/main.luau": "error('probe exploded on purpose')\n"},
        "dev.aleks2.h36_runtime_error")
package("no_main", {"res/other.luau": "return 1\n"}, "dev.aleks2.h36_no_main")
package("missing_module", {"res/main.luau": "local m = require('./no_such_module')\n"},
        "dev.aleks2.h36_missing_module")
files = {f"res/{p.name}": p.read_text() for p in (probe / "res").iterdir() if p.name != "probe.txt"}
package("missing_resource", files, "dev.aleks2.h36_missing_resource")
PYEOF

# --- build ------------------------------------------------------------------------------
san=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
if ! echo 'int main(){}' | "$cxx" -x c++ "${san[@]}" - -o "$out/san-probe" 2>/dev/null; then
  san=()
fi
gen="$out/gen"
mkdir -p "$gen/borealis"
sed -e 's/@BOREALIS_APP_DESCRIBE@/h36-test/' -e 's/@BOREALIS_APP_VERSION_CODE@/0/' \
    -e 's/@[A-Z_]*@/0/g' "$borealis/cmake/version.h.in" > "$gen/borealis/version.h"
cat > "$gen/prelude.hpp" <<'EOF'
// The game's own sources get these through its precompiled header.
#include <dolphin/types.h>
#include <optional>
#include "dusk/config_var.hpp"
namespace dusk::mods { using dusk::config::ConfigVar; }
EOF

luau_inc=(-I"$luau/Common/include" -I"$luau/Ast/include" -I"$luau/Bytecode/include"
          -I"$luau/Compiler/include" -I"$luau/VM/include")
objs="$out/obj-luau"
mkdir -p "$objs"
luau_srcs=("$luau"/Common/src/*.cpp "$luau"/Ast/src/*.cpp "$luau"/Bytecode/src/*.cpp
           "$luau"/Compiler/src/*.cpp "$luau"/VM/src/*.cpp)
# Luau is compiled once per checkout and kept; it is not what is under test.
stamp="$objs/.stamp-$(printf '%s' "$luau ${san[*]}" | cksum | cut -d' ' -f1)"
if [ ! -f "$stamp" ]; then
  rm -f "$objs"/*.o "$objs"/.stamp-*
  printf '%s\n' "${luau_srcs[@]}" | xargs -P "$(nproc 2>/dev/null || echo 4)" -I{} sh -c \
    'f="$1"; shift; n=$(echo "$f" | sed "s|.*/\([A-Za-z]*\)/src/|\1_|; s|\.cpp$|.o|"); exec "$@" -c "$f" -o "'"$objs"'/$n"' \
    _ {} "$cxx" -std=c++17 -O1 -w "${san[@]}" "${luau_inc[@]}" -I"$luau/VM/src" -I"$luau/Bytecode/src"
  touch "$stamp"
fi

common=(-std=c++20 -O1 -g -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter
        -DTARGET_PC=1 -DDUSK_CODE_MODS=1
        -I"$root/src" -I"$root/include" -I"$root/sdk/include" -I"$root/extern/aurora/include"
        -I"$root/extern/aurora/include/dolphin" -I"$root/libs/JSystem/include"
        -I"$borealis/include" -I"$gen" -I"$fmt/include" -DFMT_HEADER_ONLY=1 -I"$miniz"
        -I"$sdl3" -I"$json_inc")
host_srcs=("$root/src/dusk/mods/loader/packages.cpp" "$root/src/dusk/mods/loader/package_choice.cpp"
           "$root/src/dusk/mods/loader/manifest.cpp"
           "$root/src/dusk/mods/loader/bundle_zip.cpp" "$root/src/dusk/mods/loader/bundle_disk.cpp"
           "$root/src/dusk/mods/svc/resource.cpp"
           "$root/src/dusk/archive.cpp" "$root/src/dusk/utilities.cpp" "$root/src/dusk/io.cpp"
           "$root/platforms/switch/source/borealis_io_switch.cpp"
           "$borealis/src/update/version.cpp"
           "$here/luau_probe_test.cpp" "$here/luau_probe_test_shims.cpp")
# The runtime mod's CMakeLists.txt makes these two its precompiled headers; force-included
# here to the same effect.
runtime_srcs=("$root"/mods/luau_runtime/src/*.cpp "$root"/mods/luau_runtime/src/services/*.cpp
              "$root"/mods/luau_runtime/src/services/audio_res/*.cpp)
hobjs="$out/obj-test"
rm -rf "$hobjs"
mkdir -p "$hobjs"
pids=()
i=0
for src in "${host_srcs[@]}"; do
  # io.cpp's CRASH comes from global.h, which drags the whole game in; a trap does the same.
  "$cxx" "${common[@]}" "${san[@]}" -w -include "$gen/prelude.hpp" '-DCRASH(msg)=__builtin_trap()' \
      -c "$src" -o "$hobjs/h$i.o" &
  pids+=($!)
  i=$((i + 1))
done
for src in "${runtime_srcs[@]}"; do
  # _GLIBCXX_ASSERTIONS: libstdc++ then checks optional/vector access, which ASan cannot see.
  "$cxx" -std=c++20 -O1 -g -w "${san[@]}" -D_GLIBCXX_ASSERTIONS -I"$root/sdk/include" "${luau_inc[@]}" \
      -I"$luabridge/Source" -DLUABRIDGE_SAFE_STACK_CHECKS=0 \
      -include "$root/mods/luau_runtime/src/lua_bridge_helpers.hpp" \
      -include "$root/mods/luau_runtime/src/lua_helpers.hpp" \
      -c "$src" -o "$hobjs/r$i.o" &
  pids+=($!)
  i=$((i + 1))
done
"$cc" -O1 -w "${san[@]}" -c "$miniz/miniz.c" -o "$hobjs/miniz.o" &
pids+=($!)
for pid in "${pids[@]}"; do wait "$pid"; done
"$cxx" "${san[@]}" "$hobjs"/*.o "$objs"/*.o -o "$out/luau_probe_test"

# --- run --------------------------------------------------------------------------------
before="$(cd "$work" && find . -type f -print0 | sort -z | xargs -0 sha256sum)"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}" "$out/luau_probe_test" "$work"
after="$(cd "$work" && find . -type f -print0 | sort -z | xargs -0 sha256sum)"
if [ "$before" != "$after" ]; then
  echo "the test changed files under $work" >&2
  diff <(echo "$before") <(echo "$after") >&2 || true
  exit 1
fi
echo "luau probe package: $(sha256sum "$work/card/mods/luau_probe.dusk" | cut -d' ' -f1)"
