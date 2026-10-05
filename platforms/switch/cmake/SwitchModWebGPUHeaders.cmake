# aleks2 (H38): WebGPU headers for a switch-arm64 module built offline.
#
# add_mod(... FEATURES webgpu) asks cmake/ModSDK.cmake for dawn::dawncpp_headers, which Aurora's Dawn provider
# normally downloads as a prebuilt package (linux-aarch64 for this toolchain). With no network, point
# DUSK_SWITCH_ARM64_WEBGPU_INCLUDE_DIR at an include directory that holds webgpu/webgpu.h of the Dawn revision
# Aurora pins, and pass this file as -DCMAKE_PROJECT_INCLUDE=: it runs right after project(), before add_mod, and
# defines the target the SDK would otherwise fetch. build_switch_mods.py does that when $DUSK_WEBGPU_INCLUDE_DIR is set.
if (DUSK_SWITCH_ARM64_WEBGPU_INCLUDE_DIR AND NOT TARGET dawn::dawncpp_headers)
    if (NOT EXISTS "${DUSK_SWITCH_ARM64_WEBGPU_INCLUDE_DIR}/webgpu/webgpu.h")
        message(FATAL_ERROR "DUSK_SWITCH_ARM64_WEBGPU_INCLUDE_DIR (${DUSK_SWITCH_ARM64_WEBGPU_INCLUDE_DIR}) has no webgpu/webgpu.h")
    endif ()
    add_library(dawn::dawncpp_headers INTERFACE IMPORTED GLOBAL)
    set_target_properties(dawn::dawncpp_headers PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${DUSK_SWITCH_ARM64_WEBGPU_INCLUDE_DIR}")
    message(STATUS "aleks2: WebGPU headers from ${DUSK_SWITCH_ARM64_WEBGPU_INCLUDE_DIR}")
endif ()
