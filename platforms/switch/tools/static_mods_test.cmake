# H36: CMake script-mode cases for platforms/switch/cmake/StaticModSet.cmake, run one at a
# time by test-static-mods.sh:
#
#   cmake -DCASE=<name> -DMODS_DIR=<dir> -DOUT=<file> -P static_mods_test.cmake
#
# A passing case exits 0 (and writes OUT where it produces something); a case that the
# functions must refuse is expected to stop with FATAL_ERROR, which the script checks
# together with the message.
cmake_minimum_required(VERSION 3.25)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/StaticModSet.cmake")

if (CASE STREQUAL "select")
    aleks2_check_static_mod_selection("${MODS_DIR}" "${SELECTION}")
    message(STATUS "selection ok: ${SELECTION}")
elseif (CASE STREQUAL "table")
    aleks2_builtin_mods_table(_source TARGETS ${TARGETS} IDS ${IDS})
    file(WRITE "${OUT}" "${_source}")
elseif (CASE STREQUAL "table_mismatch")
    aleks2_builtin_mods_table(_source TARGETS a b IDS dev.x.a)
elseif (CASE STREQUAL "table_duplicate_id")
    aleks2_builtin_mods_table(_source TARGETS a b IDS dev.x.same dev.x.same)
elseif (CASE STREQUAL "table_bad_target")
    aleks2_builtin_mods_table(_source TARGETS "not-a-symbol" IDS dev.x.a)
elseif (CASE STREQUAL "table_empty")
    aleks2_builtin_mods_table(_source)
    file(WRITE "${OUT}" "${_source}")
else ()
    message(FATAL_ERROR "unknown CASE '${CASE}'")
endif ()
