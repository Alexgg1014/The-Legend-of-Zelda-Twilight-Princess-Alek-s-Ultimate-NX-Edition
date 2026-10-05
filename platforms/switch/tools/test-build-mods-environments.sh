#!/usr/bin/env bash
# H37-c6: build-h37-test-mods.sh on a machine whose toolchains live apart -- aarch64-linux-gnu-* in Docker/Linux, devkitA64's
# aarch64-none-elf-* on Windows -- run twice on one OUT_DIR. Stand-in cmake and compilers (no real build); a clang-made
# shared object plays the module. Shows: Randomizer alone with devkitA64 asks for nothing from the Linux toolchain and uses
# devkitA64's readelf; Linux packages still require theirs; Randomizer without devkitA64 fails before building anything unless
# the host-test option is named, and then never as randomizer.dusk; the second run keeps the first run's packages and
# SHA256SUMS lists both groups; a path with a space is refused before configuring, with the SUBST commands.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
t="${1:-$root/build/test-build-mods-environments}"; rm -rf "$t"; mkdir -p "$t"; t="$(cd "$t" && pwd)"
command -v clang >/dev/null && command -v ld.lld >/dev/null || { echo "SKIPPED: needs clang and ld.lld for the stand-in module"; exit 0; }
[ -f "$root/mods/randomizer/CMakeLists.txt" ] || { echo "SKIPPED: the Randomizer sources (mods/randomizer) are not checked out"; exit 0; }
py=python3; command -v "$py" >/dev/null 2>&1 || py=python
failures=0
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }

# --- the stand-in module and tools ---------------------------------------------------------------
cat > "$t/mod.c" <<'EOF'
extern char f0(void) __asm__("_Znwm");
int mod_meta, mod_ctx; int mod_initialize(void){return 0;} int mod_update(void){return 0;} int mod_shutdown(void){return 0;}
int use(void){ return f0(); }
EOF
clang --target=aarch64-linux-gnu -O1 -fPIC -ffreestanding -c "$t/mod.c" -o "$t/mod.o"
ld.lld -shared -o "$t/mod.so" "$t/mod.o"

mkdir -p "$t/base" "$t/bin" "$t/linuxbin" "$t/devbin"
for tool in bash sh git python3 python awk sha256sum dirname basename mkdir cp find grep cat rm sed diff tr head sort mv env uname cut wc ls tee date readlink tar gzip; do
  p="$(command -v "$tool" 2>/dev/null || true)"; [ -n "$p" ] && ln -sf "$p" "$t/base/$tool"
done
cat > "$t/bin/cmake" <<EOF
#!/bin/bash
echo "cmake \$*" >> "\$LOG"
args=("\$@")
for ((i = 0; i < \${#args[@]}; i++)); do
  case "\${args[i]}" in
    -B) build="\${args[i+1]}" ;;
    --build) built="\${args[i+1]}" ;;
    -DCMAKE_TOOLCHAIN_FILE=*) toolchain="\${args[i]#*=}" ;;
  esac
done
if [ -n "\${build:-}" ]; then mkdir -p "\$build"; echo "\${toolchain:-}" > "\$build/.toolchain"; exit 0; fi
if [ -n "\${built:-}" ]; then
  name="\$(basename "\$built")"; name="\${name#build-}"
  platform=linux-aarch64; grep -q SwitchArm64Mod "\$built/.toolchain" && platform=switch-arm64
  mkdir -p "\$built/mods"
  $py - "\$built/mods/\$name.dusk" "\$platform" "$t/mod.so" "\$name" <<'PYEOF'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w") as z:
    z.writestr("mod.json", '{"id":"dev.test.' + sys.argv[4] + '","version":"1"}')
    z.writestr(f"lib/{sys.argv[2]}/mod.so", open(sys.argv[3], "rb").read())
PYEOF
  exit 0
fi
exit 0
EOF
chmod +x "$t/bin/cmake"
for tool in g++ gcc; do
  printf '#!/bin/bash\necho "LINUXTOOL %s $*" >> "$LOG"\nexit 0\n' "$tool" > "$t/linuxbin/aarch64-linux-gnu-$tool"
  printf '#!/bin/bash\necho "DEVKITTOOL %s $*" >> "$LOG"\nexit 0\n' "$tool" > "$t/devbin/aarch64-none-elf-$tool"
