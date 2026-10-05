#!/usr/bin/env bash
# H37-c6: the switch-arm64 package platform is explicit and checked (cmake/ModSDK.cmake), and the module toolchain
# (platforms/switch/cmake/SwitchArm64Mod.toolchain.cmake) is what it says: devkitA64 tools found, nothing of the C++
# runtime linked in, crtbegin/crtend around the objects, PIC, no outline atomics. Runs without devkitA64: the toolchain
# file is read against stand-in aarch64-none-elf-* tools. Whether devkitA64's g++ really links such a module is what the
# real build (build-h37-test-mods.sh) shows.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-mod-sdk-platform}"; rm -rf "$out"; mkdir -p "$out/bin" "$out/lib"
out="$(cd "$out" && pwd)"
sdk="$root/cmake/ModSDK.cmake"; tc="$root/platforms/switch/cmake/SwitchArm64Mod.toolchain.cmake"
failures=0
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }
flat() { tr '\n' ' ' | tr -s ' '; }  # cmake wraps its error text
run() { cmake -DCASE="$1" -DSDK="$sdk" -DTOOLCHAIN="$tc" -P "$here/mod_sdk_platform_test.cmake" 2>&1 | flat; }

# stand-in devkitA64 tools
for t in gcc g++ strip readelf; do
  printf '#!/bin/sh\nfor a in "$@"; do case "$a" in -print-file-name=*) f="${a#-print-file-name=}"; if [ -e "%s/lib/$f" ]; then echo "%s/lib/$f"; else echo "$f"; fi;; esac; done\n' \
    "$out" "$out" > "$out/bin/aarch64-none-elf-$t"
  chmod +x "$out/bin/aarch64-none-elf-$t"
done
touch "$out/lib/crtbeginS.o" "$out/lib/crtendS.o"

o="$(run inferred-linux)";            [[ "$o" == *"platform=linux-aarch64 name=mod.so"* ]] || fail "inference changed: $o"
o="$(run explicit-without-toolchain)" && fail "switch-arm64 accepted without the toolchain" || [[ "$o" == *"SwitchArm64Mod.toolchain.cmake"* ]] || fail "wrong reason: $o"
o="$(run explicit-wrong-arch)" && fail "switch-arm64 accepted on x86_64" || [[ "$o" == *"needs an AArch64 target"* ]] || fail "wrong reason: $o"
o="$(run explicit-unknown)" && fail "an unknown platform name was accepted" || [[ "$o" == *"is not supported"* ]] || fail "wrong reason: $o"
o="$(run toolchain-without-platform)" && fail "the toolchain without a platform was accepted" || [[ "$o" == *"DUSK_MOD_LIB_PLATFORM is not set"* ]] || fail "wrong reason: $o"
o="$(run nintendo-switch-system)" && fail "a NintendoSwitch system name was turned into a platform" || [[ "$o" == *"is a console, not a package platform"* ]] || fail "wrong reason: $o"
o="$(DEVKITPRO="$out/none" PATH="$out/bin:$PATH" run toolchain)"
[[ "$o" == *"platform=switch-arm64 name=mod.so"* ]] || fail "the toolchain did not select switch-arm64: $o"

# the toolchain file itself
cat > "$out/read-toolchain.cmake" <<EOF
include("$tc")
message("CXX=\${CMAKE_CXX_COMPILER}")
message("STRIP=\${CMAKE_STRIP}")
message("CXXFLAGS=\${CMAKE_CXX_FLAGS_INIT}")
message("MODFLAGS=\${CMAKE_MODULE_LINKER_FLAGS_INIT}")
message("RULE=\${CMAKE_CXX_CREATE_SHARED_MODULE}")
message("PLATFORM=\${DUSK_MOD_LIB_PLATFORM} PROC=\${CMAKE_SYSTEM_PROCESSOR}")
EOF
o="$(PATH="$out/bin:$PATH" cmake -DDUSK_SWITCH_ARM64_EXTRA_FLAGS=-ffile-prefix-map=/a=/b -P "$out/read-toolchain.cmake" 2>&1)"
[[ "$o" == *"-ffile-prefix-map=/a=/b"* ]] || fail "extra flags are not passed on: $o"
[[ "$o" == *"CXX=$out/bin/aarch64-none-elf-g++"* ]] || fail "devkitA64's g++ was not selected: $o"
[[ "$o" == *"STRIP=$out/bin/aarch64-none-elf-strip"* ]] || fail "devkitA64's strip was not selected"
[[ "$o" == *"-fPIC"* && "$o" == *"-mno-outline-atomics"* && "$o" == *"-march=armv8-a+crc+crypto"* ]] || fail "code generation flags: $o"
flags="$(grep '^MODFLAGS=' <<<"$o")"
[[ "$flags" == *"-nostartfiles"* && "$flags" == *"-nodefaultlibs"* ]] || fail "the runtime libraries are not excluded: $flags"
rule="$(grep '^RULE=' <<<"$o")"
[[ "$rule" == *"crtbeginS.o"*"<OBJECTS>"*"crtendS.o"* ]] || fail "crtbegin/crtend are not around the objects: $rule"
[[ "$rule" != *"-lstdc++"* && "$rule" != *"-lgcc"* && "$rule" != *"-lc "* && "$rule" != *"-static"* ]] || fail "a runtime library is linked: $rule"
[[ "$o" == *"PLATFORM=switch-arm64 PROC=aarch64"* ]] || fail "platform/processor: $o"
# no crtbegin at all: refused with the reason
rm "$out/lib/crtbeginS.o"
o="$(PATH="$out/bin:$PATH" cmake -P "$out/read-toolchain.cmake" 2>&1 | flat)" && fail "a toolchain without crtbegin was accepted" || [[ "$o" == *"no crtbegin/crtend"* ]] || fail "wrong reason: $o"
# no devkitA64: refused
o="$(PATH=/usr/bin:/bin DEVKITPRO="$out/none" cmake -P "$out/read-toolchain.cmake" 2>&1 | flat)" && fail "no devkitA64, no error" || [[ "$o" == *"aarch64-none-elf-gcc"* ]] || fail "wrong reason: $o"

[ "$failures" -eq 0 ] && echo "mod sdk platform: ok" || { echo "$failures check(s) failed"; exit 1; }
