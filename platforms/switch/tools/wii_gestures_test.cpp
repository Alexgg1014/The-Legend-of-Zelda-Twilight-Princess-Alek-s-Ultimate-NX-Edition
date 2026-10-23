// H38-c3: the Wii-style gesture detector (src/dusk/wii_gesture_detector.hpp) on simulated Joy-Con motion.
//
// The sword signals are simulated, not drawn: a hand moves in a WORLD frame (z up) with an acceleration profile that
// accelerates, brakes and rebounds (so the net integral of the acceleration is near zero), while the controller rotates
// with a gyro profile; the sensor reads the specific force and the angular velocity in ITS frame, from any starting pose,
// and optionally with a gyro frame of the opposite handedness (libnx does not say which it is). What must come out is
// decided by the world: down is down and a stroke to the right is to the right, whatever way the Joy-Con is held.
#include "dusk/wii_gesture_detector.hpp"
#include "dusk/wii_sample_log.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

using namespace dusk::wii;

namespace {

int failures = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                      \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

constexpr double kPi = 3.14159265358979323846;
constexpr double kTick = 1.0 / 60.0;
constexpr double kStep = 0.005;  // 200 Hz sensor

using Signal = std::function<Sample(double)>;

// --- small 3x3 maths --------------------------------------------------------------------------------------------------
struct Mat3 {
    float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};
Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            r.m[i][j] = 0;
            for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
        }
    return r;
}
Vec3 apply_t(const Mat3& a, const Vec3& v) {  // transpose
    return {a.m[0][0] * v.x + a.m[1][0] * v.y + a.m[2][0] * v.z, a.m[0][1] * v.x + a.m[1][1] * v.y + a.m[2][1] * v.z,
        a.m[0][2] * v.x + a.m[1][2] * v.y + a.m[2][2] * v.z};
}
Mat3 rodrigues(Vec3 axis, float angle) {
    const float n = norm(axis);
    if (n < 1e-9f || std::fabs(angle) < 1e-12f) return {};
    axis = axis * (1.0f / n);
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    Mat3 r;
    r.m[0][0] = t * axis.x * axis.x + c;
    r.m[0][1] = t * axis.x * axis.y - s * axis.z;
    r.m[0][2] = t * axis.x * axis.z + s * axis.y;
    r.m[1][0] = t * axis.x * axis.y + s * axis.z;
    r.m[1][1] = t * axis.y * axis.y + c;
    r.m[1][2] = t * axis.y * axis.z - s * axis.x;
    r.m[2][0] = t * axis.x * axis.z - s * axis.y;
    r.m[2][1] = t * axis.y * axis.z + s * axis.x;
    r.m[2][2] = t * axis.z * axis.z + c;
    return r;
}

uint32_t rngState = 12345;
float noise(float amplitude) {
    rngState = rngState * 1664525u + 1013904223u;
    return (static_cast<float>(rngState >> 8) / 16777216.0f - 0.5f) * 2.0f * amplitude;
}

// --- a controller moving in the world ----------------------------------------------------------------------------------
// accelG(t): the hand's acceleration in the world, in g (z up). omega(t): the controller's angular velocity in the world.
struct Motion {
    Mat3 R;  // sensor -> world
    bool flipGyro = false;
    float noiseG = 0.02f;
    std::function<Vec3(double)> accelG = [](double) { return Vec3{}; };
    std::function<Vec3(double)> omega = [](double) { return Vec3{}; };
    double tLast = -1.0;

    Sample at(double t) {
        if (tLast >= 0.0) {
            const Vec3 w = omega(t);
            R = mul(rodrigues(w, static_cast<float>(norm(w) * (t - tLast))), R);
        }
        tLast = t;
        const Vec3 force = accelG(t) + Vec3{0.0f, 0.0f, 1.0f};  // specific force in g: linear + the reaction to gravity
        Vec3 a = apply_t(R, force);
        a = a + Vec3{noise(noiseG), noise(noiseG), noise(noiseG)};
        Vec3 w = apply_t(R, omega(t));
        if (flipGyro) w = w * -1.0f;
        return {t, a * kGravity, w, false};
    }
};

// Starting poses: which way the Joy-Con is held (gravity along sensor z, y, x and two oblique ones).
Mat3 pose(int i) {
    switch (i) {
    case 0: return {};
    case 1: return rodrigues({1, 0, 0}, static_cast<float>(kPi / 2));
    case 2: return rodrigues({0, 1, 0}, static_cast<float>(kPi / 2));
    case 3: return mul(rodrigues({1, 0, 0}, 0.61f), rodrigues({0, 1, 0}, 0.70f));
    default: return mul(rodrigues({0, 0, 1}, 1.2f), rodrigues({1, 0, 0}, 2.1f));
    }
}

// A realistic stroke: accelerate (+), brake (-), a small rebound (+); `u` is the time since it started. The net integral
// is a few percent of the single-phase one: a detector that reads the sign of the integral sees almost nothing.
float stroke(double u, float peak, double d = 0.05) {
    if (u < 0.0) return 0.0f;
    if (u < d) return peak * static_cast<float>(std::sin(kPi * u / d));
    if (u < 2 * d) return -peak * static_cast<float>(std::sin(kPi * (u - d) / d));
    if (u < 3 * d) return 0.2f * peak * static_cast<float>(std::sin(kPi * (u - 2 * d) / d));
    return 0.0f;
}
// A smooth bell of angular speed over `len` seconds.
float bell(double u, float peak, double len = 0.1) {
    if (u < 0.0 || u > len) return 0.0f;
    return peak * static_cast<float>(std::sin(kPi * u / len));
}

