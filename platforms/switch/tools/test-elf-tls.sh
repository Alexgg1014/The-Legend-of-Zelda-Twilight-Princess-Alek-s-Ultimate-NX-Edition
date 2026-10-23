#!/usr/bin/env bash
# TLS-lab: dynamic thread-local storage for modules mapped by the native mod loader
# (src/dusk/mods/loader/elf_tls.cpp, elf_tls_aarch64.S), really executed under qemu-aarch64.
#
#   test-elf-tls.sh [OUT_DIR] [CATALOG_LIB...]
#
# Needs aarch64-linux-gnu-g++ and qemu-aarch64-static (Debian: g++-aarch64-linux-gnu, qemu-user-static).
# 1. Builds elf_tls_test_module.cpp three ways: TLSDESC (the default, and every catalog mod's),
#    -mtls-dialect=trad and -ftls-model=initial-exec; checks with readelf that each has the
#    relocations it is meant to have.
# 2. Builds elf_tls_exec_test.cpp with the loader, the runtime policy and the TLS runtime, and runs it:
#    see that file for what it checks. CATALOG_LIB... (published mods' linux-aarch64 mod.so) are
#    planned with TLS off and on and mapped without running.
set -euo pipefail
failed=""
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-elf-tls}"
shift || true
mkdir -p "$out"
out="$(cd "$out" && pwd)"
cxx=aarch64-linux-gnu-g++
command -v "$cxx" >/dev/null && command -v qemu-aarch64-static >/dev/null || {
  echo "test-elf-tls.sh needs aarch64-linux-gnu-g++ and qemu-aarch64-static"; exit 1; }

echo "== test modules"
module=("$here/elf_tls_test_module.cpp" "$here/elf_tls_test_regs.S")
"$cxx" -std=c++20 -O2 -fPIC -shared "${module[@]}" -o "$out/tls_desc.so"
"$cxx" -std=c++20 -O2 -fPIC -shared -mtls-dialect=trad "${module[@]}" -o "$out/tls_trad.so"
"$cxx" -std=c++20 -O2 -fPIC -shared -ftls-model=initial-exec "${module[@]}" -o "$out/tls_ie.so"
relocs() { aarch64-linux-gnu-readelf -rW "$1" | grep -oE 'R_AARCH64_TLS[A-Z_0-9]*' | sort | uniq -c | tr '\n' ' '; }
for so in tls_desc tls_trad tls_ie; do
  echo "  $so: $(relocs "$out/$so.so")$(aarch64-linux-gnu-readelf -dW "$out/$so.so" | grep -q STATIC_TLS && echo '[DF_STATIC_TLS]')"
done
relocs "$out/tls_desc.so" | grep -q TLSDESC || { echo "FAIL: tls_desc.so has no TLSDESC"; exit 1; }
relocs "$out/tls_trad.so" | grep -q TLS_DTPMOD64 || { echo "FAIL: tls_trad.so has no DTPMOD64"; exit 1; }
relocs "$out/tls_ie.so" | grep -q TLS_TPREL64 || { echo "FAIL: tls_ie.so has no TPREL64"; exit 1; }
# std::call_once as glibc's libstdc++ inlines it: the two once thread-locals, __once_proxy and pthread_once.
# (Read once into a variable: under pipefail, grep -q quitting early can SIGPIPE readelf and fail the check.)
dynsyms="$(aarch64-linux-gnu-readelf --dyn-syms -W "$out/tls_desc.so")"
for sym in _ZSt11__once_call _ZSt15__once_callable __once_proxy pthread_once; do
  grep -qE " UND $sym(@|$)" <<<"$dynsyms" ||
    { echo "FAIL: tls_desc.so does not import $sym"; exit 1; }
done

echo "== Python gate (elf_audit.verdict) against the same modules, TLS off and on"
python3 "$here/tls_gate_test.py" "$out" || failed="$failed tls_gate_test"

loader="$root/src/dusk/mods/loader"
san=(-std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined -D_GLIBCXX_ASSERTIONS)
echo "== host test (x86-64, ASan, UBSan, _GLIBCXX_ASSERTIONS): words written, corruptions, mutations"
"${CC:-gcc}" -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -c "$loader/elf_image_runtime.c" -o "$out/elf_image_runtime.host.o"
"${CXX:-g++}" "${san[@]}" -I"$root/src" "$here/elf_tls_host_test.cpp" "$loader/elf_loader.cpp" "$loader/elf_runtime_policy.cpp" \
  "$out/elf_image_runtime.host.o" -o "$out/elf_tls_host_test"
"$out/elf_tls_host_test" "$out" "$@" || failed="$failed elf_tls_host_test"
# The loader's own host test, without the mod packages (they need the mod SDK build): its policy
# checks still run against the changed loader.
echo "== elf_loader_test (policy only, no packages)"
"${CXX:-g++}" "${san[@]}" -I"$root/src" "$here/elf_loader_test.cpp" "$loader/elf_loader.cpp" "$loader/elf_runtime_policy.cpp" \
  "$out/elf_image_runtime.host.o" -o "$out/elf_loader_test"
mkdir -p "$out/no-packages"
"$out/elf_loader_test" "$out/no-packages" || failed="$failed elf_loader_test"

echo "== execution test (aarch64, qemu-user)"
aarch64-linux-gnu-gcc -O1 -g -Wall -Wextra -Werror -fPIE -c "$loader/elf_image_runtime.c" -o "$out/elf_image_runtime.o"
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -I"$root/src" \
  "$here/elf_tls_exec_test.cpp" "$loader/elf_loader.cpp" "$loader/elf_runtime_policy.cpp" "$loader/elf_tls.cpp" \
  "$loader/elf_tls_once.cpp" "$loader/elf_tls_aarch64.S" "$out/elf_image_runtime.o" -o "$out/elf_tls_exec_test" \
  -ldl -pthread -rdynamic
qemu-aarch64-static -L /usr/aarch64-linux-gnu "$out/elf_tls_exec_test" "$out" "$@" || failed="$failed elf_tls_exec_test"

# Injected resource faults, each in a fresh process (the key is created once per process). No key left
# is refused before the module runs and loads on retry; a failure inside a TLS access must stop the
# process after logging its reason, never go on with a made-up address.
echo "== injected faults"
qemu-aarch64-static -L /usr/aarch64-linux-gnu "$out/elf_tls_exec_test" --fault=key "$out" || failed="$failed fault-key"
for fault in setspecific alloc; do
  set +e
  qemu-aarch64-static -L /usr/aarch64-linux-gnu "$out/elf_tls_exec_test" --fault=$fault "$out" > "$out/fault-$fault.txt" 2>&1
  rc=$?
  set -e
  if [ $rc -ne 0 ] && [ $rc -ne 3 ] && grep -q "aleks2 TLS: " "$out/fault-$fault.txt"; then
    echo "fault $fault: stopped (exit $rc) after: $(grep 'aleks2 TLS: ' "$out/fault-$fault.txt")"
  else
    echo "FAIL fault $fault (exit $rc):"; cat "$out/fault-$fault.txt"; failed="$failed fault-$fault"
  fi
done
[ -z "$failed" ] || { echo "FAILED:$failed"; exit 1; }
echo "test-elf-tls.sh: all passed"
