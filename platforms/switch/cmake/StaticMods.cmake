# aleks2: code mods on Switch, compiled into the NRO.
#
# Horizon has no dynamic loader, so nothing can dlopen a mod's library. The
# loader does not actually need one: it looks up five symbols in a module
# (mod_meta, mod_ctx, mod_initialize, mod_update, mod_shutdown), and those can
# come from the executable itself. See src/dusk/switch_builtin_mods.hpp.
#
# Including this file REPLACES add_mod(), so every mod's own CMakeLists.txt is
# used unchanged and nothing in cmake/ModSDK.cmake is touched. The replacement
# builds a static library and then "seals" it: all of the mod's objects are
# merged into one relocatable object, and the names that have to be unique per
# mod are rewritten with the mod's target name.
#
# What exactly gets rewritten, and why each one has to be, is in
# platforms/switch/tools/seal_mod.py, which does the rewriting.
#
# Resources are unaffected: a built-in mod still needs its folder (mod.json and
# res/) in mods/ on the SD card, which is also what makes it appear in the mod
# browser and lets it be turned off. Those folders are staged into
# build/<preset>/switch-mods/.

if (NOT DUSK_PLATFORM_SWITCH)
    message(FATAL_ERROR "StaticMods.cmake is only for the Switch build")
endif ()
if (NOT COMMAND _mod_resolve_source_path)
    message(FATAL_ERROR "StaticMods.cmake must be included after cmake/ModSDK.cmake")
endif ()

set(ALEKS2_STATIC_MODS_STAGE "${CMAKE_BINARY_DIR}/switch-mods")

if (NOT CMAKE_OBJCOPY)
    find_program(CMAKE_OBJCOPY NAMES aarch64-none-elf-objcopy
            HINTS "$ENV{DEVKITPRO}/devkitA64/bin" REQUIRED)
endif ()
if (NOT CMAKE_LINKER)
    find_program(CMAKE_LINKER NAMES aarch64-none-elf-ld
            HINTS "$ENV{DEVKITPRO}/devkitA64/bin" REQUIRED)
endif ()
if (NOT CMAKE_NM)
    find_program(CMAKE_NM NAMES aarch64-none-elf-nm
            HINTS "$ENV{DEVKITPRO}/devkitA64/bin" REQUIRED)
endif ()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(ALEKS2_SEAL_MOD "${CMAKE_CURRENT_LIST_DIR}/../tools/seal_mod.py")
# H36: the checks and the link-time symbol table that need every sealed mod at once.
set(ALEKS2_STATIC_MOD_LINK "${CMAKE_CURRENT_LIST_DIR}/../tools/static_mod_link.py")
set(ALEKS2_STATIC_MOD_SYMBOLS "${CMAKE_CURRENT_LIST_DIR}/../static_mod_symbols.txt")
include("${CMAKE_CURRENT_LIST_DIR}/StaticModSet.cmake")

