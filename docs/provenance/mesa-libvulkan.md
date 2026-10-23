# Mesa libvulkan.a (the Dawn/NVK fallback)

The Switch builds link Mesa's Vulkan driver for the Dawn/NVK backend. deko3d is the default backend; Dawn/NVK is the fallback, and it is compiled into every NRO.

## What is linked

- **Package:** `mesa-26.2.3-switch-unified-horizon-sdk.zip`, SHA-256 `9b16d6dd07d01963d4c713281f3e6692cacca858195a143d7b2202afc2aeb346`.
- **Source:** Mesa 26.2.3, commit `d4a00ea0ab` (embedded in the binary). This includes danfromtico/mesa-switch PR #4 (head `3a69711b8d3055900423cc7b201d751125ff7a2c`). Mesa is MIT-licensed; see `third-party-notices.md`.
- **The file linked:** `opt/devkitpro/portlibs/switch/lib/libvulkan.a`. Only its archive symbol index was regenerated (below).

## Why it differs from the package

The distributed `libvulkan.a` has an incomplete archive symbol index: 13 408 entries, and the members of the Rust `core`/`alloc`/`std` libraries are not in it. GNU ld therefore never pulls those members, and the link fails with about 4950 undefined `core::panicking::*` references.

The fix regenerates the index and nothing else:

```
aarch64-none-elf-ranlib -D libvulkan.a     # GNU ranlib 2.46.0.20260210 (devkitA64)
```

| | SHA-256 | Bytes |
|---|---|---|
| as distributed | `af39adc64bf62d260fb7617f16eedc54d813848a5cbca17ebf0216e959e90100` | 89 042 696 |
| as linked | `5d59d3b961bfcab6b93b5b882f2519dc25481318984f66d0d1ae17b48a939892` | 89 468 318 |

- **Members:** 1545 before and after, in the same order, each byte-identical (digest of the members: `bb66b488ddc380b6181a8cc462a9ac84f1b2fe7aadbe6d5d03d14d0e58a44df7`).
- **Index:** grows from 13 408 to 18 432 entries; every earlier entry is kept, and `core::panicking::panic` is now indexed.
- **Archive:** only the symbol-table member changes, from 924 676 to 1 350 298 bytes.

## Reproducing it

```
platforms/switch/tools/reproduce-mesa-libvulkan.sh mesa-26.2.3-switch-unified-horizon-sdk.zip out/
```

The script:
1. checks the zip's SHA-256;
2. extracts `libvulkan.a` and checks it is the distributed one;
3. runs `ranlib -D`;
4. checks that the result is `5d59d3b9…`.

It exits non-zero on any mismatch.
