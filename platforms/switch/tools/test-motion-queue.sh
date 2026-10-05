#!/usr/bin/env bash
# H38-c3: Aurora's sensor state-queue bookkeeping (extern/aurora/lib/motion_queue.hpp): only new states, delta_time unit
# and fallback, gaps (overflow, counter restart, disconnected state), nothing handed out twice. Host, no libnx.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-motion-queue-test"
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -Wall -Wextra -O1 -g -I"$root/extern/aurora/lib" "$here/motion_queue_test.cpp" -o "$out/motion_queue_test"
"$out/motion_queue_test"
