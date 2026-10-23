#pragma once

// aleks2 H38: the Wii-style gestures as a pure detector -- no hardware, no game state, no clock of its own.
//
// Twilight Princess on the Wii reads three motions from the Wii Remote and Nunchuk accelerometers
// (src/m_Re/m_Re_controller_pad.cpp, Pad::Acc::calcSttsRem / calcSttsFs, getTrigSwing / getFSTrigSwing):
//   * shaking the Wii Remote swings the sword;
//   * moving the Nunchuk sideways is the spin attack;
//   * pushing the Nunchuk forward raises the shield (shield bash, when the game allows it).
// On the Switch a detached Joy-Con pair stands in: by default right = Wii Remote, left = Nunchuk (Motion Sword Hand can
// swap them). One controller with one sensor (Pro Controller, handheld, a lone Joy-Con) can only swing the sword.
//
// H38-c3 (after the first console test lost about one swing in three and could not tell a chop from a slash):
//   * Gravity is followed with the gyroscope, not by waiting for stillness. A wrist roll rotates the gravity vector in
//     the sensor frame; the old detector read that as acceleration and stayed disarmed until its slow estimate caught
//     up. Here the estimate is rotated by the measured angular velocity, so "dynamic" acceleration is the hand's own.
//   * A gesture is a WINDOW (<= 120 ms) of samples, not the first sample over a threshold: its direction is read from
//     the energy per axis and the sign of the rotation, never from the net integral of acceleration (acceleration and
//     braking cancel in a whole stroke) and not from the first peak (which can be the reaction).
//   * The sword's gesture is Vertical, LeftToRight, RightToLeft or Generic (strong but ambiguous: it still attacks).
//   * Everything that depends on the console's axes is a validated Tuning value, so it can be corrected from config.json.
//
// V1-rc7 (after the rc5/rc6 console tests): raising the controller to wind up fired the attack and the chop that followed
// fell in the cooldown; the return stroke right after a sideways one was often lost; a gyro flick alone could open a
// window; there was no thrust.
//   * A window opens on acceleration: |dyn| >= half the threshold and |dyn|/thr + gyroWeight * |w|/gyroThr >= 1. The
//     gyroscope helps, it never opens one by itself.
//   * Wind-up filter: a vertical window whose first vertical peak is upward (raising the arm) is not an attack, and the
//     detector re-arms at once so the chop that follows counts.
//   * The cooldown is a Tuning value (0.12 s by default, was 0.25), and after a left/right stroke the detector re-arms as
//     soon as it ends, without waiting for the hand to be still: the return stroke counts.
//   * Thrust: horizontal energy with little rotation (gyro peak and swept yaw low) is a thrust (the game's stab), not a
//     generic swing; optionally along one sensor axis.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace dusk::wii {

constexpr float kGravity = 9.80665f;  // m/s^2 per g; Aurora delivers m/s^2

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
    float& at(int i) { return i == 0 ? x : i == 1 ? y : z; }
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float norm(Vec3 a) { return std::sqrt(dot(a, a)); }

// One motion sample: accelerometer in m/s^2 including gravity, gyroscope in rad/s, its time in seconds (any fixed origin,
// increasing). `gap` says samples were lost just before it (Aurora's kMotionGapBefore).
struct Sample {
    double t = 0.0;
    Vec3 accel;
    Vec3 gyro;
    bool gap = false;
};

// What the active controller offers.
enum class Layout {
    None,          // no usable sensor
    Single,        // one sensor: Pro Controller, handheld, a lone Joy-Con (sword only)
    DetachedPair,  // two Joy-Con: a sword hand and a Nunchuk hand
};

// How the sword was swung. Generic = strong enough to be an attack but not clearly one direction.
enum class SwordGesture { None, Generic, Vertical, LeftToRight, RightToLeft, Thrust };

inline const char* gesture_name(SwordGesture g) {
    switch (g) {
    case SwordGesture::Generic: return "generic";
    case SwordGesture::Vertical: return "vertical";
    case SwordGesture::LeftToRight: return "left-to-right";
    case SwordGesture::RightToLeft: return "right-to-left";
    case SwordGesture::Thrust: return "thrust";
    default: return "none";
    }
}

enum class RejectReason { None, Short, RotationOnly, Gap, Ambiguous, Windup, Rebound };

inline const char* reject_name(RejectReason r) {
    switch (r) {
    case RejectReason::Short: return "short";
    case RejectReason::RotationOnly: return "rotation-only";
    case RejectReason::Gap: return "gap";
    case RejectReason::Ambiguous: return "ambiguous";
    case RejectReason::Windup: return "windup";
    case RejectReason::Rebound: return "rebound";
    default: return "none";
    }
}

// For each axis gravity may lie along while a Joy-Con is held as a Nunchuk (index = gravity axis), which of the other two
// axes is lateral and which is forward. Joy-Con axes in libnx: x, y in the plane of the face, z out of it. The optional
// overrides (-1 = use the table) are the config.json values; forwardSign 0 accepts a push in either direction.
struct SideAxes {
    int lateralTable[3] = {1, 0, 0};
    int forwardTable[3] = {2, 2, 1};
    int lateralOverride = -1;
    int forwardOverride = -1;
    int forwardSign = 0;  // +1: the push must start toward +axis, -1 toward -axis, 0: either
    int lateral(int gravityAxis) const {
        return valid_override(lateralOverride, gravityAxis) ? lateralOverride : lateralTable[gravityAxis];
    }
    int forward(int gravityAxis) const {
        const int f = valid_override(forwardOverride, gravityAxis) ? forwardOverride : forwardTable[gravityAxis];
        return f == lateral(gravityAxis) ? forwardTable[gravityAxis] : f;
    }

private:
    static bool valid_override(int v, int gravityAxis) { return v >= 0 && v <= 2 && v != gravityAxis; }
};

