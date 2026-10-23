# Third-party notices

The complete notices, with every license text, are in
[`res/licenses/THIRD_PARTY_NOTICES.txt`](../../res/licenses/THIRD_PARTY_NOTICES.txt). The same file ships inside the
release. It is generated from the build's own dependency tree by
[`platforms/switch/tools/make-third-party-notices.py`](../../platforms/switch/tools/make-third-party-notices.py),
so it lists exactly what is linked.

## Main components

| Component | License | Notes |
|---|---|---|
| Dusklight (Twilit Realm) | CC0 | The game reimplementation this port builds on |
| Aurora (encounter), this project's fork | MIT | Renderer |
| Aurora `lib/deko`, from SwitchWaker (centollOS: Pulpparty, depende3000 and the SwitchWaker contributors) | MIT | deko3d renderer |
| Aurora `lib/deko/mpl`, from SwitchWakerHD | MPL-2.0 | |
| Dawn / Tint | BSD-3-Clause | WebGPU implementation |
| Mesa (NVK, NIR) for Horizon | MIT | Vulkan driver |
| SDL3, devkitPro `switch-sdl-3.4` branch | zlib | Window, audio, controllers |
| libnx | ISC | |
| borealis (encounter) | MIT | |
| RmlUi | MIT | Menus |
| Luau, LuaBridge3 | MIT | Script mods |
| funchook | GPL-2.0-or-later, with a linking exception | Modified for the Switch. Its complete source, with those changes, ships next to every release as `funchook-source-*.tar.gz` |
| Capstone | BSD-3-Clause | |
| Tracy | BSD-3-Clause | Profiler markers |
| SQLite | Public domain | Shader caches |
| libcurl, Mbed TLS | curl license, Apache-2.0 | Mod browser, updates, online mods |
| zlib, Zstandard, bzip2, libpng, FreeType | Their own permissive licenses | |
| Fira Sans, Inter, Alegreya SC, Noto Mono | SIL OFL 1.1 | Fonts |
| Material Symbols | Apache-2.0 | Icons |

## Ideas credited without code

Some techniques come from other homebrew ports, with no file or text copied. Each place names its source in a
comment, and the notices file lists them all:
- `sa2-nx` and `banjo-nx` (ChanseyIsTheBest);
- `peggle2-nx`, for syncing the shader cache;
- `futari-nx`, for the background log writer;
- `nfsmw-nx`, for the official handheld GPU profile behind "GPU Boost (handheld)".

See also [permissions.md](permissions.md).

## Bundled mods

Bundled mods keep their authors' licenses. Each mod carries its own license inside its package or folder (for
example `mods/luau_runtime/res/licenses/`).
