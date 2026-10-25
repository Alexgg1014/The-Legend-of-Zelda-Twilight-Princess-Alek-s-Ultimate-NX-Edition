# Troubleshooting

## Before anything else

- Start the game by holding **R** on a game title, **not from the Album**. From the Album, homebrew gets much less
  memory, and the game can crash or refuse to start.
- Back up `sdmc:/switch/dusklight/` before you change builds or mods.

## The game crashes or freezes at start

1. **Start without mods.** Create an empty file named `start_without_mods` in
   `sdmc:/switch/dusklight/mods/.guard/` and start the game. If it starts, one of your mods is the cause: delete the
   file, then turn your mods back on one at a time.
2. A mod that crashes the game while it starts is turned off automatically at the next start, and the game tells
   you which one.
3. If you came from a beta, remove the old `randomizer.dusk` and `twilit_essentials.dusk` from
   `sdmc:/switch/dusklight/mods/`. Then install Twilit Essentials again from the mod browser.

## The first minutes stutter

The game compiles shaders the first time it sees each effect. They are stored in `cache/`, so it gets smoother as you
play, and later sessions start faster. A "Preparing shaders" indicator shows while that work is going on.

If the game reports a shader cache as damaged or malformed:
1. Close the game.
2. Move `cache/pipeline_cache.db` and `cache/dawn_cache.db` out of `sdmc:/switch/dusklight/`.
3. Start the game. The caches are rebuilt, so expect some stutter again for a while.

## Slow in some places

- Towns, caves and Castle Town are the heaviest places, and at stock clocks they are limited by the CPU. No preset
  changes that much.
- Mods that hook many game functions cost CPU every frame. If a place got slower after you installed a mod, try
  without it.
- Flip Grip mode draws a second screen. It is off by default: use it only if you play with a Flip Grip.

## My controller does nothing / the buttons are mixed up

- The game reads player 1: handheld, a Pro Controller, or two Joy-Con held together.
- Motion controls are optional. If you turned on the Wii-style sword swings by accident, turn them off in
  **Settings → Input → Wii-style Motion**.

## The mod browser says "Secure connection failed" or "SSL peer certificate ... was not OK"

The console's date and time are almost certainly wrong. The catalog's certificate is renewed every few months, and
the console rejects one that, by its clock, has not started yet. Set the date and time in **System Settings → System
→ Date and Time** (or turn on "Synchronise Clock via Internet"), then press **Retry**. Updates from inside the game
need the same.

## Reporting a bug

Open an issue on this repository and attach:
- the newest log from `sdmc:/switch/dusklight/cache/logs/`;
- if the game crashed, the newest file from `sdmc:/atmosphere/crash_reports/` (sort by date: the names do not
  always sort in order);
- what you were doing, and which mods were on.

If a mod is the cause, please also tell the mod's author. Mods are their authors' work, not part of this port.
