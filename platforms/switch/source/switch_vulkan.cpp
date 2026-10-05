// aleks2: the small amount of Vulkan setup this console needs. See vulkan.hpp.
#include "switch/vulkan.hpp"

#ifdef __SWITCH__
#include <cstdlib>
#endif

// The driver's archive is listed before Dawn on the link line, so by the time
// the linker reaches Dawn's reference to vk_icdGetInstanceProcAddr it has
// already walked past the member that defines it and nothing pulls it in.
// Referencing it from an object that is linked whole extracts that member
// first, after which Dawn resolves against the same copy.
extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);

extern "C" void* const aleks2_vulkan_icd_anchor =
    reinterpret_cast<void*>(&vk_icdGetInstanceProcAddr);

namespace dusk::sw {

void vulkan_env_setup() {
#ifdef __SWITCH__
  // NVK refuses to start without this, and says so plainly in its own words.
  // The name is Mesa's joke about the driver's state on desktop Tegra; on this
  // console it is simply how the driver is turned on.
  setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
#endif
}

const char* const* vulkan_instance_extensions(unsigned* count) {
  // VK_NN_vi_surface is how a surface is made from an NWindow; there is no
  // platform-specific alternative on Horizon.
  static const char* const extensions[] = {
      "VK_KHR_surface",
      "VK_NN_vi_surface",
  };
  if (count != nullptr) {
    *count = static_cast<unsigned>(sizeof(extensions) / sizeof(extensions[0]));
  }
  return extensions;
}

}  // namespace dusk::sw
