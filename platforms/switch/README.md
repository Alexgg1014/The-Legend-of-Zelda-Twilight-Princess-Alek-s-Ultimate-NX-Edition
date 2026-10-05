# Switch port

## Build

```
cmake --preset switch-devkitA64-release
cmake --build --preset switch-devkitA64-release
```

Needs `DEVKITPRO` set and the Mesa Horizon driver archive (`NAGA_VULKAN_ROOT`). The full
list of requirements, and where each piece comes from, is in
[docs/building-switch.md](../../docs/building-switch.md).

## Roadmap

See [Switch 1.0 performance, shader and mod-loader roadmap](../../docs/switch-v1-roadmap.md).
