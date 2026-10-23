# Building for Switch

The Switch build is a cross build with devkitA64 and libnx, producing `dusklight.elf` and
`dusklight.nro`. It runs under a native Windows CMake (the MSYS2 one, not devkitPro's own).

## What it needs

- devkitPro with `devkitA64`, `libnx` and the switch portlibs (zlib, zstd, bzip2, xz, freetype,
  harfbuzz, libpng, curl, expat). `DEVKITPRO` must point at the install.
- CMake 3.25+, Ninja, Python 3 (Dawn's dependency fetch uses it) and Git.
- GNU `patch` (devkitPro's msys2 and Git for Windows both ship one) for the two small patches
  below.
- The Mesa Horizon driver as one static archive plus headers (`libvulkan.a`), see
  [Driver](#driver).
- Network access at configure time: Dawn, SDL, Tracy, RmlUi and the other third-party sources are
  fetched by CMake.

## Configure and build

```sh
cmake --preset switch-devkitA64-release \
  -DALEKS2_STATIC_MODS="template_mod;flow_demo;rollandsprint" \
  -DNAGA_VULKAN_ROOT=<mesa sdk>/opt/devkitpro/portlibs/switch
cmake --build --preset switch-devkitA64-release
```

For H36 the proposed mod set (Cosmetics, the Luau runtime and Roll and Sprint, without the
`template_mod`/`flow_demo` demos) has its own preset, `switch-devkitA64-release-h36`, which sets
`ALEKS2_STATIC_MODS` to `"cosmetics;luau_runtime;rollandsprint"`:

```sh
cmake --preset switch-devkitA64-release-h36 -DNAGA_VULKAN_ROOT=<mesa sdk>/opt/devkitpro/portlibs/switch
cmake --build --preset switch-devkitA64-release-h36
```

`tools/configure.sh` and `tools/build.sh` wrap this with the environment the workspace uses
(`tools/env.sh`). The NRO is written next to `platforms/switch`'s build output as `dusklight.nro`.
`ALEKS2_BUILD_TAG` names the build in the log and in the benchmark report; it is a cache variable,
so change it with `-D`.

## Where things come from

| Piece | Source |
|---|---|
| Game, Aurora, borealis | this repository and its submodules |
| Dawn (WebGPU) | `encounter/dawn` at the revision Aurora pins, plus `platforms/switch/patches/dawn-optional-layers.patch` |
| abseil (inside Dawn) | the Horizon patch `encounter/dawn` itself ships and applies |
| SDL3 | `devkitPro/SDL` branch `switch-sdl-3.4` (pinned commit and hash in `cmake/SwitchDeps.cmake`), with our video and joystick drivers swapped in by `cmake/SwitchSdlOverlay.cmake` |
| Tracy | Aurora's pin, plus `platforms/switch/patches/tracy-horizon.patch` |
| Driver | Mesa for Horizon, `danfromtico/mesa-switch` |

`cmake/SwitchPatches.cmake` applies the patches and does nothing when they are already there.

## Driver

The driver is linked from a prebuilt archive, `libvulkan.a`, found by
`platforms/switch/cmake/FindNagaVulkan.cmake` through `NAGA_VULKAN_ROOT`. Which archive a build
used is recorded in `PROVENANCE.md`.

## What the Switch layer is

`platforms/switch` holds the runtime library (startup, POSIX shims, folders on the card, the
controller-to-port-one binder), the borealis pieces borealis does not build for this target (io,
data, file selection, net, websocket), the disc reader (`iso_nod_shim.cpp`, which stands in for
the Rust `nod`), and the NRO wiring. `platforms/switch/sdl` is the SDL3 video and joystick driver.
`src/dusk/switch_*.cpp` is the part that needs game headers.

SDL3 stays: window, audio and gamepads go through it, using devkitPro's backends for audio, time,
threads and the filesystem. Rendering does not: Dawn takes libnx's default NWindow directly.

## Code generation that mods rely on

Mods hook game functions at run time, so the build must not let the compiler assume what a game
function's body does. Two release-gate checks cover that:

- `platforms/switch/tools/test-hook-inline-elf.sh`: no hook target of the mod catalog is inlined
  anywhere in the image (targets that were get `DUSK_NOINLINE`).
- `platforms/switch/tools/test-no-ipa-ra.sh`: every C/C++ unit in `build.ninja` is compiled with
  `-fno-ipa-ra` and `-fno-inline-functions-called-once`; `CMakeLists.txt` sets both for the whole
  Switch build.
  - `-fno-ipa-ra`: with GCC's IPA register allocation a caller may keep values in caller-saved
    registers across a call it knows leaves them alone; a hook is an ordinary call that may clobber
    them (rc19/rc20: a crash in the collection screen with Twilit Essentials). (Not `noipa` on hook
    targets: it also hides that they cannot throw, which gives their callers extra exception edges
    and costs them their PGO profile.)
  - `-fno-inline-functions-called-once`: a function with a single caller stays out of line, so a
    mod update that hooks a new file-local function (Dawnlight 5.3.0: `d_a_e_sf.cpp#action`) finds
    it without a new build. About +0.6 % code; functions with several callers can still be inlined,
    which the hook-inline check catches.

## Run

Copy `dusklight.nro` to `sdmc:/switch/dusklight/` and start it as a title takeover (hold R while
starting a game), not from the album. Started as an applet it prints how to do that and exits.
The disc image goes in `sdmc:/switch/dusklight/` (or `sdmc:/switch/dusklight/discs/`). ISO/GCM and RVZ
have booted on the console; WIA and GCZ are read by the same container code but are untested there.
Settings > Disc Image lists the images in those two folders (H34); a new choice is used the next
time Dusklight starts.
