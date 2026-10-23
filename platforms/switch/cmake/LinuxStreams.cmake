# aleks2 (DM2-b): glibc-layout std::basic_filebuf / basic_[io]fstream<char> for linux-aarch64 mods
# (src/dusk/mods/loader/linux_fstream.cpp says why). Three steps:
#
#   1. a copy of the toolchain's <fstream> in which basic_filebuf::_M_lock is followed by padding up to
#      glibc's 48 bytes (only that member; __c_lock keeps its type, which mangles __basic_file's
#      constructor in the game's libstdc++), first on the include path of linux_fstream.cpp only;
#   2. linux_fstream.cpp instantiates the four classes; platforms/switch/tools/linux_streams_rename.py
#      renames every symbol of those classes it defines to aleks2lx_<name> (objcopy --redefine-syms)
#      and writes the table elf_runtime_policy.cpp's linux_stream_shims() returns;
#   3. the renamed object and the table are linked into the game.
#
# The game's own stream instances are untouched: nothing in the game refers to an aleks2lx_ name.
function(aleks2_linux_streams target)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    foreach (_tool CMAKE_OBJCOPY CMAKE_NM)
        if (NOT ${_tool})
            message(FATAL_ERROR "aleks2 (DM2-b): ${_tool} is required for the Linux stream layout")
        endif ()
    endforeach ()
    set(_dir "${CMAKE_BINARY_DIR}/aleks2-linux-streams")
    set(_shadow "${_dir}/include")

    # 1. The toolchain's <fstream>: <compiler dir>/../aarch64-none-elf/include/c++/<version>/fstream.
    get_filename_component(_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_fstream "${_bin}/../aarch64-none-elf/include/c++/${CMAKE_CXX_COMPILER_VERSION}/fstream")
    if (NOT EXISTS "${_fstream}")
        message(FATAL_ERROR "aleks2 (DM2-b): no <fstream> at ${_fstream}")
    endif ()
    file(READ "${_fstream}" _text)
    set(_member_rx "\n([ \t]*)__c_lock[ \t]+_M_lock;")
    # Counted without the ';' (a CMake list would split the match there).
    string(REGEX MATCHALL "\n[ \t]*__c_lock[ \t]+_M_lock[ \t]*" _found "${_text}")
    list(LENGTH _found _count)
    if (NOT _count EQUAL 1)
        message(FATAL_ERROR "aleks2 (DM2-b): basic_filebuf::_M_lock not found exactly once in ${_fstream}")
    endif ()
    string(REGEX REPLACE "${_member_rx}"
        "\n\\1__c_lock\t\t_M_lock;\n\\1// aleks2 (DM2-b): glibc's __c_lock is 48 bytes; libstdc++ never uses it.\n\\1unsigned char _M_aleks2_glibc_lock_tail[48 - sizeof(__c_lock)];"
        _text "${_text}")
    set(_text "// aleks2 (DM2-b): generated from ${_fstream} by platforms/switch/cmake/LinuxStreams.cmake.\n// For src/dusk/mods/loader/linux_fstream.cpp ONLY.\n#define ALEKS2_LINUX_FILEBUF_LAYOUT 1\n${_text}")
    file(MAKE_DIRECTORY "${_shadow}")
    # Through a temporary and COPYONLY: the header is rewritten (and linux_fstream.cpp rebuilt) only
    # when it changes.
    file(WRITE "${_dir}/fstream.new" "${_text}")
    configure_file("${_dir}/fstream.new" "${_shadow}/fstream" COPYONLY)

    # 2. The instances, then renamed.
    add_library(aleks2_linux_fstream OBJECT "${CMAKE_SOURCE_DIR}/src/dusk/mods/loader/linux_fstream.cpp")
    target_include_directories(aleks2_linux_fstream BEFORE PRIVATE "${_shadow}")
    set_target_properties(aleks2_linux_fstream PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set(_renamed "${_dir}/linux_fstream_renamed.o")
    set(_table "${_dir}/linux_streams_table.cpp")
    add_custom_command(
        OUTPUT "${_renamed}" "${_table}"
        COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/platforms/switch/tools/linux_streams_rename.py"
                --nm "${CMAKE_NM}" --objcopy "${CMAKE_OBJCOPY}"
                --in "$<TARGET_OBJECTS:aleks2_linux_fstream>" --out "${_renamed}" --table "${_table}"
        DEPENDS aleks2_linux_fstream "$<TARGET_OBJECTS:aleks2_linux_fstream>"
                "${CMAKE_SOURCE_DIR}/platforms/switch/tools/linux_streams_rename.py"
        COMMENT "aleks2: glibc-layout file streams for Linux mods (DM2-b)"
        COMMAND_EXPAND_LISTS VERBATIM)
    set_source_files_properties("${_renamed}" PROPERTIES EXTERNAL_OBJECT ON GENERATED ON)
    set_source_files_properties("${_table}" PROPERTIES GENERATED ON SKIP_PRECOMPILE_HEADERS ON)

    # 3. Into the game.
    target_sources(${target} PRIVATE "${_renamed}" "${_table}")
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/src")
endfunction()