namespace tuning {
constexpr float kMinSensitivity = 0.25f;
constexpr float kMaxSensitivity = 4.0f;

// Defaults and the bounds config.json values are held to.
constexpr float kAccelThresholdG = 1.3f;  // dynamic acceleration that opens a gesture window (the original's ~1.3)
constexpr float kAccelMinG = 0.6f, kAccelMaxG = 3.0f;
constexpr float kGyroThresholdRad = 3.0f;  // angular speed that opens a window (about 170 deg/s)
constexpr float kGyroMinRad = 1.0f, kGyroMaxRad = 12.0f;
constexpr float kDominance = 1.5f;  // amplitude ratio between the winning axis and the other to call a direction
constexpr float kDominanceMin = 1.1f, kDominanceMax = 3.0f;
constexpr float kMinYawRad = 0.25f;  // swept angle about the vertical for a left/right call (about 14 degrees)
constexpr float kMinYawMin = 0.05f, kMinYawMax = 1.5f;
constexpr float kGyroWeight = 0.5f;  // V1-rc7: how much the gyroscope adds to the acceleration that opens a window
constexpr float kGyroWeightMin = 0.0f, kGyroWeightMax = 1.0f;
constexpr float kMinOpenAccelShare = 0.5f;  // a window never opens below this share of the acceleration threshold
constexpr float kCooldownDefaultS = 0.12f;
constexpr float kCooldownMinS = 0.05f, kCooldownMaxS = 0.5f;
constexpr float kThrustMaxGyroRad = 4.0f;  // a thrust turns the controller less than this (V1-rc8: was 2.5;
                                           // a jab "without moving" peaked at 2.6-4.9 rad/s on the console)
constexpr float kThrustMaxGyroMin = 0.5f, kThrustMaxGyroMax = 12.0f;
constexpr float kThrustAxisShare = 0.6f;   // with a thrust axis set: its share of the energy
// V1-rc8 (rc7 console logs): raising the arm was often "generic" -- it rose first by 2-5 g, but its vertical energy
// was only 1.05-1.46 times the horizontal, under the 1.5 dominance, so the wind-up rule (vertical windows only) never
// saw it. A window that rises first by more than this, with vertical energy at least kWindupShare of the horizontal,
// is a wind-up whatever dominates.
constexpr float kWindupMinG = 1.0f;
constexpr float kWindupShare = 0.6f;
// V1-rc9 (rc8 console, log 111711): both wind-up rules ate real attacks -- the 19 windows rejected averaged 4.2 g and
// 21.9 rad/s, 18 of them over 14. Raising the arm is a weak movement; only a window under both of these is a wind-up
// (4 g: the synthetic rises of wii_gestures_test peak at 3 g; the gyro limit is what separates the console data).
constexpr float kWindupMaxG = 4.0f, kWindupMaxGMin = 1.0f, kWindupMaxGMax = 8.0f;
constexpr float kWindupMaxGyroRad = 12.0f, kWindupMaxGyroMin = 2.0f, kWindupMaxGyroMax = 40.0f;
// V1-rc8: one swing gave two attacks: the arm coming back right after. A window closing within kReboundWindowS of an
// accepted one, with less than kReboundRatio of its acceleration peak, is that rebound (a deliberate return stroke
// is about as strong as the first).
constexpr double kReboundWindowS = 0.35;
constexpr float kReboundRatio = 0.6f;

constexpr double kWindowMinS = 0.06;     // a window never closes before this
constexpr double kWindowMaxS = 0.12;     // and always has by this
constexpr double kQuietCloseS = 0.025;   // quiet this long (after the minimum) closes it
constexpr double kMinActiveS = 0.015;    // time above half the thresholds: shorter is a tap or vibration
constexpr double kCooldownS = 0.25;      // after a window: the original's lock of ~10 samples, with room for the return stroke
constexpr double kRearmQuietS = 0.03;    // quiet this long (and past the cooldown) re-arms
constexpr double kRearmFailsafeS = 0.5;  // re-arms by time alone this long after the cooldown: never stuck
constexpr double kGapHoldoffS = 0.15;    // after lost samples nothing starts
constexpr double kGravityTauS = 0.25;    // gyro-aided estimate: correction toward the accelerometer when it reads ~1 g
constexpr double kPlainGravityTauS = 0.6;  // no usable gyro: the old slow low-pass
constexpr double kWarmupS = 0.3;           // after a reset: adapt fast to the resting pose, fire nothing
constexpr double kWarmupTauS = 0.08;
constexpr float kAimedAtOneG = 0.12f;      // | |a| - 1 g | below this: the accelerometer is mostly gravity
constexpr float kQuietGyroRad = 1.5f;      // below this angular speed gravity may be corrected from the accelerometer
constexpr float kRotationOnlyAccelRatio = 0.7f;  // a window whose accel peak is below this share of the threshold is aiming
constexpr float kPlanarShare = 0.36f;      // Nunchuk: share of the energy that must lie in the lateral+forward plane (0.6^2)
constexpr double kStrokeWindowS = 0.07;    // the first part of a window decides which way a push started
}  // namespace tuning

