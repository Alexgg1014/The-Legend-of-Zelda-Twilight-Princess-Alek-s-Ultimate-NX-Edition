#!/usr/bin/env bash
# H37-c6: SwitchArm64Mod.toolchain.cmake makes ONE link command. Configures a minimal MODULE with the toolchain (stand-in
# aarch64-none-elf-* wrappers around clang) and reads the rule CMake wrote into rules.ninja: a single compiler invocation
# carrying the flags, SONAME, -o <target>, crtbegin, the objects (or response file), the libraries and crtend, in that order.
# H37-c6 on Windows had three commands joined by && ("g++ <flags>", "-o x", "crtbegin @rsp crtend"): ld said "no input files".
#   TOOLCHAIN=<file> test-toolchain-link-rule.sh [OUT]     (TOOLCHAIN: to try another version of the file)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
t="${1:-$root/build/test-toolchain-link-rule}"; rm -rf "$t"; mkdir -p "$t/bin" "$t/lib" "$t/src"; t="$(cd "$t" && pwd)"
tc="${TOOLCHAIN:-$root/platforms/switch/cmake/SwitchArm64Mod.toolchain.cmake}"
command -v clang++ >/dev/null && command -v ninja >/dev/null || { echo "SKIPPED: needs clang++ and ninja"; exit 0; }
touch "$t/lib/crtbeginS.o" "$t/lib/crtendS.o"
for pair in gcc:clang g++:clang++; do
  tool="${pair%%:*}" real="${pair##*:}"
  printf '#!/bin/bash\nfor a in "$@"; do case "$a" in -print-file-name=*) f="${a#-print-file-name=}"; if [ -e "%s/lib/$f" ]; then echo "%s/lib/$f"; else echo "$f"; fi; exit 0;; esac; done\nexec %s --target=aarch64-linux-gnu "$@"\n' \
    "$t" "$t" "$real" > "$t/bin/aarch64-none-elf-$tool"
done
for tool in strip readelf; do printf '#!/bin/sh\nexit 0\n' > "$t/bin/aarch64-none-elf-$tool"; done
chmod +x "$t"/bin/*
printf 'int a(){return 1;}\n' > "$t/src/a.cpp"; printf 'int b(){return 2;}\n' > "$t/src/b.cpp"
cat > "$t/src/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(linkrule CXX)
add_library(extra STATIC b.cpp)
add_library(m MODULE a.cpp)
target_link_libraries(m PRIVATE extra)
EOF
PATH="$t/bin:$PATH" cmake -S "$t/src" -B "$t/build" -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$tc" > "$t/configure.log" 2>&1 ||
  { cat "$t/configure.log"; echo "FAIL: configure"; exit 1; }
python3 - "$t/build/CMakeFiles/rules.ninja" "$t" <<'PYEOF'
import re, sys
text = open(sys.argv[1]).read()
m = re.search(r"^rule CXX_MODULE_LIBRARY_LINKER__m_\w*\n(?:[ \t]+.*\n)+", text, re.M)
if not m:
    sys.exit("FAIL: no module link rule in rules.ninja")
command = re.search(r"^\s+command = (.*)$", m.group(0), re.M).group(1)
print("link rule:", command)
parts = [p.strip() for p in command.split(" && ")]
real = [p for p in parts if p not in ("$PRE_LINK", "$POST_BUILD", "cd .") and not p.startswith(("cmd /C", "cd ."))]
bad = []
if len(real) != 1:
    bad.append(f"{len(real)} commands, expected 1: {real}")
one = real[0] if real else command
if one.count("aarch64-none-elf-g++") != 1:
    bad.append("the compiler is invoked more than once or not at all")
for needle in ("-o $TARGET_FILE", "crtbeginS.o", "crtendS.o", "$SONAME_FLAG$SONAME", "$LINK_FLAGS", "$LANGUAGE_COMPILE_FLAGS"):
    if needle not in one:
        bad.append(f"missing {needle!r} in the single command")
objs = [one.find(x) for x in ("$in", "@$RSP_FILE") if x in one]
if not objs:
    bad.append("no objects ($in) or response file (@$RSP_FILE) in the command")
else:
    o = min(objs)
    order = [one.find("-o $TARGET_FILE"), one.find("crtbeginS.o"), o, one.find("$LINK_PATH"), one.rfind("crtendS.o")]
    if not (order[0] < order[1] < order[2] < order[4]):
        bad.append(f"order is not -o, crtbegin, objects, crtend: {order}")
    if "$LINK_LIBRARIES" in one and not (o < one.find("$LINK_LIBRARIES") < one.rfind("crtendS.o")):
        bad.append("the libraries are not between the objects and crtend")
if bad:
    sys.exit("FAIL: " + "; ".join(bad))
print("link rule: one compiler invocation with -o, crtbegin, objects, libraries, crtend: ok")
PYEOF
grep -q -- '-Wl,--eh-frame-hdr' "$t/build/CMakeFiles/rules.ninja" || {
  echo "FAIL: switch-arm64 link flags do not request PT_GNU_EH_FRAME" >&2; exit 1;
}
echo "link flags: --eh-frame-hdr: ok"
