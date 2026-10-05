# aleks2 (H38-c4): the application version of a Switch build, never "UNKNOWN-VERSION" and never 0.0.0.
#
# Borealis (extern/borealis/cmake/DetectVersion.cmake) derives the version from `git describe --match "v*"`. A checkout
# without tags (the clean-publish repository has none) makes it answer UNKNOWN-VERSION with the numeric 0.0.0.0, which is
# what the menu and the NRO's NACP showed from H34 on. This file runs right after borealis_detect_version() and settles it
# in this order:
#
#   1. DUSK_APP_VERSION_OVERRIDE, when set: "v1.0.0" or "v1.0.0-beta.1" (anything else stops the configure). The H38
#      preset sets it to v1.0.0-beta.1. Numeric version a.b.c.0.
#   2. a version tag git found (v1.0.0, or v1.0.0-12 for twelve commits after it): kept as Borealis made it.
#   3. otherwise the Build ID, "<ALEKS2_BUILD_TAG>-<commit>" (H38-c4-0123456789): that is what the menu shows, with the
#      numeric version DUSK_APP_BASE_VERSION.0 (1.0.0.0), so the NACP and the project version stay valid.
#
# Outputs: BOREALIS_APP_DESCRIBE / _VERSION / _SHORT_VERSION / _VERSION_CODE (the names the rest of the build reads), and
# DUSK_APP_VERSION_SOURCE (override | tag | build-id) and DUSK_NACP_VERSION (the NACP's display version: the label without
# its leading v when it fits the 15 characters the NACP holds, else the numeric version).
#
# dusk_app_version_compute() is pure (no git, no cache): platforms/switch/tools/test-app-version.sh runs it on the host.
include_guard(GLOBAL)

set(DUSK_APP_VERSION_REGEX "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z][0-9A-Za-z.]*)?$")

