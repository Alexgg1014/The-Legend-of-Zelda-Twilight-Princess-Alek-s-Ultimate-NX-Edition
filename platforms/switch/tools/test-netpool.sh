#!/usr/bin/env bash
# aleks2 (V1-rc5): host test of the NRO's socket counters (source/switch_sockets.cpp, linked with the same
# -Wl,--wrap options as the NRO) and of the mod socket buffer sizes (src/dusk/mods/net_buffers.hpp), under
# ASan and UBSan: 200 HTTP requests in a row with a UDP socket open, counters and fds back to baseline, and
# the console's buffer pool arithmetic for rc4's and rc5's sizes (netpool_test.cpp).
# usage: test-netpool.sh [WORKDIR]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../../.."
out="${1:-/tmp/netpool-test}"
mkdir -p "$out"
c++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -pthread \
  -I"$root/src" "$here/netpool_test.cpp" "$here/../source/switch_sockets.cpp" \
  -Wl,--wrap=socket -Wl,--wrap=connect -Wl,--wrap=close -o "$out/netpool_test"
ASAN_OPTIONS=detect_leaks=1 "$out/netpool_test"
echo "test-netpool.sh: all passed"
