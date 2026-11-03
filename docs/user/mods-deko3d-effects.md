# Mod graphics effects on deko3d

Since 1.1, mods that draw their own effects through the game's graphics service (WebGPU) work with **deko3d**, the default renderer on Switch. That covers the screen effects of Twilit Essentials and mods such as Cinematic Depth of Field and VBAO. You no longer need to switch to Vulkan to see them.

If a mod's drawing fails under deko3d, the game names the mod and offers to restart with Vulkan. To stay on deko3d, say no and turn that mod off. You can also set Graphics Backend yourself in the Prelaunch settings tab.

These effects cost frame rate. On Switch, these Twilit Essentials settings are too heavy for most areas:

- Realistic Water
- Volumetric fog
- High shadows

Deferred Fog 2.0.0 crashes on the Switch while compiling its fog shader, so the browser blocks it.

Mods that don't use the service (Cosmetics, Randomizer, Linkle, texture packs and the rest of the catalog) look the same under deko3d and Vulkan.
