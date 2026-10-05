# nod-c

`nod.h` is the C header of the [`nod`](https://github.com/encounter/nod-rs)
Rust crate (encounter, MIT), which is how Aurora's DVD layer
(`extern/aurora/lib/dolphin/dvd`) reads disc images. Aurora generates it with
cbindgen while building the crate.

Horizon does not build the crate: cross-compiling Rust for this console needs
cargo and a custom sysroot, and the whole point of this port's build is that it
needs nothing but devkitA64. So the header is vendored here verbatim, and the
ten entry points it declares are implemented in
`platforms/switch/source/iso_nod_shim.cpp` -- for uncompressed images only.

Verbatim: do not edit. If Aurora moves to a newer nod, regenerate this file
rather than patching it, and the shim will fail to compile where the ABI moved,
which is the point.
