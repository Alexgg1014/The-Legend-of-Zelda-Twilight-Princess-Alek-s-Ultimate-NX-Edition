# Cloud Save (cross-save), lab build. One normal mod package for every platform: PC and Android
# load their native library; Switch loads the linux-aarch64 one through the native mod bridge.
# Nothing here is compiled into the NRO (no StaticMods entry).
#
# core/      provider-neutral sync logic (host-tested: platforms/switch/tools/test-cross-save.sh)
# firebase/  Firebase Auth REST + Firestore REST adapter, firestore.rules
# src/       SDK glue (SaveSnapshotService, HttpService, files) and the panel
cmake_minimum_required(VERSION 3.25)
project(cross_save C CXX)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(DUSK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Path to dusk source root")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    add_subdirectory("${DUSK_DIR}/sdk" dusk-sdk EXCLUDE_FROM_ALL)
endif ()

# An official build bakes in its project (the Web API key is public by design; the rules protect the
# data). Empty in the lab: <data_dir>/firebase.json configures it.
set(CROSSSAVE_FIREBASE_API_KEY "" CACHE STRING "Firebase Web API key compiled into Cloud Save")
set(CROSSSAVE_FIREBASE_PROJECT_ID "" CACHE STRING "Firebase project ID compiled into Cloud Save")

add_mod(cross_save
    SOURCES
        core/sha256.cpp
        core/codec.cpp
        core/json.cpp
        core/sync_engine.cpp
        core/dusksave.cpp
        firebase/firebase_backend.cpp
        src/platform_io.cpp
        src/file_local_store.cpp
        src/sdk_adapters.cpp
        src/mod.cpp
        third_party/miniz/miniz.c
    MOD_JSON mod.json
)
# miniz reads and writes the .dusksave in memory only: no stdio, no clock (fewer libc imports
# through the Switch's glibc bridge).
target_compile_definitions(cross_save PRIVATE MINIZ_NO_STDIO MINIZ_NO_TIME
    CROSSSAVE_FIREBASE_API_KEY="${CROSSSAVE_FIREBASE_API_KEY}"
    CROSSSAVE_FIREBASE_PROJECT_ID="${CROSSSAVE_FIREBASE_PROJECT_ID}")