enum class Dir { Down, Up, LeftToRight, RightToLeft, Diagonal, Forward };

// A sword stroke at t0 in direction `dir`, strength `peakG`; the wrist turns with it (yaw for a sideways stroke, pitch for
// a vertical one). Added onto whatever `base` already does.
void add_stroke(Motion& m, double t0, Dir dir, float peakG, float wrist = 6.0f) {
    auto a0 = m.accelG;
    auto w0 = m.omega;
    m.accelG = [=](double t) {
        Vec3 a = a0(t);
        const float s = stroke(t - t0, peakG);
        switch (dir) {
        case Dir::Down: a.z -= s; break;
        case Dir::Up: a.z += s; break;
        case Dir::LeftToRight: a.x += s; break;
        case Dir::RightToLeft: a.x -= s; break;
        case Dir::Diagonal: a.x += 0.7f * s; a.z -= 0.7f * s; break;
        case Dir::Forward: a.y += s; break;  // V1-rc7: a thrust, straight ahead
        }
        return a;
    };
    m.omega = [=](double t) {
        Vec3 w = w0(t);
        const float b = bell(t - t0, wrist);
        switch (dir) {
        case Dir::Down: w.x -= b; break;       // the nose pitches down
        case Dir::Up: w.x += b; break;
        case Dir::LeftToRight: w.z -= b; break;  // clockwise seen from above
        case Dir::RightToLeft: w.z += b; break;
        case Dir::Diagonal: w.z -= 0.3f * b; w.x -= 0.3f * b; break;
        case Dir::Forward: w.x -= 0.1f * b; break;  // a thrust hardly turns the controller
        }
        return w;
    };
}

// The wrist rolling about the world x axis between strokes, by `angle` over `len`, with no linear acceleration.
void add_roll(Motion& m, double t0, double len, float angle) {
    auto w0 = m.omega;
    m.omega = [=](double t) {
        Vec3 w = w0(t);
        if (t >= t0 && t < t0 + len) w.x += static_cast<float>(angle / len) * (1.0f - std::cos(static_cast<float>(2 * kPi * (t - t0) / len)));
        return w;
    };
}

struct Rig {
    GestureEngine engine;
    double t = 0.0;
    int sword = 0, spin = 0, bash = 0;
    std::vector<SwordGesture> gestures;
    std::vector<std::uint32_t> serials;
    std::vector<Report> reports;

    // Runs `seconds` of ticks; each tick feeds the samples taken during it (a gap flag marks the first one if asked).
    void run(double seconds, const Inputs& in, Signal sword_sig, Signal nunchuk_sig, double gapAt = -1.0) {
        const double end = t + seconds;
        while (t < end - 1e-9) {
            std::vector<Sample> p, n;
            for (double s = t; s < t + kTick - 1e-9; s += kStep) {
                Sample a = sword_sig(s);
                Sample b = nunchuk_sig(s);
                if (gapAt >= 0.0 && s >= gapAt && s < gapAt + kStep) {
                    a.gap = true;
                    b.gap = true;
                    gapAt = -1.0;
                }
                p.push_back(a);
                n.push_back(b);
            }
            t += kTick;
            const Pulses out = engine.update(in, {p.data(), p.size()}, {n.data(), n.size()});
            sword += out.sword;
            spin += out.spin;
            bash += out.shield_bash;
            if (out.sword) {
                gestures.push_back(out.gesture);
                serials.push_back(out.serial);
            }
            for (int i = 0; i < out.reportCount; ++i) reports.push_back(out.reports[i]);
        }
    }
};

Signal quiet_signal() {
    return [](double t) { return Sample{t, Vec3{0, 0, kGravity}, Vec3{}, false}; };
}

Inputs pair_inputs() {
    Inputs in;
    in.enabled = true;
    in.layout = Layout::DetachedPair;
    in.swordSide = 1;
    in.nunchukSide = 0;
    return in;
}

