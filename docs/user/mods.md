# Mods

The port runs mods from the [Twilit Realm](https://twilitrealm.dev/) catalog: code mods, Luau script mods, and
resource packs (models, textures, translations). Most of the catalog's code mods were made for PC Linux, and the port
loads them on the Switch without the mod's author having to do anything.

## Installing mods

- **From the game:** the main menu's mod browser lists the catalog. Installing, updating and removing happen there.
- **By hand:** copy `.dusk` packages to `sdmc:/switch/dusklight/mods/`.

Some mods ask you to restart after installing; the game offers a **Restart now** button. Mod settings and mod data
live in `sdmc:/switch/dusklight/mod_data/`.

**Included:**
- Dusklight Randomizer and Cosmetics are built into the game. Randomizer has its own **Play / Randomizer** selector
  on the main menu.
- The Luau runtime that script mods need comes in the release ZIP.

**Twilit Essentials:** install it from the mod browser. The catalog release runs on the Switch.

## How compatible is the catalog?

Two different levels, and the port never mixes them up:

- **Passes the checks.** Before a mod's code runs, the game checks the package against this build: the platform,
  what the mod needs from the game, and what it would change. The catalog is re-checked against each release. A mod
  that passes can still misbehave in play.
- **Tested on the console.** Someone actually played with it on a Switch.

Catalog check of **20 October 2026**: 66 of the 69 mods pass. That is 45 code mods, 16 resource packs and 5 Luau
mods. Three cannot load:

| Mod | Why |
|---|---|
| Dual Screen Companion | The package has no library for the Switch's processor. Not needed: the port has its own companion screen (Flip Grip) |
| Ragdoll Mod | Needs Linux system functions the Switch does not have |
| Twilight Princess Arabic (Android) | Android-only package |

### Tested on the console

| Mod | Notes |
|---|---|
| Dusklight Randomizer | Built in. Seeds, starting and loading games |
| Twilit Essentials | Catalog release |
| Cosmetics | Built in |
| Roll and Sprint | |
| Cowboy Link | |
| TP Classic Modern Controller UI 1.3.2 | Don't turn it off in the middle of play: go back to the title screen and restart |
| Dawnlight | Heavy on the CPU: expect lower frame rates in Hyrule Field |
| Twilight Compass 1.0.2 | Leave the mod's **Show Minimap** option on |

### Known not to work on this build

The in-game browser blocks these exact releases. A later release of the same mod shows up as *Unverified*, not as
blocked.

| Mod | Release | Problem |
|---|---|---|
| TP Classic Modern Controller UI | 1.3.1, the old Debug package | Crashes the game. 1.3.2 works |
| Deferred Fog | 2.0.0 | Crashes while compiling its fog shader |

## Tips

- **Add mods one at a time**, and play a little before adding the next. If something breaks, you know which one.
- **There is a limit to how many mods fit at once.** Code mods share a fixed amount of memory for their changes to
  the game. With many large mods installed, one may refuse to load, and the game says so.
- **Mods cost frame rate.** Mods that hook many game functions run code every frame. The session log names the
  costly ones.
- A mod that crashes the game while starting is turned off at the next start. To start with no mods at all, create
  an empty file named `start_without_mods` in `sdmc:/switch/dusklight/mods/.guard/`.

Some mods draw visual effects that need Graphics Backend = Vulkan: see [Mod effects that need Vulkan](mods-deko3d-effects.md).

Mods are their authors' work. If a mod misbehaves only on Switch, report it here. If it also misbehaves on PC, report
it to its author.
