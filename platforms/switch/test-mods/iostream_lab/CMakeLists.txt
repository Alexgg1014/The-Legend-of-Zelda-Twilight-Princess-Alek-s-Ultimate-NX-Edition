# DM2-b LAB: a Linux-built mod that exercises file streams, string streams, ctype and std::regex against
# the game's libstdc++ and the loader's glibc-layout file streams (src/lab.hpp). Built with the mod SDK
# like any published mod, for linux-aarch64 (build-h37-test-mods.sh, H37_TEST_MODS=iostream_lab).
cmake_minimum_required(VERSION 3.25)
project(iostream_lab CXX)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(DUSK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." CACHE PATH "Path to dusk source root")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    add_subdirectory("${DUSK_DIR}/sdk" dusk-sdk EXCLUDE_FROM_ALL)
endif ()

add_mod(iostream_lab
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
)
