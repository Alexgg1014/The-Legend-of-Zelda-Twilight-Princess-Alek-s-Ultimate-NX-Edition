#!/usr/bin/env bash
# aleks2 (V1): reproduces, byte for byte, the libvulkan.a the Switch builds link (the Dawn/NVK fallback), from the
# public Mesa SDK zip. The zip's libvulkan.a has an incomplete archive symbol index (the Rust core/alloc/std members
# are not indexed, so GNU ld fails with ~4950 undefined core::panicking::* references). The only change is
# regenerating that index with GNU ranlib; every object member stays byte-identical.
# See docs/provenance/mesa-libvulkan.md.
#
#   reproduce-mesa-libvulkan.sh SDK_ZIP OUT_DIR [RANLIB]
# RANLIB: aarch64-none-elf-ranlib (devkitA64; GNU ranlib 2.46.0.20260210 was used). Exit 0 only if every hash matches.
set -euo pipefail
zip="${1:?the SDK zip: mesa-26.2.3-switch-unified-horizon-sdk.zip}"
out="${2:?output directory}"
ranlib="${3:-aarch64-none-elf-ranlib}"

ZIP_SHA=9b16d6dd07d01963d4c713281f3e6692cacca858195a143d7b2202afc2aeb346
BEFORE_SHA=af39adc64bf62d260fb7617f16eedc54d813848a5cbca17ebf0216e959e90100
AFTER_SHA=5d59d3b961bfcab6b93b5b882f2519dc25481318984f66d0d1ae17b48a939892
MEMBER="opt/devkitpro/portlibs/switch/lib/libvulkan.a"

sha() { sha256sum "$1" | cut -d' ' -f1; }
check() {  # what file expected
  local got; got="$(sha "$2")"
  if [ "$got" != "$3" ]; then echo "MISMATCH $1: $got (expected $3)"; exit 1; fi
  echo "ok $1: $got"
}

check "SDK zip" "$zip" "$ZIP_SHA"
mkdir -p "$out"
unzip -o -q "$zip" "$MEMBER" -d "$out"
lib="$out/$MEMBER"
check "libvulkan.a as distributed" "$lib" "$BEFORE_SHA"
"$ranlib" -D "$lib"
check "libvulkan.a with its index regenerated" "$lib" "$AFTER_SHA"
echo "reproduced: $lib (the copy the builds link: aleks2/deps/mesa-naga-2623/$MEMBER)"
