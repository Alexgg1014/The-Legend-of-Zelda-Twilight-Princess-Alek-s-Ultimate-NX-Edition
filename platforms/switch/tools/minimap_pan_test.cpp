// aleks2 (rc9): src/dusk/minimap_pan.hpp. A world point seen through the panned window
// must land where a render centred on the current centre puts it, in both mirror modes,
// and covers() must agree with the window staying inside the base texture.
#include "dusk/minimap_pan.hpp"

#include <cstdio>
#include <random>

using namespace dusk::minimap_pan;

static int g_failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

int main() {
    std::mt19937 rng{42};
    std::uniform_real_distribution<float> any(-50000.f, 50000.f), unit(0.f, 1.f), ext(2000.f, 40000.f);
    int checked = 0;
    for (int i = 0; i < 200000; ++i) {
        const bool mirror = (i & 1) != 0;
        const uint16_t texW = static_cast<uint16_t>(64 + (rng() % 200) * 2), texH = static_cast<uint16_t>(64 + (rng() % 200) * 2);
        const float scaleX = static_cast<float>(base_size(texW)) / texW, scaleY = static_cast<float>(base_size(texH)) / texH;
        CHECK(scaleX >= kBaseScale && scaleY >= kBaseScale && base_size(texW) % 2 == 0);
        const float ew = ext(rng), eh = ext(rng);
        const float bx = any(rng), bz = any(rng);
        // a centre that drifted up to 1.2 margins away
        const float cx = bx + (unit(rng) * 2.f - 1.f) * 0.6f * (scaleX - 1.f) * ew;
        const float cz = bz + (unit(rng) * 2.f - 1.f) * 0.6f * (scaleY - 1.f) * eh;
        const Window w = window(cx, cz, bx, bz, ew, eh, scaleX, scaleY, mirror);
        const bool inside = w.u0 >= -1e-5f && w.u1 <= 1.f + 1e-5f && w.v0 >= -1e-5f && w.v1 <= 1.f + 1e-5f;
        CHECK(covers(cx, cz, bx, bz, ew, eh, scaleX, scaleY) == inside ||
              std::fabs(std::fabs(cx - bx) - 0.5f * (scaleX - 1.f) * ew) < 1e-3f * ew ||
              std::fabs(std::fabs(cz - bz) - 0.5f * (scaleY - 1.f) * eh) < 1e-3f * eh);
        // a world point in the visible window
        const float sx = mirror ? -1.f : 1.f;
        const float a = unit(rng), b = unit(rng);  // its visible-texture coordinates
        const float X = cx + sx * (a - 0.5f) * ew, Z = cz + (b - 0.5f) * eh;
        // where the base has it, and where the window samples for (a, b)
        const float ub = 0.5f + sx * (X - bx) / (ew * scaleX), vb = 0.5f + (Z - bz) / (eh * scaleY);
        const float us = w.u0 + a * (w.u1 - w.u0), vs = w.v0 + b * (w.v1 - w.v0);
        CHECK(std::fabs(ub - us) < 1e-4f && std::fabs(vb - vs) < 1e-4f);
        ++checked;
    }
    // no drift: the window is the base's middle
    const Window mid = window(10.f, 20.f, 10.f, 20.f, 1000.f, 800.f, 1.5f, 1.5f, false);
    CHECK(std::fabs(mid.u0 - 1.f / 6.f) < 1e-6f && std::fabs(mid.u1 - 5.f / 6.f) < 1e-6f);
    CHECK(std::fabs(mid.v0 - 1.f / 6.f) < 1e-6f && std::fabs(mid.v1 - 5.f / 6.f) < 1e-6f);
    // +X moves the window right, or left when mirrored; +Z moves it down
    CHECK(window(110.f, 20.f, 10.f, 20.f, 1000.f, 800.f, 1.5f, 1.5f, false).u0 > mid.u0);
    CHECK(window(110.f, 20.f, 10.f, 20.f, 1000.f, 800.f, 1.5f, 1.5f, true).u0 < mid.u0);
    CHECK(window(10.f, 120.f, 10.f, 20.f, 1000.f, 800.f, 1.5f, 1.5f, false).v0 > mid.v0);
    CHECK(base_size(192) == 288 && base_size(100) == 150 && base_size(101) == 152);
    std::printf("minimap_pan_test: %d windows, %s\n", checked, g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
