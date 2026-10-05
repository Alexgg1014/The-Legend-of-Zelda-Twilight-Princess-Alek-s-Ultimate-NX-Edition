// aleks2: what the Switch build needs from its Vulkan driver before Dawn starts.
//
// The driver is Mesa for Horizon, built by danfromtico and linked in as a static
// archive (see cmake/FindNagaVulkan.cmake). It is a real NVK for the Tegra X1,
// not a translation layer, so the only things that have to happen around it are
// the two below.
#pragma once

#include <vulkan/vulkan.h>

namespace dusk::sw {

// Called from userAppInit, before anything touches Vulkan.
void vulkan_env_setup();

// The instance extensions the surface needs on this console: the generic one
// plus Nintendo's own VI surface. Returns the array and writes its length.
const char* const* vulkan_instance_extensions(unsigned* count);

}  // namespace dusk::sw
