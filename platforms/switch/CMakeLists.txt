# platforms/switch
#
# Everything the Horizon target adds on top of the game and Aurora: the runtime
# library with the platform shims, the borealis pieces borealis does not build
# for this target, the disc reader, and the NRO. Added from the root CMakeLists
# after Aurora and borealis, so their targets exist and can be adjusted here.

if(NOT DUSK_PLATFORM_SWITCH)
  return()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")  # FindNagaVulkan

set(_sw_src "${CMAKE_CURRENT_LIST_DIR}/source")
set(_sw_inc "${CMAKE_CURRENT_LIST_DIR}/include")
set(_borealis_inc "${CMAKE_SOURCE_DIR}/extern/borealis/include")

# --- Third-party fixes ----------------------------------------------------------

# Tracy arrives with Aurora and is patched here, after it has been configured and
# before anything is compiled: its client is built even with profiling off, and its
# TracySystem.cpp does not know this platform. (abseil needs a Horizon patch too,
# but encounter/dawn ships it and applies it itself, because DAWN_PLATFORM_SWITCH is
# on: patches/abseil-cpp.patch in the Dawn tree.)
include(${CMAKE_SOURCE_DIR}/cmake/SwitchPatches.cmake)
include(FetchContent)
FetchContent_GetProperties(tracy SOURCE_DIR _tracy_dir)
if(_tracy_dir)
  dusk_switch_apply_patch("${_tracy_dir}" "${CMAKE_CURRENT_LIST_DIR}/patches/tracy-horizon.patch"
    public/common/TracySystem.cpp "__SWITCH__")
endif()
unset(_tracy_dir)

# SQLite maps nothing into memory on this platform: devkitA64 is neither Linux nor
# Darwin, so its default SQLITE_MAX_MMAP_SIZE is already 0 and no call to mmap,
# munmap or mprotect is compiled (an aarch64-none-elf nm of sqlite3.c.obj finds
# none). The definition is explicit so that a future SQLite that decides otherwise
# stops the build instead of failing on the console, where there is no mmap.
if(TARGET sqlite3)
  target_compile_definitions(sqlite3 PRIVATE SQLITE_MAX_MMAP_SIZE=0)
endif()

# Portlibs' freetype was built with HarfBuzz as its autofit shaper, and HarfBuzz
# uses freetype back (hb-ft), so the link has to go freetype, harfbuzz, freetype.
if(TARGET Freetype::Freetype)
  set(_portlibs_lib "$ENV{DEVKITPRO}/portlibs/switch/lib")
  if(EXISTS "${_portlibs_lib}/libharfbuzz.a" AND EXISTS "${_portlibs_lib}/libfreetype.a")
    get_target_property(_ft_deps Freetype::Freetype INTERFACE_LINK_LIBRARIES)
    if(NOT _ft_deps OR _ft_deps MATCHES "-NOTFOUND$")
      set(_ft_deps "")
    endif()
    list(APPEND _ft_deps "${_portlibs_lib}/libharfbuzz.a" "${_portlibs_lib}/libfreetype.a")
    if(EXISTS "${_portlibs_lib}/libbz2.a")
      list(APPEND _ft_deps "${_portlibs_lib}/libbz2.a")
    endif()
    set_target_properties(Freetype::Freetype PROPERTIES INTERFACE_LINK_LIBRARIES "${_ft_deps}")
    unset(_ft_deps)
  endif()
  unset(_portlibs_lib)
endif()

# --- The driver -----------------------------------------------------------------
# danfromtico's Mesa for Horizon, one self-contained archive (see
# cmake/FindNagaVulkan.cmake). There used to be a second provider behind a dispatch
# shim and thirteen --wrap flags; it is gone. This one is faster where it counts
# (Castle Town held 26 fps against 21), MIT where the other was GPL-2.0, and needs
# none of that machinery.
find_package(NagaVulkan REQUIRED)
message(STATUS "aleks2: Vulkan driver = Mesa for Horizon (${NAGA_VULKAN_LIB})")

# --- Runtime library ------------------------------------------------------------
add_library(dusk_switch_runtime STATIC
  ${_sw_src}/switch_posix_shim.cpp
  ${_sw_src}/switch_startup.cpp
  ${_sw_src}/switch_pad_port.cpp
  ${_sw_src}/switch_text_input.cpp
  ${_sw_src}/switch_fs.cpp
  ${_sw_src}/switch_vulkan.cpp
  ${_sw_src}/aleks2_elf_stubs.c
)
target_include_directories(dusk_switch_runtime
  PUBLIC ${_sw_inc}
  PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/sdk/include"
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/extern/aurora/include/dolphin"
    "${NAGA_VULKAN_INCLUDE_DIR}")
