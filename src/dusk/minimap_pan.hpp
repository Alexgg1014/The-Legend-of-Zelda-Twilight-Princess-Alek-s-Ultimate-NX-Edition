#pragma once
// aleks2 (rc9, minimap_pan): the arithmetic of the panned minimap, free of game types so
// platforms/switch/tools/test-minimap-pan.sh can check it on a PC.
//
// The map path is rendered once into a "base" texture that covers the visible window
// plus kMargin of its size on every side, centred on (bx, bz). Each frame the visible
// window, centred on the current centre (cx, cz), is a sub-rectangle of that base. The
// map renderer's projection (d_map_path.cpp preRenderingMap / d_map_path_dmap.cpp
// preDrawPath) puts world +Z down the texture (row 0 = smallest Z) and world +X across it,
// mirrored when mirror mode makes the projection's right edge negative.
#include <cmath>
#include <cstdint>

namespace dusk::minimap_pan {

constexpr float kMargin = 0.25f;
constexpr float kBaseScale = 1.0f + 2.0f * kMargin;

struct Window {
    float u0, v0, u1, v1;  // base texture coordinates of the visible window's corners
};

// scaleX/scaleY: the base's extent over the visible window's, per axis (base texels over
// visible texels, so both have the same world units per texel; about kBaseScale).

// Whether the base rendered around (bx, bz) still holds the whole window around (cx, cz).
// ew/eh: the visible window's extent in world units (field_0x8 / field_0xc).
inline bool covers(float cx, float cz, float bx, float bz, float ew, float eh, float scaleX, float scaleY) {
    return std::fabs(cx - bx) <= 0.5f * (scaleX - 1.0f) * ew && std::fabs(cz - bz) <= 0.5f * (scaleY - 1.0f) * eh;
}

// u0/v0 is the base coordinate of the visible window's first column/row, u1/v1 of its end.
inline Window window(float cx, float cz, float bx, float bz, float ew, float eh, float scaleX, float scaleY,
                     bool mirror) {
    const float bw = ew * scaleX;
    const float bh = eh * scaleY;
    const float sx = mirror ? -1.0f : 1.0f;
    Window w{};
    w.u0 = 0.5f + sx * (cx - bx) / bw - 0.5f * ew / bw;
    w.u1 = w.u0 + ew / bw;
    w.v0 = 0.5f + (cz - bz) / bh - 0.5f * eh / bh;
    w.v1 = w.v0 + eh / bh;
    return w;
}

// The base texture's size for a visible texture of `size` texels (even, at least size).
inline uint16_t base_size(uint16_t size) {
    const uint32_t scaled = static_cast<uint32_t>(std::ceil(size * kBaseScale));
    return static_cast<uint16_t>((scaled + 1u) & ~1u);
}

}  // namespace dusk::minimap_pan
