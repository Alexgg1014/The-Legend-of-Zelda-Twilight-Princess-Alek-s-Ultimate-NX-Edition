# Provenance

Where the code of Alek's Ultimate NX Edition comes from, what was replaced, and how to check it.

## Summary

- **What this is.** A Nintendo Switch port of [Dusklight](https://github.com/TwilitRealm/dusklight) (Twilit Realm),
  rendered through [Aurora](https://github.com/encounter/aurora). Unless credited below, the Switch work in this
  repository was written for this project by **Alexgg1014 (Aleks)**, with AI assistance (see
  [How this port was made](../../README.md#how-this-port-was-made)). That covers the platform layer, the native
  mod loader, the performance work and the tooling.
- **Independent.** This project is developed independently of the other Switch forks of Dusklight. It uses earlier
  Switch work only where its author gave permission, as listed below.
- **Inherited code was replaced.** Development started in September 2026 on top of an earlier community NX fork of
  Dusklight. At its author's request, that fork's code was then measured and removed. Each inherited file was
  either deleted, swapped for public upstream code, or rewritten from the public interfaces. Milestone H32 closed
  this on **1 October 2026**, and the result was validated on a console the same day.
- **Clean public history.** This public repository, and the public Aurora fork it uses, both started on
  **5 October 2026** from a fresh history of one commit each. Neither contains the earlier fork's commits.
- **Everything else is credited.** Every outside contribution is listed below and in
  [third-party-notices.md](third-party-notices.md), with its license or the written permission it is used under.

## Timeline

| Date (2026) | What happened |
|---|---|
| 13–19 September | Dates of the earlier fork's 26 commits found in the private development history |
| 29 September | First measurement: 54 % of the lines that fork added were still present, verbatim |
| 1 October | **H32 closed and validated on hardware.** By the content measure, 463 of 6,172 inherited lines (8 %) still match verbatim, and every file that still matches is justified in the ledger (0 pending) |
| 5 October | Public repository and public Aurora fork published with fresh one-commit histories |
| 5–20 October | Public betas and the V1 release candidates, all built on the H32 tree |

## What was replaced

Full detail, in Spanish as it was written at the time: [h32-provenance-closure.md](h32-provenance-closure.md).
In short:

- **Deleted.** Dead code paths and duplicates nothing called, such as a full Dawn patch for a tree the build no
  longer used, Aurora patch helpers that did nothing, an EGL stub, an input layer and the old icon.
- **Replaced with public upstream code.**
  - SDL3 now comes from devkitPro's public `switch-sdl-3.4` branch (zlib license), pinned by hash. Our own video
    and joystick back ends sit on top of it.
  - abseil's Switch patch is the one that `encounter/dawn` ships and applies itself.
- **Rewritten from public interfaces.**
  - The CMake build, on devkitPro's own macros.
  - The borealis back ends (I/O, data, net, websocket, file picker).
  - libnx start-up, the controller port and the SD folders.
  - The profiler.
  - The disc and launcher checks.
  - The Dawn and Tracy patches.
  - The Aurora changes, such as the draw filtering, the uniform repeat and the cache opening.
- **Metadata.**
  - The NACP author is now `Twilit Realm, Alexgg1014`.
  - `extern/aurora` points to this project's own public fork.

## What still matches, and why

Rewriting code that can only be written one way does not make it look different. So the H32 closure keeps a
**ledger**, [`platforms/switch/tools/provenance-ledger.json`](../../platforms/switch/tools/provenance-ledger.json).
For each file where some lines still match the earlier fork, it gives the reason those lines may stay:

| Class | Files | What it means |
|---:|---:|---|
| `api` | 22 | Dictated by an interface we do not own: CMake commands, POSIX signatures, borealis headers, GX SDK functions |
| `trivial` | 41 | One-line boilerplate that can only be written one way: `#ifdef __SWITCH__` guards, profiler markers, a constant |
| `independent` | 14 | Rewritten from behaviour. What matches are the API names and types the interface forces |
| `upstream` | 0 | — |
| pending | **0** | — |

Each entry has a `max_lines` cap so that it cannot grow unnoticed. Most also have `refs`, the public headers or
references the file follows, so the claim can be checked against them instead of taken on trust.

What this cannot prove, and does not claim: that nobody had the *idea* of some optimisations before, such as
filtering repeated binds or not interpolating particles in fast mode. What was removed or rewritten is the
inherited *expression* of those ideas.

## How to check it yourself

The measuring tool is [`platforms/switch/tools/provenance-residue.py`](../../platforms/switch/tools/provenance-residue.py).
It takes the author to look for as an argument (`--author`, or the `PROVENANCE_AUTHOR` environment variable).
It has two instruments:

- **Content.** Lines that the earlier fork's net diff adds and that are still present verbatim. This is an upper
  bound.
- **History** (`--blame`). Lines whose last writer is one of that fork's commits, in this repository and in
  `extern/aurora`.

```
python platforms/switch/tools/provenance-residue.py --author NAME --blame \
    --ledger platforms/switch/tools/provenance-ledger.json
```

The tool exits with an error while any file is unexplained, or when a ledger entry no longer matches the tree.
It needs a checkout whose history still has the earlier fork's commits. This public repository starts from a fresh
history, so here **the ledger is the record**: the paths in it are those of the H32 tree. The figures above come
from the development history, where they were last re-run on 20 October 2026 with the renamed tool. The output
was identical to the original's.

## Outside work used in this port

| Who | What | Basis |
|---|---|---|
| [Twilit Realm](https://github.com/TwilitRealm/dusklight) | Dusklight, the base of everything here | CC0 |
| [encounter](https://github.com/encounter) | Aurora, the Dawn fork with Switch support, nod | MIT |
| HayatoG | The original Switch layer (window, input, disc, shims), `switch-nvk`, the SQLite VFS fix | Author's permission |
| danfromtico | Mesa for Horizon (`mesa-switch`), the Vulkan driver this port links | Author's permission; Mesa is MIT |
| [igawa6](https://github.com/igawa6/dusklight) | Dual-screen companion (dashboard, pages, guide, touch) and the mod ABI rules, from v1.4.1-ds-2.7 with fixes from v2.0.3-ds-3.0.3 | CC0 |
| centollOS ([SwitchWaker](https://github.com/centollOS/SwitchWaker): Pulpparty, depende3000 and the SwitchWaker contributors) | The base of the deko3d renderer (Aurora's `lib/deko`), and the trigger-axis buttons | MIT; `lib/deko/mpl` from SwitchWakerHD is MPL-2.0 |
| PalindromicBreadLoaf | Pioneered deko3d on the Switch with a Wii emulator: the deko3d work we learned from | Credit; no code |
| ChanseyIsTheBest | Ideas from `renut_nx` (deko3d staging and present) and from `sa2-nx` / `banjo-nx` (frame pacing, hook early-outs, GPU timestamps, the PGO workflow) | Written permission, 19 and 20 October 2026. No sa2-nx code is included |

Each place that uses an idea or code from someone else says so in a comment that names the source. The full list of
libraries and their license texts is in [third-party-notices.md](third-party-notices.md). The permissions are
described in [permissions.md](permissions.md).
