#!/usr/bin/env bash
# H37-c6: which fmt a mod gets (cmake/ModSDK.cmake, FEATURES fmt). With -DFETCHCONTENT_SOURCE_DIR_FMT=<fmt 12.1.0> the
# explicit source is used and find_package(fmt) is not even run -- a fmt installed on the host (MinGW's, found through the
# compiler's own prefix) never reaches the module. Without it the old order stands: the host package first. Host fmt and
# explicit source here are stand-ins that say which one was configured.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
t="${1:-$root/build/test-mod-sdk-fmt}"; rm -rf "$t"; mkdir -p "$t"; t="$(cd "$t" && pwd)"
failures=0
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }

# a "wrong" fmt installed on the host
h="$t/host/lib/cmake/fmt"; mkdir -p "$h" "$t/host/include-WRONG"
cat > "$h/fmt-config.cmake" <<EOF
message(STATUS "HOST-FMT-CONFIG-LOADED")
add_library(fmt::fmt-header-only INTERFACE IMPORTED GLOBAL)
set_target_properties(fmt::fmt-header-only PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "$t/host/include-WRONG")
EOF
cat > "$h/fmt-config-version.cmake" <<'EOF'
set(PACKAGE_VERSION "11.0.0")
set(PACKAGE_VERSION_COMPATIBLE TRUE)
EOF
# the explicit source: fmt 12.1.0
mkdir -p "$t/fmt-src/include"
cat > "$t/fmt-src/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(fmt CXX)
message(STATUS "EXPLICIT-FMT-SOURCE-USED")
add_library(fmt-header-only INTERFACE)
add_library(fmt::fmt-header-only ALIAS fmt-header-only)
target_include_directories(fmt-header-only INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
EOF
mkdir -p "$t/proj"
cat > "$t/proj/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.25)
project(fmtpick CXX)
include("$root/cmake/ModSDK.cmake")
file(WRITE "\${CMAKE_BINARY_DIR}/a.cpp" "int a(){return 1;}\n")
add_library(m MODULE "\${CMAKE_BINARY_DIR}/a.cpp")
_mod_add_fmt(m)
get_target_property(_aliased fmt::fmt-header-only ALIASED_TARGET)
if (_aliased)
    get_target_property(_inc \${_aliased} INTERFACE_INCLUDE_DIRECTORIES)
else ()
    get_target_property(_inc fmt::fmt-header-only INTERFACE_INCLUDE_DIRECTORIES)
endif ()
message(STATUS "FMT-INCLUDES=\${_inc}")
get_target_property(_libs m LINK_LIBRARIES)
message(STATUS "MODULE-LINKS=\${_libs}")
EOF
configure() {  # configure LABEL [cmake -D args]
  local label="$1"; shift
  cmake -S "$t/proj" -B "$t/build-$label" -G Ninja "-DCMAKE_PREFIX_PATH=$t/host" "$@" > "$t/$label.log" 2>&1 || { cat "$t/$label.log"; fail "$label: configure failed"; }
}

configure explicit "-DFETCHCONTENT_SOURCE_DIR_FMT=$t/fmt-src" -DFETCHCONTENT_FULLY_DISCONNECTED=ON
grep -q "EXPLICIT-FMT-SOURCE-USED" "$t/explicit.log" || fail "the explicit source was not used"
grep -q "HOST-FMT-CONFIG-LOADED" "$t/explicit.log" && fail "find_package(fmt) ran although a source was given"
inc="$(grep 'FMT-INCLUDES=' "$t/explicit.log" | sed 's/.*FMT-INCLUDES=//')"
[ "$inc" = "$t/fmt-src/include" ] || fail "include directories are '$inc', expected only $t/fmt-src/include"
grep -q "include-WRONG" "$t/explicit.log" && fail "the host fmt's include directory reached the module"
grep -q "MODULE-LINKS=.*fmt::fmt-header-only" "$t/explicit.log" || fail "the module does not link fmt::fmt-header-only"

# without the variable: the host package is still found first (and is the one used)
configure host
grep -q "HOST-FMT-CONFIG-LOADED" "$t/host.log" && grep -q "FMT-INCLUDES=$t/host/include-WRONG" "$t/host.log" ||
  fail "without an explicit source the host fmt should still be tried first"
grep -q "EXPLICIT-FMT-SOURCE-USED" "$t/host.log" && fail "the explicit source was used without being asked"

[ "$failures" -eq 0 ] && echo "mod sdk fmt: ok" || { echo "$failures check(s) failed"; exit 1; }
