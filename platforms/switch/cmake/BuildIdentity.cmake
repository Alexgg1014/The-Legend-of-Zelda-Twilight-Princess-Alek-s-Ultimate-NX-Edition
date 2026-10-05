# aleks2 (H28): which build is this, in words the benchmark report can carry.
#
# A report that says "29.7 fps" is only useful if it also says which NRO made it.
# The tag is the candidate's name (a person decides that); the commit is read from
# git at configure time. Re-run the configure step after committing -- the build
# directory does not know the commit changed.
#
# "Dirty" means tracked files differ from HEAD, ignoring the two documentation
# paths that are edited by hand between candidates; it is a prompt to look, not a
# proof. Untracked files do not count.

set(ALEKS2_BUILD_TAG "H32-c1" CACHE STRING "Name of this build candidate, shown in Diagnostics and benchmark reports")

set(_aleks2_commit "unknown")
set(_aleks2_dirty 0)
find_package(Git QUIET)
if (GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" rev-parse --short=10 HEAD
            OUTPUT_VARIABLE _head RESULT_VARIABLE _rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if (_rc EQUAL 0 AND NOT _head STREQUAL "")
        set(_aleks2_commit "${_head}")
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" status --porcelain --untracked-files=no
                OUTPUT_VARIABLE _status ERROR_QUIET)
        string(REPLACE "\n" ";" _lines "${_status}")
        foreach (_line IN LISTS _lines)
            if (_line STREQUAL "" OR _line MATCHES "platforms/switch/README\.md$" OR _line MATCHES " docs/")
                continue()
            endif ()
            set(_aleks2_dirty 1)
        endforeach ()
    endif ()
endif ()

string(TIMESTAMP _aleks2_time "%Y-%m-%dT%H:%M:%SZ" UTC)

set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/dusk/benchmark.cpp" PROPERTY COMPILE_DEFINITIONS
        "ALEKS2_BUILD_TAG=\"${ALEKS2_BUILD_TAG}\""
        "ALEKS2_BUILD_COMMIT=\"${_aleks2_commit}\""
        "ALEKS2_BUILD_DIRTY=${_aleks2_dirty}"
        "ALEKS2_BUILD_TIME=\"${_aleks2_time}\"")

message(STATUS "aleks2: build identity ${ALEKS2_BUILD_TAG}-${_aleks2_commit} (dirty=${_aleks2_dirty})")
