# SwitchOptions
#
# Build choices for the Horizon target. Included from the root CMakeLists when
# DUSK_PLATFORM_SWITCH is on, right after project().

set(DUSK_PLATFORM_SWITCH ON CACHE BOOL "" FORCE)

# CMake's platform module for this system sets CMAKE_DL_LIBS to "dl" as a normal
# variable, which hides the empty cache value the toolchain file sets, and
# Horizon's newlib has no libdl. Dawn, Tracy and funchook all link
# ${CMAKE_DL_LIBS}.
set(CMAKE_DL_LIBS "")

# Features the console either lacks or does not need.
set(DUSK_ENABLE_CODE_MODS OFF CACHE BOOL "" FORCE)
set(DUSK_GFX_DEBUG_GROUPS OFF CACHE BOOL "" FORCE)
set(DUSK_HAS_FUNCHOOK OFF CACHE INTERNAL "" FORCE)
set(DUSK_HAS_PREPATCH OFF CACHE INTERNAL "" FORCE)

# SQLite: plain POSIX file locking, no write-ahead log (it needs shared memory
# mappings), no extension loading (it needs dlopen).
add_compile_definitions(
  SQLITE_OS_UNIX=1
  SQLITE_OMIT_WAL=1
  SQLITE_OMIT_LOAD_EXTENSION=1
)

# Borealis is built for its UI layer only. Its network, crash-report, chat and
# file-selection integrations are either replaced by the sources in
# platforms/switch/source or not wanted.
foreach(_feature IN ITEMS SENTRY DISCORD UPDATE CRASH NET WEBSOCKET DATA IO FILE_SELECT DISC HTTP)
  set(BOREALIS_ENABLE_${_feature} OFF CACHE BOOL "" FORCE)
endforeach()
unset(_feature)

# Aurora: disc access goes through platforms/switch/source/iso_nod_shim.cpp, and
# the RmlUi menus stay.
set(AURORA_ENABLE_DVD OFF CACHE BOOL "No nod (Rust) here; see platforms/switch" FORCE)
set(AURORA_ENABLE_RMLUI ON CACHE BOOL "" FORCE)

# Dawn and SDL3 (patched and pinned there).
include(${CMAKE_CURRENT_LIST_DIR}/SwitchDeps.cmake)

add_compile_definitions(DUSK_PLATFORM_SWITCH=1 TARGET_SWITCH=1 VK_USE_PLATFORM_VI_NN)

# Optimisation flags the Switch targets add to what the build type gives them.
set(DUSK_SWITCH_OPT_FLAGS
  -O3
  -ffunction-sections
  -fdata-sections
  -fomit-frame-pointer
  -fno-math-errno
  -fno-trapping-math
)

# aleks2 (H37): the post-link symbol manifest native mods resolve by-name hooks through
# (platforms/switch/cmake/SymbolManifestSwitch.cmake). symgen reads names and source files
# from DWARF, so everything is compiled with -g1: function names and line tables, no types or
# locals. It does not change the generated code (GCC keeps -g out of code generation), and
# elf2nro drops the debug sections, so the NRO is the same size; the .elf and the link grow.
option(ALEKS2_SYMBOL_MANIFEST "Make the symbol manifest native mods need for by-name hooks" ON)
option(ALEKS2_SYMBOL_CHECK_STRICT "Fail the build when a required mod symbol is missing" ON)
if (ALEKS2_SYMBOL_MANIFEST)
  string(APPEND CMAKE_C_FLAGS " -g1")
  string(APPEND CMAKE_CXX_FLAGS " -g1")
endif ()
