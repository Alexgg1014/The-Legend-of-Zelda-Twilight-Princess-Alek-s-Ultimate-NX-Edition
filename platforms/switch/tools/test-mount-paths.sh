#!/usr/bin/env bash
# H37-c5: mount-qualified ("sdmc:/...") paths in the mod host and HTTP services (src/dusk/mount_path.hpp), host test
# with ASan+UBSan. Reproduces H37-c4's "sdmc:/switch/dusklight-h37-c4/sdmc:/switch/dusklight/mod_data/..." and checks
# the corrected path, the HTTP containment rules and ordinary host paths.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-mount-paths}"; mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -I"$root/src" \
  "$here/mount_path_test.cpp" -o "$out/mount_path_test"
"$out/mount_path_test"
