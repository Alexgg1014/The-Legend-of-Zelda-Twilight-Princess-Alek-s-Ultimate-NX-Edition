# aleks2: Roll and Sprint, built in tree.
#
# MelonSpeedruns' mod, MIT, unchanged except for this file. Its own
# CMakeLists.txt fetches a Dusklight checkout and adds the mod SDK from it,
# which is right for somebody building the mod on its own and wrong here: the
# Dusklight checkout IS the tree this sits in. So the build file is ours and
# src/mod.cpp is theirs, byte for byte from
# github.com/MelonSpeedruns/Roll-and-Sprint at v1.1.0.
#
# Why this mod: it is real, it is popular (23 500 downloads), and it is the
# smallest published mod that uses hooks -- four of them, all on daAlink_c. On
# Switch those four will fail to install, because this build has no hook
# backend, and the mod is here precisely so that the day one exists there is
# something real to point at instead of a demo we wrote.
#
# It still does something today: it ignores the result of add_pre/add_post and
# returns MOD_OK, so it loads, registers its three settings and its panel, and
# runs mod_update every frame. What will not work is rolling into a sprint.
cmake_minimum_required(VERSION 3.25)
project(roll_and_sprint CXX)

# Only when someone builds this folder on its own; in tree, the root has
# already set the SDK up. Same guard the in-tree mods use.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(DUSK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Path to dusk source root")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    add_subdirectory("${DUSK_DIR}/sdk" dusk-sdk EXCLUDE_FROM_ALL)
endif ()

add_mod(roll_and_sprint
    FEATURES game
    SOURCES  src/mod.cpp
    MOD_JSON mod.json
    RES_DIR  res
)
