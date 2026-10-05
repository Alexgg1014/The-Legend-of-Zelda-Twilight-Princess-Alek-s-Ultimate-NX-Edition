#!/usr/bin/env bash
# H37-c6: the loader's per-import policy (elf_runtime_policy.cpp), host test with ASan/UBSan: the libstdc++ ABI refusal
# applies to Linux-built modules only, ios_base::Init and ordinary library calls stay allowed.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-runtime-policy}"; mkdir -p "$out"
"${CC:-gcc}" -O1 -g -fsanitize=address,undefined -c "$root/src/dusk/mods/loader/elf_image_runtime.c" -o "$out/runtime.o"
"${CXX:-g++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I"$root/src" "$here/elf_runtime_policy_test.cpp" "$root/src/dusk/mods/loader/elf_runtime_policy.cpp" "$out/runtime.o" \
  -o "$out/elf_runtime_policy_test"
"$out/elf_runtime_policy_test" "$here/abi_policy_names.txt"