// The values a console test may need to correct. Raw = as read from config.json (anything), Tuning = held to its range.
struct RawTuning {
    float accelThresholdG = tuning::kAccelThresholdG;
    float gyroThresholdRad = tuning::kGyroThresholdRad;
    float dominance = tuning::kDominance;
    float minYawRad = tuning::kMinYawRad;
    float gyroWeight = tuning::kGyroWeight;
    float cooldownS = tuning::kCooldownDefaultS;
    bool windupFilter = true;
    float windupMaxG = tuning::kWindupMaxG;
    float windupMaxGyroRad = tuning::kWindupMaxGyroRad;
    float thrustMaxGyroRad = tuning::kThrustMaxGyroRad;
    int thrustAxis = -1;  // -1: any horizontal direction; 0..2: the sensor axis a thrust runs along
    int lateralAxis[2] = {-1, -1};  // [0] left Joy-Con, [1] right Joy-Con; -1 = the built-in table
    int forwardAxis[2] = {-1, -1};
    int forwardSign[2] = {0, 0};
};

struct Tuning {
    float accelThresholdG = tuning::kAccelThresholdG;
    float gyroThresholdRad = tuning::kGyroThresholdRad;
    float dominance = tuning::kDominance;
    float minYawRad = tuning::kMinYawRad;
    float gyroWeight = tuning::kGyroWeight;
    float cooldownS = tuning::kCooldownDefaultS;
    bool windupFilter = true;
    float windupMaxG = tuning::kWindupMaxG;
    float windupMaxGyroRad = tuning::kWindupMaxGyroRad;
    float thrustMaxGyroRad = tuning::kThrustMaxGyroRad;
    int thrustAxis = -1;
    SideAxes axes[2];  // by physical side: [0] left Joy-Con, [1] right Joy-Con (the sensor acting as the Nunchuk)
};

inline float clamp_sensitivity(float s) {
    if (!std::isfinite(s)) {
        return 1.0f;
    }
    return s < tuning::kMinSensitivity ? tuning::kMinSensitivity
         : s > tuning::kMaxSensitivity ? tuning::kMaxSensitivity
                                       : s;
}

// Holds every value to its range: one that is missing (default), not a number or out of range is replaced by the default
// and named in `notes` (one short phrase each), never clamped silently. Axes: -1..2, signs -1..1, lateral != forward.
inline Tuning sanitize(const RawTuning& raw, std::string* notes = nullptr) {
    Tuning out;
    const auto note = [&](const char* name, float got, float use) {
        if (notes != nullptr) {
            *notes += std::string(name) + "=" + std::to_string(got) + " is outside its range, using " +
                      std::to_string(use) + "; ";
        }
    };
    const auto pick = [&](const char* name, float got, float lo, float hi, float def) {
        if (std::isfinite(got) && got >= lo && got <= hi) {
            return got;
        }
        note(name, got, def);
        return def;
    };
    out.accelThresholdG = pick("wiiAccelThresholdG", raw.accelThresholdG, tuning::kAccelMinG, tuning::kAccelMaxG,
        tuning::kAccelThresholdG);
    out.gyroThresholdRad = pick("wiiGyroThresholdRad", raw.gyroThresholdRad, tuning::kGyroMinRad, tuning::kGyroMaxRad,
        tuning::kGyroThresholdRad);
    out.dominance = pick("wiiDirectionDominance", raw.dominance, tuning::kDominanceMin, tuning::kDominanceMax,
        tuning::kDominance);
    out.minYawRad = pick("wiiMinYawRad", raw.minYawRad, tuning::kMinYawMin, tuning::kMinYawMax, tuning::kMinYawRad);
    out.gyroWeight = pick("wiiGyroWeight", raw.gyroWeight, tuning::kGyroWeightMin, tuning::kGyroWeightMax,
        tuning::kGyroWeight);
    out.cooldownS = pick("wiiCooldownS", raw.cooldownS, tuning::kCooldownMinS, tuning::kCooldownMaxS,
        tuning::kCooldownDefaultS);
    out.windupFilter = raw.windupFilter;
    out.windupMaxG = pick("wiiWindupMaxG", raw.windupMaxG, tuning::kWindupMaxGMin, tuning::kWindupMaxGMax,
        tuning::kWindupMaxG);
    out.windupMaxGyroRad = pick("wiiWindupMaxGyroRad", raw.windupMaxGyroRad, tuning::kWindupMaxGyroMin,
        tuning::kWindupMaxGyroMax, tuning::kWindupMaxGyroRad);
    out.thrustMaxGyroRad = pick("wiiThrustMaxGyroRad", raw.thrustMaxGyroRad, tuning::kThrustMaxGyroMin,
        tuning::kThrustMaxGyroMax, tuning::kThrustMaxGyroRad);
    if (raw.thrustAxis >= -1 && raw.thrustAxis <= 2) {
        out.thrustAxis = raw.thrustAxis;
    } else if (notes != nullptr) {
        *notes += "wiiThrustAxis=" + std::to_string(raw.thrustAxis) + " is outside -1..2, using -1; ";
    }
    for (int side = 0; side < 2; ++side) {
        const char* label = side == 0 ? "left" : "right";
        SideAxes& a = out.axes[side];
        const auto axis = [&](const char* what, int got) {
            if (got >= -1 && got <= 2) {
                return got;
            }
            if (notes != nullptr) {
                *notes += std::string("wiiAxis") + what + "(" + label + ")=" + std::to_string(got) +
                          " is outside -1..2, using -1; ";
            }
            return -1;
        };
        a.lateralOverride = axis("Lateral", raw.lateralAxis[side]);
        a.forwardOverride = axis("Forward", raw.forwardAxis[side]);
        if (a.lateralOverride >= 0 && a.lateralOverride == a.forwardOverride) {
            if (notes != nullptr) {
                *notes += std::string("wiiAxis lateral and forward (") + label + ") are the same axis, using the table; ";
            }
            a.lateralOverride = a.forwardOverride = -1;
        }
        const int sign = raw.forwardSign[side];
        if (sign < -1 || sign > 1) {
            if (notes != nullptr) {
                *notes += std::string("wiiForwardSign(") + label + ")=" + std::to_string(sign) + " is outside -1..1, using 0; ";
            }
            a.forwardSign = 0;
        } else {
            a.forwardSign = sign;
        }
    }
    return out;
}