function(add_mod target_name)
    cmake_parse_arguments(ARG "BUNDLE" "MOD_JSON;RES_DIR;OVERLAY_DIR;TEXTURES_DIR;OUTPUT_DIR"
            "SOURCES;RUNTIME_LIBRARIES;FEATURES" ${ARGN})
    if (NOT ARG_MOD_JSON)
        message(FATAL_ERROR "add_mod: MOD_JSON is required")
    endif ()
    _mod_resolve_source_path(_mod_json "${ARG_MOD_JSON}")
    file(READ "${_mod_json}" _mod_json_text)
    string(JSON _mod_id GET "${_mod_json_text}" id)

    # Stage what the player has to copy to the card, library or not.
    set(_stage "${ALEKS2_STATIC_MODS_STAGE}/${target_name}")
    file(MAKE_DIRECTORY "${_stage}")
    file(COPY "${_mod_json}" DESTINATION "${_stage}")
    foreach (_dir RES_DIR OVERLAY_DIR TEXTURES_DIR)
        if (ARG_${_dir})
            _mod_resolve_source_path(_src "${ARG_${_dir}}")
            if (EXISTS "${_src}")
                file(COPY "${_src}" DESTINATION "${_stage}")
            endif ()
        endif ()
    endforeach ()

    if (NOT ARG_SOURCES)
        # An asset-only mod has no code to compile in; the staged folder is all of it.
        return()
    endif ()
    if (ARG_RUNTIME_LIBRARIES)
        message(FATAL_ERROR
                "add_mod(${target_name}): RUNTIME_LIBRARIES cannot be compiled into the NRO")
    endif ()

    add_library(${target_name} STATIC ${ARG_SOURCES})
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_link_libraries(${target_name} PRIVATE dusklight_mod_api)
    # Signed char to match the game (and x86), as add_mod does on desktop ARM.
    target_compile_options(${target_name} PRIVATE -fsigned-char)
    foreach (_feature IN LISTS ARG_FEATURES)
        target_link_libraries(${target_name} PRIVATE dusklight_mod_feature_${_feature})
        if (_feature STREQUAL "webgpu")
            _mod_add_webgpu_headers(${target_name})
        elseif (_feature STREQUAL "fmt")
            _mod_add_fmt(${target_name})
        endif ()
    endforeach ()

    set_property(GLOBAL APPEND PROPERTY ALEKS2_STATIC_MOD_TARGETS "${target_name}")
    set_property(GLOBAL APPEND PROPERTY ALEKS2_STATIC_MOD_IDS "${_mod_id}")
    if (TARGET dusklight_mods)
        add_dependencies(dusklight_mods ${target_name})
    endif ()
    message(STATUS "aleks2: mod '${_mod_id}' is compiled into the NRO")
endfunction()

# Call once after every add_subdirectory(mods/...): seals each mod, links it into
# the executable and writes the table switch_native_module.cpp reads.
# Portability fixes for third-party code a mod pulls in, applied where the
# targets exist -- after the mods are added, and without editing either the mod's
# own CMakeLists.txt or the fetched source (which FetchContent would overwrite).
function(_aleks2_fix_third_party_mod_deps)
    if (TARGET Luau.VM)
        # lbuiltins.cpp uses LLONG_MIN without including <climits>. glibc and the
        # MSVC headers drag it in; newlib does not.
        target_compile_options(Luau.VM PRIVATE "SHELL:-include climits")
    endif ()
endfunction()

