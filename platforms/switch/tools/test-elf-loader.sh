#!/usr/bin/env bash
# H37: the native mod loader (src/dusk/mods/loader/elf_loader.cpp) against real mod libraries.
#
#   test-elf-loader.sh [OUT_DIR]
#
# 1. Builds the packages with build-h37-test-mods.sh (or reuses H37_MODS_DIR if set): the lab
#    module, template_mod, rollandsprint, cosmetics, luau_runtime and randomizer, all
#    linux-aarch64 through the mod SDK, plus a readelf relocation oracle per library.
# 2. Host test (x86-64, ASan + UBSan + _GLIBCXX_ASSERTIONS): every relocation against readelf,
#    page permissions, constructors, exports, the runtime policy, 36+ targeted corruptions and
#    12000 deterministic mutations; writes a Dry Run report per library and package ABI.
# 3. Execution test (aarch64 under qemu-user, if aarch64-linux-gnu-g++ and qemu-aarch64-static
#    exist): the loader maps the libraries into real memory with the final permissions,
#    registers .eh_frame, runs constructors and calls the mod entry points -- C++ exceptions,
#    static constructors, virtual calls, and the whole Luau runtime running a script.
# 4. Two readers agree: elf_audit.py's verdict (Python, its own ELF reader) and the loader's
#    Dry Run, per library and per package ABI.
# 5. H37-c2, the image runtime (aarch64 under qemu-user): elf_loader_image_test.cpp, linked like
#    the NRO -- static PIE, libgcc/libgcc_eh/libm inside with their hidden functions local, an
#    explicit --dynamic-list, src/dusk/mods/loader/elf_image_runtime.c -- resolves a module's
#    imports the way switch_native_loader.cpp does. Without the image runtime table
#    _Unwind_Resume is missing (H37-c1's Dry Run); with it, it is the image's own. The module
#    (elf_image_test_module.cpp) then runs its constructor once and has exceptions cross its
#    frames every way, through its cleanups and _Unwind_Resume; the control, the same module with
#    its .eh_frame not registered, must end in std::terminate. Every H37 package's hidden-runtime
#    imports resolve through the table, and check_symbol_manifest.py's reader finds the same
#    table in the linked ELF, each entry at the image's own function.
#
# Host-only: Horizon's code memory (switch_elf_memory.cpp) is not exercised here.
set -euo pipefail
# H37-c6: these host tests run on Linux without devkitA64; the linux-aarch64 Randomizer they need is a host-test-only
# package (randomizer-linux-host-test.dusk), which build-h37-test-mods.sh builds only when asked.
export H37_ALLOW_UNSAFE_LINUX_RANDOMIZER_FOR_HOST_TESTS=1
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-elf-loader}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
cxx="${CXX:-g++}"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python

mods="${H37_MODS_DIR:-$out/mods}"
if [ -z "${H37_MODS_DIR:-}" ]; then
  "$here/build-h37-test-mods.sh" "$mods"
fi
for lib in loader_lab template_mod rollandsprint; do
  [ -f "$mods/$lib.so" ] || { echo "missing $mods/$lib.so"; exit 1; }
done

sources=("$root/src/dusk/mods/loader/elf_loader.cpp" "$root/src/dusk/mods/loader/elf_runtime_policy.cpp")
runtime_c="$root/src/dusk/mods/loader/elf_image_runtime.c"  # C: compiled on its own, per target

echo "== host test (ASan, UBSan, _GLIBCXX_ASSERTIONS)"
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -D_GLIBCXX_ASSERTIONS -I"$root/src" -c "$here/elf_loader_test.cpp" -o "$out/elf_loader_test.o"
"${CC:-gcc}" -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -c "$runtime_c" -o "$out/elf_image_runtime.host.o"
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -D_GLIBCXX_ASSERTIONS -I"$root/src" "$out/elf_loader_test.o" "${sources[@]}" "$out/elf_image_runtime.host.o" \
  -o "$out/elf_loader_test"
rm -rf "$out/reports"
"$out/elf_loader_test" "$mods" "$out/reports"

if command -v aarch64-linux-gnu-g++ >/dev/null && command -v qemu-aarch64-static >/dev/null; then
  echo "== execution test (aarch64, qemu-user)"
  aarch64-linux-gnu-gcc -O1 -g -Wall -Wextra -Werror -fPIE -c "$runtime_c" -o "$out/elf_image_runtime.aarch64.o"
  aarch64-linux-gnu-g++ -std=c++20 -O1 -g -Wall -Wextra -I"$root/src" -I"$root/sdk/include" \
    "$here/elf_loader_exec_test.cpp" "${sources[@]}" "$out/elf_image_runtime.aarch64.o" -o "$out/elf_loader_exec_test" \
    -ldl -shared-libgcc
  qemu-aarch64-static -L /usr/aarch64-linux-gnu "$out/elf_loader_exec_test" "$mods"
