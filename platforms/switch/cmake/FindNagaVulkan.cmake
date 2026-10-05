# FindNagaVulkan -- aleks2
#
# Naga's "mesa-<ver>-switch-unified-horizon-sdk" is a devkitPro portlib
# prefix whose lib/libvulkan.a is a complete static Vulkan for Horizon: the
# vk* API, vk_icdGetInstanceProcAddr, VK_NN_vi_surface and its own Horizon
# winsys. Dawn's Switch platform links straight against it; no dispatch layer
# or symbol wrapping sits in between.
#
# NAGA_VULKAN_ROOT: the prefix (…/opt/devkitpro/portlibs/switch).
set(NAGA_VULKAN_ROOT "" CACHE PATH "Naga Mesa SDK prefix (portlibs/switch layout)")

find_library(NAGA_VULKAN_LIB
  NAMES vulkan
  PATHS "${NAGA_VULKAN_ROOT}"
  PATH_SUFFIXES lib
  NO_DEFAULT_PATH
  NO_CMAKE_FIND_ROOT_PATH
)
find_path(NAGA_VULKAN_INCLUDE_DIR
  NAMES vulkan/vulkan.h
  PATHS "${NAGA_VULKAN_ROOT}"
  PATH_SUFFIXES include
  NO_DEFAULT_PATH
  NO_CMAKE_FIND_ROOT_PATH
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NagaVulkan REQUIRED_VARS NAGA_VULKAN_LIB NAGA_VULKAN_INCLUDE_DIR)

if(NagaVulkan_FOUND AND NOT TARGET Naga::vulkan)
  add_library(Naga::vulkan INTERFACE IMPORTED)
  # vulkan.pc: Requires.private expat zlib libzstd (libelf is stubbed here:
  # only the VK_NVX_binary_import cubin loader uses it), Libs.private -lnx -lm
  # -lstdc++. -u keeps the ICD entry point alive for Dawn even under
  # --gc-sections and makes the archive member resolve before Dawn's reference.
  # From 26.2.2 the Rust halves (nil, nak_rs) reference core::panicking symbols
  # that live in archive members GNU ld has already walked past in its single
  # pass, so the archive needs a real rescan group. The devkitA64 toolchain
  # declares no RESCAN feature; declare it in the cache so it is visible in the
  # scope that creates the executable (same spelling as the SDK's own
  # VulkanConfig.cmake).
  set(CMAKE_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group"
      CACHE INTERNAL "" FORCE)
  set(CMAKE_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE CACHE INTERNAL "" FORCE)
  set(_naga_link "$<LINK_GROUP:RESCAN,${NAGA_VULKAN_LIB},expat,zstd,z,nx,stdc++,m>")
  set_target_properties(Naga::vulkan PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${NAGA_VULKAN_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "${_naga_link}"
    INTERFACE_LINK_OPTIONS "-Wl,-u,vk_icdGetInstanceProcAddr")
endif()
