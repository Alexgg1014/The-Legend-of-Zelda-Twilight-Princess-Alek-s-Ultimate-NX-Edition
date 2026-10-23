# Switch port

## Build

```
cmake --preset switch-devkitA64-release
cmake --build --preset switch-devkitA64-release
```

Needs `DEVKITPRO` set and the Mesa Horizon driver archive (`NAGA_VULKAN_ROOT`). The full
list of requirements, and where each piece comes from, is in
[docs/dev/building-switch.md](../../docs/dev/building-switch.md).

## Roadmap

See [Switch 1.0 performance, shader and mod-loader roadmap](../../docs/history/switch-v1-roadmap.md) (historical)
and the [documentation index](../../docs/README.md).
