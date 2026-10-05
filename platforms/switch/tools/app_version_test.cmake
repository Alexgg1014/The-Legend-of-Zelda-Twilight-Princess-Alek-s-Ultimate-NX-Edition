# H38-c4: the application version rules (cmake/DuskAppVersion.cmake). No git, no build:
#   cmake -DDUSK_SOURCE_DIR=<repo root> -P app_version_test.cmake
# What Borealis hands over is faked: DESCRIBE/VERSION as DetectVersion.cmake leaves them for the case being tested.
if (NOT DUSK_SOURCE_DIR)
    message(FATAL_ERROR "pass -DDUSK_SOURCE_DIR=<repo root>")
endif ()
include("${DUSK_SOURCE_DIR}/cmake/DuskAppVersion.cmake")

set(_failures 0)
macro(expect _what _actual _expected)
    if (NOT "${_actual}" STREQUAL "${_expected}")
        message(SEND_ERROR "FAIL ${_what}: got '${_actual}', expected '${_expected}'")
        math(EXPR _failures "${_failures} + 1")
    endif ()
endmacro()

# 1. the beta override: the label the menu shows is 1.0.0-beta.1 (the menu drops the v), the number is valid
dusk_app_version_compute(r DESCRIBE "UNKNOWN-VERSION" VERSION "0.0.0.0" OVERRIDE "v1.0.0-beta.1"
        BUILD_ID "H38-c4-0123456789" BASE "1.0.0")
expect("beta describe" "${r_DESCRIBE}" "v1.0.0-beta.1")
expect("beta numeric" "${r_VERSION}" "1.0.0.0")
expect("beta short" "${r_SHORT_VERSION}" "1.0.0")
expect("beta code" "${r_VERSION_CODE}" "10000000")
expect("beta source" "${r_SOURCE}" "override")
expect("beta NACP" "${r_NACP_VERSION}" "1.0.0-beta.1")

# 2. the override beats a tag git found
dusk_app_version_compute(r DESCRIBE "v2.0.3-105" VERSION "2.0.3.105" OVERRIDE "v1.0.0-beta.1"
        BUILD_ID "H38-c4-0123456789" BASE "1.0.0")
expect("override over tag" "${r_DESCRIBE}" "v1.0.0-beta.1")
expect("override over tag source" "${r_SOURCE}" "override")

# 3. a stable tag at HEAD (no override)
dusk_app_version_compute(r DESCRIBE "v1.0.0" VERSION "1.0.0.0" OVERRIDE "" BUILD_ID "H38-c4-0123456789" BASE "1.0.0")
expect("stable describe" "${r_DESCRIBE}" "v1.0.0")
expect("stable numeric" "${r_VERSION}" "1.0.0.0")
expect("stable source" "${r_SOURCE}" "tag")
expect("stable NACP" "${r_NACP_VERSION}" "1.0.0")
expect("stable code" "${r_VERSION_CODE}" "10000000")

# 3b. commits after a tag: v1.2.3-12 is 1.2.3.12; a dirty tree keeps the tag path
dusk_app_version_compute(r DESCRIBE "v1.2.3-12" VERSION "1.2.3.12" OVERRIDE "" BUILD_ID "x-1" BASE "1.0.0")
expect("distance describe" "${r_DESCRIBE}" "v1.2.3-12")
expect("distance numeric" "${r_VERSION}" "1.2.3.12")
expect("distance code" "${r_VERSION_CODE}" "10203012")
dusk_app_version_compute(r DESCRIBE "v1.2.3-12-dirty" VERSION "1.2.3.12" OVERRIDE "" BUILD_ID "x-1" BASE "1.0.0")
expect("dirty tag source" "${r_SOURCE}" "tag")
expect("dirty tag NACP (fits 15)" "${r_NACP_VERSION}" "1.2.3-12-dirty")
dusk_app_version_compute(r DESCRIBE "v1.2.3-beta.1-12-dirty" VERSION "1.2.3.12" OVERRIDE "" BUILD_ID "x-1" BASE "1.0.0")
expect("long label NACP falls back to the number" "${r_NACP_VERSION}" "1.2.3.12")

