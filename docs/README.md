# Documentation

## Players

| | |
|---|---|
| [Installing](user/install.md) | Requirements, first install, updating, where your data lives |
| [FAQ](user/faq.md) | Price, frame rate, clocks, resolutions, controls, who made it |
| [Troubleshooting](user/troubleshooting.md) | Crashes, stutter, starting without mods, reporting a bug |
| [Mods](user/mods.md) | Installing mods, what has been tested, known problems |
| [Texture packs](user/texture-packs.md) | HD texture packs on the Switch |
| [Cross-save](user/cross-save.md) | One save on Switch and PC (optional, experimental) |
| [Co-op](user/co-op.md) | Online co-op with the catalog's Crests of Courage |

## Provenance and licenses

| | |
|---|---|
| [Provenance](provenance/PROVENANCE.md) | Where the code comes from, what was replaced and when, how to check it |
| [Third-party notices](provenance/third-party-notices.md) | Every library in the build and its license |
| [Licenses](provenance/licenses.md) | The project's license, components, mods, game data |
| [Permissions](provenance/permissions.md) | Work used with its author's written permission |
| [H32 provenance closure](provenance/h32-provenance-closure.md) | The full record of the replacement (Spanish) |

## Developers

| | |
|---|---|
| [Developer docs](dev/README.md) | Building, conventions, tools |
| [Building for Switch](dev/building-switch.md) | The `.nro` build |
| [Cross-save design](dev/cross-save-design.md) | Sync rules of the cross-save mod (Spanish) |

Upstream Dusklight's own documents stay where upstream keeps them, so that merging upstream stays simple:
- [building.md](building.md);
- [code-conventions.md](code-conventions.md);
- [modding.md](modding.md), the mod SDK;
- [jaudio.md](jaudio.md);
- [ios-install-altstore.md](ios-install-altstore.md).

## History

[Milestone notes](history/README.md) from H28 to V1, as written during development.

## Moved files

The documents were reorganised for 1.0. Some comments in the code still name the old paths:

| Old path | New path |
|---|---|
| `docs/building-switch.md` | [`docs/dev/building-switch.md`](dev/building-switch.md) |
| `docs/cross-save-1.0.md` | [`docs/dev/cross-save-design.md`](dev/cross-save-design.md) |
| `docs/switch-h32-provenance.md` | [`docs/provenance/h32-provenance-closure.md`](provenance/h32-provenance-closure.md) |
| `docs/switch-h28-…` to `docs/switch-h35-…`, `docs/switch-v1-roadmap.md` | Same names under [`docs/history/`](history/README.md) |
| `docs/switch-h36-mod-platform.md`, `docs/switch-h37-dynamic-loader.md`, `docs/switch-h38-mods.md`, `docs/h37/audit.json` | Summarised in [`docs/history/mod-platform.md`](history/mod-platform.md). The full notes stay in the private development repository |