// Whether the gyroscope turns the same way as the accelerometer's frame. libnx does not say; it is found from the data. A
// vector fixed in the world changes in the sensor's frame at -omega x v. In blocks of 10 samples (50 ms) in which the
// controller turns (more than 1 rad/s on average) while the accelerometer reads about 1 g throughout (so the change is the
// pose's, not the hand's), the reading at the start is rotated through the block's gyro samples both ways and compared with
// the reading at the end; the way that lands closer collects the difference. Until the evidence is clear the gyro is not
// used to rotate gravity and left/right is not called.
class Handedness {
public:
    // +1 / -1 once settled, 0 while unknown.
    int sign() const { return mSign; }

    void feed(const Vec3& aG, const Vec3& w, double dt, bool contiguous) {
        if (!contiguous || dt <= 0.0) {
            mN = 0;
        }
        if (mN == 0) {
            mStart = {};
            mEnd = {};
            mWSum = 0.0f;
            mOk = true;
        }
        if (mN < 3) {
            mStart = mStart + aG;
            if (mN == 2) {
                mP[0] = mP[1] = mStart * (1.0f / 3.0f);
            }
        } else {
            for (int k = 0; k < 2; ++k) {
                const float h = k == 0 ? 1.0f : -1.0f;
                const float len = norm(mP[k]);
                mP[k] = mP[k] - cross(w, mP[k]) * (h * static_cast<float>(dt));
                const float nl = norm(mP[k]);
                if (nl > 1e-6f) {
                    mP[k] = mP[k] * (len / nl);
                }
            }
        }
        if (mN >= 8) {
            mEnd = mEnd + aG;
        }
        mWSum += norm(w);
        mOk = mOk && std::fabs(norm(aG) - 1.0f) < 0.25f;
        if (++mN == kBlock) {
            mN = 0;
            const float meanW = mWSum / static_cast<float>(kBlock);
            if (mOk && meanW > 1.0f && meanW < 10.0f) {
                const Vec3 end = mEnd * 0.5f;
                mEvidence = mEvidence * 0.98f + (norm(end - mP[1]) - norm(end - mP[0]));
                if (mEvidence > 0.2f) {
                    mSign = 1;
                } else if (mEvidence < -0.2f) {
                    mSign = -1;
                }
            }
        }
    }
    void break_continuity() { mN = 0; }
    void reset() { *this = Handedness{}; }

private:
    static constexpr int kBlock = 10;
    Vec3 mStart, mEnd, mP[2];
    float mWSum = 0.0f;
    float mEvidence = 0.0f;
    int mN = 0;
    int mSign = 0;
    bool mOk = true;
};

// What a closed window measured, in g, rad/s, seconds.
struct Features {
    double duration = 0.0;
    double activeTime = 0.0;  // time above half the start thresholds
    float accelPeakG = 0.0f;
    float gyroPeakRad = 0.0f;
    float energyVertical = 0.0f;    // integral of (dyn . up)^2 dt
    float energyHorizontal = 0.0f;  // integral of |dyn - (dyn . up) up|^2 dt
    float energyAxis[3] = {};       // integral of dyn[k]^2 dt, in the sensor's axes
    float firstPeak[3] = {};        // signed dyn[k] at its largest |.| in the first part of the window
    float firstVert = 0.0f;         // V1-rc7: signed dyn . up at its largest |.| in the first part (> 0: it rose first)
    float vertPeakPos = 0.0f, vertPeakNeg = 0.0f;  // signed extremes along "up"
    float yaw = 0.0f;               // angle swept about the vertical, rad (counter-clockwise seen from above is positive)
    float yawPeak = 0.0f;           // signed angular speed about the vertical at its largest |.|
    int gravityAxis = 0;
    bool handednessKnown = false;
    bool sawGap = false;
};

// One sensor's window detector.
class MotionTracker {
public:
    struct Result {
        bool closed = false;
        bool aborted = false;  // a gap cut the window
        Features f;
    };

    void reset() { *this = MotionTracker{}; }
    bool warmed_up(double t) const { return mSeeded && t >= mWarmupUntil; }
    bool capturing() const { return mPhase == Phase::Capturing; }

