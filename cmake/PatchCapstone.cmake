# Patches capstone's CMakeLists.txt for compatibility with CMake >= 4.0:
#   - Bumps cmake_minimum_required to 3.10 (CMake >= 4.0 dropped < 3.5 support; < 3.10 warns)
#   - Removes cmake_policy(SET CMP0048 OLD) (rejected by CMake >= 3.27)
# aleks2: DIR is optional. ExternalProject runs a PATCH_COMMAND with the
# source tree as the working directory, and in script mode (-P) that is what
# CMAKE_CURRENT_SOURCE_DIR is -- so the path does not have to be passed at
# all. It used to be, through -DDIR=, and a project path with a space in it
# ("My Projects") was split by the generated command line into an argument
# nobody could quote their way out of.
if (NOT DIR)
    set(DIR "${CMAKE_CURRENT_SOURCE_DIR}")
endif ()

file(READ "${DIR}/CMakeLists.txt" _content)
string(REGEX REPLACE
    "cmake_minimum_required[ \t]*\\([ \t]*VERSION[ \t]+[0-9]+\\.[0-9]+(\\.[0-9]+)?[ \t]*\\)"
    "cmake_minimum_required(VERSION 3.10)"
    _content "${_content}")
string(REGEX REPLACE
    "cmake_policy[ \t]*\\([ \t]*SET[ \t]+CMP0048[ \t]+OLD[ \t]*\\)"
    "# cmake_policy(SET CMP0048 OLD)"
    _content "${_content}")
file(WRITE "${DIR}/CMakeLists.txt" "${_content}")
