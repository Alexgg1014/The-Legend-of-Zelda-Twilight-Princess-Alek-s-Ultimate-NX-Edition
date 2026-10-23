#!/usr/bin/env bash
# DM2-d: the general glibc ABI layer (src/dusk/mods/loader/glibc_abi.cpp) and the synthetic dl_iterate_phdr
# (elf_dl.cpp), executed under qemu-aarch64. See elf_dm2d_exec_test.cpp.
#
#   test-dm2d.sh [OUT_DIR] [TWILIT_SO]
#
# Needs aarch64-linux-gnu-g++ and qemu-aarch64-static. The newlib side of the table is checked apart, with
# devkitA64 (aleks2\tools-dm2d\check-newlib-table.sh), where it can be compiled.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-dm2d}"
shift || true
mkdir -p "$out"
out="$(cd "$out" && pwd)"
cxx=aarch64-linux-gnu-g++
loader="$root/src/dusk/mods/loader"

echo "== test modules"
"$cxx" -std=c++20 -O2 -fPIC -shared "$here/dm2d_dl_module.cpp" -o "$out/dm2d_dl.so"
"$cxx" -std=c++20 -O2 -fPIC -shared -static-libgcc "$here/dm2d_dl_module.cpp" -o "$out/dm2d_unwind.so"
# The second must carry its own unwinder (libgcc_eh): _Unwind_Backtrace defined, dl_iterate_phdr imported.
# libgcc 12+ against glibc 2.35+ asks _dl_find_object first and dl_iterate_phdr only as a fallback.
aarch64-linux-gnu-nm -D --undefined-only "$out/dm2d_unwind.so" | grep -Eq " (dl_iterate_phdr|_dl_find_object)" ||
  { echo "FAIL: dm2d_unwind.so imports neither dl_iterate_phdr nor _dl_find_object"; exit 1; }
aarch64-linux-gnu-nm -D --undefined-only "$out/dm2d_unwind.so" | grep -E " (dl_iterate_phdr|_dl_find_object)" | sed "s/^/  imports: /"
aarch64-linux-gnu-nm "$out/dm2d_unwind.so" 2>/dev/null | grep -q "_Unwind_Find_FDE" ||
  aarch64-linux-gnu-objdump -d "$out/dm2d_unwind.so" | grep -q "<_Unwind_Find_FDE" ||
  echo "  note: _Unwind_Find_FDE not named in dm2d_unwind.so (stripped local); the depth check decides"

echo "== execution test (aarch64, qemu-user)"
aarch64-linux-gnu-gcc -O1 -g -fPIE -c "$loader/elf_image_runtime.c" -o "$out/elf_image_runtime.o"
"$cxx" -std=gnu++20 -O1 -g -Wall -Wextra -I"$root/src" \
  "$here/elf_dm2d_exec_test.cpp" "$loader/elf_loader.cpp" "$loader/elf_runtime_policy.cpp" "$loader/elf_tls.cpp" \
  "$loader/elf_tls_once.cpp" "$loader/elf_tls_aarch64.S" "$loader/glibc_abi.cpp" "$loader/elf_dl.cpp" \
  "$out/elf_image_runtime.o" -o "$out/elf_dm2d_exec_test" -ldl -pthread -rdynamic
qemu-aarch64-static -L /usr/aarch64-linux-gnu "$out/elf_dm2d_exec_test" "$out" "$@"
echo "test-dm2d.sh: all passed"
