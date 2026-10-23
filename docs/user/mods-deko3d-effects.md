# Mod effects that need the Vulkan backend

**These visual effects need Graphics Backend = Vulkan.**

On Switch, Dusklight draws with **deko3d** by default. Some mods draw their own effects through the game's graphics service (WebGPU). deko3d can't run that yet: it is planned for V1.1. Under deko3d those draws are skipped and the rest of each mod keeps working. When such a mod starts, the game offers to restart with Vulkan. To see the effects, say yes, or set Graphics Backend to Vulkan in the Prelaunch settings tab.

This list comes from reading each mod's library (catalog of 2026-10-22). It hasn't been checked on the console mod by mod.

| Mod | What triggers the effect | What is missing under deko3d | Gameplay |
|---|---|---|---|
| Twilit Essentials 2.8.4 | "Damage vignette", "Drowning vignette", the Flurry Rush and Bullet Time screen effects, the graphics grade (saturation and contrast settings), the Boss Rush leaderboard background blur | The red edge flash and low-health pulse, the blue drowning edge, the slow-motion screen tint, the colour grade, the blur behind the leaderboard | None: the features work, only their screen effects are missing |
| Dawnlight 5.1.0 | Playing as Dark Link ("Show the flowing aura around Dark Link"), Dark Link's eye glow and veil, the Boss Rush portals | Dark Link's aura and veil, the eye bloom (the eyes stay lit, without the glow), the symbols on the boss portals | None |
| Cinematic Depth of Field 1.1.0 | The whole mod ("Enabled", "Ambient Blur") | All of its depth-of-field blur | None |
| VBAO 1.1.0 | The whole mod ("Enabled") | All ambient occlusion | None |
| Evil Interlacing Mod 1.2.0 | The whole mod | The interlacing filter (its 60 fps cap and half-rate mode are separate settings) | None |
| A Link Between Twilight 0.3.0 | The tears (rifts) the mod places in the world | The glow around a tear (the tear itself is still drawn) | None |
| Dusklight GZ 0.2.3 | Nothing expected: it asks for the service, but draws its overlays (collision view, hitboxes) through the game's own graphics | Nothing expected | None |

Deferred Fog 2.0.0 is not in the table: it crashes on the Switch while compiling its fog shader, so the browser blocks it.

Mods that don't use the service (Cosmetics, Randomizer, Linkle, texture packs and the rest of the catalog) look the same under deko3d and Vulkan.
