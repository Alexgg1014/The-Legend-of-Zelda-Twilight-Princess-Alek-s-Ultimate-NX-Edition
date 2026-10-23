#!/usr/bin/env bash
# aleks2 (V1-rc7): host test of redirect_counter_reads (src/dusk/mods/loader/elf_loader.hpp).
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../../.."
out="${TMPDIR:-/tmp}/dusk-elf-counter-test"
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -Wall -Wextra -O1 -g -fsanitize=address,undefined -I"$root/src" "$here/elf_counter_test.cpp" \
  -o "$out/elf_counter_test"
"$out/elf_counter_test"
