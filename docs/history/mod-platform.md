# Mod platform (H36–H38): summary

How mod support on the Switch came together, milestone by milestone. This is a short public summary. The full
working notes for these milestones are kept in the private development repository.

For using mods, see [Mods](../user/mods.md).

## H36: built-in mods (October 2026)

- The Switch build began shipping useful mods instead of the SDK's demos: **Cosmetics**, the **Luau runtime** for
  script mods, and **Roll and Sprint**, all compiled into the game.
- A Luau script mod was shown to load from the SD card.
- Randomizer was left out at this point and audited for the next step.

## H37: loading mods from the SD card

- **Native mods load at run time** from `.dusk` packages on the SD card, the same packages PC players use. No
  rebuild of the game is needed.
- **Every package is checked against the build before its code runs**: the platform, what the mod needs from the
  game, and what it would change. A package that fails is not run, and a report says why.
- **A safety net.** A mod that crashes the game while starting is turned off at the next start. An empty file can
  start the game with no mods at all.
- **Dusklight Randomizer loads from the card.**

## H38: Switch builds and the catalog

- Switch builds of **Dusklight Randomizer** and **Twilit Essentials**, and a **Play / Randomizer** selector on the
  main menu.
- The system keyboard for text fields.
- Optional **Wii-style motion controls**.
- A **compatibility matrix** of the whole Twilit Realm catalog: each package classified as tested on the console,
  passing the checks, or not working. The in-game browser blocks the exact releases known to crash.

## Towards 1.0

- Most code mods built for PC Linux now run on the Switch without their authors rebuilding them.
- The catalog is re-checked against every release candidate with the same build that ships. On 20 October 2026, 66
  of its 69 mods passed.
- The loader was made general, so that catalog updates keep working without per-mod patches.

Current status of individual mods: [docs/user/mods.md](../user/mods.md).
