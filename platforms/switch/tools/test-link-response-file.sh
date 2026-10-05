#!/usr/bin/env bash
# H37-c1: the linker roots of platforms/switch/cmake/ModAbiExports.cmake go to GNU ld through one
# response file, not as hundreds of options (H37-c1's Windows link failed: "La línea de comandos es
# demasiado larga", ~17 500 characters through cmd.exe, which takes 8191).
#
#   test-link-response-file.sh [OUT_DIR]
#
# A real CMake + Ninja project, in a directory whose name has spaces, includes the very
# ModAbiExports.cmake the Switch build includes, with copies of the real lists (mod_abi_pinned.txt,
# mod_required_symbols.txt, mod_abi_surface.txt) plus two test roots -- a C function and a C++
# function, defined only in a static library nothing references -- and links an AArch64 PIE with
# GNU ld (aarch64-linux-gnu-g++; the host g++ if that is missing). Then:
#   1. the link command has no --undefined option left, only one -Wl,@<file>;
#   2. it is shorter than cmd.exe's 8191 characters;
#   3. the file holds exactly the expected roots -- derived here independently from the lists --
#      in the same order, with no duplicate and none lost (321 from the real lists);
#   4. the C and C++ roots were pulled out of the archive and exported, mangled name intact;
#   5. all of it with spaces in the source and build paths;
#   6. on Linux (this test);
#   7. a build with nothing changed relinks nothing; adding a root to mod_required_symbols.txt
#      reconfigures, rewrites the file and relinks, and the new root is in the image;
#   8. the post-link check (symgen manifest + check_symbol_manifest.py, as
#      SymbolManifestSwitch.cmake runs them) still reads the image and reports the roots exported.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-link-response-file}"
rm -rf "$out"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
src="$out/source dir with spaces"
build="$out/build dir with spaces"
mkdir -p "$src"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
command -v ninja >/dev/null || { echo "ninja is required"; exit 2; }
cxx=aarch64-linux-gnu-g++
cc=aarch64-linux-gnu-gcc
command -v "$cxx" >/dev/null || { cxx=g++; cc=gcc; }

failures=0
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }

# The real lists, plus two roots defined only in an archive.
cp "$root/platforms/switch/mod_abi_pinned.txt" "$root/platforms/switch/mod_abi_surface.txt" \
   "$root/platforms/switch/mod_required_symbols.txt" "$src/"
printf 'dev.test.link import libc aleks2_link_test_c\ndev.test.link import libstdc++ _ZN6aleks24test9link_rootEi\n' \
  >> "$src/mod_required_symbols.txt"
