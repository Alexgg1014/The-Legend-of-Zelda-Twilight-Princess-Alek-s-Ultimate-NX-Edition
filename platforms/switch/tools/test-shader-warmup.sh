#!/usr/bin/env bash
# aleks2 (SHADERS-c1): host test for the game-free part of shader preparation
# (src/dusk/shader_warmup.cpp), with ASan/UBSan, then the Prepare Shaders test (which now
# also checks the Shader Tour's zones against the warp table).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-shader-warmup-test"
mkdir -p "$out"
cd "$out"
g++ -std=c++20 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I"$root/src" \
    "$root/src/dusk/shader_warmup.cpp" "$here/shader_warmup_test.cpp" -o shader_warmup_test
./shader_warmup_test
bash "$here/test-shader-prep.sh"
