# Installing

## What you need

- A Switch that can run homebrew: Atmosphère, plus hbmenu or Sphira.
- A dump of **your own** *Twilight Princess* disc, GameCube or Wii. No game data comes with this port.
  - To dump a disc, see [Dolphin's guide](https://wiki.dolphin-emu.org/index.php?title=Ripping_Games).
  - Supported formats: `.iso`, `.gcm`, `.rvz`, `.wia` and `.gcz`. Dolphin or
    [nodtool](https://github.com/encounter/nod/releases) can turn an `.iso` into a much smaller `.rvz`.

## First install

1. Download the release ZIP from the
   [Releases page](https://github.com/Alexgg1014/The-Legend-of-Zelda-Twilight-Princess-Alek-s-Ultimate-NX-Edition/releases/latest).
   It is free. If someone sold it to you, you were scammed.
2. Copy the `switch` folder from the ZIP to the **root** of your SD card, merging it with the one already there.
3. Copy your disc image to `sdmc:/switch/dusklight/` or `sdmc:/switch/dusklight/discs/`.
4. Start the game **from a game title, not from the Album**: on the HOME menu, hold **R** while you start any game,
   then pick the port. Started from the Album, homebrew gets far less memory.
5. Pick your disc in the list that appears.

`SHA256SUMS.txt` in the ZIP lists the checksum of every file, if you want to verify your download.

## Updating

> [!WARNING]
> **Back up first.** Close the game and copy the whole `sdmc:/switch/dusklight/` folder to your PC. It holds your
> saves, settings, mods and mod data.

- **From the game:** **Settings → Aleks** has an update check. When a newer release is out, the game downloads it
  from this repository's Releases page and checks it. It replaces the `.nro` when you quit.
- **By hand:** extract the new ZIP over the old files, as in a first install.

If you used the betas, remove the old `randomizer.dusk` and `twilit_essentials.dusk` from
`sdmc:/switch/dusklight/mods/`:
- Randomizer is now built into the game.
- Install Twilit Essentials again from the mod browser.

## Where your data lives

Everything the game writes goes to `sdmc:/switch/dusklight/`, not to the folder of the `.nro`:

| What | Where |
|---|---|
| Saves | `USA/`, `EUR/` or `JAP/` (following your disc's region), and the `MemoryCard*` files |
| Settings | `config.json` |
| Mods and their data | `mods/`, `mod_data/` |
| Texture packs | `texture_replacements/` |
| Shader caches | `cache/` |
| Logs | `cache/logs/` |

Back up that folder before you try a new build or a new mod.

## Next

- [FAQ](faq.md)
- [Troubleshooting](troubleshooting.md)
- [Mods](mods.md), [texture packs](texture-packs.md), [cross-save](cross-save.md), [co-op](co-op.md)
