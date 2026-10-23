# Cloud Save (cross-save)

Cloud Save keeps one save in sync between the Switch and Dusklight on PC. It is an **optional** mod made for this
project. On the Switch it is **built into the game** from 1.0.0; on PC it is a mod you add.

## How it works

- You sign in with an account in the mod's settings. Every device signed in to the same account shares the save.
- When you save, the mod uploads a new revision. When you open the game on another device, it downloads the newest
  one and applies it, after keeping a backup of the save it replaces.
- If both devices changed the save, the mod asks which copy to keep.
- On PC it works with the official Dusklight 2.0.3. Switch ↔ PC was tested with the same save on both.

## Setting it up

> [!WARNING]
> Back up `sdmc:/switch/dusklight/` (on PC, Dusklight's data folder) before the first sync.

1. **Switch:** nothing to install. **PC:** copy `dev.aleks2.cross_save.dusk` from the release page to Dusklight's
   `mods` folder.
2. Start the game and open Cloud Save's settings. Create an account or sign in.
3. On the first device, choose **Keep this device** to upload its save.
4. On the other device, sign in to the same account, or link it with the code the first device shows. It downloads
   the save.

## Known limits

- The save follows your disc's region (`USA`, `EUR` or `JAP`). Use the same region on both sides.
- Keep your own backups.
