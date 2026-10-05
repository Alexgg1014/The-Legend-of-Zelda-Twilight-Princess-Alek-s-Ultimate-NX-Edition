file(READ "${SOURCE_DIR}/cmake/capstone.cmake.in" _content)

# Insert PATCH_COMMAND before CONFIGURE_COMMAND in the ExternalProject_Add.
#
# aleks2: -DDIR= is quoted. Unquoted, a project path containing a space --
# "My Projects", for example -- is split by the generated command line and the
# patch script is handed half a path, reports that it cannot read a
# CMakeLists.txt nobody asked about, and capstone then fails to configure for
# a reason that looks nothing like the cause.
# Bracket args prevent cmake from substituting ${...} while writing this file.
if (NOT _content MATCHES "CAPSTONE_FIX_SCRIPT")
    string(REPLACE
        "    CONFIGURE_COMMAND \"\""
        [=[    PATCH_COMMAND "${CMAKE_COMMAND}" -P "${CAPSTONE_FIX_SCRIPT}"
    CONFIGURE_COMMAND ""]=]
        _content "${_content}")
    file(WRITE "${SOURCE_DIR}/cmake/capstone.cmake.in" "${_content}")
else ()
    # Already injected, possibly by a version of this file that passed the
    # directory as -DDIR=. FetchContent does not re-clone and re-patch a source
    # it already has, so an existing build tree would keep failing the same
    # way; rewrite the line instead.
    string(REGEX REPLACE
        "PATCH_COMMAND[^
]*CAPSTONE_FIX_SCRIPT[^
]*"
        [=[PATCH_COMMAND "${CMAKE_COMMAND}" -P "${CAPSTONE_FIX_SCRIPT}"]=]
        _fixed "${_content}")
    if (NOT _fixed STREQUAL _content)
        file(WRITE "${SOURCE_DIR}/cmake/capstone.cmake.in" "${_fixed}")
    endif ()
endif ()

file(READ "${SOURCE_DIR}/src/funchook.c" _content)
if (NOT _content MATCHES "commit_code_patch")
    string(REPLACE "#include \"funchook_internal.h\""
        "#include \"funchook_internal.h\"\n#ifdef __APPLE__\n#include \"code_patch_macos.hpp\"\n#endif\n#ifdef __SWITCH__\nint commit_code_patch(void *target, const void *expected, const void *replacement, size_t size);\n#endif"
        _content "${_content}")

    foreach(_operation install uninstall)
        if (_operation STREQUAL "install")
            set(_expected old_code)
            set(_replacement new_code)
        else ()
            set(_expected new_code)
            set(_replacement old_code)
        endif ()
        set(_original "            mem_state_t mstate;
            int rv = funchook_unprotect_begin(funchook, &mstate, entry->target_func, JUMP32_BYTE_SIZE);

            if (rv != 0) {
                return rv;
            }
            memcpy(entry->target_func, entry->${_replacement}, JUMP32_BYTE_SIZE);
            rv = funchook_unprotect_end(funchook, &mstate);
            if (rv != 0) {
                return rv;
            }
            flush_instruction_cache(entry->target_func, JUMP32_BYTE_SIZE);")
        string(FIND "${_content}" "${_original}" _position)
        if (_position EQUAL -1)
            message(FATAL_ERROR "Funchook ${_operation} patch site changed")
        endif ()
        string(REPLACE "${_original}" "#if defined(__APPLE__) || defined(__SWITCH__)
            int rv = commit_code_patch(entry->target_func, entry->${_expected},
                                            entry->${_replacement}, JUMP32_BYTE_SIZE);
            if (rv != 0) {
                funchook_set_error_message(funchook, \"Code patch commit failed (%d)\", rv);
                return FUNCHOOK_ERROR_MEMORY_FUNCTION;
            }
#else
${_original}
#endif" _content "${_content}")
    endforeach ()
    file(WRITE "${SOURCE_DIR}/src/funchook.c" "${_content}")
endif ()

# aleks2 (H37): Switch trampoline pages have two addresses (platforms/switch/source/code_arena.h):
# funchook is given the executable one, which it must branch to and can always read, and its
# writes to a page go through the writable one. Three places write: get_page's header, prepare's
# entry, and the target patch fix_code computes from the entry's address. Each is routed here,
# for __SWITCH__ only; other platforms compile the original lines.
#
# Idempotent (it checks for its own marker), so platforms/switch/CMakeLists.txt also runs this
# file at configure time over a funchook already fetched by an older build tree. C code holds
# semicolons, so no CMake lists here: one call per site.
function(_aleks2_funchook_site name original replacement)
    string(FIND "${_content}" "${original}" _position)
    if (_position EQUAL -1)
        message(FATAL_ERROR "Funchook page-write site '${name}' changed")
    endif ()
    string(REPLACE "${original}" "${replacement}" _replaced "${_content}")
    set(_content "${_replaced}" PARENT_SCOPE)
endfunction()

file(READ "${SOURCE_DIR}/src/funchook.c" _content)
if (NOT _content MATCHES "funchook_switch_writable")
    _aleks2_funchook_site(get_page
        [=[    page->used = 0;
    page->next = funchook->page_list;]=]
        [=[#ifdef __SWITCH__
    ((funchook_page_t *)funchook_switch_writable(page))->used = 0;
    ((funchook_page_t *)funchook_switch_writable(page))->next = funchook->page_list;
#else
    page->used = 0;
    page->next = funchook->page_list;
#endif]=])
    _aleks2_funchook_site(entry
        [=[    entry = &page->entries[page->used];
    /* fill members */]=]
        [=[#ifdef __SWITCH__
    funchook_entry_t *entry_exec = &page->entries[page->used];
    entry = (funchook_entry_t *)funchook_switch_writable(entry_exec);
#else
    entry = &page->entries[page->used];
#endif
    /* fill members */]=])
    _aleks2_funchook_site(fix_code
        [=[    funchook_fix_code(funchook, entry, &disp);]=]
        [=[    funchook_fix_code(funchook, entry, &disp);
#ifdef __SWITCH__
    funchook_switch_fix_entry(entry_exec, entry);
#endif]=])
    _aleks2_funchook_site(used
        [=[    page->used++;
    *target_func = (void*)entry->trampoline;]=]
        [=[#ifdef __SWITCH__
    ((funchook_page_t *)funchook_switch_writable(page))->used++;
    *target_func = (void*)entry_exec->trampoline;
#else
    page->used++;
    *target_func = (void*)entry->trampoline;
#endif]=])
    # The prototypes, once, ahead of the first mention of get_page (its forward declaration).
    string(FIND "${_content}" "static int get_page(" _first)
    if (_first EQUAL -1)
        message(FATAL_ERROR "Funchook get_page changed")
    endif ()
    string(SUBSTRING "${_content}" 0 ${_first} _head)
    string(SUBSTRING "${_content}" ${_first} -1 _tail)
    set(_content "${_head}#ifdef __SWITCH__
void *funchook_switch_writable(void *executable);
void funchook_switch_fix_entry(const funchook_entry_t *executable, funchook_entry_t *writable);
#endif
${_tail}")
    file(WRITE "${SOURCE_DIR}/src/funchook.c" "${_content}")
endif ()
