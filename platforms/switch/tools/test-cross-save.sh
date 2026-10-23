#!/usr/bin/env bash
# aleks2 (cross-save): the cross_save mod's provider-neutral library and Firebase adapter, on the host with ASan,
# UBSan and _GLIBCXX_ASSERTIONS. No network, no accounts: the engine runs against an in-memory provider
# (mods/cross_save/tests/fakes.hpp) and the adapter against an in-process Firestore/Auth REST emulator
# (mods/cross_save/tests/fake_firestore.hpp).
#
#   test-cross-save.sh [OUT_DIR]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
mod="$root/mods/cross_save"
out="${1:-$root/build/test-cross-save}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
cxx="${CXX:-g++}"
flags=(-std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-missing-field-initializers -fsanitize=address,undefined -fno-sanitize-recover=undefined
  -D_GLIBCXX_ASSERTIONS -I"$mod")
core=("$mod/core/sha256.cpp" "$mod/core/codec.cpp" "$mod/core/json.cpp" "$mod/core/sync_engine.cpp")
cc="${CC:-gcc}"
"$cc" -O1 -g -fsanitize=address,undefined -w -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -c "$mod/third_party/miniz/miniz.c" -o "$out/miniz.o"
compat=("$mod/core/dusksave.cpp" "$mod/src/platform_io.cpp" "$mod/src/file_local_store.cpp" "$out/miniz.o")
"$cxx" "${flags[@]}" "$mod/tests/core_test.cpp" "${core[@]}" -o "$out/core_test"
"$cxx" "${flags[@]}" "$mod/tests/sync_engine_test.cpp" "${core[@]}" -o "$out/sync_engine_test"
rc=0
timeout 300 "$out/core_test" || rc=1
timeout 300 "$out/sync_engine_test" || rc=1
"$cxx" "${flags[@]}" -DMINIZ_NO_STDIO -DMINIZ_NO_TIME "$mod/tests/compat_test.cpp" "${core[@]}" "${compat[@]}"   -o "$out/compat_test"
timeout 300 "$out/compat_test" "$out/compat-work" || rc=1
if [ -f "$mod/tests/firebase_test.cpp" ]; then
  "$cxx" "${flags[@]}" "$mod/tests/firebase_test.cpp" "${core[@]}" "$mod/firebase/firebase_backend.cpp" \
    -o "$out/firebase_test"
  timeout 300 "$out/firebase_test" || rc=1
fi
exit $rc