    // The largest dynamic magnitude (g) seen outside windows since the last call, for telemetry of near misses.
    float take_idle_peak() {
        const float p = mIdlePeak;
        mIdlePeak = 0.0f;
        return p;
    }

    // V1-rc7: re-arm now (a wind-up was rejected: the chop after it must count).
    void rearm_now(double t) {
        mArmed = true;
        mCooldownUntil = t;
        mQuietFor = 0.0;
        mQuickRearm = false;
    }
    // V1-rc7: re-arm as soon as the cooldown ends, without waiting for stillness (after a left/right stroke).
    void quick_rearm() { mQuickRearm = true; }

    Result feed(const Sample& s, float accelThrG, float gyroThrRad, Handedness& hand, float gyroWeight = 0.0f,
        double cooldownS = tuning::kCooldownS) {
        Result out;
        const Vec3 aG = s.accel * (1.0f / kGravity);
        if (!mSeeded) {
            mSeeded = true;
            mG = aG;
            mLast = s.t;
            mWarmupUntil = s.t + tuning::kWarmupS;
            hand.feed(aG, s.gyro, 0.0, false);
            return out;
        }
        double dt = s.t - mLast;
        mLast = s.t;
        const bool broken = s.gap || dt > 0.05 || dt < 0.0;
        if (broken) {
            hand.break_continuity();
            mHoldoffUntil = s.t + tuning::kGapHoldoffS;
            if (mPhase == Phase::Capturing) {
                mPhase = Phase::Idle;
                mArmed = false;
                mCooldownUntil = s.t + tuning::kGapHoldoffS;
                out.closed = true;
                out.aborted = true;
                out.f.sawGap = true;
                mLast = s.t;
                return out;
            }
            dt = 0.005;
        }
        dt = std::clamp(dt, 0.0005, 0.02);
        hand.feed(aG, s.gyro, dt, !broken);

        // Follow gravity: rotate the estimate with the gyroscope when its handedness is known, then correct it toward the
        // accelerometer only while that reads about 1 g and the controller is turning slowly.
        const int h = hand.sign();
        if (h != 0) {
            const float len = norm(mG);
            mG = mG - cross(s.gyro, mG) * (static_cast<float>(h) * static_cast<float>(dt));
            const float nl = norm(mG);
            if (nl > 1e-6f && len > 0.0f) {
                mG = mG * (len / nl);
            }
        }
        const bool warming = s.t < mWarmupUntil;
        const float gyroMag = norm(s.gyro);
        const Vec3 dyn = aG - mG;
        const float mag = norm(dyn);
        const bool mostlyGravity = std::fabs(norm(aG) - 1.0f) < tuning::kAimedAtOneG;

        if (mPhase == Phase::Idle) {
            if (!warming && mag > mIdlePeak) {
                mIdlePeak = mag;
            }
            if (!warming && s.t >= mHoldoffUntil) {
                if (mArmed) {
                    // V1-rc7: acceleration opens a window; the gyroscope only adds to it (see the top of the file).
                    if (mag >= accelThrG * tuning::kMinOpenAccelShare &&
                        mag / accelThrG + gyroWeight * gyroMag / gyroThrRad >= 1.0f)
                    {
                        open_window(s.t);
                    }
                } else if (s.t >= mCooldownUntil && mQuickRearm) {
                    mArmed = true;
                    mQuickRearm = false;
                    mQuietFor = 0.0;
                } else if (s.t >= mCooldownUntil) {
                    if (mag < accelThrG * 0.4f && gyroMag < gyroThrRad * 0.5f) {
                        mQuietFor += dt;
                    } else {
                        mQuietFor = 0.0;
                    }
                    bool rearm = mQuietFor >= tuning::kRearmQuietS;
                    if (!rearm && s.t >= mCooldownUntil + tuning::kRearmFailsafeS && mostlyGravity) {
                        // Not quiet for long although the accelerometer reads ~1 g: the gravity estimate is stale. Take
                        // it from the reading and re-arm. A real long push (far from 1 g) never gets here.
                        mG = aG;
                        rearm = true;
                    }
                    if (rearm) {
                        mArmed = true;
                        mQuietFor = 0.0;
                    }
                }
            }
        }
        if (mPhase == Phase::Capturing) {
            accumulate(dyn, s.gyro, mag, gyroMag, dt, hand.sign(), accelThrG, gyroThrRad);
            const bool low = mag < accelThrG * 0.25f && gyroMag < gyroThrRad * 0.25f;
            mLowFor = low ? mLowFor + dt : 0.0;
            const bool close = mF.duration >= tuning::kWindowMaxS ||
                               (mF.duration >= tuning::kWindowMinS && mLowFor >= tuning::kQuietCloseS);
            if (close) {
                mPhase = Phase::Idle;
                mArmed = false;
                mCooldownUntil = s.t + cooldownS;
                mQuietFor = 0.0;
                mQuickRearm = false;
                out.closed = true;
                out.f = mF;
                out.f.handednessKnown = hand.sign() != 0;
                out.f.gravityAxis = gravity_axis(mG);
            }
        } else {
            // Correct gravity (not during a window: a stroke would be absorbed into it).
            float rate;
            if (warming) {
                rate = static_cast<float>(1.0 - std::exp(-dt / tuning::kWarmupTauS));
            } else if (h != 0) {
                rate = (mostlyGravity && gyroMag < tuning::kQuietGyroRad)
                           ? static_cast<float>(1.0 - std::exp(-dt / tuning::kGravityTauS))
                           : 0.0f;
            } else {
                const bool still = mag < accelThrG * 0.4f;
                rate = static_cast<float>(1.0 - std::exp(-dt / (tuning::kPlainGravityTauS * (still ? 1.0 : 5.0))));
            }
            mG = mG + (aG - mG) * rate;
        }
        return out;
    }

