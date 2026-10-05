# aleks2 (H37-c6): a native mod compiled for the console with the compiler the game is built with.
#
# Why. A mod built for Linux (Ubuntu's GCC 13) runs inside the game against the game's libstdc++ (devkitA64,
# GCC 16). Their headers disagree about how std::ios_base, basic_ios and std::locale are laid out, so the inline
# constructors of the module's std::ofstream hand the game's ios_base::_M_init an object it reads wrongly: H37-c5's
# Randomizer died in std::locale::operator= (null locale) when it opened Seed Management. Nothing but compiling the
# module against the game's own headers is safe, and that makes it a switch-arm64 package.
#
# What it is. A module -- a shared object -- made by aarch64-none-elf-g++, the devkitA64 compiler, in the shape the
# loader (src/dusk/mods/loader/elf_loader.cpp) maps:
#   * ET_DYN, position independent, no DT_NEEDED, no TLS, no text relocations;
#   * every game, newlib, libstdc++ and libgcc symbol it uses is left UNDEFINED: the loader resolves them against the
#     NRO's export surface. Nothing of libstdc++, libgcc, libc or libnx is linked into it (-nodefaultlibs, -nostartfiles:
#     a private copy would carry its own, differently laid out ios_base and its own unwinder);
#   * the crtbegin/crtend objects of the toolchain are linked around the objects (the __dso_handle the module's
#     atexit registrations name, the .init_array frame_dummy, and the zero that ends .eh_frame, which the loader's
#     __register_frame needs) -- but not crt0/crti/crtn, which are for programs;
#   * -mno-outline-atomics: GCC 10+ calls libgcc's __aarch64_* helpers for atomics, which the NRO does not export.
#
# Use:  cmake -S <mod> -B <build> -G Ninja -DCMAKE_TOOLCHAIN_FILE=platforms/switch/cmake/SwitchArm64Mod.toolchain.cmake
# with the devkitA64 tools on PATH or $DEVKITPRO set. The package it makes holds lib/switch-arm64/mod.so
# (cmake/ModSDK.cmake: DUSK_MOD_LIB_PLATFORM, set here). It is not the toolchain of the NRO.
set(CMAKE_SYSTEM_NAME Linux)            # ModSDK's UNIX branch (export version script, .so naming); only the name
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(DUSK_SWITCH_ARM64_MODULE_TOOLCHAIN ON)
set(DUSK_MOD_LIB_PLATFORM "switch-arm64" CACHE STRING "Package platform of the mod library (explicit)" FORCE)

if (DEFINED ENV{DEVKITPRO})
    set(_dk_hint "$ENV{DEVKITPRO}/devkitA64/bin")
else ()
    set(_dk_hint "/opt/devkitpro/devkitA64/bin")
endif ()
foreach (_tool IN ITEMS gcc g++ strip readelf)
    find_program(DUSK_DEVKITA64_${_tool} aarch64-none-elf-${_tool} HINTS "${_dk_hint}" REQUIRED)
endforeach ()
set(CMAKE_C_COMPILER "${DUSK_DEVKITA64_gcc}")
set(CMAKE_CXX_COMPILER "${DUSK_DEVKITA64_g++}")
set(CMAKE_STRIP "${DUSK_DEVKITA64_strip}")
set(CMAKE_READELF "${DUSK_DEVKITA64_readelf}")