# One function per file: each is its own archive member, pulled in only when named.
echo 'int aleks2_link_test_c(void) { return 37; }' > "$src/roots.c"
echo 'int aleks2_link_test_second(void) { return 38; }' > "$src/roots2.c"
cat > "$src/roots_cpp.cpp" <<'EOF'
namespace aleks2::test { int link_root(int x) { return x + 1; } }
EOF
echo 'int main() { return 0; }' > "$src/main.cpp"
cat > "$src/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.25)
project(link_rsp_test C CXX)
add_library(roots STATIC roots.c roots2.c roots_cpp.cpp)
add_executable(dusklight main.cpp)
target_compile_options(dusklight PRIVATE -g1)
target_link_options(dusklight PRIVATE -pie -Wl,--build-id=sha1)
set_target_properties(dusklight PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(dusklight PRIVATE roots)
set(ALEKS2_MOD_ABI ON)
include("$root/platforms/switch/cmake/ModAbiExports.cmake")
EOF

cmake -S "$src" -B "$build" -G Ninja -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
  -DCMAKE_BUILD_TYPE=Release > "$out/configure.log" 2>&1 || { tail -20 "$out/configure.log"; exit 1; }
cmake --build "$build" > "$out/build.log" 2>&1 || { tail -20 "$out/build.log"; exit 1; }
elf="$build/dusklight"
rsp="$build/mod_abi_undefined.rsp"

# 1, 2: the link command, as Ninja will run it.
"$py" - "$build/build.ninja" <<'PYEOF' || failures=$((failures + 1))
import re, sys
text = open(sys.argv[1]).read()
# The link edge of dusklight and its LINK_FLAGS variable.
edge = re.search(r"^build dusklight: CXX_EXECUTABLE_LINKER__dusklight_\S*.*?(?=^build |\Z)", text, re.S | re.M)
if not edge:
    print("FAIL: no link edge for dusklight in build.ninja")
    sys.exit(1)
flags = re.search(r"^\s+LINK_FLAGS = (.*)$", edge.group(0), re.M)
link = flags.group(1) if flags else ""
bad = 0
if "--undefined" in edge.group(0):
    print("FAIL: --undefined options are still on the link command")
    bad += 1
if link.count("-Wl,@") != 1:
    print(f"FAIL: expected one -Wl,@<file> in LINK_FLAGS, found {link.count('-Wl,@')}")
    bad += 1
length = len(edge.group(0))
print(f"link edge: {length} characters (LINK_FLAGS {len(link)}), one response file")
if length >= 8191:
    print("FAIL: the link command would not fit cmd.exe's 8191 characters")
    bad += 1
sys.exit(1 if bad else 0)
PYEOF

# 3: the file against the lists, derived here and not by the CMake code under test.
"$py" - "$root/platforms/switch/cmake/ModAbiExports.cmake" "$src/mod_abi_pinned.txt" \
    "$src/mod_required_symbols.txt" "$root/platforms/switch/mod_required_symbols.txt" "$rsp" <<'PYEOF' || failures=$((failures + 1))
import re, sys
cmake, pinned, required, real_required, rsp = sys.argv[1:6]
builtin = re.search(r"set\(_aleks2_mod_abi_symbols\s+(.*?)\)", open(cmake).read(), re.S).group(1).split()
def expected(required_file):
    names = list(builtin)
    names += [l.strip() for l in open(pinned) if l.strip() and not l.startswith("#")]
    for line in open(required_file):
        parts = line.split()
        if len(parts) == 4 and parts[1] == "import" and parts[2] != "game":
            names.append(parts[3])
    seen, ordered = set(), []
    for n in names:
        if n not in seen:
            seen.add(n)
            ordered.append(n)
    return ordered
lines = open(rsp).read().splitlines()
got = [l[len("--undefined="):] for l in lines if l.startswith("--undefined=")]
bad = 0
if len(got) != len(lines):
    print("FAIL: the response file has lines that are not --undefined=NAME")
    bad += 1
want = expected(required)
if got != want:
    print(f"FAIL: response file roots differ from the lists ({len(got)} against {len(want)})")
    bad += 1
if len(set(got)) != len(got):
    print("FAIL: duplicate roots")
    bad += 1
real = expected(real_required)
print(f"response file: {len(got)} roots ({len(real)} from the real lists + 2 test roots), "
      f"same order as the lists, no duplicates")
if len(got) != len(real) + 2:
    bad += 1
    print("FAIL: test roots not added as expected")
sys.exit(1 if bad else 0)
PYEOF

# 4: pulled from the archive and exported, mangled name intact.
nm="aarch64-linux-gnu-nm"; readelf="aarch64-linux-gnu-readelf"
command -v "$nm" >/dev/null || { nm=nm; readelf=readelf; }
# (Listings go to files first: grep -q closing a pipe early would fail it under pipefail.)
"$nm" "$elf" > "$out/nm.txt"
"$readelf" --dyn-syms -W "$elf" > "$out/dynsym.txt"
for sym in aleks2_link_test_c _ZN6aleks24test9link_rootEi; do
  grep -q " T $sym\$" "$out/nm.txt" || fail "$sym was not pulled into the image"
  grep -q " $sym\$" "$out/dynsym.txt" || fail "$sym is not exported"
done
if grep -q " T aleks2_link_test_second\$" "$out/nm.txt"; then
  fail "a symbol nobody named was pulled in"
fi
echo "image: the C and C++ roots were pulled from the archive and exported; the unnamed one was not"

# 7: no change, no relink; a new root reconfigures, rewrites the file, relinks.
before="$(stat -c %Y.%N "$elf" 2>/dev/null || stat -f %m "$elf")"
rsp_before="$(cat "$rsp")"
ninja -C "$build" -n > "$out/noop.txt"
if grep -q "Linking" "$out/noop.txt"; then
  fail "a build with nothing changed would relink"
fi
sleep 1
echo 'dev.test.link import libc aleks2_link_test_second' >> "$src/mod_required_symbols.txt"
cmake --build "$build" > "$out/rebuild.log" 2>&1 || { tail -20 "$out/rebuild.log"; exit 1; }
grep -q "Re-running CMake" "$out/rebuild.log" || fail "editing mod_required_symbols.txt did not reconfigure"
grep -q "Linking" "$out/rebuild.log" || fail "editing mod_required_symbols.txt did not relink"
grep -qx -- "--undefined=aleks2_link_test_second" "$rsp" || fail "the new root is not in the response file"
[ "$(cat "$rsp")" != "$rsp_before" ] || fail "the response file did not change"
"$nm" "$elf" > "$out/nm2.txt"
grep -q " T aleks2_link_test_second\$" "$out/nm2.txt" || fail "the new root is not in the relinked image"
after="$(stat -c %Y.%N "$elf" 2>/dev/null || stat -f %m "$elf")"
[ "$before" != "$after" ] || fail "the image was not relinked"

# 8: the post-link check on this image, as SymbolManifestSwitch.cmake runs it.
symgen="${SYMGEN:-}"
if [ -n "$symgen" ] && [ -x "$symgen" ]; then
  "$symgen" -C "$src" manifest --binary "$elf" -o "$out/dusklight.symdb.raw" --no-compress >/dev/null 2>&1
  printf 'dev.test.link import libc aleks2_link_test_c\ndev.test.link import libstdc++ _ZN6aleks24test9link_rootEi\ndev.test.link import libc aleks2_link_test_second\n' \
    > "$out/required.txt"
  "$py" "$here/check_symbol_manifest.py" --elf "$elf" --symdb "$out/dusklight.symdb.raw" \
    --required "$out/required.txt" --report "$out/dusklight.symbols.json" --strict > "$out/check.txt" 2>&1 ||
    fail "check_symbol_manifest.py on the image: $(cat "$out/check.txt")"
  grep -q "dev.test.link: compatible: imports 3/3" "$out/check.txt" || fail "the roots are not reported exported"
  [ -s "$out/dusklight.symbols.json" ] || fail "no dusklight.symbols.json"
  echo "post-link check: $(grep dev.test.link "$out/check.txt")"
else
  echo "SYMGEN not set: post-link check skipped (test-symbol-manifest.sh covers it)"
fi

if [ "$failures" -gt 0 ]; then
  echo "$failures check(s) failed"
  exit 1
fi
echo "test-link-response-file: all checks passed"