function(dusk_app_version_compute _prefix)
    cmake_parse_arguments(_a "" "DESCRIBE;VERSION;OVERRIDE;BUILD_ID;BASE" "" ${ARGN})
    set(_source "")
    set(_describe "")
    set(_version "")
    set(_short "")
    set(_major 0)
    set(_minor 0)
    set(_patch 0)

    if (_a_OVERRIDE)
        if (NOT _a_OVERRIDE MATCHES "${DUSK_APP_VERSION_REGEX}")
            message(FATAL_ERROR "DUSK_APP_VERSION_OVERRIDE '${_a_OVERRIDE}' is not a version: use v1.0.0 or v1.0.0-beta.1")
        endif ()
        set(_major ${CMAKE_MATCH_1})
        set(_minor ${CMAKE_MATCH_2})
        set(_patch ${CMAKE_MATCH_3})
        if (_major EQUAL 0 AND _minor EQUAL 0 AND _patch EQUAL 0)
            message(FATAL_ERROR "DUSK_APP_VERSION_OVERRIDE '${_a_OVERRIDE}' is 0.0.0")
        endif ()
        set(_source "override")
        set(_describe "${_a_OVERRIDE}")
        set(_short "${_major}.${_minor}.${_patch}")
        set(_version "${_short}.0")
    elseif (_a_DESCRIBE MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)([-+].*)?$" AND _a_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)\\.[0-9]+$"
            AND NOT _a_VERSION MATCHES "^0\\.0\\.0\\.")
        # Both regexes above matched: the second one is the numeric version, so its groups are the ones to read.
        set(_source "tag")
        set(_describe "${_a_DESCRIBE}")
        set(_version "${_a_VERSION}")
        string(REGEX REPLACE "^([0-9]+)\\.([0-9]+)\\.([0-9]+)\\.[0-9]+$" "\\1;\\2;\\3" _parts "${_version}")
        list(GET _parts 0 _major)
        list(GET _parts 1 _minor)
        list(GET _parts 2 _patch)
        set(_short "${_major}.${_minor}.${_patch}")
    else ()
        if (NOT "${_a_BASE}" MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" OR "${_a_BASE}" STREQUAL "0.0.0")
            message(FATAL_ERROR "DUSK_APP_BASE_VERSION '${_a_BASE}' is not a valid a.b.c version")
        endif ()
        set(_major ${CMAKE_MATCH_1})
        set(_minor ${CMAKE_MATCH_2})
        set(_patch ${CMAKE_MATCH_3})
        set(_source "build-id")
        set(_describe "${_a_BUILD_ID}")
        if (NOT _describe OR _describe STREQUAL "UNKNOWN-VERSION")
            set(_describe "untagged-unknown")
        endif ()
        set(_short "${_a_BASE}")
        set(_version "${_short}.0")
    endif ()

    # major*1e7 + minor*1e5 + patch*1e3 + tweak (borealis' own formula)
    math(EXPR _code "${_major} * 10000000 + ${_minor} * 100000 + ${_patch} * 1000")
    if (_source STREQUAL "tag")
        string(REGEX MATCH "[0-9]+$" _tweak "${_version}")
        if (_tweak GREATER 999)
            set(_tweak 999)
        endif ()
        math(EXPR _code "${_code} + ${_tweak}")
    endif ()

    # The NACP's display version holds 15 characters.
    string(REGEX REPLACE "^v" "" _nacp "${_describe}")
    string(LENGTH "${_nacp}" _nacp_length)
    if (_source STREQUAL "build-id" OR _nacp_length GREATER 15)
        set(_nacp "${_version}")
    endif ()

    set(${_prefix}_DESCRIBE "${_describe}" PARENT_SCOPE)
    set(${_prefix}_VERSION "${_version}" PARENT_SCOPE)
    set(${_prefix}_SHORT_VERSION "${_short}" PARENT_SCOPE)
    set(${_prefix}_VERSION_CODE "${_code}" PARENT_SCOPE)
    set(${_prefix}_SOURCE "${_source}" PARENT_SCOPE)
    set(${_prefix}_NACP_VERSION "${_nacp}" PARENT_SCOPE)
endfunction()

# Reads the cache (DUSK_APP_VERSION_OVERRIDE, DUSK_APP_BASE_VERSION, ALEKS2_BUILD_TAG), git for the commit and what
# borealis_detect_version() just set; leaves the BOREALIS_APP_* names settled. Call it straight after
# borealis_detect_version(), before project().
macro(dusk_resolve_app_version)
    set(DUSK_APP_VERSION_OVERRIDE "" CACHE STRING
            "Application version to show (v1.0.0 or v1.0.0-beta.1); empty: a git tag, else the Build ID")
    set(DUSK_APP_BASE_VERSION "1.0.0" CACHE STRING "Numeric version of a build that has neither an override nor a version tag")

    set(_dav_commit "unknown")
    find_package(Git QUIET)
    if (GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short=10 HEAD
                OUTPUT_VARIABLE _dav_head RESULT_VARIABLE _dav_rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if (_dav_rc EQUAL 0 AND NOT _dav_head STREQUAL "")
            set(_dav_commit "${_dav_head}")
        endif ()
    endif ()
    if (ALEKS2_BUILD_TAG)
        set(_dav_tag "${ALEKS2_BUILD_TAG}")
    else ()
        set(_dav_tag "untagged")
    endif ()

    dusk_app_version_compute(_dav
            DESCRIBE "${BOREALIS_APP_DESCRIBE}" VERSION "${BOREALIS_APP_VERSION}"
            OVERRIDE "${DUSK_APP_VERSION_OVERRIDE}" BUILD_ID "${_dav_tag}-${_dav_commit}"
            BASE "${DUSK_APP_BASE_VERSION}")
    set(BOREALIS_APP_DESCRIBE "${_dav_DESCRIBE}")
    set(BOREALIS_APP_VERSION "${_dav_VERSION}")
    set(BOREALIS_APP_SHORT_VERSION "${_dav_SHORT_VERSION}")
    set(BOREALIS_APP_VERSION_CODE "${_dav_VERSION_CODE}")
    set(DUSK_APP_VERSION_SOURCE "${_dav_SOURCE}")
    set(DUSK_NACP_VERSION "${_dav_NACP_VERSION}")
    message(STATUS "aleks2: application version ${BOREALIS_APP_DESCRIBE} (${DUSK_APP_VERSION_SOURCE}), numeric "
            "${BOREALIS_APP_VERSION}, NACP '${DUSK_NACP_VERSION}'")
endmacro()