    static int gravity_axis(const Vec3& g) {
        int axis = 0;
        float best = std::fabs(g.x);
        if (std::fabs(g.y) > best) {
            axis = 1;
            best = std::fabs(g.y);
        }
        if (std::fabs(g.z) > best) {
            axis = 2;
        }
        return axis;
    }

private:
    enum class Phase { Idle, Capturing };

    void open_window(double t) {
        mPhase = Phase::Capturing;
        mF = Features{};
        mF.vertPeakPos = 0.0f;
        mF.vertPeakNeg = 0.0f;
        mWindowStart = t;
        mLowFor = 0.0;
    }

    void accumulate(const Vec3& dyn, const Vec3& w, float mag, float gyroMag, double dt, int hand, float accelThr,
        float gyroThr) {
        const float dtf = static_cast<float>(dt);
        const float gn = norm(mG);
        const Vec3 up = gn > 1e-6f ? mG * (1.0f / gn) : Vec3{0.0f, 1.0f, 0.0f};
        const float av = dot(dyn, up);
        const Vec3 ah = dyn - up * av;
        mF.duration += dt;
        mF.energyVertical += av * av * dtf;
        mF.energyHorizontal += dot(ah, ah) * dtf;
        for (int k = 0; k < 3; ++k) {
            mF.energyAxis[k] += dyn[k] * dyn[k] * dtf;
            if (mF.duration <= tuning::kStrokeWindowS && std::fabs(dyn[k]) > std::fabs(mF.firstPeak[k])) {
                mF.firstPeak[k] = dyn[k];
            }
        }
        if (mF.duration <= tuning::kStrokeWindowS && std::fabs(av) > std::fabs(mF.firstVert)) {
            mF.firstVert = av;
        }
        mF.vertPeakPos = std::max(mF.vertPeakPos, av);
        mF.vertPeakNeg = std::min(mF.vertPeakNeg, av);
        mF.accelPeakG = std::max(mF.accelPeakG, mag);
        mF.gyroPeakRad = std::max(mF.gyroPeakRad, gyroMag);
        if (mag > accelThr * 0.5f || gyroMag > gyroThr * 0.5f) {
            mF.activeTime += dt;
        }
        // Rotation about the vertical, counter-clockwise seen from above is positive: with the accelerometer reading
        // "up", the right-hand rule about it, corrected by the gyro frame's handedness. Unknown handedness: no yaw.
        const float wv = hand != 0 ? dot(w, up) * static_cast<float>(hand) : 0.0f;
        mF.yaw += wv * dtf;
        if (std::fabs(wv) > std::fabs(mF.yawPeak)) {
            mF.yawPeak = wv;
        }
    }

    Vec3 mG;
    Features mF;
    double mLast = 0.0;
    double mWarmupUntil = 0.0;
    double mCooldownUntil = 0.0;
    double mHoldoffUntil = 0.0;
    double mWindowStart = 0.0;
    double mQuietFor = 0.0;
    double mLowFor = 0.0;
    float mIdlePeak = 0.0f;
    Phase mPhase = Phase::Idle;
    bool mSeeded = false;
    bool mArmed = true;
    bool mQuickRearm = false;
};

// The sword's gesture from a closed window. Returns None with a reason for a window that is not an attack.
struct SwordVerdict {
    SwordGesture gesture = SwordGesture::None;
    RejectReason reason = RejectReason::None;
};

