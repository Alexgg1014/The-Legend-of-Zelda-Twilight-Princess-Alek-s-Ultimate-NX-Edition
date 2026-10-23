#pragma once
// aleks2 (V1-rc16): HD Rumble -- what the game's vibration becomes on the Switch's own controllers. Pure: no settings,
// no clocks, no hardware. hd_rumble.cpp feeds it the state of dVibration_c once per game frame and sends what it asks
// for through aurora/hd_rumble.h; platforms/switch/tools/hd_rumble_test.cpp checks it on a host.
//
// The game drives a GameCube motor: on or off, frame by frame, from a bit pattern per vibration "mode" -- a shock
// (VIBMODE_S_POWER1..8: a hit, stronger with the number), a quake (VIBMODE_Q_POWER1..8: a sustained rumble), the horse
// gallop (VIBMODE_Q_HORSE), a heartbeat-like double thump (VIBMODE_S_DOKUTT), a short cue for 2D screens
// (VIBMODE_S_FOR2D) and custom patterns. Here each mode gets an envelope the Switch's actuators can play: a hit is a
// short, sharp burst in the high band with some body in the low band, getting lower and longer as it gets stronger;
// the strongest (a big impact, a boss's stomp) are a low thud with a tail; a quake rolls in the low band; the gallop
// alternates left and right. Shocks play once (layer "shot"); quakes loop (layer "loop") until the game stops them.
#include <array>
#include <cstdint>

namespace dusk::hd_rumble {

struct Key {
    uint16_t ms;
    float ampLow, freqLow, ampHigh, freqHigh, pan;
};

constexpr int kMaxKeys = 8;
struct Envelope {
    std::array<Key, kMaxKeys> keys{};
    int count = 0;
    bool loop = false;
};

// The modes, as d_vibration.h numbers them.
constexpr int kShockNone = -1, kShockCustom = 0, kShockPower1 = 1, kShockPower8 = 8, kShockDokutt = 9, kShockFor2D = 10;
constexpr int kQuakeNone = -1, kQuakeCustom = 0, kQuakePower1 = 1, kQuakePower8 = 8, kQuakeHorse = 9;

inline Envelope shock_envelope(int mode) {
    Envelope e;
    auto add = [&e](uint16_t ms, float al, float fl, float ah, float fh, float pan = 0.f) {
        if (e.count < kMaxKeys) {
            e.keys[e.count++] = {ms, al, fl, ah, fh, pan};
        }
    };
    if (mode == kShockFor2D) {
        add(18, 0.05f, 160.f, 0.30f, 320.f);  // a light tick for menus and 2D screens
        add(10, 0.f, 160.f, 0.f, 320.f);
        return e;
    }
    if (mode == kShockDokutt) {
        add(60, 0.55f, 90.f, 0.10f, 200.f);  // two thumps
        add(70, 0.f, 90.f, 0.f, 200.f);
        add(60, 0.40f, 85.f, 0.08f, 200.f);
        add(40, 0.f, 85.f, 0.f, 200.f);
        return e;
    }
    // Power 1..8 (a custom pattern plays as a middle one).
    const int n = mode >= kShockPower1 && mode <= kShockPower8 ? mode : 4;
    const float s = static_cast<float>(n) / 8.0f;  // 0.125 .. 1
    if (n >= 7) {
        // A big impact or a stomp: a low thud and a tail that dies away.
        add(40, 0.75f + 0.2f * s, 75.f, 0.25f, 180.f);
        add(80, 0.55f, 65.f, 0.08f, 160.f);
        add(140, 0.25f, 55.f, 0.f, 160.f);
        add(60, 0.f, 50.f, 0.f, 160.f);
        return e;
    }
    // A hit: sharp at the start, high band first, lower and longer as it gets stronger.
    add(static_cast<uint16_t>(20 + 10 * n), 0.15f + 0.5f * s, 180.f - 60.f * s, 0.35f + 0.45f * s, 320.f - 40.f * s);
    add(static_cast<uint16_t>(30 + 15 * n), 0.08f + 0.3f * s, 150.f - 50.f * s, 0.10f + 0.15f * s, 280.f);
    add(20, 0.f, 120.f, 0.f, 260.f);
    return e;
}

inline Envelope quake_envelope(int mode) {
    Envelope e;
    e.loop = true;
    auto add = [&e](uint16_t ms, float al, float fl, float ah, float fh, float pan = 0.f) {
        if (e.count < kMaxKeys) {
            e.keys[e.count++] = {ms, al, fl, ah, fh, pan};
        }
    };
    if (mode == kQuakeHorse) {
        // The gallop: a beat on the left, one on the right.
        add(70, 0.45f, 110.f, 0.10f, 220.f, -0.8f);
        add(130, 0.f, 110.f, 0.f, 220.f, -0.8f);
        add(70, 0.45f, 110.f, 0.10f, 220.f, 0.8f);
        add(130, 0.f, 110.f, 0.f, 220.f, 0.8f);
        return e;
    }
    const int n = mode >= kQuakePower1 && mode <= kQuakePower8 ? mode : 4;
    const float s = static_cast<float>(n) / 8.0f;
    // A rolling low rumble: the amplitude swells and eases, the pitch wanders a little.
    add(120, 0.12f + 0.45f * s, 70.f, 0.03f + 0.05f * s, 160.f);
    add(120, 0.20f + 0.55f * s, 62.f, 0.05f + 0.07f * s, 170.f);
    add(120, 0.10f + 0.40f * s, 78.f, 0.03f + 0.05f * s, 150.f);
    add(120, 0.18f + 0.50f * s, 66.f, 0.04f + 0.06f * s, 165.f);
    return e;
}

// What to do this frame.
struct Command {
    bool playShot = false;
    Envelope shot{};
    bool playLoop = false;  // start (or change) the loop
    Envelope loop{};
    bool stopLoop = false;
    bool stopAll = false;
};

// One frame of dVibration_c's motor state, after its Run(): the shock's mode and frame (frame 1 = it started this frame;
// negative = not running) and the quake's.
struct MotorState {
    int shockMode = kShockNone;
    int shockFrame = -99;
    int quakeMode = kQuakeNone;
    int quakeFrame = -99;
};

class Tracker {
public:
    Command frame(const MotorState& m) {
        Command c;
        if (m.shockMode != kShockNone && m.shockFrame == 1) {
            c.playShot = true;
            c.shot = shock_envelope(m.shockMode);
        }
        const bool quake = m.quakeMode != kQuakeNone && m.quakeFrame >= 0;
        if (quake && (!mQuakeOn || m.quakeMode != mQuakeMode)) {
            c.playLoop = true;
            c.loop = quake_envelope(m.quakeMode);
        } else if (!quake && mQuakeOn) {
            c.stopLoop = true;
        }
        mQuakeOn = quake;
        mQuakeMode = quake ? m.quakeMode : kQuakeNone;
        return c;
    }
    // Paused, killed, the game not running its vibration any more (a load, a menu), or the app in the background.
    Command stop() {
        mQuakeOn = false;
        mQuakeMode = kQuakeNone;
        Command c;
        c.stopAll = true;
        return c;
    }
    bool quake_on() const { return mQuakeOn; }

private:
    bool mQuakeOn = false;
    int mQuakeMode = kQuakeNone;
};

}  // namespace dusk::hd_rumble