# A bare-metal compiler cannot link a program: configure-time checks only compile.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The code generation of the NRO (cmake/SwitchOptions.cmake: DUSK_SWITCH_OPT_FLAGS' -march/-mtune) and PIC.
# DUSK_SWITCH_ARM64_EXTRA_FLAGS (-D): more flags for every compile, e.g. -ffile-prefix-map for reproducible builds.
set(_arch_flags "-march=armv8-a+crc+crypto -mtune=cortex-a57 -fPIC -mno-outline-atomics ${DUSK_SWITCH_ARM64_EXTRA_FLAGS}")
set(CMAKE_C_FLAGS_INIT "${_arch_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_arch_flags}")
# PT_GNU_EH_FRAME is how the custom loader finds .eh_frame before it runs a module's
# constructors.  Without --eh-frame-hdr the section is present but there is no program
# header for it, so the loader cannot call __register_frame and a throw inside a module
# reaches std::terminate instead of the module's catch handlers.
set(_link_flags "-nostartfiles -nodefaultlibs -Wl,--build-id=sha1 -Wl,--eh-frame-hdr -Wl,-z,noexecstack")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_link_flags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_link_flags}")

# crtbegin first and crtend last around the module's objects (and the static libraries it links).
execute_process(COMMAND "${DUSK_DEVKITA64_gcc}" -print-file-name=crtbeginS.o OUTPUT_VARIABLE _crtbegin
        OUTPUT_STRIP_TRAILING_WHITESPACE)
if (NOT IS_ABSOLUTE "${_crtbegin}" AND NOT "${_crtbegin}" MATCHES "^[A-Za-z]:[/\\\\]")
    execute_process(COMMAND "${DUSK_DEVKITA64_gcc}" -print-file-name=crtbegin.o OUTPUT_VARIABLE _crtbegin
            OUTPUT_STRIP_TRAILING_WHITESPACE)
endif ()
execute_process(COMMAND "${DUSK_DEVKITA64_gcc}" -print-file-name=crtendS.o OUTPUT_VARIABLE _crtend
        OUTPUT_STRIP_TRAILING_WHITESPACE)
if (NOT IS_ABSOLUTE "${_crtend}" AND NOT "${_crtend}" MATCHES "^[A-Za-z]:[/\\\\]")
    execute_process(COMMAND "${DUSK_DEVKITA64_gcc}" -print-file-name=crtend.o OUTPUT_VARIABLE _crtend
            OUTPUT_STRIP_TRAILING_WHITESPACE)
endif ()
if ((NOT IS_ABSOLUTE "${_crtbegin}" AND NOT "${_crtbegin}" MATCHES "^[A-Za-z]:[/\\\\]") OR
    (NOT IS_ABSOLUTE "${_crtend}" AND NOT "${_crtend}" MATCHES "^[A-Za-z]:[/\\\\]"))
    message(FATAL_ERROR "SwitchArm64Mod: ${DUSK_DEVKITA64_gcc} has no crtbegin/crtend object (-print-file-name gave "
            "'${_crtbegin}', '${_crtend}'): the module's .eh_frame could not be terminated")
endif ()
set(DUSK_SWITCH_ARM64_CRTBEGIN "${_crtbegin}")
set(DUSK_SWITCH_ARM64_CRTEND "${_crtend}")
# ONE string: set() with several quoted arguments joins them with ";", and CMake reads a ";" in a link rule as the
# end of a command -- the rule became three commands (g++ with no -o and no objects; "-o x"; "crtbegin objects crtend")
# and ld said "no input files" (H37-c6 on Windows). string(CONCAT) keeps it one command, one compiler invocation.
foreach (_lang IN ITEMS C CXX)
    string(CONCAT _rule
            "<CMAKE_${_lang}_COMPILER> <CMAKE_SHARED_LIBRARY_${_lang}_FLAGS> <LANGUAGE_COMPILE_FLAGS> <LINK_FLAGS> "
            "<CMAKE_SHARED_LIBRARY_CREATE_${_lang}_FLAGS> <SONAME_FLAG><TARGET_SONAME> -o <TARGET> "
            "\"${_crtbegin}\" <OBJECTS> <LINK_LIBRARIES> \"${_crtend}\"")
    set(CMAKE_${_lang}_CREATE_SHARED_LIBRARY "${_rule}")
    set(CMAKE_${_lang}_CREATE_SHARED_MODULE "${_rule}")
endforeach ()