// Turns the controller about for a second so the gyroscope's handedness is settled (what hand-holding does anyway).
Motion warmed(int poseIndex, bool flip) {
    Motion m;
    m.R = pose(poseIndex);
    m.flipGyro = flip;
    // slow rolls about two world axes, 1.5..2.5 rad/s: below the gyro window threshold, with no linear acceleration
    m.omega = [](double t) {
        if (t < 0.4 || t > 1.9) return Vec3{};
        const float k = static_cast<float>(std::sin(2 * kPi * 1.2 * (t - 0.4)));
        return Vec3{2.2f * k, 1.8f * std::cos(static_cast<float>(2 * kPi * 0.9 * (t - 0.4))), 0.0f};
    };
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) rngState = static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10));  // a different noise sequence
    const Inputs pair = pair_inputs();
    Inputs single = pair;
    single.layout = Layout::Single;
    single.swordSide = 2;

    // --- the gyroscope's handedness is found from the data, either way ------------------------------------------------------
    for (bool flip : {false, true}) {
        Rig r;
        Motion m = warmed(3, flip);
        r.run(2.2, pair, [&](double t) { return m.at(t); }, quiet_signal());
        CHECK(r.engine.handedness(1) == (flip ? -1 : 1));
        CHECK(r.sword == 0);  // slow turning is not a gesture
    }

    // --- direction: the world decides, whatever the pose or the gyro frame -------------------------------------------------
    for (int pi = 0; pi < 5; ++pi) {
        for (bool flip : {false, true}) {
            struct Case { Dir dir; SwordGesture want; };
            // V1-rc7: raising the controller (Up) is the wind-up, not an attack (wiiWindupFilter, on by default).
            const Case cases[] = {{Dir::Down, SwordGesture::Vertical}, {Dir::Up, SwordGesture::None},
                {Dir::LeftToRight, SwordGesture::LeftToRight}, {Dir::RightToLeft, SwordGesture::RightToLeft},
                {Dir::Diagonal, SwordGesture::Generic}, {Dir::Forward, SwordGesture::Thrust}};
            for (const Case& c : cases) {
                Rig r;
                Motion m = warmed(pi, flip);
                add_stroke(m, 2.6, c.dir, 3.0f, c.dir == Dir::Forward ? 1.0f : 6.0f);
                r.run(3.5, pair, [&](double t) { return m.at(t); }, quiet_signal());
                bool windupSeen = false;
                for (const auto& rep : r.reports) windupSeen |= rep.reason == RejectReason::Windup;
                const bool ok = c.want == SwordGesture::None
                                    ? (r.sword == 0 && windupSeen)
                                    : (r.sword == 1 && r.gestures.size() == 1 && r.gestures[0] == c.want);
                if (!ok) {
                    std::fprintf(stderr, "pose %d flip %d dir %d: %d pulses, got %s want %s\n", pi, flip, static_cast<int>(c.dir),
                        r.sword, r.gestures.empty() ? "-" : gesture_name(r.gestures[0]), gesture_name(c.want));
                    for (const auto& rep : r.reports) {
                        std::fprintf(stderr, "   report accepted=%d verdict=%d reason=%s Ev=%.3f Eh=%.3f yaw=%.2f yawPeak=%.2f aPeak=%.2f act=%.3f hand=%d\n",
                            rep.accepted, rep.verdict, reject_name(rep.reason), rep.f.energyVertical, rep.f.energyHorizontal,
                            rep.f.yaw, rep.f.yawPeak, rep.f.accelPeakG, rep.f.activeTime, rep.f.handednessKnown);
                    }
                }
                CHECK(ok);
                CHECK(r.spin == 0 && r.bash == 0);
            }
        }
    }

    // --- the stroke is not read from the net integral: accelerate / brake / rebound nearly cancel ------------------------------
    {
        float net = 0.0f, absSum = 0.0f;
        for (double u = 0.0; u < 0.2; u += 0.0005) {
            net += stroke(u, 3.0f) * 0.0005f;
            absSum += std::fabs(stroke(u, 3.0f)) * 0.0005f;
        }
        CHECK(std::fabs(net) < 0.1f * absSum);  // the simulated stroke really is cancelling
    }

    // --- direction is not guessed when it is not known ----------------------------------------------------------------------
    {
        Rig r;
        Motion m;  // no warm-up: the gyro's handedness is unknown
        m.R = pose(3);
        add_stroke(m, 1.5, Dir::LeftToRight, 3.0f);
        r.run(2.5, pair, [&](double t) { return m.at(t); }, quiet_signal());
        CHECK(r.sword == 1 && r.gestures[0] != SwordGesture::LeftToRight && r.gestures[0] != SwordGesture::RightToLeft);
        // a vertical chop needs no gyro
        Rig v;
        Motion mv;
        mv.R = pose(2);
        add_stroke(mv, 1.5, Dir::Down, 3.0f);
        v.run(2.5, pair, [&](double t) { return mv.at(t); }, quiet_signal());
        CHECK(v.sword == 1 && v.gestures[0] == SwordGesture::Vertical);
    }

    // --- natural swings: weak, normal and strong, a wrist roll of 45..90 degrees between them: none lost -------------------
    {
        Rig r;
        Motion m = warmed(3, false);
        const float strengths[] = {1.7f, 2.4f, 3.5f, 5.0f};
        const Dir dirs[] = {Dir::Down, Dir::LeftToRight, Dir::RightToLeft, Dir::Down, Dir::Diagonal};
        double at = 2.4;
        int n = 0;
        for (int i = 0; i < 20; ++i) {
            add_stroke(m, at, dirs[i % 5], strengths[i % 4]);
            add_roll(m, at + 0.28, 0.2, (i % 2 ? 1.0f : -1.0f) * (0.8f + 0.1f * (i % 8)));  // 45..90+ degrees
            at += 0.75;
            ++n;
        }
        r.run(at + 0.5, pair, [&](double t) { return m.at(t); }, quiet_signal());
        CHECK(r.sword == n);
        // the 4.5 g of the strongest is not read as two
        CHECK(r.serials.size() == static_cast<size_t>(n));
        for (size_t i = 1; i < r.serials.size(); ++i) CHECK(r.serials[i] == r.serials[i - 1] + 1);  // distinct, increasing
    }

    // --- the old failure, kept as a regression: 6 strokes, each ending with the wrist turned 45 degrees ----------------------
    {
        Rig r;
        Motion m = warmed(0, false);
        double at = 2.4;
        for (int i = 0; i < 6; ++i) {
            add_stroke(m, at, Dir::LeftToRight, 3.0f);
            add_roll(m, at + 0.1, 0.15, (i % 2 ? -1.0f : 1.0f) * 0.785f);
            at += 0.7;
        }
        r.run(at + 0.5, pair, [&](double t) { return m.at(t); }, quiet_signal());
        CHECK(r.sword == 6);
    }

    // --- nothing that is not an attack ------------------------------------------------------------------------------------
    {
        // rest and noise
        Rig r;
        Motion m = warmed(3, false);
        r.run(8.0, pair, [&](double t) { return m.at(t); }, quiet_signal());
        CHECK(r.sword == 0 && r.spin == 0 && r.bash == 0);
        // slow aiming: the controller turns at 2.4 rad/s (gyro aim), nothing else
        Rig a;
        Motion aim = warmed(3, false);
        aim.omega = [](double t) { return t < 2.0 ? Vec3{} : Vec3{2.4f * static_cast<float>(std::sin(2 * kPi * 0.6 * (t - 2.0))), 1.5f, 0.0f}; };
        a.run(8.0, pair, [&](double t) { return aim.at(t); }, quiet_signal());
        CHECK(a.sword == 0);
        // a fast flick of the controller with no hand motion (rotation only, 9 rad/s): not a sword swing
        Rig f;
        Motion flick = warmed(3, false);
        auto w0 = flick.omega;
        flick.omega = [=](double t) { Vec3 w = w0(t); w.y += bell(t - 2.6, 9.0f, 0.09); return w; };
        f.run(4.0, pair, [&](double t) { return flick.at(t); }, quiet_signal());
        CHECK(f.sword == 0);  // V1-rc7: the gyroscope alone no longer even opens a window
        // walking: 0.35 g at 2 Hz up and down plus a sway
        Rig w;
        Motion walk = warmed(3, false);
        walk.accelG = [](double t) { return t < 2.0 ? Vec3{} : Vec3{0.12f * std::sin(static_cast<float>(2 * kPi * 1.0 * t)), 0.10f * std::sin(static_cast<float>(2 * kPi * 2.0 * t)), 0.35f * std::sin(static_cast<float>(2 * kPi * 2.0 * t))}; };
        w.run(10.0, pair, [&](double t) { return walk.at(t); }, quiet_signal());
        CHECK(w.sword == 0);
        // vibration: a 60 Hz, 2 g buzz for 10 ms
        Rig b;
        Motion buzz = warmed(3, false);
        buzz.accelG = [](double t) { return (t > 2.6 && t < 2.61) ? Vec3{2.0f * static_cast<float>(std::sin(2 * kPi * 60.0 * t)), 0, 0} : Vec3{}; };
        b.run(4.0, pair, [&](double t) { return buzz.at(t); }, quiet_signal());
        CHECK(b.sword == 0);
        // a gentle movement well under the threshold
        Rig q;
        Motion gentle = warmed(3, false);
        add_stroke(gentle, 2.6, Dir::LeftToRight, 0.6f, 1.0f);
        q.run(4.0, pair, [&](double t) { return gentle.at(t); }, quiet_signal());
        CHECK(q.sword == 0);
    }

    // --- V1-rc7: wind-up then chop, return strokes, the filter switched off -----------------------------------------------------
    {
        // raise (Up) then chop (Down) 0.18 s later: one vertical attack, the raise rejected as a wind-up
        Rig r;
        Motion m = warmed(3, false);
        add_stroke(m, 2.6, Dir::Up, 2.0f, 4.0f);
        add_stroke(m, 2.78, Dir::Down, 3.5f);
        r.run(3.6, pair, [&](double t) { return m.at(t); }, quiet_signal());
        bool windupSeen = false;
        for (const auto& rep : r.reports) windupSeen |= rep.reason == RejectReason::Windup;
        if (!(r.sword == 1 && !r.gestures.empty() && r.gestures[0] == SwordGesture::Vertical && windupSeen)) {
            std::fprintf(stderr, "wind-up then chop: %d pulses, windup %d\n", r.sword, windupSeen);
            for (const auto& rep : r.reports)
                std::fprintf(stderr, "   accepted=%d verdict=%d reason=%s fv=%.2f Ev=%.3f Eh=%.3f\n", rep.accepted, rep.verdict,
                    reject_name(rep.reason), rep.f.firstVert, rep.f.energyVertical, rep.f.energyHorizontal);
        }
        CHECK(r.sword == 1 && !r.gestures.empty() && r.gestures[0] == SwordGesture::Vertical && windupSeen);
        // left-to-right, then right-to-left 0.3 s later, three times: six attacks, alternating
        Rig b;
        Motion back = warmed(3, false);
        double at = 2.6;
        for (int i = 0; i < 6; ++i) {
            add_stroke(back, at, i % 2 ? Dir::RightToLeft : Dir::LeftToRight, 3.0f);
            at += 0.3;
        }
        b.run(at + 0.6, pair, [&](double t) { return back.at(t); }, quiet_signal());
        CHECK(b.sword == 6);
        for (size_t i = 0; i < b.gestures.size(); ++i)
            CHECK(b.gestures[i] == (i % 2 ? SwordGesture::RightToLeft : SwordGesture::LeftToRight));
        // the filter off: raising is an attack again
        Inputs noFilter = pair;
        noFilter.tuning.windupFilter = false;
        Rig u;
        Motion up = warmed(3, false);
        add_stroke(up, 2.6, Dir::Up, 3.0f);
        u.run(3.5, noFilter, [&](double t) { return up.at(t); }, quiet_signal());
        CHECK(u.sword == 1 && u.gestures[0] == SwordGesture::Vertical);
        // a slow raise (0.5 g) is neither an attack nor a rejected window
        Rig s;
        Motion slow = warmed(3, false);
        add_stroke(slow, 2.6, Dir::Up, 0.5f, 1.0f);
        s.run(3.5, pair, [&](double t) { return slow.at(t); }, quiet_signal());
        CHECK(s.sword == 0 && s.reports.empty());
    }

    // --- V1-rc8: from the rc7 console logs -------------------------------------------------------------------------------------
    {
        // raising the arm with a sideways component (vertical not dominant): a wind-up, not an attack
        Rig r;
        Motion m = warmed(3, false);
        add_stroke(m, 2.6, Dir::Up, 3.0f, 4.0f);
        add_stroke(m, 2.6, Dir::LeftToRight, 2.2f, 2.0f);
        r.run(3.5, pair, [&](double t) { return m.at(t); }, quiet_signal());
        bool windupSeen = false;
        for (const auto& rep : r.reports) windupSeen |= rep.reason == RejectReason::Windup;
        CHECK(r.sword == 0 && windupSeen);
        // a chop, then a weak return of the arm 0.15 s after it closes: one attack, the return a rebound
        Rig b;
        Motion chop = warmed(3, false);
        add_stroke(chop, 2.6, Dir::RightToLeft, 3.5f);
        add_stroke(chop, 2.88, Dir::LeftToRight, 1.6f, 3.0f);
        b.run(3.6, pair, [&](double t) { return chop.at(t); }, quiet_signal());
        bool reboundSeen = false;
        for (const auto& rep : b.reports) reboundSeen |= rep.reason == RejectReason::Rebound;
        if (!(b.sword == 1 && reboundSeen)) {
            std::fprintf(stderr, "rebound: %d pulses, rebound %d\n", b.sword, reboundSeen);
            for (const auto& rep : b.reports)
                std::fprintf(stderr, "   accepted=%d verdict=%d reason=%s aPeak=%.2f\n", rep.accepted, rep.verdict,
                    reject_name(rep.reason), rep.f.accelPeakG);
        }
        CHECK(b.sword == 1 && reboundSeen);
        // a jab that turns the controller at 3.5 rad/s (rc7 rejected it as generic): a thrust
        Rig j;
        Motion jab = warmed(3, false);
        add_stroke(jab, 2.6, Dir::Forward, 2.0f, 35.0f);  // Dir::Forward turns at 0.1 x wrist
        j.run(3.5, pair, [&](double t) { return jab.at(t); }, quiet_signal());
        CHECK(j.sword == 1 && !j.gestures.empty() && j.gestures[0] == SwordGesture::Thrust);
    }

    // --- one oscillation is one attack ----------------------------------------------------------------------------------------
    {
        Rig r;
        Motion m = warmed(1, false);
        m.accelG = [](double t) {  // 8 Hz shaking for 0.3 s at 4 g
            return (t >= 2.6 && t < 2.9) ? Vec3{4.0f * static_cast<float>(std::sin(2 * kPi * 8.0 * (t - 2.6))), 0, 0} : Vec3{};
        };
        r.run(5.0, pair, [&](double t) { return m.at(t); }, quiet_signal());
        CHECK(r.sword == 1);
        // a long steady push is one too
        Rig l;
        Motion held = warmed(1, false);
        held.accelG = [](double t) { return (t >= 2.6 && t < 4.1) ? Vec3{3.0f, 0, 0} : Vec3{}; };
        l.run(6.0, pair, [&](double t) { return held.at(t); }, quiet_signal());
        CHECK(l.sword == 1);
    }

    // --- sensitivity moves the thresholds ------------------------------------------------------------------------------------
    {
        Inputs lo = pair, hi = pair;
        lo.sensitivity = 0.5f;
        hi.sensitivity = 2.0f;
        Rig a, b;
        Motion ma = warmed(3, false), mb = warmed(3, false);
        add_stroke(ma, 2.6, Dir::Down, 1.5f);
        add_stroke(mb, 2.6, Dir::Down, 1.5f);
        a.run(3.8, lo, [&](double t) { return ma.at(t); }, quiet_signal());
        b.run(3.8, hi, [&](double t) { return mb.at(t); }, quiet_signal());
        CHECK(a.sword == 0 && b.sword == 1);
    }

    // --- a gap in the samples: the window it falls in is not an attack, and nothing starts right after ------------------------
    {
        Rig r;
        Motion m = warmed(3, false);
        add_stroke(m, 2.6, Dir::Down, 3.0f);
        r.run(1.0 + 2.0, pair, [&](double t) { return m.at(t); }, quiet_signal(), /*gapAt=*/2.63);
        CHECK(r.sword == 0);
        bool sawGap = false;
        for (const auto& rep : r.reports) sawGap |= rep.reason == RejectReason::Gap;
        CHECK(sawGap);
        // a stroke 0.6 s later (past the hold-off) is fine
        Rig g;
        Motion mg = warmed(3, false);
        add_stroke(mg, 2.6, Dir::Down, 3.0f);
        add_stroke(mg, 3.4, Dir::Down, 3.0f);
        g.run(4.5, pair, [&](double t) { return mg.at(t); }, quiet_signal(), 2.63);
        CHECK(g.sword == 1);
        // a gap while resting does not start anything
        Rig z;
        Motion mz = warmed(3, false);
        z.run(4.0, pair, [&](double t) { return mz.at(t); }, quiet_signal(), 3.0);
        CHECK(z.sword == 0);
    }

    // --- Nunchuk: spin sideways, shield bash forward, per physical side and with the config corrections --------------------------
    {
        auto nun = [](Vec3 gravity, int axis, float peak, double t0, int sign = 1) {
            return [=](double t) {
                Vec3 a = gravity * kGravity;
                a.at(axis) += static_cast<float>(sign) * stroke(t - t0, peak) * kGravity;
                a = a + Vec3{noise(0.02f) * kGravity, noise(0.02f) * kGravity, noise(0.02f) * kGravity};
                return Sample{t, a, Vec3{}, false};
            };
        };
        const Vec3 gy{0.0f, -1.0f, 0.0f};  // gravity on y: lateral = x, forward = z
        auto rest = [](Vec3 gravity) { return [=](double t) { return Sample{t, gravity * kGravity, Vec3{}, false}; }; };
        for (int side : {0, 1}) {
            Inputs in = pair;
            in.nunchukSide = side;
            in.swordSide = 1 - side;
            Rig a;
            a.run(1.0, in, quiet_signal(), rest(gy));
            a.run(2.0, in, quiet_signal(), nun(gy, 0, 2.5f, a.t + 0.1));
            CHECK(a.spin == 1 && a.bash == 0 && a.sword == 0);
            Rig b;
            b.run(1.0, in, quiet_signal(), rest(gy));
            b.run(2.0, in, quiet_signal(), nun(gy, 2, 2.5f, b.t + 0.1));
            CHECK(b.bash == 1 && b.spin == 0 && b.sword == 0);
            Rig v;  // a vertical movement (along gravity) is neither
            v.run(1.0, in, quiet_signal(), rest(gy));
            v.run(2.0, in, quiet_signal(), nun(gy, 1, 2.5f, v.t + 0.1));
            CHECK(v.spin == 0 && v.bash == 0);
            Rig d;  // a diagonal one (equal sideways and forward) is neither
            d.run(1.0, in, quiet_signal(), rest(gy));
            auto sx = nun(gy, 0, 2.5f, d.t + 0.1), sz = nun(gy, 2, 2.5f, d.t + 0.1);
            d.run(2.0, in, quiet_signal(), [=](double t) { Sample s = sx(t), z = sz(t); s.accel = s.accel + z.accel - Vec3{0, -kGravity, 0}; return s; });
            CHECK(d.spin == 0 && d.bash == 0);
            // gravity along x: lateral = y, forward = z
            const Vec3 gx{-1.0f, 0.0f, 0.0f};
            Rig s;
            s.run(1.0, in, quiet_signal(), rest(gx));
            s.run(2.0, in, quiet_signal(), nun(gx, 1, 2.5f, s.t + 0.1));
            CHECK(s.spin == 1 && s.bash == 0);
        }
        // config: the axes swapped for the left Joy-Con swap the gestures; the right Joy-Con is untouched
        Inputs fix = pair;
        RawTuning raw;
        raw.lateralAxis[0] = 2;
        raw.forwardAxis[0] = 0;
        fix.tuning = sanitize(raw);
        Rig a;
        a.run(1.0, fix, quiet_signal(), rest(gy));
        a.run(2.0, fix, quiet_signal(), nun(gy, 0, 2.5f, a.t + 0.1));
        CHECK(a.bash == 1 && a.spin == 0);
        // a push must start toward +axis when the sign is set; a pull is rejected
        RawTuning signedRaw;
        signedRaw.forwardSign[0] = 1;
        Inputs sgn = pair;
        sgn.tuning = sanitize(signedRaw);
        Rig p, q;
        p.run(1.0, sgn, quiet_signal(), rest(gy));
        p.run(2.0, sgn, quiet_signal(), nun(gy, 2, 2.5f, p.t + 0.1, +1));
        q.run(1.0, sgn, quiet_signal(), rest(gy));
        q.run(2.0, sgn, quiet_signal(), nun(gy, 2, 2.5f, q.t + 0.1, -1));
        CHECK(p.bash == 1 && q.bash == 0);
    }

    // --- one sensor: the sword works and is not left disabled -------------------------------------------------------------------
    {
        Rig r;
        Motion m = warmed(3, false);
        add_stroke(m, 2.6, Dir::Down, 3.0f);
        // a stray second sensor is ignored
        r.run(3.8, single, [&](double t) { return m.at(t); }, [](double t) { return Sample{t, Vec3{0, 0, kGravity * 4}, Vec3{}, false}; });
        CHECK(r.sword == 1 && r.gestures[0] == SwordGesture::Vertical && r.spin == 0 && r.bash == 0);
        Rig s;
        Motion ms = warmed(3, true);
        add_stroke(ms, 2.6, Dir::RightToLeft, 3.0f);
        s.run(3.8, single, [&](double t) { return ms.at(t); }, quiet_signal());
        CHECK(s.sword == 1 && s.gestures[0] == SwordGesture::RightToLeft);
    }

    // --- Motion Sword Hand: either Joy-Con swings the sword, the other is the Nunchuk -------------------------------------------
    {
        Inputs leftSword = pair;
        leftSword.swordSide = 0;
        leftSword.nunchukSide = 1;
        Rig r;
        Motion m = warmed(3, false);
        add_stroke(m, 2.6, Dir::LeftToRight, 3.0f);
        r.run(3.8, leftSword, [&](double t) { return m.at(t); }, quiet_signal());
        CHECK(r.sword == 1 && r.gestures[0] == SwordGesture::LeftToRight);
        // the right Joy-Con is the Nunchuk in that arrangement
        Rig n;
        auto spinSig = [](double t) {
            Vec3 a = Vec3{0, -1, 0} * kGravity;
            a.x += stroke(t - 1.1, 2.5f) * kGravity;
            return Sample{t, a, Vec3{}, false};
        };
        n.run(1.0, leftSword, quiet_signal(), [](double t) { return Sample{t, Vec3{0, -1, 0} * kGravity, Vec3{}, false}; });
        n.run(2.0, leftSword, quiet_signal(), spinSig);
        CHECK(n.spin == 1 && n.sword == 0);
        // switching the hand mid-session starts both trackers clean: a window open on the old side cannot fire
        Rig sw;
        Motion ms = warmed(3, false);
        add_stroke(ms, 2.6, Dir::Down, 3.0f);
        sw.run(2.62, pair, [&](double t) { return ms.at(t); }, quiet_signal());
        sw.run(0.05, leftSword, [&](double t) { return ms.at(t); }, quiet_signal());
        sw.run(1.0, leftSword, [&](double t) { return ms.at(t); }, quiet_signal());
        CHECK(sw.sword == 0);
    }

    // --- blocked / disabled / disconnected / changed controller: nothing, and nothing pending afterwards -------------------------
    {
        for (int reason = 0; reason < 6; ++reason) {
            Rig r;
            Inputs in = pair;
            BlockContext ctx;
            if (reason == 0) in.enabled = false;
            if (reason == 1) ctx.ui_visible = true;
            if (reason == 2) ctx.benchmark = true;
            if (reason == 3) ctx.no_gameplay = true;
            if (reason == 4) ctx.paused = true;
            if (reason == 5) ctx.calibrating = true;
            in.blocked = ctx.blocked();
            CHECK(in.blocked == (reason != 0));
            Motion m = warmed(3, false);
            add_stroke(m, 2.6, Dir::Down, 3.0f);
            r.run(3.8, in, [&](double t) { return m.at(t); }, quiet_signal());
            CHECK(r.sword == 0 && r.spin == 0 && r.bash == 0);
        }
        CHECK(!BlockContext{}.blocked());
        // a stroke begun, then blocked, then unblocked: the window died with the block
        Rig r;
        Motion m = warmed(3, false);
        add_stroke(m, 2.6, Dir::Down, 3.0f);
        r.run(2.62, pair, [&](double t) { return m.at(t); }, quiet_signal());
        Inputs paused = pair;
        paused.blocked = true;
        r.run(0.06, paused, [&](double t) { return m.at(t); }, quiet_signal());
        r.run(1.0, pair, [&](double t) { return m.at(t); }, quiet_signal());
        CHECK(r.sword == 0);
        // disconnection and a change of controller type start clean: a stroke right after is inside the warm-up
        Rig d;
        Motion md = warmed(3, false);
        d.run(2.0, pair, [&](double t) { return md.at(t); }, quiet_signal());
        Inputs gone = pair;
        gone.layout = Layout::None;
        d.run(kTick, gone, quiet_signal(), quiet_signal());
        add_stroke(md, d.t + 0.05, Dir::Down, 3.0f);
        d.run(0.3, pair, [&](double t) { return md.at(t); }, quiet_signal());
        CHECK(d.sword == 0);
        // re-enabling after a disabled stretch also warms up
        Rig e;
        Inputs off = pair;
        off.enabled = false;
        Motion me = warmed(3, false);
        e.run(2.0, off, [&](double t) { return me.at(t); }, quiet_signal());
        add_stroke(me, e.t + 0.05, Dir::Down, 3.0f);
        e.run(0.25, pair, [&](double t) { return me.at(t); }, quiet_signal());
        CHECK(e.sword == 0);
    }

    // --- sample timing: the same signal in ticks of 3, 7 or 16 samples gives the same result ----------------------------------------
    {
        for (double tick : {0.015, 0.035, 0.08}) {
            GestureEngine engine;
            Motion m = warmed(3, false);
            add_stroke(m, 2.6, Dir::RightToLeft, 3.0f);
            int pulses = 0;
            SwordGesture g = SwordGesture::None;
            double t = 0.0;
            while (t < 4.0) {
                std::vector<Sample> p;
                for (double s = t; s < t + tick - 1e-9; s += kStep) p.push_back(m.at(s));
                t += tick;
                const Pulses out = engine.update(pair, {p.data(), p.size()}, {nullptr, 0});
                if (out.sword) {
                    ++pulses;
                    g = out.gesture;
                }
            }
            CHECK(pulses == 1 && g == SwordGesture::RightToLeft);
        }
    }

    // --- Tuning values from config.json are held to their ranges --------------------------------------------------------------------
    {
        std::string notes;
        const Tuning def = sanitize(RawTuning{}, &notes);
        CHECK(notes.empty());
        CHECK(def.accelThresholdG == tuning::kAccelThresholdG && def.gyroThresholdRad == tuning::kGyroThresholdRad);
        CHECK(def.dominance == tuning::kDominance && def.minYawRad == tuning::kMinYawRad);
        RawTuning bad;
        bad.accelThresholdG = 99.0f;
        bad.gyroThresholdRad = std::nanf("");
        bad.dominance = 0.5f;
        bad.minYawRad = -1.0f;
        bad.lateralAxis[0] = 7;
        bad.forwardAxis[1] = -5;
        bad.forwardSign[0] = 3;
        bad.lateralAxis[1] = 1;
        bad.forwardAxis[1] = 1;  // same axis as lateral
        notes.clear();
        const Tuning fixed = sanitize(bad, &notes);
        CHECK(fixed.accelThresholdG == tuning::kAccelThresholdG && fixed.gyroThresholdRad == tuning::kGyroThresholdRad);
        CHECK(fixed.dominance == tuning::kDominance && fixed.minYawRad == tuning::kMinYawRad);
        CHECK(fixed.axes[0].lateralOverride == -1 && fixed.axes[0].forwardSign == 0);
        CHECK(fixed.axes[1].lateralOverride == -1 && fixed.axes[1].forwardOverride == -1);
        CHECK(notes.find("wiiAccelThresholdG") != std::string::npos && notes.find("wiiGyroThresholdRad") != std::string::npos);
        CHECK(notes.find("wiiDirectionDominance") != std::string::npos && notes.find("wiiMinYawRad") != std::string::npos);
        CHECK(notes.find("left") != std::string::npos && notes.find("right") != std::string::npos);
        // in range: kept as given
        RawTuning good;
        good.accelThresholdG = 1.8f;
        good.gyroThresholdRad = 5.0f;
        good.dominance = 2.0f;
        good.minYawRad = 0.4f;
        good.lateralAxis[1] = 0;
        good.forwardAxis[1] = 2;
        good.forwardSign[1] = -1;
        notes.clear();
        const Tuning ok = sanitize(good, &notes);
        CHECK(notes.empty() && ok.accelThresholdG == 1.8f && ok.gyroThresholdRad == 5.0f && ok.dominance == 2.0f && ok.minYawRad == 0.4f);
        CHECK(ok.axes[1].lateralOverride == 0 && ok.axes[1].forwardOverride == 2 && ok.axes[1].forwardSign == -1);
        // an override equal to the gravity axis is not used (the table answers)
        SideAxes a;
        a.lateralOverride = 1;
        CHECK(a.lateral(1) == a.lateralTable[1]);
        CHECK(clamp_sensitivity(std::nanf("")) == 1.0f);
        // a stricter dominance turns a clear stroke into Generic
        Inputs strict = pair;
        RawTuning sr;
        sr.dominance = 3.0f;
        strict.tuning = sanitize(sr);
        Rig r;
        Motion m = warmed(3, false);
        add_stroke(m, 2.6, Dir::Diagonal, 3.0f);
        r.run(3.8, strict, [&](double t) { return m.at(t); }, quiet_signal());
        CHECK(r.sword == 1 && r.gestures[0] == SwordGesture::Generic);
    }

    // --- V1-rc12: the raw-sample log (wii_sample_log.hpp) replays to the same gestures ---------------------------------------
    {
        Motion m = warmed(3, false);
        const Dir dirs[] = {Dir::Down, Dir::LeftToRight, Dir::RightToLeft, Dir::Diagonal};
        double at = 2.4;
        for (int i = 0; i < 8; ++i) {
            add_stroke(m, at, dirs[i % 4], 2.4f + 0.4f * static_cast<float>(i % 3));
            at += 0.75;
        }
        const auto through_log = [](const Sample& in) {
            dusk::wii::LoggedSample l;
            l.side = 1;
            l.t = in.t;
            l.accel = {in.accel.x, in.accel.y, in.accel.z};
            l.gyro = {in.gyro.x, in.gyro.y, in.gyro.z};
            l.flags = in.gap ? 1u : 0u;
            const auto back = dusk::wii::parse_sample_line(dusk::wii::format_sample_line(l));
            CHECK(back.has_value());
            Sample out = in;
            if (back) {
                out.t = back->t;
                out.accel = Vec3{back->accel[0], back->accel[1], back->accel[2]};
                out.gyro = Vec3{back->gyro[0], back->gyro[1], back->gyro[2]};
                out.gap = (back->flags & 1u) != 0;
            }
            return out;
        };
        Rig direct, replayed;
        direct.run(at + 0.5, pair, [&](double t) { return m.at(t); }, quiet_signal());
        replayed.run(at + 0.5, pair, [&](double t) { return through_log(m.at(t)); }, quiet_signal());
        CHECK(direct.sword == 8 && replayed.sword == direct.sword);
        CHECK(replayed.gestures == direct.gestures);
        // comments and malformed lines are skipped; the attitude columns survive
        CHECK(!dusk::wii::parse_sample_line("# header").has_value());
        CHECK(!dusk::wii::parse_sample_line("1,2,3").has_value());
        CHECK(!dusk::wii::parse_sample_line("5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0").has_value());
        dusk::wii::LoggedSample a;
        a.dir = {1.f, 0.f, 0.f, 0.f, 0.5f, -0.25f, 0.f, 0.25f, 0.5f};
        a.flags = 2;
        const auto b = dusk::wii::parse_sample_line(dusk::wii::format_sample_line(a));
        CHECK(b && b->flags == 2 && b->dir[5] == -0.25f && b->dir[7] == 0.25f);
    }

    if (failures == 0) {
        std::puts("wii gestures: gyro-aided gravity, vertical/left-right/generic from realistic strokes in any pose and gyro "
                  "handedness, no loss after wrist rolls, no phantom (aim, flick, walk, buzz), one per oscillation, gaps, "
                  "sample timing, Nunchuk per side, sword hand, single sensor, resets, tuning ranges: ok");
    }
    return failures == 0 ? 0 : 1;
}
