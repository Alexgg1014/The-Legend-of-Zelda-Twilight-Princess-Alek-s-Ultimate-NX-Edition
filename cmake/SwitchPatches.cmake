# SwitchPatches
#
# The Switch build changes two third-party trees it does not own, and each change
# is a small unified diff kept in platforms/switch/patches:
#
#   dawn-optional-layers.patch  Dawn   a driver without vkEnumerateInstanceLayerProperties
#   tracy-horizon.patch         Tracy  two platform branches TracySystem.cpp lacks
#
# (abseil needs a Horizon patch as well, but that one belongs to encounter/dawn, which
# applies it itself.)
#
# dusk_switch_apply_patch() puts one on a tree, and does nothing if it is already
# there, so a reconfigure is harmless. It uses GNU patch and not `git apply`: when
# the tree sits inside another repository (everything under _deps/ does), git
# skips every file and still exits 0.
#
# The same file doubles as a script so FetchContent can use it as a PATCH_COMMAND:
#   cmake -DTREE=<dir> -DPATCH=<file> [-DMARKER_FILE=<file> -DMARKER_TEXT=<text>] -P SwitchPatches.cmake

# A patch that reports success but did nothing is worse than one that fails, so a
# caller can name a text the patched file must now contain.
function(_dusk_patch_verify tree name marker_file marker_text)
  if(marker_file STREQUAL "")
    return()
  endif()
  file(READ "${tree}/${marker_file}" _content)
  string(FIND "${_content}" "${marker_text}" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "SwitchPatches: ${name} was reported as applied, but ${marker_file} "
      "does not contain '${marker_text}'.")
  endif()
endfunction()

function(dusk_switch_apply_patch tree patch)
  # Optional: dusk_switch_apply_patch(<tree> <patch> <file in tree> <text it must contain after>)
  set(_marker_file "${ARGV2}")
  set(_marker_text "${ARGV3}")
  if(NOT EXISTS "${patch}")
    message(FATAL_ERROR "SwitchPatches: no such patch: ${patch}")
  endif()
  if(NOT IS_DIRECTORY "${tree}")
    message(FATAL_ERROR "SwitchPatches: no such source tree: ${tree}")
  endif()

  find_program(_dusk_gnu_patch NAMES patch
    HINTS "$ENV{DEVKITPRO}/msys2/usr/bin" "C:/Program Files/Git/usr/bin")
  if(NOT _dusk_gnu_patch)
    message(FATAL_ERROR "SwitchPatches: GNU patch not found "
      "(devkitPro's msys2 and Git for Windows both ship one).")
  endif()

  get_filename_component(_name "${patch}" NAME)

  # Is it already there? A forward dry run says: it applies (nothing to do yet), or
  # every hunk is reported as "previously applied" (done), or something else is
  # wrong. Asking patch to reverse the patch instead does not work for this: in
  # batch mode it silently drops -R when the patch is not applied and reports success.
  execute_process(
    COMMAND "${_dusk_gnu_patch}" -p1 --forward --dry-run --batch -i "${patch}"
    WORKING_DIRECTORY "${tree}"
    RESULT_VARIABLE _dry
    OUTPUT_VARIABLE _dry_out
    ERROR_VARIABLE _dry_err)
  if(NOT _dry EQUAL 0)
    if(_dry_out MATCHES "previously applied" AND NOT _dry_out MATCHES "FAILED")
      message(STATUS "SwitchPatches: ${_name} already applied")
      _dusk_patch_verify("${tree}" "${_name}" "${_marker_file}" "${_marker_text}")
      return()
    endif()
    message(FATAL_ERROR
      "SwitchPatches: ${_name} does not apply to ${tree}\n${_dry_out}${_dry_err}\n"
      "The tree is either a different revision than the patch was written "
      "against or holds a different version of the same patch.")
  endif()

  execute_process(
    COMMAND "${_dusk_gnu_patch}" -p1 --forward --batch --no-backup-if-mismatch -i "${patch}"
    WORKING_DIRECTORY "${tree}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR
      "SwitchPatches: ${_name} does not apply to ${tree}\n${_out}${_err}\n"
      "The tree is either a different revision than the patch was written "
      "against or holds a different version of the same patch.")
  endif()
  message(STATUS "SwitchPatches: applied ${_name}")
  _dusk_patch_verify("${tree}" "${_name}" "${_marker_file}" "${_marker_text}")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  if(NOT DEFINED TREE OR NOT DEFINED PATCH)
    message(FATAL_ERROR "SwitchPatches: pass -DTREE=<dir> -DPATCH=<file>")
  endif()
  dusk_switch_apply_patch("${TREE}" "${PATCH}" "${MARKER_FILE}" "${MARKER_TEXT}")
endif()
