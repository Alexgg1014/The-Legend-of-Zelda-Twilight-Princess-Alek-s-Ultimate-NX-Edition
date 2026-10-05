# H37 Level 0: the smallest real native mod, for the dynamic loader. Built with the mod SDK
# like any published mod (add_mod), so the package it produces has the same shape:
# lib/<platform>/mod.so beside mod.json.
cmake_minimum_required(VERSION 3.25)
project(loader_lab CXX)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(DUSK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." CACHE PATH "Path to dusk source root")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    add_subdirectory("${DUSK_DIR}/sdk" dusk-sdk EXCLUDE_FROM_ALL)
endif ()

add_mod(loader_lab
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
)