inline SwordVerdict classify_sword(const Features& f, const Tuning& t, float accelThrG) {
    SwordVerdict v;
    if (f.sawGap) {
        v.reason = RejectReason::Gap;
        return v;
    }
    if (f.activeTime < tuning::kMinActiveS) {
        v.reason = RejectReason::Short;
        return v;
    }
    if (f.accelPeakG < accelThrG * tuning::kRotationOnlyAccelRatio) {
        v.reason = RejectReason::RotationOnly;  // turning the controller (aiming) without moving the hand
        return v;
    }
    const float sv = std::sqrt(f.energyVertical);
    const float sh = std::sqrt(f.energyHorizontal);
    // V1-rc9: a wind-up is weak; a strong window is an attack whichever way it started.
    const bool weak = f.accelPeakG < t.windupMaxG && f.gyroPeakRad < t.windupMaxGyroRad;
    if (t.windupFilter && weak && f.firstVert > tuning::kWindupMinG && sv >= tuning::kWindupShare * sh) {
        v.reason = RejectReason::Windup;  // V1-rc8: rising first, even when the vertical does not dominate
        return v;
    }
    v.gesture = SwordGesture::Generic;
    const float total = f.energyAxis[0] + f.energyAxis[1] + f.energyAxis[2];
    const bool thrustAxisOk =
        t.thrustAxis < 0 || (total > 0.0f && f.energyAxis[t.thrustAxis] >= tuning::kThrustAxisShare * total);
    if (sv >= t.dominance * sh) {
        // Energy along gravity, whichever way the stroke ran: down, up, or down-up-down. The net integral of the
        // acceleration is not used (acceleration and braking cancel); the signed extremes only say it moved both ways.
        // V1-rc7: one that rose first is the arm winding up, not an attack.
        if (t.windupFilter && weak && f.firstVert > 0.0f) {
            v.gesture = SwordGesture::None;
            v.reason = RejectReason::Windup;
            return v;
        }
        v.gesture = SwordGesture::Vertical;
    } else if (sh >= t.dominance * sv && f.gyroPeakRad < t.thrustMaxGyroRad && std::fabs(f.yaw) < t.minYawRad &&
               thrustAxisOk) {
        v.gesture = SwordGesture::Thrust;  // V1-rc7: straight ahead, the controller hardly turning
    } else if (f.handednessKnown &&
               ((sh >= t.dominance * sv && std::fabs(f.yaw) >= t.minYawRad &&
                    std::fabs(f.yawPeak) >= 0.35f * t.gyroThresholdRad) ||
                   // V1-rc9 (log 111711): a fast flick sweeps little net angle (0.12-0.2 rad at 5-7 rad/s), and a
                   // diagonal has neither axis dominant; the yaw rate says which way the hand went.
                   (std::fabs(f.yawPeak) >= t.gyroThresholdRad && std::fabs(f.yaw) >= 0.5f * t.minYawRad))) {
        const bool peakPositive = f.yawPeak > 0.0f;
        const bool netPositive = f.yaw > 0.0f;
        if (peakPositive == netPositive) {
            // Positive = counter-clockwise seen from above: the hand sweeps from the right to the left.
            v.gesture = peakPositive ? SwordGesture::RightToLeft : SwordGesture::LeftToRight;
        }
    }
    return v;
}

enum class NunchukGesture { None, Spin, ShieldBash };

inline NunchukGesture classify_nunchuk(const Features& f, const SideAxes& axes, const Tuning& t, float accelThrG,
    RejectReason* why = nullptr) {
    const auto reject = [&](RejectReason r) {
        if (why != nullptr) {
            *why = r;
        }
        return NunchukGesture::None;
    };
    if (f.sawGap) {
        return reject(RejectReason::Gap);
    }
    if (f.activeTime < tuning::kMinActiveS) {
        return reject(RejectReason::Short);
    }
    if (f.accelPeakG < accelThrG * tuning::kRotationOnlyAccelRatio) {
        return reject(RejectReason::RotationOnly);
    }
    const float total = f.energyAxis[0] + f.energyAxis[1] + f.energyAxis[2];
    const int lat = axes.lateral(f.gravityAxis);
    const int fwd = axes.forward(f.gravityAxis);
    const float el = f.energyAxis[lat];
    const float ef = f.energyAxis[fwd];
    if (total <= 0.0f || el + ef < tuning::kPlanarShare * total) {
        return reject(RejectReason::Ambiguous);  // mostly vertical
    }
    if (std::sqrt(el) >= t.dominance * std::sqrt(ef)) {
        return NunchukGesture::Spin;
    }
    if (std::sqrt(ef) >= t.dominance * std::sqrt(el)) {
        if (axes.forwardSign != 0 && f.firstPeak[fwd] * static_cast<float>(axes.forwardSign) <= 0.0f) {
            return reject(RejectReason::Ambiguous);  // a pull, not a push
        }
        return NunchukGesture::ShieldBash;
    }
    return reject(RejectReason::Ambiguous);
}

// Why gestures must not act: anything that is not plain gameplay with the Dusklight UI closed.
struct BlockContext {
    bool ui_visible = false;      // a Dusklight document is open
    bool benchmark = false;       // benchmark, tour, matrix or prep run
    bool no_gameplay = false;     // no player, a cutscene or event, a menu or the map
    bool paused = false;
    bool calibrating = false;     // the recenter window
    bool blocked() const { return ui_visible || benchmark || no_gameplay || paused || calibrating; }
};

// Which physical Joy-Con a role is on: 0 left, 1 right, 2 the only sensor of a single controller.
struct Inputs {
    bool enabled = false;
    bool blocked = false;          // UI open, benchmark/tour, no gameplay, paused, calibrating
    Layout layout = Layout::None;
    float sensitivity = 1.0f;
    int swordSide = 1;             // which sensor swings the sword (Layout::Single: the only one)
    int nunchukSide = 0;           // and which is the Nunchuk (DetachedPair only)
    Tuning tuning;
};

// One line of telemetry: a window that closed, accepted or not.
struct Report {
    enum class Kind { Sword, Nunchuk } kind = Kind::Sword;
    bool accepted = false;
    int verdict = 0;  // SwordGesture or NunchukGesture
    RejectReason reason = RejectReason::None;
    int side = 0;
    Features f;
};

struct Pulses {
    bool sword = false;
    SwordGesture gesture = SwordGesture::None;  // with `sword`
    std::uint32_t serial = 0;                   // with `sword`: identifies this gesture, never reused
    bool spin = false;
    bool shield_bash = false;
    static constexpr int kMaxReports = 6;
    Report reports[kMaxReports];
    int reportCount = 0;
    bool any() const { return sword || spin || shield_bash; }
    void add(const Report& r) {
        if (reportCount < kMaxReports) {
            reports[reportCount++] = r;
        }
    }
};

struct SampleSpan {
    const Sample* data = nullptr;
    std::size_t count = 0;
};

