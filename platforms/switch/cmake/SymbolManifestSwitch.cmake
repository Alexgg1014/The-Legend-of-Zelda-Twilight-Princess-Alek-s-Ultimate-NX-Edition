# aleks2 (H37): the symbol manifest for the Switch build, made after the link and before elf2nro.
#
# Native mods loaded at run time (src/dusk/mods/loader/elf_loader.cpp) need two things from the
# game image, and this is where both are checked against the image actually built:
#
#  * their imports, resolved against the NRO's own .dynsym -- the names ModAbiExports.cmake
#    exports by name, and nothing else (the allowlist);
#  * their hooks declared by name (DEFINE_HOOK_SYMBOL), including file-local statics that no
#    symbol table exports: Randomizer has 17 of them, 12 file-local. Those need a manifest of
#    every global and local symbol, which symgen (pinned in cmake/SymbolManifest.cmake, the same
#    one the desktop builds embed) makes from the linked ELF's symbols and DWARF.
#
# The desktop builds embed the manifest into the executable as a new section; elf2nro keeps only
# the three NRO segments, so here it goes into the RomFS instead (romfs:/dusklight.symdb), keyed
# by the ELF's GNU build id, which elf2nro copies into the NRO header. manifest.cpp refuses a
# manifest whose build id is not the running NRO's.
#
# Post-link, in this order, on every link of dusklight:
#   1. symgen writes the manifest uncompressed (for the check) and compressed (for the RomFS);
#   2. platforms/switch/tools/check_symbol_manifest.py checks that the manifest is this link's,
#      that every `import` in platforms/switch/mod_required_symbols.txt is exported and every
#      `hook` resolves to one address, and writes romfs:/dusklight.symbols.json, which the NRO
#      carries so the mods UI can say which mods this build cannot run. With
#      ALEKS2_SYMBOL_CHECK_STRICT (default ON) a missing symbol fails the build.
#
# symgen needs DWARF: SwitchOptions.cmake adds -g1 (function names and line tables only) when
# ALEKS2_SYMBOL_MANIFEST is on. -g1 does not change the generated code; elf2nro drops the debug
# sections, so it costs link time and the size of the .elf, not the NRO.
include_guard(GLOBAL)

