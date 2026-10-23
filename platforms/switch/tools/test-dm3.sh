#!/usr/bin/env bash
# DM3: the network layer for mods, off the console.
#   1. elf_dm3_exec_test (aarch64, qemu-user): translation units, and dm3_net_lab.so run natively and through the
#      real loader with the production Linux shim tables; both must pass and report the same checks.
#   2. dm3_asan (x86-64, ASan + UBSan): the same lab module with every socket call renamed to its shim.
#   3. ws_transport_test (x86-64, ASan + UBSan): RFC 6455 units and the Switch WebSocket transport (ws://) against
#      dm3_ws_server.py.
# A local HTTP server (python3 -m http.server on 127.0.0.1) is the lab module's HTTP target; DNS_NAME (default
# example.com) is resolved only if this machine can resolve it.
#
#   test-dm3.sh [OUT_DIR]
#
# Needs aarch64-linux-gnu-g++, qemu-aarch64-static, g++ (x86-64) and python3. Run with setarch x86_64 -R
# (ASan under the container's ASLR).
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-dm3}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
loader="$root/src/dusk/mods/loader"
src_sw="$root/platforms/switch/source"
bor="$root/extern/borealis"
rc=0

http_port=18080
ws_port=18081
(cd "$out" && exec python3 -m http.server "$http_port" --bind 127.0.0.1 >"$out/http.log" 2>&1) &
http_pid=$!
python3 "$here/dm3_ws_server.py" "$ws_port" >"$out/ws-server.log" 2>&1 &
ws_pid=$!
trap 'kill $http_pid $ws_pid 2>/dev/null' EXIT
for _ in $(seq 50); do
  grep -q ready "$out/ws-server.log" 2>/dev/null && (exec 3<>/dev/tcp/127.0.0.1/$http_port) 2>/dev/null && break
  sleep 0.1
done
dns="${DNS_NAME:-example.com}"
getent hosts "$dns" >/dev/null 2>&1 || { echo "note: $dns does not resolve here; the DNS-name check is skipped"; dns=""; }

echo "== 1. aarch64 under qemu: loader + shims vs native"
aarch64-linux-gnu-gcc -O2 -fPIC -shared -Wall "$here/dm3_net_lab_module.c" -o "$out/dm3_net_lab.so" || exit 1
aarch64-linux-gnu-gcc -O1 -g -fPIE -c "$loader/elf_image_runtime.c" -o "$out/elf_image_runtime.o" || exit 1
aarch64-linux-gnu-g++ -std=gnu++20 -O1 -g -Wall -Wextra -I"$root/src" \
  "$here/elf_dm3_exec_test.cpp" "$loader/elf_loader.cpp" "$loader/elf_runtime_policy.cpp" "$loader/elf_tls.cpp" \
  "$loader/elf_tls_once.cpp" "$loader/elf_tls_aarch64.S" "$loader/glibc_abi.cpp" "$loader/glibc_net.cpp" "$loader/elf_dl.cpp" \
  "$out/elf_image_runtime.o" -o "$out/elf_dm3_exec_test" -ldl -pthread -rdynamic || exit 1
qemu-aarch64-static -L /usr/aarch64-linux-gnu "$out/elf_dm3_exec_test" "$out" 127.0.0.1 "$http_port" $dns || rc=1

echo "== 2. x86-64 ASan + UBSan: the lab module with its socket calls renamed to the shims"
san="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
gcc -O1 -g $san -c "$here/dm3_net_lab_module.c" -o "$out/lab-x64.o" || exit 1
names=$(grep -o '{"[A-Za-z_0-9]*", addr(&aleks2_glibc_' "$loader/glibc_net.cpp" | sed 's/{"\([A-Za-z_0-9]*\)".*/\1/' | grep -v '^epoll_')
used=$(nm -u "$out/lab-x64.o" | awk '{print $2}')
names=$(for n in $names; do echo "$used" | grep -qx "$n" && echo "$n"; done)
args=()
for n in $names fcntl; do
  target=$(grep -o "{\"$n\", addr(&aleks2_glibc_[a-z_0-9]*" "$loader/glibc_net.cpp" "$loader/glibc_abi.cpp" | head -1 | sed 's/.*addr(&//')
  args+=(--redefine-sym "$n=$target")
done
objcopy "${args[@]}" "$out/lab-x64.o" "$out/lab-x64-shimmed.o" || exit 1
echo "  renamed $(( ${#args[@]} / 2 )) call(s) to shims"
g++ -std=gnu++20 -O1 -g $san -I"$root/src" "$here/dm3_asan_main.cpp" "$loader/glibc_net.cpp" "$loader/glibc_abi.cpp" \
  "$out/lab-x64-shimmed.o" -o "$out/dm3_asan" -pthread || exit 1
ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 "$out/dm3_asan" "$out" 127.0.0.1 "$http_port" $dns || rc=1

echo "== 3. x86-64 ASan + UBSan: WebSocket (RFC 6455 units, the transport over ws://)"
g++ -std=gnu++20 -O1 -g $san -I"$bor/include" -I"$bor/src" -I"$src_sw" -I"$root/src" \
  "$here/ws_transport_test.cpp" "$src_sw/ws_transport_switch.cpp" "$src_sw/ws_rfc6455.cpp" -o "$out/ws_transport_test" -pthread || exit 1
ASAN_OPTIONS=detect_leaks=1 "$out/ws_transport_test" "$ws_port" || rc=1

[ $rc -eq 0 ] && echo "test-dm3.sh: all passed" || echo "test-dm3.sh: FAILED"
exit $rc
