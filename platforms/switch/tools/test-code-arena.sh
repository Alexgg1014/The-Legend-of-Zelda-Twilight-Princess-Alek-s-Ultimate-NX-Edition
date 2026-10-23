#!/usr/bin/env bash
# H37: the Switch hook backend's code arenas (code_arena_test.c says what is checked).
#
#   test-code-arena.sh [OUT_DIR]
#
# Part A, the arena manager against a fake kernel limited to ten CodeMemory objects, runs on the
# host with ASan and UBSan. Part B, funchook v1.1.3 -- patched by cmake/PatchFunchook.cmake, as
# the build patches it -- hooking 1155 real functions and code_arena_shapes.S through the arenas
# (DM2-a: several to a page, the 12-byte patch), runs on aarch64 under
# qemu-user when aarch64-linux-gnu-gcc and qemu-aarch64-static exist. It fetches funchook and
# capstone 4.0.2 (funchook's own pin) with git unless FUNCHOOK_SRC / CAPSTONE_SRC point at them.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-code-arena}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
src="$root/platforms/switch/source"
cc="${CC:-gcc}"

echo "== part A: the arena manager (host, ASan + UBSan)"
"$cc" -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$src" "$here/code_arena_test.c" "$src/code_arena.c" -o "$out/code_arena_test"
"$out/code_arena_test"

if ! command -v aarch64-linux-gnu-gcc >/dev/null || ! command -v qemu-aarch64-static >/dev/null; then
  echo "== part B skipped: needs aarch64-linux-gnu-gcc and qemu-aarch64-static"
  exit 0
fi
echo "== part B: funchook through the arenas (aarch64, qemu-user)"
fetch() {  # fetch DIR URL TAG
  [ -d "$1/.git" ] || [ -f "$1/CMakeLists.txt" ] || git clone -q --depth 1 --branch "$3" "$2" "$1" 2>/dev/null
}
funchook="${FUNCHOOK_SRC:-$out/funchook-src}"
capstone="${CAPSTONE_SRC:-$out/capstone-src}"
fetch "$funchook" https://github.com/kubo/funchook.git v1.1.3
fetch "$capstone" https://github.com/aquynh/capstone.git 4.0.2
# The build patches a fresh copy; so does this.
rm -rf "$out/funchook"
cp -R "$funchook" "$out/funchook"
cmake -DSOURCE_DIR="$out/funchook" -P "$root/cmake/PatchFunchook.cmake"
grep -q funchook_switch_writable "$out/funchook/src/funchook.c" || { echo "patch not applied"; exit 1; }
grep -q "aleks2 DM2-a" "$out/funchook/src/funchook_arm64.c" || { echo "DM2-a patch not applied"; exit 1; }
# Idempotent: a second run over the patched tree changes nothing.
cp -R "$out/funchook" "$out/funchook-again"
cmake -DSOURCE_DIR="$out/funchook-again" -P "$root/cmake/PatchFunchook.cmake"
diff -r -x .git "$out/funchook" "$out/funchook-again" >/dev/null || { echo "patch is not idempotent"; exit 1; }
rm -rf "$out/funchook-again"
if [ ! -f "$out/capstone-build/libcapstone.a" ]; then
  cmake -S "$capstone" -B "$out/capstone-build" -G Ninja -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCAPSTONE_BUILD_SHARED=OFF \
    -DCAPSTONE_BUILD_TESTS=OFF -DCAPSTONE_BUILD_CSTOOL=OFF -DCAPSTONE_ARCHITECTURE_DEFAULT=OFF \
    -DCAPSTONE_ARM64_SUPPORT=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > "$out/capstone.log" 2>&1
  cmake --build "$out/capstone-build" >> "$out/capstone.log" 2>&1
fi
mkdir -p "$out/gen"
printf '#define DISASM_CAPSTONE 1\n#define SIZEOF_VOID_P 8\n' > "$out/gen/config.h"
# 1200 targets and their hooks: target_i(x) = 3x + i; hook_i(x) = original(x) + 1000.
python3 - "$out/gen/targets.c" <<'PYEOF'
import sys
n = 1200
lines = ["int (*g_orig[%d])(int);" % n]
for i in range(n):
    lines.append("__attribute__((noinline)) int target_%d(int x) { volatile int v = x; v = v * 3 + %d; return v; }" % (i, i))
    lines.append("int hook_%d(int x) { return g_orig[%d](x) + 1000; }" % (i, i))
lines.append("int (*const k_targets[])(int) = {%s};" % ", ".join("target_%d" % i for i in range(n)))
lines.append("int (*const k_hooks[])(int) = {%s};" % ", ".join("hook_%d" % i for i in range(n)))
lines.append("const int k_target_count = %d;" % n)
open(sys.argv[1], "w").write("\n".join(lines) + "\n")
PYEOF
fh="$out/funchook/src"
cflags=(-std=gnu11 -O1 -g -Wall -I"$src" -I"$out/gen" -I"$out/funchook/include" -I"$fh" -I"$capstone/include")
objs=()
for f in "$fh/funchook.c" "$fh/funchook_arm64.c" "$fh/disasm_capstone.c"; do
  # __SWITCH__: funchook's patched page writes; nothing in these files needs libnx.
  aarch64-linux-gnu-gcc "${cflags[@]}" -w -D__SWITCH__ -c "$f" -o "$out/$(basename "$f" .c).o"
  objs+=("$out/$(basename "$f" .c).o")
done
# __SWITCH__ here too: funchook_arena_glue.c must see funchook's entry as funchook.c does (DM2-a's
# patch_size and 3-instruction arrays exist only there).
aarch64-linux-gnu-gcc "${cflags[@]}" -D__SWITCH__ -DCODE_ARENA_TEST_FUNCHOOK -static \
  "$here/code_arena_test.c" "$here/code_arena_funchook_test.c" "$src/code_arena.c" \
  "$src/funchook_arena_glue.c" "$out/gen/targets.c" "$here/code_arena_shapes.S" "${objs[@]}" \
  "$out/capstone-build/libcapstone.a" \
  -lpthread -o "$out/code_arena_test_aarch64"
qemu-aarch64-static "$out/code_arena_test_aarch64"