target_compile_features(dusk_switch_runtime PUBLIC cxx_std_20)
# The game's headers expect global.h (DUSK_GAME_DATA and friends) and the same
# dolphin/TARGET_PC definitions the game's own sources get.
target_link_libraries(dusk_switch_runtime PRIVATE dusklight_game_headers)
target_compile_options(dusk_switch_runtime PRIVATE -include global.h ${DUSK_SWITCH_OPT_FLAGS})

# --- borealis pieces -----------------------------------------------------------
# borealis builds its own io, data, file_select, net and websocket targets on
# every platform but this one; here each is a small source in source/.
function(dusk_switch_borealis_part name source)
  if(TARGET borealis_${name})
    return()
  endif()
  cmake_parse_arguments(PART "" "" "LINKS;PRIVATE_LINKS" ${ARGN})
  add_library(borealis_${name} STATIC ${_sw_src}/${source})
  add_library(borealis::${name} ALIAS borealis_${name})
  target_include_directories(borealis_${name} PUBLIC ${_borealis_inc})
  target_compile_features(borealis_${name} PUBLIC cxx_std_20)
  target_compile_options(borealis_${name} PRIVATE ${DUSK_SWITCH_OPT_FLAGS})
  if(PART_LINKS)
    target_link_libraries(borealis_${name} PUBLIC ${PART_LINKS})
  endif()
  if(PART_PRIVATE_LINKS)
    target_link_libraries(borealis_${name} PRIVATE ${PART_PRIVATE_LINKS})
  endif()
endfunction()

dusk_switch_borealis_part(io borealis_io_switch.cpp PRIVATE_LINKS ${AURORA_SDL3_TARGET})
dusk_switch_borealis_part(data borealis_data_switch.cpp LINKS borealis::core borealis::io)
dusk_switch_borealis_part(file_select borealis_file_select_switch.cpp)
dusk_switch_borealis_part(net borealis_net_switch.cpp LINKS borealis::core)

if(NOT TARGET nlohmann_json::nlohmann_json)
  include(FetchContent)
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# HTTP: borealis' own libcurl backend over devkitPro's libcurl, which reaches TLS
# through Horizon's ssl service, so no TLS library ships with the program.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/CurlHttp.cmake)

# The update check is borealis' own too, on top of that HTTP. All it does is ask
# GitHub which release is newest and compare versions; nothing is downloaded,
# nothing replaces the NRO, and no repository is configured to ask about yet.
if(NOT TARGET borealis_update)
  if(NOT TARGET borealis_http)
    message(FATAL_ERROR "aleks2: borealis::update needs borealis::http")
  endif()
  add_library(borealis_update STATIC
    "${CMAKE_SOURCE_DIR}/extern/borealis/src/update/github.cpp"
    "${CMAKE_SOURCE_DIR}/extern/borealis/src/update/version.cpp")
  add_library(borealis::update ALIAS borealis_update)
  target_include_directories(borealis_update
    PUBLIC ${_borealis_inc}
    PRIVATE "${CMAKE_SOURCE_DIR}/extern/borealis/src")
  target_link_libraries(borealis_update
    PUBLIC borealis::core borealis::http
    PRIVATE fmt::fmt nlohmann_json::nlohmann_json)
  target_compile_features(borealis_update PUBLIC cxx_std_20)
  target_compile_options(borealis_update PRIVATE ${DUSK_SWITCH_OPT_FLAGS})
endif()

dusk_switch_borealis_part(ws borealis_ws_switch.cpp LINKS borealis::core borealis::http)