done
printf '#!/bin/bash\necho "READELF linux $*" >> "$LOG"\nexit 0\n' > "$t/linuxbin/aarch64-linux-gnu-readelf"
printf '#!/bin/bash\necho "READELF devkit $*" >> "$LOG"\nexit 0\n' > "$t/devbin/aarch64-none-elf-readelf"
chmod +x "$t"/linuxbin/* "$t"/devbin/*

run() {  # run LABEL OUT "linux|nolinux" "devkit|nodevkit" [ENV=VALUE ...]; sets rc, $t/LABEL.log (tools) and .err
  local label="$1" out="$2" lin="$3" dev="$4"; shift 4
  local path="$t/bin:$t/base"
  [ "$lin" = linux ] && path="$t/linuxbin:$path"
  [ "$dev" = devkit ] && path="$t/devbin:$path"
  : > "$t/$label.log"
  set +e
  env -i HOME="$t" PATH="$path" LOG="$t/$label.log" "$@" bash "$here/build-h37-test-mods.sh" "$out" > "$t/$label.out" 2> "$t/$label.err"
  rc=$?
  set -e
}
zip_libs() { "$py" -c "import sys, zipfile; print(' '.join(n for n in zipfile.ZipFile(sys.argv[1]).namelist() if n.startswith('lib/')))" "$1"; }

# 1. The Linux step (Docker): no devkitA64 on the machine.
out="$t/out"
run linux-step "$out" linux nodevkit H37_TEST_MODS="loader_lab template_mod"
[ "$rc" = 0 ] || fail "linux step failed ($rc): $(cat "$t/linux-step.err")"
[ -f "$out/loader_lab.dusk" ] && [ -f "$out/loader_lab.so" ] && [ -f "$out/aarch64-linux.cmake" ] || fail "linux step: packages or toolchain file missing"
grep -q "READELF linux" "$t/linux-step.log" && ! grep -q "DEVKIT\|READELF devkit" "$t/linux-step.log" || fail "linux step used the wrong readelf: $(cat "$t/linux-step.log")"
before="$(sha256sum "$out/loader_lab.dusk" "$out/loader_lab.so" "$out/loader_lab.relocs" | cut -d' ' -f1 | tr '\n' ' ')"
[ "$(zip_libs "$out/loader_lab.dusk")" = "lib/linux-aarch64/mod.so" ] || fail "linux package platform"

# 2. The devkitA64 step (Windows), same OUT_DIR, no Linux tools on the machine at all.
run devkit-step "$out" nolinux devkit H37_TEST_MODS=randomizer
[ "$rc" = 0 ] || fail "devkit step failed ($rc): $(cat "$t/devkit-step.err")"
[ "$(zip_libs "$out/randomizer.dusk")" = "lib/switch-arm64/mod.so" ] || fail "randomizer.dusk must hold exactly lib/switch-arm64/mod.so: $(zip_libs "$out/randomizer.dusk")"
! grep -q "LINUXTOOL\|READELF linux" "$t/devkit-step.log" || fail "the devkit step touched the Linux toolchain: $(cat "$t/devkit-step.log")"
grep -q "READELF devkit .*randomizer.so" "$t/devkit-step.log" || fail "the oracle did not use aarch64-none-elf-readelf: $(cat "$t/devkit-step.log")"
grep -q "SwitchArm64Mod.toolchain.cmake" "$t/devkit-step.log" && ! grep -q "aarch64-linux.cmake" "$t/devkit-step.log" || fail "wrong toolchain file: $(cat "$t/devkit-step.log")"
[ -f "$out/randomizer.relocs" ] || fail "no oracle for randomizer"
after="$(sha256sum "$out/loader_lab.dusk" "$out/loader_lab.so" "$out/loader_lab.relocs" | cut -d' ' -f1 | tr '\n' ' ')"
[ "$before" = "$after" ] || fail "the second run changed the first run's packages"
for f in loader_lab.dusk loader_lab.so template_mod.so randomizer.dusk randomizer.so; do
  [ -e "$out/$f" ] || [ "$f" = template_mod.so ] || fail "$f is gone"
  if [ -e "$out/$f" ]; then grep -q " $f\$" "$out/SHA256SUMS" || fail "SHA256SUMS lacks $f: $(cat "$out/SHA256SUMS")"; fi
done
grep -q "randomizer-linux" "$out/SHA256SUMS" && fail "a host-test randomizer is listed"

# 3. Linux packages still need their toolchain.
run need-linux "$t/out3" nolinux devkit H37_TEST_MODS=loader_lab
[ "$rc" = 2 ] && grep -q "aarch64-linux-gnu-g++ is required" "$t/need-linux.err" || fail "linux packages without their toolchain: rc=$rc $(cat "$t/need-linux.err")"
run full-nolinux "$t/out4" nolinux devkit
[ "$rc" = 2 ] && grep -q "aarch64-linux-gnu-g++ is required" "$t/full-nolinux.err" || fail "a full run without Linux tools: rc=$rc"

# 4. Randomizer without devkitA64.
run no-devkit "$t/out5" linux nodevkit H37_TEST_MODS=randomizer
[ "$rc" = 2 ] && grep -q "randomizer needs devkitA64" "$t/no-devkit.err" || fail "Randomizer without devkitA64 must fail: rc=$rc"
! compgen -G "$t/out5/randomizer*" > /dev/null && [ ! -d "$t/out5/build-randomizer" ] && ! grep -q "^cmake" "$t/no-devkit.log" ||
  fail "something was built before the refusal: $(ls "$t/out5")"
run no-devkit-full "$t/out6" linux nodevkit
[ "$rc" = 2 ] && grep -q "nothing was built" "$t/no-devkit-full.err" && ! grep -q "^cmake" "$t/no-devkit-full.log" || fail "a full run without devkitA64 built things first"
# ... unless the host-test option is named; then under a name that is never the deliverable.
run host-test "$t/out7" linux nodevkit H37_TEST_MODS=randomizer H37_ALLOW_UNSAFE_LINUX_RANDOMIZER_FOR_HOST_TESTS=1
[ "$rc" = 0 ] || fail "the explicit host-test fallback failed: $(cat "$t/host-test.err")"
[ -f "$t/out7/randomizer-linux-host-test.dusk" ] && [ ! -e "$t/out7/randomizer.dusk" ] && [ ! -e "$t/out7/randomizer.so" ] ||
  fail "the fallback must not produce randomizer.dusk: $(ls "$t/out7")"
[ "$(zip_libs "$t/out7/randomizer-linux-host-test.dusk")" = "lib/linux-aarch64/mod.so" ] || fail "fallback platform"
grep -q "host tests only" "$t/host-test.err" || fail "the fallback did not warn"
run host-test-nolinux "$t/out8" nolinux nodevkit H37_TEST_MODS=randomizer H37_ALLOW_UNSAFE_LINUX_RANDOMIZER_FOR_HOST_TESTS=1
[ "$rc" = 2 ] || fail "the fallback needs the Linux toolchain: rc=$rc"
# the option does not change the devkit flow
run host-test-devkit "$t/out9" nolinux devkit H37_TEST_MODS=randomizer H37_ALLOW_UNSAFE_LINUX_RANDOMIZER_FOR_HOST_TESTS=1
[ "$rc" = 0 ] && [ -f "$t/out9/randomizer.dusk" ] && [ ! -e "$t/out9/randomizer-linux-host-test.dusk" ] || fail "option + devkit must still build switch-arm64"

# 5. A path with a space: refused before configuring, with the SUBST commands.
run spaces "$t/with space/out" nolinux devkit H37_TEST_MODS=randomizer
[ "$rc" = 2 ] && grep -q "subst X:" "$t/spaces.err" && grep -q "My Projects" "$t/spaces.err" && ! grep -q "^cmake" "$t/spaces.log" ||
  fail "a path with a space was not refused up front: rc=$rc $(cat "$t/spaces.err")"

[ "$failures" -eq 0 ] && echo "build environments: ok" || { echo "$failures check(s) failed"; exit 1; }