// The gestures. update() is called once per simulated tick with the samples taken since the last tick: `sword` is the
// sensor in the sword hand, `nunchuk` the other Joy-Con and is looked at only for a detached pair. Pulses last one call.
class GestureEngine {
public:
    // Forgets windows, filters and cooldowns. The gyroscope's handedness is a property of the hardware and is kept.
    void reset() {
        mSword.reset();
        mNunchuk.reset();
        mLayout = Layout::None;
        mLastSwordAt = 0.0;
        mLastSwordPeak = 0.0f;
    }
    void reset_all() {
        reset();
        for (auto& h : mHand) {
            h.reset();
        }
    }

    // The largest dynamic magnitude (g) outside windows since the last call, per role (near-miss telemetry).
    float take_sword_idle_peak() { return mSword.take_idle_peak(); }
    float take_nunchuk_idle_peak() { return mNunchuk.take_idle_peak(); }
    int handedness(int side) const { return side >= 0 && side < 3 ? mHand[side].sign() : 0; }

    Pulses update(const Inputs& in, SampleSpan sword, SampleSpan nunchuk) {
        Pulses out;
        if (!in.enabled || in.blocked || in.layout == Layout::None) {
            reset();
            return out;
        }
        if (in.layout != mLayout || in.swordSide != mSwordSide || in.nunchukSide != mNunchukSide) {
            // controller changed, a sensor came or went, or the sword hand was switched: start clean
            reset();
            mLayout = in.layout;
            mSwordSide = in.swordSide;
            mNunchukSide = in.nunchukSide;
        }
        const float sens = clamp_sensitivity(in.sensitivity);
        const float accelThr = in.tuning.accelThresholdG / sens;
        const float gyroThr = in.tuning.gyroThresholdRad / sens;
        const int swordHand = std::clamp(in.swordSide, 0, 2);

        for (std::size_t i = 0; i < sword.count; ++i) {
            const auto r = mSword.feed(sword.data[i], accelThr, gyroThr, mHand[swordHand], in.tuning.gyroWeight,
                in.tuning.cooldownS);
            if (!r.closed) {
                continue;
            }
            SwordVerdict v = r.aborted ? SwordVerdict{SwordGesture::None, RejectReason::Gap}
                                       : classify_sword(r.f, in.tuning, accelThr);
            const double closedAt = sword.data[i].t;
            if (v.gesture != SwordGesture::None && mLastSwordAt > 0.0 &&
                closedAt - mLastSwordAt < tuning::kReboundWindowS &&
                r.f.accelPeakG < tuning::kReboundRatio * mLastSwordPeak)
            {
                v = SwordVerdict{SwordGesture::None, RejectReason::Rebound};  // V1-rc8: the swing's own return
            }
            if (v.gesture != SwordGesture::None) {
                mLastSwordAt = closedAt;
                mLastSwordPeak = r.f.accelPeakG;
            }
            if (v.reason == RejectReason::Windup) {
                mSword.rearm_now(sword.data[i].t);
            } else if (v.gesture == SwordGesture::LeftToRight || v.gesture == SwordGesture::RightToLeft) {
                mSword.quick_rearm();
            }
            Report rep;
            rep.kind = Report::Kind::Sword;
            rep.accepted = v.gesture != SwordGesture::None;
            rep.verdict = static_cast<int>(v.gesture);
            rep.reason = v.reason;
            rep.side = swordHand;
            rep.f = r.f;
            out.add(rep);
            if (rep.accepted) {
                out.sword = true;
                out.gesture = v.gesture;
                out.serial = ++mSerial;
            }
        }
        if (in.layout == Layout::DetachedPair) {
            const int nunHand = std::clamp(in.nunchukSide, 0, 1);
            const float nunThr = accelThr * 0.8f;  // a Nunchuk is moved with less force than a Remote is shaken with
            for (std::size_t i = 0; i < nunchuk.count; ++i) {
                const auto r = mNunchuk.feed(nunchuk.data[i], nunThr, gyroThr, mHand[nunHand], in.tuning.gyroWeight,
                    in.tuning.cooldownS);
                if (!r.closed) {
                    continue;
                }
                RejectReason why = RejectReason::None;
                const NunchukGesture g = r.aborted ? NunchukGesture::None
                                                   : classify_nunchuk(r.f, in.tuning.axes[nunHand], in.tuning, nunThr, &why);
                Report rep;
                rep.kind = Report::Kind::Nunchuk;
                rep.accepted = g != NunchukGesture::None;
                rep.verdict = static_cast<int>(g);
                rep.reason = r.aborted ? RejectReason::Gap : why;
                rep.side = nunHand;
                rep.f = r.f;
                out.add(rep);
                if (g == NunchukGesture::Spin) {
                    out.spin = true;
                } else if (g == NunchukGesture::ShieldBash) {
                    out.shield_bash = true;
                }
            }
        }
        return out;
    }

private:
    MotionTracker mSword;
    MotionTracker mNunchuk;
    Handedness mHand[3];
    Layout mLayout = Layout::None;
    int mSwordSide = -1;
    int mNunchukSide = -1;
    std::uint32_t mSerial = 0;
    double mLastSwordAt = 0.0;    // V1-rc8: when the last accepted sword window closed
    float mLastSwordPeak = 0.0f;  // and its acceleration peak
};

}  // namespace dusk::wii
