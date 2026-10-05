#pragma once

// Internal-resolution setting values (game.internalResolutionScale).
//
//   0        auto: the size follows the display (see switch_display.cpp)
//   1..12    scale the native render size by that factor (upstream's meaning)
//   13..18   an exact height in rows, from the table below
//
// The exact heights exist because a factor cannot name the same picture on both
// display modes: 0.75 of the panel is 540 rows handheld and 810 docked.
namespace dusk::ladder {

inline constexpr int kExactHeights[] = {540, 720, 900, 1080, 1440, 2160};

inline constexpr int kFirstExact = 13;
inline constexpr int kLastValue = kFirstExact + static_cast<int>(sizeof(kExactHeights) / sizeof(kExactHeights[0])) - 1;

// The height a setting value stands for, or 0 if the value is not an exact one.
constexpr int exact_height(int value) {
    if (value < kFirstExact || value > kLastValue) {
        return 0;
    }
    return kExactHeights[value - kFirstExact];
}

// The setting value for an exact height, or 0 if the table has no such height.
constexpr int exact_value(int height) {
    for (int value = kFirstExact; value <= kLastValue; ++value) {
        if (kExactHeights[value - kFirstExact] == height) {
            return value;
        }
    }
    return 0;
}

}  // namespace dusk::ladder