else
  echo "== execution test skipped: needs aarch64-linux-gnu-g++ and qemu-aarch64-static"
fi

if command -v aarch64-linux-gnu-g++ >/dev/null && command -v qemu-aarch64-static >/dev/null; then
  echo "== image runtime: a static image like the NRO, its unwinder, a module's exceptions (aarch64, qemu-user)"
  img="$out/image-runtime"
  mkdir -p "$img"
  aarch64-linux-gnu-g++ -std=c++20 -O2 -fPIC -shared -fno-stack-protector "$here/elf_image_test_module.cpp" \
    -o "$img/module.so"
  # The surface this image exports: what the module imports from the C++ runtime, and
  # _Unwind_Resume too -- listed, as H37-c1 listed it, and still not exported, being hidden.
  printf '{\n  game_throw;\n  __cxa_allocate_exception; __cxa_free_exception; __cxa_throw;\n  __cxa_begin_catch; __cxa_end_catch; __gxx_personality_v0;\n  _ZTIi; _ZTISt13runtime_error; _ZNSt13runtime_errorC1EPKc; _ZNSt13runtime_errorD1Ev;\n  _Unwind_Resume;\n};\n' \
    > "$img/exports.list"
  aarch64-linux-gnu-g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fPIE -static-pie -I"$root/src" \
    "$here/elf_loader_image_test.cpp" "${sources[@]}" "$root/src/dusk/mods/loader/image_symbols.cpp" \
    "$out/elf_image_runtime.aarch64.o" "-Wl,--dynamic-list=$img/exports.list" \
    -Wl,--undefined=_ZNSt13runtime_errorC1EPKc -Wl,--undefined=_ZTISt13runtime_error -o "$img/image_test"
  packages=()
  for lib in loader_lab cosmetics randomizer rollandsprint; do
    [ -f "$mods/$lib.so" ] && packages+=("$mods/$lib.so")
  done
  qemu-aarch64-static "$img/image_test" "$img/module.so" "${packages[@]}"
  set +e
  (qemu-aarch64-static "$img/image_test" --unregistered "$img/module.so"; ) > "$img/unregistered.txt" 2>&1
  control=$?
  set -e
  if [ "$control" -eq 0 ]; then
    echo "FAIL: with its .eh_frame unregistered the module's exception was still handled"; exit 1
  fi
  echo "control: without __register_frame the same exception ends the process (exit $control)"
  "$py" - "$here" "$img/image_test" <<'PYEOF'
import sys
sys.path.insert(0, sys.argv[1])
import elf_audit, image_surface
elf = elf_audit.Elf(open(sys.argv[2], "rb").read(), "image_test")
symtab = image_surface.symbols(elf)
entries, problems = image_surface.image_runtime(elf, symtab, image_surface.relative_relocations(elf))
exported = image_surface.exports(elf)
local = {n for n, _v, _s, t, b, sh in symtab if t == image_surface.STT_FUNC and b == 0 and sh}
problems += [e["name"] + " is exported" for e in entries if e["name"] in exported]
hidden = sorted(e["name"] for e in entries if e["name"] in local)
print(f"check_symbol_manifest's reader: {len(entries)} table entries, each at the image's own function; "
      f"local (hidden) in this image: {', '.join(hidden)}")
if problems or "_Unwind_Resume" not in hidden:
    print("FAIL: " + "; ".join(problems or ["_Unwind_Resume is not local here"]))
    sys.exit(1)
PYEOF
else
  echo "== image runtime test skipped: needs aarch64-linux-gnu-g++ and qemu-aarch64-static"
fi

echo "== elf_audit.py against the loader's Dry Run"
"$py" "$here/elf_audit.py" --json "$out/audit.json" "$mods"/*.so > "$out/audit.txt"
"$py" - "$out/audit.json" "$out/reports" <<'PYEOF'
import json, pathlib, sys
audit = json.load(open(sys.argv[1]))
reports = pathlib.Path(sys.argv[2])
bad = 0
for lib in audit:
    stem = pathlib.Path(lib["file"]).stem
    v = lib["verdict"]
    for abi, says in (("linux", v["linux_compat"]["loadable_if_game_defines_imports"]),
                      ("switch", v["switch_package"]["loadable"])):
        report = json.loads((reports / f"{stem}.{abi}.json").read_text())
        loads = report["ok"]
        why = report.get("error") or (report.get("problems") or [{}])[0].get("error", "")
        agree = loads == says
        bad += not agree
        print(f"{'ok  ' if agree else 'FAIL'} {stem:14} {abi:6} audit {'loads' if says else 'refused':7} "
              f"loader {'loads' if loads else 'refused (' + str(why) + ')'}")
sys.exit(1 if bad else 0)
PYEOF
echo "test-elf-loader: all stages passed"