# 4. no tag, no override: the Build ID, with a valid number
dusk_app_version_compute(r DESCRIBE "UNKNOWN-VERSION" VERSION "0.0.0.0" OVERRIDE "" BUILD_ID "H38-c4-0123456789" BASE "1.0.0")
expect("fallback describe" "${r_DESCRIBE}" "H38-c4-0123456789")
expect("fallback numeric" "${r_VERSION}" "1.0.0.0")
expect("fallback short" "${r_SHORT_VERSION}" "1.0.0")
expect("fallback source" "${r_SOURCE}" "build-id")
expect("fallback NACP (the Build ID does not fit 15)" "${r_NACP_VERSION}" "1.0.0.0")
expect("fallback code" "${r_VERSION_CODE}" "10000000")
if ("${r_DESCRIBE}" STREQUAL "UNKNOWN-VERSION" OR "${r_VERSION}" MATCHES "^0\\.0\\.0")
    message(SEND_ERROR "FAIL fallback produced UNKNOWN-VERSION or 0.0.0")
    math(EXPR _failures "${_failures} + 1")
endif ()

# 4b. a tag v0.0.0 (or Borealis' own 0.0.0.0) is not a version either
dusk_app_version_compute(r DESCRIBE "v0.0.0" VERSION "0.0.0.0" OVERRIDE "" BUILD_ID "H38-c4-abc" BASE "1.0.0")
expect("v0.0.0 tag is refused" "${r_SOURCE}" "build-id")
expect("v0.0.0 numeric" "${r_VERSION}" "1.0.0.0")

# 4c. a Build ID that is itself empty or UNKNOWN-VERSION still never reaches the menu
dusk_app_version_compute(r DESCRIBE "UNKNOWN-VERSION" VERSION "0.0.0.0" OVERRIDE "" BUILD_ID "UNKNOWN-VERSION" BASE "1.0.0")
expect("never UNKNOWN-VERSION" "${r_DESCRIBE}" "untagged-unknown")
dusk_app_version_compute(r DESCRIBE "" VERSION "" OVERRIDE "" BUILD_ID "" BASE "1.0.0")
expect("empty everything" "${r_DESCRIBE}" "untagged-unknown")
expect("empty everything numeric" "${r_VERSION}" "1.0.0.0")

# 5. another base version
dusk_app_version_compute(r DESCRIBE "UNKNOWN-VERSION" VERSION "0.0.0.0" OVERRIDE "" BUILD_ID "H39-c1-abc" BASE "1.2.0")
expect("base 1.2.0" "${r_VERSION}" "1.2.0.0")
expect("base 1.2.0 code" "${r_VERSION_CODE}" "10200000")

# 6. an override that is not a version stops the configure (checked by running a child script)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/app_version_bad.cmake"
    "include(\"${DUSK_SOURCE_DIR}/cmake/DuskAppVersion.cmake\")\n"
    "dusk_app_version_compute(r DESCRIBE x VERSION y OVERRIDE \"\${BAD}\" BUILD_ID b BASE \"1.0.0\")\n")
foreach (_bad IN ITEMS "1.0.0" "v1.0" "beta" "v1.0.0-" "v1.0.0 beta" "v0.0.0")
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DBAD=${_bad}" -P "${CMAKE_CURRENT_BINARY_DIR}/app_version_bad.cmake"
            RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
    if (_rc EQUAL 0)
        message(SEND_ERROR "FAIL override '${_bad}' was accepted")
        math(EXPR _failures "${_failures} + 1")
    endif ()
endforeach ()
foreach (_good IN ITEMS "v1.0.0" "v1.0.0-beta.1" "v2.3.4-rc.2")
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DBAD=${_good}" -P "${CMAKE_CURRENT_BINARY_DIR}/app_version_bad.cmake"
            RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
    if (NOT _rc EQUAL 0)
        message(SEND_ERROR "FAIL override '${_good}' was refused")
        math(EXPR _failures "${_failures} + 1")
    endif ()
endforeach ()

if (_failures EQUAL 0)
    message(STATUS "app version: all checks passed")
else ()
    message(FATAL_ERROR "app version: ${_failures} check(s) failed")
endif ()
