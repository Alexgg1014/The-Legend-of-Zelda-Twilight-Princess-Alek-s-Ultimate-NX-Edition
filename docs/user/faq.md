# FAQ

**Is it free?**
Yes, always. The port and the mod ports made for it are never sold, and no build is held back behind a donation or a
paywall. If you paid for it, you got scammed; please get your money back and download it from the
[Releases page](https://github.com/Alexgg1014/The-Legend-of-Zelda-Twilight-Princess-Alek-s-Ultimate-NX-Edition/releases/latest).

**Does it include the game?**
No. You need a dump of your own *Twilight Princess* disc. See [Installing](install.md).

**Is this official? Is it Nintendo's, or Twilit Realm's?**
Neither. It is a fan project, not affiliated with Nintendo, and not an official Dusklight release.
[Dusklight](https://github.com/TwilitRealm/dusklight) is Twilit Realm's work, and this port adds a Switch layer on
top of it. Please report problems with this port here, not to the Dusklight team.

**Who made it?**
Alexgg1014 (Aleks), with AI assistance. It builds on Dusklight, Aurora and other people's work, all credited. See
[the credits](../../README.md#credits) and [How this port was made](../../README.md#how-this-port-was-made).

**Is this the same as other Switch forks of Dusklight?**
No. It is developed independently. Earlier code inherited from another community NX fork was replaced before the
first public release. The dates, what was replaced, and the ledger are in
[Provenance](../provenance/PROVENANCE.md).

**What frame rate should I expect?**
The goal is a steady **30 FPS at stock clocks**, the game's own frame rate. **Settings → Aleks → Performance** has
three presets:
- **Quality** keeps 30 FPS with everything at its best.
- **Balanced** and **Performance** interpolate up to 60 where the console keeps up.

60 FPS everywhere is not promised.

**Do I need to overclock?**
No. Every preset runs at stock clocks. **GPU Boost (handheld)** is an optional setting, off by default. In handheld
mode it asks the console for its own official 460 MHz GPU profile, which is not an overclock. It costs battery and
heat, and most players don't need it.

**Which resolution does it use?**
By default it picks one for you: docked, handheld and Flip Grip each have their own. You can choose a resolution by
hand in the settings.

**Do motion controls replace the buttons?**
No. Gyro aiming and the Wii-style sword swings are optional, and the buttons always work. The Wii-style controls are
marked experimental.

**Can I use mods?**
Yes. The game has a mod browser for the Twilit Realm catalog, and you can also copy `.dusk` packages to the SD card.
See [Mods](mods.md).

**Does it work with my PC save?**
Your GameCube or Wii save is in `sdmc:/switch/dusklight/`. For keeping one save in sync between PC and Switch, see
[Cross-save](cross-save.md).

**Can I put the NRO somewhere else, for example for a forwarder?**
Yes. You can keep `dusklight.nro` in `sdmc:/switch/dusklight/` or any folder and point a forwarder at it. The game
keeps its data in `sdmc:/switch/dusklight/` wherever the NRO is, and the in-game updater replaces the NRO it was
started from. If you update by extracting the ZIP instead, it puts a copy in `switch/dusklight-aleks/`: move it again
or delete the old one.

**What is the `cache` folder? Can I delete it?**
`sdmc:/switch/dusklight/cache/` holds what the game builds once and reuses: compiled shaders (`dksh_local.bin` for
deko3d, `pipeline_cache.db` for Vulkan), and the logs (`cache/logs/`). Deleting it is safe: the game rebuilds it, but
the first visit to each area is slower until it has. The unpacked code of your mods is kept separately, in
`mods/.libcache/`; deleting that is safe too, and only makes the next start slower.

**Where do I report a bug?**
Open an issue on this repository. Attach the newest log from `sdmc:/switch/dusklight/cache/logs/`. If the game
crashed, also attach the newest file from `sdmc:/atmosphere/crash_reports/`. See
[Troubleshooting](troubleshooting.md).