function(aleks2_finish_static_mods)
    _aleks2_fix_third_party_mod_deps()
    get_property(_targets GLOBAL PROPERTY ALEKS2_STATIC_MOD_TARGETS)
    get_property(_ids GLOBAL PROPERTY ALEKS2_STATIC_MOD_IDS)

    set(_sealed_objects "")
    set(_sealed_pairs "")
    foreach (_target IN LISTS _targets)
        set(_merged "${ALEKS2_STATIC_MODS_STAGE}/${_target}.merged.o")
        set(_sealed "${ALEKS2_STATIC_MODS_STAGE}/${_target}.sealed.o")
        add_custom_command(OUTPUT "${_sealed}"
                COMMAND "${CMAKE_LINKER}" -r --whole-archive "$<TARGET_FILE:${_target}>"
                        -o "${_merged}"
                COMMAND "${Python3_EXECUTABLE}" "${ALEKS2_SEAL_MOD}"
                        --objcopy "${CMAKE_OBJCOPY}" --nm "${CMAKE_NM}"
                        --name "${_target}" "${_merged}" "${_sealed}"
                DEPENDS ${_target} "${ALEKS2_SEAL_MOD}"
                COMMENT "aleks2: sealing mod ${_target} for the NRO"
                VERBATIM)
        add_custom_target(${_target}_sealed DEPENDS "${_sealed}")
        add_dependencies(dusklight ${_target}_sealed)
        target_link_libraries(dusklight PRIVATE "${_sealed}")
        list(APPEND _sealed_objects "${_sealed}")
        list(APPEND _sealed_pairs "${_target}=${_sealed}")

        # A mod's own libraries have to reach the executable too: the sealed
        # object holds the mod's code, not the code of what it links against
        # (Luau, for instance, which the Luau runtime mod fetches itself).
        # They are linked as libraries rather than merged into the seal, so
        # two mods using the same one share a single copy, and so a library
        # the game already has is not duplicated. Only static libraries:
        # the interface targets carry headers and sources meant for the mod,
        # and CMake brings their transitive dependencies along by itself.
        get_target_property(_mod_deps ${_target} LINK_LIBRARIES)
        foreach (_dep IN LISTS _mod_deps)
            if (TARGET ${_dep})
                get_target_property(_dep_type ${_dep} TYPE)
                if (_dep_type STREQUAL "STATIC_LIBRARY")
                    target_link_libraries(dusklight PRIVATE ${_dep})
                endif ()
            endif ()
        endforeach ()
    endforeach ()

    # H36: with every mod sealed, refuse SDK template code two mods would share (it would
    # run with one mod's context for all of them) and write the table of names the mods
    # resolve by name, which stands in for the symbol manifest this build does not have.
    # hook_targets.txt is for platforms/switch/tools/hook_call_sites.py after linking.
    set(_symbols "${CMAKE_BINARY_DIR}/generated/switch_builtin_symbols.cpp")
    set(_hook_targets "${ALEKS2_STATIC_MODS_STAGE}/hook_targets.txt")
    add_custom_command(OUTPUT "${_symbols}" "${_hook_targets}"
            COMMAND "${Python3_EXECUTABLE}" "${ALEKS2_STATIC_MOD_LINK}"
                    --aliases "${ALEKS2_STATIC_MOD_SYMBOLS}" --output "${_symbols}"
                    --targets "${_hook_targets}" ${_sealed_pairs}
            DEPENDS ${_sealed_objects} "${ALEKS2_STATIC_MOD_LINK}" "${ALEKS2_STATIC_MOD_SYMBOLS}"
            COMMENT "aleks2: checking the sealed mods together and writing their symbol table"
            VERBATIM)
    target_sources(dusk_internal PRIVATE "${_symbols}")
    set_source_files_properties("${_symbols}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

    # H36: the Luau probe, packed for the card, whenever the Luau runtime is in the build.
    # It goes to switch-test-mods/, not switch-mods/: that folder is what a player copies,
    # and the probe is a diagnostic (the H36 milestone notes say how to use it).
    if ("dev.twilitrealm.luau" IN_LIST _ids)
        set(_probe_dir "${CMAKE_SOURCE_DIR}/platforms/switch/test-mods/luau_probe")
        set(_probe "${CMAKE_BINARY_DIR}/switch-test-mods/luau_probe.dusk")
        execute_process(
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/make_dusk.py"
                        "${_probe_dir}" "${_probe}"
                RESULT_VARIABLE _probe_result
                OUTPUT_VARIABLE _probe_output
                ERROR_VARIABLE _probe_error
                OUTPUT_STRIP_TRAILING_WHITESPACE)
        if (NOT _probe_result EQUAL 0)
            message(FATAL_ERROR "aleks2: could not pack the Luau probe: ${_probe_error}")
        endif ()
        if (_probe_error)  # make_dusk.py's note: CRLF text files were packed with LF
            message(STATUS "aleks2: ${_probe_error}")
        endif ()
        file(GLOB_RECURSE _probe_files "${_probe_dir}/*")
        set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                ${_probe_files})
        message(STATUS "aleks2: Luau probe for the card: ${_probe_output}")
    endif ()

    aleks2_builtin_mods_table(_table_source TARGETS ${_targets} IDS ${_ids})
    list(LENGTH _targets _count)
    set(_table "${CMAKE_BINARY_DIR}/generated/switch_builtin_mods_table.cpp")
    file(CONFIGURE OUTPUT "${_table}" CONTENT "${_table_source}" @ONLY)
    target_sources(dusk_internal PRIVATE "${_table}")
    # api.h is written for mods; keep the game's forced-include PCH out of it.
    set_source_files_properties("${_table}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_link_libraries(dusk_internal PRIVATE dusklight_mod_api)
    message(STATUS "aleks2: ${_count} code mod(s) compiled into the NRO")
endfunction()