# --- funchook's OS layer --------------------------------------------------------
# funchook ships os_unix.c and os_windows.c. Horizon has no mmap, no mprotect and
# no /proc/self/maps, so neither runs. This swaps in our own layer without editing
# the fetched source, and adds the patch-site writer that does what upstream's
# code_patch_macos.cpp does there: write to the game's code through a second
# mapping instead of making the original writable.
if(DUSK_HAS_FUNCHOOK AND TARGET funchook-static)
  # aleks2 (H37): FetchContent patches funchook once, when it first fetches it, so a build tree
  # made by H36 holds a funchook.c without the code-arena patch -- and this OS layer would then
  # hand funchook pages it cannot write. The patch script is idempotent: run it again every
  # configure, and refuse to build without it.
  execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${funchook_SOURCE_DIR}"
                          -P "${CMAKE_SOURCE_DIR}/cmake/PatchFunchook.cmake"
                  RESULT_VARIABLE _fh_patch_result)
  file(READ "${funchook_SOURCE_DIR}/src/funchook.c" _fh_patched)
  if(NOT _fh_patch_result EQUAL 0 OR NOT _fh_patched MATCHES "funchook_switch_writable")
    message(FATAL_ERROR "aleks2: funchook.c lacks the code-arena patch (cmake/PatchFunchook.cmake)")
  endif()
  unset(_fh_patched)
  get_target_property(_fh_sources funchook-static SOURCES)
  # v1.1.3 names the file funchook_unix.c, newer trees os_unix.c: match the
  # suffix, so a version bump cannot compile both and let the Unix one win.
  list(FILTER _fh_sources EXCLUDE REGEX "_unix\\.c$")
  list(APPEND _fh_sources "${_sw_src}/funchook_os_switch.c" "${_sw_src}/code_arena.c"
       "${_sw_src}/funchook_arena_glue.c" "${_sw_src}/code_patch_switch.c")
  set_target_properties(funchook-static PROPERTIES SOURCES "${_fh_sources}")
  target_include_directories(funchook-static PRIVATE
    ${_sw_inc}
    "${funchook_SOURCE_DIR}/src"
    "${funchook_BINARY_DIR}/src")
  target_link_libraries(funchook-static PRIVATE nx)
  unset(_fh_sources)
  # It also names libdl, for dlsym, to walk a PLT stub to the real function. Our OS
  # layer returns the address unchanged, nothing here is dynamically linked, and
  # newlib has no libdl to link.
  foreach(_prop IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_libs funchook-static ${_prop})
    if(_libs AND NOT _libs MATCHES "-NOTFOUND$")
      list(FILTER _libs EXCLUDE REGEX "^(-l)?dl$|:dl>$")
      set_target_properties(funchook-static PROPERTIES ${_prop} "${_libs}")
    endif()
  endforeach()
  unset(_libs)
  message(STATUS "aleks2: code hooks through funchook (arm64, libnx memory layer)")
endif()

# --- The disc reader ------------------------------------------------------------
# Aurora already has a DVD layer and it is good; what it wants underneath is
# `nod`, which is Rust, and cross-building a Rust crate for Horizon needs cargo and
# a custom sysroot, where the rest of this build needs nothing but devkitA64. So
# source/iso_nod_shim.cpp implements nod's ten C entry points over an uncompressed
# image and Aurora's sources are used as they are (AURORA_ENABLE_DVD=OFF keeps
# Aurora from building its nod-backed target of the same name).
#
# RVZ, WIA and GCZ go through source/disc_container.cpp. The format lets whoever
# made the file choose the codec, so all four are needed (zlib, zstd, bzip2, xz) and
# they come from devkitPro's portlibs; a missing one stops the configure, instead of
# producing a build that quietly cannot read half of the images it offers.
# source/disc_header.cpp asks that same reader which game an image holds, so that the
# validator and the game cannot disagree about what an image is. source/disc_scan.cpp
# (H34) lists the images the console's disc selector offers, through that same header.
if(NOT TARGET aurora_dvd)
  set(_portlibs_roots "$ENV{DEVKITPRO}/portlibs/switch" /opt/devkitpro/portlibs/switch)
  set(_codec_libs "")
  set(_codec_missing "")
  foreach(_codec IN ITEMS z zstd bz2 lzma)
    unset(_found)
    foreach(_root IN LISTS _portlibs_roots)
      if(EXISTS "${_root}/lib/lib${_codec}.a")
        set(_found "${_root}/lib/lib${_codec}.a")
        break()
      endif()
    endforeach()
    if(_found)
      list(APPEND _codec_libs "${_found}")
    else()
      list(APPEND _codec_missing ${_codec})
    endif()
  endforeach()
  if(_codec_missing)
    message(FATAL_ERROR
      "aleks2: compressed disc images need these portlibs: ${_codec_missing}. "
      "Install them with: dkp-pacman -S switch-zlib switch-zstd switch-bzip2 switch-xz")
  endif()

  add_library(aurora_dvd STATIC
    "${CMAKE_SOURCE_DIR}/extern/aurora/lib/dolphin/dvd/dvd.cpp"
    "${CMAKE_SOURCE_DIR}/extern/aurora/lib/dolphin/dvd/fst.cpp"
    ${_sw_src}/iso_nod_shim.cpp
    ${_sw_src}/disc_container.cpp
    ${_sw_src}/disc_header.cpp
    ${_sw_src}/disc_scan.cpp)
  add_library(aurora::dvd ALIAS aurora_dvd)
  target_compile_definitions(aurora_dvd PUBLIC AURORA TARGET_PC)
  target_include_directories(aurora_dvd
    PUBLIC
      "${CMAKE_SOURCE_DIR}/extern/aurora/include"
      "${CMAKE_SOURCE_DIR}/extern/aurora/include/dolphin"
    PRIVATE
      "${CMAKE_SOURCE_DIR}/extern/aurora/lib"
      "${CMAKE_CURRENT_LIST_DIR}/third_party/nod-c"
      ${_sw_inc})
  foreach(_root IN LISTS _portlibs_roots)
    target_include_directories(aurora_dvd PRIVATE "${_root}/include")
  endforeach()
  target_link_libraries(aurora_dvd
    PUBLIC aurora::core fmt::fmt ${AURORA_SDL3_TARGET}
    PRIVATE Tracy::TracyClient ${_codec_libs})
  target_compile_features(aurora_dvd PUBLIC cxx_std_20)
  target_compile_options(aurora_dvd PRIVATE ${DUSK_SWITCH_OPT_FLAGS})
  unset(_portlibs_roots)
  unset(_codec_libs)
  unset(_codec_missing)
