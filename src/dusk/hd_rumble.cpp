// aleks2 (V1-rc16): see hd_rumble.hpp. Switch only (CMakeLists.txt).
#include "dusk/hd_rumble.hpp"

#include "dusk/hd_rumble_core.hpp"
#include "dusk/settings.h"

#include <aurora/hd_rumble.h>
#include <switch.h>

#include <algorithm>
#include <chrono>

namespace dusk::hd_rumble {
namespace {

using clock = std::chrono::steady_clock;

Tracker s_tracker;
bool s_active = false;        // as of the last tick
bool s_playing = false;       // something may be playing (a stop is owed)
clock::time_point s_lastRun{};
int s_gainPercent = -1;
bool s_background = false;

void send(int layer, const Envelope& e) {
    AuroraHdKey keys[kMaxKeys];
    for (int i = 0; i < e.count; ++i) {
        const Key& k = e.keys[static_cast<size_t>(i)];
        keys[i] = {k.ms, k.ampLow, k.freqLow, k.ampHigh, k.freqHigh, k.pan};
    }
    aurora_hd_rumble_play(layer, keys, e.count, e.loop);
    s_playing = true;
}

void apply(const Command& c) {
    if (c.stopAll) {
        if (s_playing) {
            aurora_hd_rumble_stop();
            s_playing = false;
        }
        return;
    }
    if (c.playShot) {
        send(AURORA_HD_LAYER_SHOT, c.shot);
    }
    if (c.playLoop) {
        send(AURORA_HD_LAYER_LOOP, c.loop);
    } else if (c.stopLoop) {
        aurora_hd_rumble_play(AURORA_HD_LAYER_LOOP, nullptr, 0, false);
    }
}

}  // namespace

bool active() {
    return s_active;
}

void vibration_frame(int shockMode, int shockFrame, int quakeMode, int quakeFrame) {
    s_lastRun = clock::now();
    if (!s_active || s_background) {
        return;
    }
    apply(s_tracker.frame({shockMode, shockFrame, quakeMode, quakeFrame}));
}

void stop() {
    apply(s_tracker.stop());
}

void tick() {
    aurora_player_one_tick(); const auto& game = getSettings().game;  // aleks2 (rc17): rumble and six-axis follow player 1
    const int percent = std::clamp(game.rumbleIntensity.getValue(), 0, 100);
    if (percent != s_gainPercent) {
        s_gainPercent = percent;
        aurora_rumble_set_gain(static_cast<float>(percent) / 100.0f);
    }
    const bool wasActive = s_active;
    s_active = game.hdRumble.getValue() && aurora_hd_rumble_available(0);
    s_background = appletGetFocusState() != AppletFocusState_InFocus;
    // Turned off, the pad changed to one that is not the console's, HOME, or no vibration frame for a while (the game
    // does not run dVibration_c during loads and pause menus): nothing may keep buzzing.
    if ((wasActive && !s_active) || s_background ||
        (s_tracker.quake_on() && clock::now() - s_lastRun > std::chrono::milliseconds{250})) {
        stop();
    }
}

}  // namespace dusk::hd_rumble
