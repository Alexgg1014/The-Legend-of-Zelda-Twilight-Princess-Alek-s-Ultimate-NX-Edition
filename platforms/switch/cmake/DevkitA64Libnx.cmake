# Toolchain file for the Switch: devkitA64 and libnx from devkitPro.
#
#   cmake --preset switch-devkitA64-release
#
# devkitPro ships its own toolchain file ($DEVKITPRO/cmake/Switch.cmake), but it is
# written for the pacman/msys2 CMake. This build runs under a native Windows
# CMake, where $DEVKITPRO is a Windows path and the cross compilers are .exe
# files, so the compilers are located here and everything else comes from
# devkitPro's own platform module (Platform/NintendoSwitch.cmake): the
# architecture and link flags, -specs=switch.specs, -lnx, and the NACP and NRO
# helpers used by platforms/switch/CMakeLists.txt.

cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED ENV{DEVKITPRO})
  message(FATAL_ERROR "DEVKITPRO is not set; point it at the devkitPro install.")
endif()
set(DEVKITPRO "$ENV{DEVKITPRO}")
set(DEVKITA64 "${DEVKITPRO}/devkitA64")
set(NX_ROOT "${DEVKITPRO}/libnx")
set(SWITCH_PORTLIBS "${DEVKITPRO}/portlibs/switch")

set(CMAKE_SYSTEM_NAME NintendoSwitch)
set(CMAKE_SYSTEM_VERSION "DKA-NX-19")
set(CMAKE_SYSTEM_PROCESSOR aarch64)
# Third-party CMake files test this name.
set(SWITCH TRUE)

# CMake finds Platform/NintendoSwitch.cmake through this path.
list(APPEND CMAKE_MODULE_PATH "${DEVKITPRO}/cmake")

# Compilers and archive tools. The gcc-ar and gcc-ranlib wrappers are the ones
# that understand LTO objects.
macro(_dusk_find_cross_tool variable program)
  find_program(${variable} NAMES ${program} HINTS "${DEVKITA64}/bin" NO_DEFAULT_PATH REQUIRED)
endmacro()
_dusk_find_cross_tool(CMAKE_C_COMPILER aarch64-none-elf-gcc)
_dusk_find_cross_tool(CMAKE_CXX_COMPILER aarch64-none-elf-g++)
_dusk_find_cross_tool(CMAKE_ASM_COMPILER aarch64-none-elf-gcc)
_dusk_find_cross_tool(CMAKE_AR aarch64-none-elf-gcc-ar)
_dusk_find_cross_tool(CMAKE_RANLIB aarch64-none-elf-gcc-ranlib)

# Host tools: the toolchain's own, and devkitPro's (elf2nro and nacptool, which the
# platform module's nx_create_nro and nx_generate_nacp look for under these names).
list(APPEND CMAKE_PROGRAM_PATH "${DEVKITPRO}/tools/bin" "${DEVKITA64}/bin")
find_program(NX_ELF2NRO_EXE NAMES elf2nro HINTS "${DEVKITPRO}/tools/bin" NO_DEFAULT_PATH)
find_program(NX_NACPTOOL_EXE NAMES nacptool HINTS "${DEVKITPRO}/tools/bin" NO_DEFAULT_PATH)

# Libraries and headers come from devkitPro and its portlibs, never the host.
set(CMAKE_FIND_ROOT_PATH "${DEVKITPRO}" "${DEVKITA64}" "${SWITCH_PORTLIBS}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_INSTALL_PREFIX "${SWITCH_PORTLIBS}" CACHE PATH "Install prefix")
set(CMAKE_PREFIX_PATH "${SWITCH_PORTLIBS}" CACHE PATH "Find prefix")

# Everything is linked statically into one executable; there is no loader.
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS FALSE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# The platform module sets the target architecture (-march=armv8-a+crc+crypto
# -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec). What it leaves out is
# position-independent *executable* code for libraries too, which is how this
# program has always been compiled.
add_compile_options(-fPIE "-I${NX_ROOT}/include")
add_link_options("-fPIE" "-L${NX_ROOT}/lib")
add_compile_definitions(SWITCH __SWITCH__=1 DUSK_PLATFORM_SWITCH=1 TARGET_SWITCH=1 _DEFAULT_SOURCE)
