# Developer documentation

## Building

- [building-switch.md](building-switch.md): building the Switch `.nro`. Covers the toolchain, the Mesa driver
  archive and where each dependency comes from.
- [../building.md](../building.md): building for the other platforms, the same as upstream Dusklight.

## Conventions

- [../code-conventions.md](../code-conventions.md): upstream Dusklight's code conventions, which this port follows.
- Changes specific to the port are marked `aleks2` in comments, with the milestone that added them, for example
  `// aleks2 (V1-rc16): …`. Settings fields added by the port are appended at the end of their struct, to keep the
  mod ABI stable.
- Code or ideas taken from someone else carry a comment that names the source. They are also listed in
  [`res/licenses/THIRD_PARTY_NOTICES.txt`](../../res/licenses/THIRD_PARTY_NOTICES.txt) and in
  [../provenance/PROVENANCE.md](../provenance/PROVENANCE.md).

## Design notes

- [cross-save-design.md](cross-save-design.md): the cross-save mod's design and sync rules (in Spanish).
- [../modding.md](../modding.md) and [../jaudio.md](../jaudio.md): upstream's mod SDK and audio documentation. Both
  apply to the port unchanged.

## Tools

Under [`platforms/switch/tools/`](../../platforms/switch/tools/), among others:

| Tool | What it does |
|---|---|
| `provenance-residue.py` | Measures code inherited from an earlier fork, against the ledger `provenance-ledger.json`. See [PROVENANCE.md](../provenance/PROVENANCE.md#how-to-check-it-yourself) |
| `make-third-party-notices.py` | Generates `THIRD_PARTY_NOTICES.txt` from the build's dependency tree |
| `test-*.sh` | Host tests run before each release |

## History

Milestone notes from development (H28 to V1) are in [../history/](../history/README.md).