endif()

# --- The program ----------------------------------------------------------------
# The runtime library is linked whole: its shims have to be found by whatever
# needs them (SDL, the driver, ...) however GNU ld's single left-to-right pass
# orders things. Spelled with raw -Wl flags because this bare-metal toolchain
# defines no LINK_LIBRARY feature for CMake to expand.
target_link_libraries(dusklight PRIVATE
  "-Wl,--whole-archive" dusk_switch_runtime "-Wl,--no-whole-archive"
  borealis::net
  nx
  Naga::vulkan)
target_link_options(dusklight PRIVATE "-Wl,--gc-sections")

# Then keep, through that --gc-sections, what mods need to find.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/ModAbiExports.cmake)

# --- The NRO --------------------------------------------------------------------
# romfs is the game's res/ plus, if present, a shader cache and a pipeline list
# built on a console for this driver (tools/make-shader-seed.py from the console's
# cache/dawn_cache.db). Without them the console compiles as it goes. Aurora reads
# a seed once, while its own cache is still empty, and ignores one recorded for a
# different driver than the one this NRO carries.
set(_romfs "${CMAKE_CURRENT_BINARY_DIR}/romfs")
file(COPY "${CMAKE_SOURCE_DIR}/res" DESTINATION "${_romfs}")
foreach(_seed IN ITEMS initial_dawn_cache.db initial_pipeline_cache.db)
  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/romfs/${_seed}")
    file(COPY "${CMAKE_CURRENT_LIST_DIR}/romfs/${_seed}" DESTINATION "${_romfs}")
    message(STATUS "aleks2: bundling ${_seed}")
  endif()
endforeach()

# The NACP's author field is what Home shows under the title. 56 bytes.
nx_generate_nacp(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/dusklight.nacp"
  NAME "Dusklight"
  AUTHOR "Twilit Realm, Aleks"
  VERSION "${DUSK_NACP_VERSION}")  # 1.0.0-beta.1, or the numeric 1.0.0.0: never 0.0.0.0 (cmake/DuskAppVersion.cmake)
nx_create_nro(dusklight
  OUTPUT dusklight.nro
  NACP "${CMAKE_CURRENT_BINARY_DIR}/dusklight.nacp"
  ICON "${CMAKE_CURRENT_LIST_DIR}/icon.jpg"
  ROMFS "${_romfs}")

# --- Optimisation ---------------------------------------------------------------
set(_hot_targets
  dusklight dusk_internal
  aurora_card aurora_core aurora_dvd aurora_gd aurora_gx aurora_main aurora_ms
  aurora_mtx aurora_os aurora_pad aurora_si aurora_thp aurora_vi
  ${JSYSTEM_LIBRARIES})
foreach(_t IN LISTS _hot_targets)
  if(TARGET ${_t})
    target_compile_options(${_t} PRIVATE -O3)
  endif()
endforeach()
unset(_hot_targets)