function(aleks2_switch_symbol_manifest target)
    if (NOT ALEKS2_SYMBOL_MANIFEST)
        message(STATUS "aleks2: no symbol manifest (ALEKS2_SYMBOL_MANIFEST=OFF): native mods "
                "that hook by name will not load")
        return()
    endif ()
    include("${CMAKE_SOURCE_DIR}/cmake/SymbolManifest.cmake")
    ensure_symgen(TRUE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    # platforms/switch/CMakeLists.txt packs this folder into the NRO (nx_create_nro ROMFS).
    set(_romfs "${CMAKE_BINARY_DIR}/platforms/switch/romfs")
    set(_raw "${CMAKE_BINARY_DIR}/dusklight.symdb.raw")
    set(_tools "${CMAKE_SOURCE_DIR}/platforms/switch/tools")
    set(_required "${CMAKE_SOURCE_DIR}/platforms/switch/mod_required_symbols.txt")
    set(_strict "")
    if (ALEKS2_SYMBOL_CHECK_STRICT)
        set(_strict "--strict")
    endif ()
    # H37-c2: with the native loader the image carries the image runtime table
    # (src/dusk/mods/loader/elf_image_runtime.c), and imports resolve through it too.
    set(_image_runtime "")
    if (ALEKS2_NATIVE_MOD_LOADER)
        set(_image_runtime "--require-image-runtime")
    endif ()

    add_dependencies(${target} symgen)
    # -C: symgen names a file-local static "path#name" with the path relative to where it runs,
    # which must be the source root for the names mods use (src/d/actor/d_a_npc_ks.cpp#...).
    add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_romfs}"
            COMMAND "${SYMGEN_EXE}" -C "${CMAKE_SOURCE_DIR}" manifest
                    --binary "$<TARGET_FILE:${target}>" -o "${_raw}" --no-compress
            COMMAND "${SYMGEN_EXE}" -C "${CMAKE_SOURCE_DIR}" manifest
                    --binary "$<TARGET_FILE:${target}>" -o "${_romfs}/dusklight.symdb"
            COMMAND "${Python3_EXECUTABLE}" "${_tools}/check_symbol_manifest.py"
                    --elf "$<TARGET_FILE:${target}>" --symdb "${_raw}" --required "${_required}"
                    --report "${_romfs}/dusklight.symbols.json" ${_strict} ${_image_runtime}
            COMMENT "aleks2: symbol manifest for native mods (romfs:/dusklight.symdb) and its check"
            VERBATIM)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${_required}")
    message(STATUS "aleks2: symbol manifest after every link: romfs:/dusklight.symdb, "
            "checked against mod_required_symbols.txt (strict: ${ALEKS2_SYMBOL_CHECK_STRICT})")

    # H37-c2: the check before an H37 candidate is delivered, never lenient whatever
    # ALEKS2_SYMBOL_CHECK_STRICT says: the NRO is this ELF's, the manifest is not stale, the
    # sources are a clean commit, and the four official packages -- read from the .dusk files
    # themselves -- resolve every import and by-name hook against this ELF as the loader will
    # (platforms/switch/tools/check_h37_candidate.py). Build it explicitly:
    #   cmake --build --preset switch-devkitA64-release-h37 --target dusklight_h37_candidate
    # It writes <build>/h37-candidate.json and fails the build when the candidate is not ready.
    if (ALEKS2_NATIVE_MOD_LOADER)
        set(ALEKS2_H37_PACKAGES_DIR "" CACHE PATH
                "The official H37 packages (loader_lab, cosmetics, rollandsprint, randomizer .dusk) for dusklight_h37_candidate")
        # H37-c5: the catalog packages (Twilit Essentials, Dawnlight, ALBW), found by the id in their mod.json.
        set(ALEKS2_H37_CATALOG_DIR "" CACHE PATH
                "Directory with the catalog .dusk packages for dusklight_h37_candidate (default: ALEKS2_H37_PACKAGES_DIR)")
        option(ALEKS2_H37_REQUIRE_CATALOG "All three catalog packages must be present AND compatible for dusklight_h37_candidate to pass (OFF: they are reported and do not decide)" OFF)
        set(_catalog_args "")
        if (ALEKS2_H37_CATALOG_DIR)
            list(APPEND _catalog_args --catalog "${ALEKS2_H37_CATALOG_DIR}")
        endif ()
        if (ALEKS2_H37_REQUIRE_CATALOG)
            list(APPEND _catalog_args --require-catalog)
        endif ()
        if (ALEKS2_H37_PACKAGES_DIR)
            add_custom_target(dusklight_h37_candidate
                    COMMAND "${Python3_EXECUTABLE}" "${_tools}/check_h37_candidate.py"
                            --elf "$<TARGET_FILE:${target}>" --symdb "${_raw}"
                            --nro "${CMAKE_BINARY_DIR}/platforms/switch/dusklight.nro"
                            --packages "${ALEKS2_H37_PACKAGES_DIR}" ${_catalog_args} --symgen "${SYMGEN_EXE}"
                            --source-root "${CMAKE_SOURCE_DIR}" --tag "${ALEKS2_BUILD_TAG}"
                            --report "${CMAKE_BINARY_DIR}/h37-candidate.json"
                    COMMENT "aleks2: H37 candidate check (h37-candidate.json)"
                    VERBATIM)
        else ()
            add_custom_target(dusklight_h37_candidate
                    COMMAND "${CMAKE_COMMAND}" -E echo
                            "dusklight_h37_candidate: set ALEKS2_H37_PACKAGES_DIR to the directory with the official .dusk packages"
                    COMMAND "${CMAKE_COMMAND}" -E false
                    VERBATIM)
        endif ()
        add_dependencies(dusklight_h37_candidate ${target}_nro)

        # H38: the same check plus the H38 policy (platforms/switch/tools/h38_policy.py): the switch-arm64 Randomizer
        # and, with ALEKS2_H38_REQUIRE_TWILIT=ON, the switch-arm64 Twilit Essentials 2.5.0 are required; no TLS in any
        # switch-arm64 module; the packages' combined hook pages must fit the loader's arena table. The option is OFF by
        # default and is read only here: ordinary builds, and the H37 target above, never require Twilit.
        #   cmake --build <build> --target dusklight_h38_candidate      (writes <build>/h38-candidate.json)
        set(ALEKS2_H38_PACKAGES_DIR "" CACHE PATH
                "Directory with the .dusk packages (build-h38-mods.sh packages OUT_DIR, plus the official four) for dusklight_h38_candidate")
        option(ALEKS2_H38_REQUIRE_TWILIT "dusklight_h38_candidate requires the switch-arm64 Twilit Essentials 2.5.0 package (H38 validation only)" OFF)
        set(_h38_args "")
        if (ALEKS2_H38_REQUIRE_TWILIT)
            list(APPEND _h38_args --require-twilit)
            if (NOT ALEKS2_H38_PACKAGES_DIR OR NOT EXISTS "${ALEKS2_H38_PACKAGES_DIR}/twilit_essentials.dusk")
                message(FATAL_ERROR "ALEKS2_H38_REQUIRE_TWILIT=ON needs "
                        "${ALEKS2_H38_PACKAGES_DIR}/twilit_essentials.dusk: set ALEKS2_H38_PACKAGES_DIR to the "
                        "directory build-h38-mods.sh packages wrote (platforms/switch/tools/build-h38-mods.sh), "
                        "or leave ALEKS2_H38_REQUIRE_TWILIT OFF")
            endif ()
        endif ()
        if (ALEKS2_H38_PACKAGES_DIR)
            add_custom_target(dusklight_h38_candidate
                    COMMAND "${Python3_EXECUTABLE}" "${_tools}/check_h38_candidate.py"
                            --elf "$<TARGET_FILE:${target}>" --symdb "${_raw}"
                            --nro "${CMAKE_BINARY_DIR}/platforms/switch/dusklight.nro"
                            --packages "${ALEKS2_H38_PACKAGES_DIR}" ${_catalog_args} ${_h38_args}
                            --symgen "${SYMGEN_EXE}" --source-root "${CMAKE_SOURCE_DIR}"
                            --tag "${ALEKS2_BUILD_TAG}" --report "${CMAKE_BINARY_DIR}/h38-candidate.json"
                    COMMENT "aleks2: H38 candidate check (h38-candidate.json)"
                    VERBATIM)
        else ()
            add_custom_target(dusklight_h38_candidate
                    COMMAND "${CMAKE_COMMAND}" -E echo
                            "dusklight_h38_candidate: set ALEKS2_H38_PACKAGES_DIR to the directory with the .dusk packages"
                    COMMAND "${CMAKE_COMMAND}" -E false
                    VERBATIM)
        endif ()
        add_dependencies(dusklight_h38_candidate ${target}_nro)
    endif ()
endfunction()
