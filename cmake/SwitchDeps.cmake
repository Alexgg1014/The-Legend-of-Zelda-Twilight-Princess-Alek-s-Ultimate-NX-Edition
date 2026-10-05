# SwitchDeps
#
# What the Switch build wants from Dawn and SDL3 before extern/aurora asks for
# them. Aurora uses a dependency that is already declared or available, so
# deciding it here is enough. Included from SwitchOptions.cmake.

include_guard(GLOBAL)
include(FetchContent)
include(${CMAKE_CURRENT_LIST_DIR}/SwitchPatches.cmake)
# Aurora's pins, so Dawn below is the revision Aurora was written against.
include(${CMAKE_SOURCE_DIR}/extern/aurora/cmake/AuroraDependencyVersions.cmake)

set(_dusk_patches "${CMAKE_CURRENT_LIST_DIR}/../platforms/switch/patches")

# --- Dawn ---------------------------------------------------------------------
# Vulkan is the only backend the console has.
set(DAWN_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
foreach(_backend IN ITEMS D3D11 D3D12 METAL DESKTOP_GL OPENGLES WEBGPU_ON_WEBGPU)
  set(DAWN_ENABLE_${_backend} OFF CACHE BOOL "" FORCE)
endforeach()
set(DAWN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_PROTOBUF OFF CACHE BOOL "No host protoc on the Switch build" FORCE)
set(DAWN_PLATFORM_SWITCH ON CACHE BOOL "Dawn's Horizon platform (NWindow surface)" FORCE)
set(TINT_BUILD_IR_BINARY OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_CMD_TOOLS OFF CACHE BOOL "" FORCE)

# encounter/dawn already knows about Horizon; the one change we need on top of it
# is dawn-optional-layers.patch. A local clone given with
# -DFETCHCONTENT_SOURCE_DIR_DAWN=<dir> is patched in place; otherwise the pinned
# tarball is fetched and patched as it is unpacked.
if(FETCHCONTENT_SOURCE_DIR_DAWN)  # FetchContent leaves the variable empty once it has fetched
  dusk_switch_apply_patch("${FETCHCONTENT_SOURCE_DIR_DAWN}" "${_dusk_patches}/dawn-optional-layers.patch"
    src/dawn/native/vulkan/VulkanInfo.cpp "EnumerateInstanceLayerProperties != nullptr")
else()
  # The patch and the script that applies it both go into the command, so editing
  # either makes FetchContent run it again.
  file(SHA256 "${_dusk_patches}/dawn-optional-layers.patch" _dusk_dawn_patch_hash)
  file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/SwitchPatches.cmake" _dusk_patch_script_hash)
  string(SHA256 _dusk_dawn_patch_id "${_dusk_dawn_patch_hash}${_dusk_patch_script_hash}")
  FetchContent_Declare(dawn
    URL "https://github.com/encounter/dawn/archive/${AURORA_DAWN_REF}.tar.gz"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    PATCH_COMMAND ${CMAKE_COMMAND}
      "-DTREE=<SOURCE_DIR>"
      "-DPATCH=${_dusk_patches}/dawn-optional-layers.patch"
      "-DMARKER_FILE=src/dawn/native/vulkan/VulkanInfo.cpp"
      "-DMARKER_TEXT=EnumerateInstanceLayerProperties != nullptr"
      "-DPATCH_ID=${_dusk_dawn_patch_id}"
      -P "${CMAKE_CURRENT_LIST_DIR}/SwitchPatches.cmake"
    EXCLUDE_FROM_ALL
  )
endif()

# --- SDL3 ---------------------------------------------------------------------
# devkitPro's public Switch branch, with our video and joystick drivers swapped
# in. See SwitchSdlOverlay.cmake for why and PROVENANCE.md for where it comes from.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)

set(_dusk_sdl_overlay "${CMAKE_CURRENT_LIST_DIR}/../platforms/switch/sdl")
# The overlay's content, and the script that applies it, go into the PATCH_COMMAND, so
# editing either changes the command FetchContent has recorded and makes it run again.
file(GLOB_RECURSE _dusk_sdl_overlay_files "${_dusk_sdl_overlay}/*.c" "${_dusk_sdl_overlay}/*.h")
list(APPEND _dusk_sdl_overlay_files "${CMAKE_CURRENT_LIST_DIR}/SwitchSdlOverlay.cmake")
set(_dusk_sdl_overlay_id "")
foreach(_f IN LISTS _dusk_sdl_overlay_files)
  file(SHA256 "${_f}" _h)
  string(APPEND _dusk_sdl_overlay_id "${_h}")
endforeach()
string(SHA256 _dusk_sdl_overlay_id "${_dusk_sdl_overlay_id}")

FetchContent_Declare(SDL
  URL https://github.com/devkitPro/SDL/archive/c329016c0e338ab4397ad930f83330c9fe058348.tar.gz
  URL_HASH SHA256=6047e4787f105ab4a0c6293ee3090ce5d1ebad112fcbe2a3e5a8874b0cc6f709
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE
  PATCH_COMMAND ${CMAKE_COMMAND}
    "-DSDL_SOURCE_DIR=<SOURCE_DIR>"
    "-DOVERLAY_DIR=${_dusk_sdl_overlay}"
    "-DOVERLAY_ID=${_dusk_sdl_overlay_id}"
    -P "${CMAKE_CURRENT_LIST_DIR}/SwitchSdlOverlay.cmake"
  EXCLUDE_FROM_ALL
)
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
FetchContent_MakeAvailable(SDL)
set(CMAKE_DISABLE_PRECOMPILE_HEADERS OFF)

unset(_dusk_patches)
