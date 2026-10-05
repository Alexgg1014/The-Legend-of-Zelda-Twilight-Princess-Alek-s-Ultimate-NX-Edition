#include "dusk/wii_controls.hpp"

#include "aurora/lib/device.hpp"
#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
#ifdef __SWITCH__
#include "dusk/benchmark.hpp"  // benchmark.cpp is part of the Switch build only
#endif
#include "dusk/logging.h"
#include "dusk/settings.h"
#include "dusk/ui/ui.hpp"
#include "dusk/wii_recenter_combo.hpp"

#include <algorithm>
#include <chrono>
#include <vector>

namespace dusk::wii {
namespace {

constexpr double kLatchSeconds = 0.3;        // how long a spin / bash gesture waits for the game to ask for it
constexpr double kCalibrationSeconds = 0.6;  // gestures held after a recenter, while gravity is re-estimated
constexpr int kMaxSamplesPerTick = 17;       // libnx keeps 17 states per sensor
constexpr int kLinesPerSecond = 4;           // telemetry: no more than this many gesture lines per second

GestureEngine s_engine;
GestureSlot s_slot;
RecenterCombo s_recenterCombo;  // L3 + R3 held for half a second
double s_now = 0.0;
bool s_swordPending = false;
double s_spinUntil = -1.0;
double s_bashUntil = -1.0;
double s_calibratingUntil = -1.0;
bool s_wasEnabled = false;
Layout s_layout = Layout::None;
int s_swordSide = 1;
int s_nunchukSide = 0;
aurora::device::SensorLayout s_sensors;

// Each sensor's own clock, from the time every sample covers; 0 = left Joy-Con, 1 = right, 2 = the only sensor.
double s_clock[3] = {};

// The tuning in force: the raw values last read from the settings, and what they became.
RawTuning s_rawTuning;
Tuning s_tuning;
bool s_haveTuning = false;

// Telemetry: state changes, totals and one line per gesture window (rate-limited), never per sample or frame.
unsigned s_swordCount = 0, s_spinCount = 0, s_bashCount = 0, s_rejectCount = 0;
unsigned s_loggedSword = 0, s_loggedSpin = 0, s_loggedBash = 0;
bool s_wasBlocked = false;
double s_lineWindowStart = 0.0;
int s_linesInWindow = 0;
unsigned s_linesSuppressed = 0;
float s_idlePeak = 0.0f;
double s_lastIdleLog = -10.0;
bool s_timingLogged[3] = {};
std::uint64_t s_lastDropped[3] = {};
double s_lastDropLog = -10.0;

BlockContext block_context() {
    BlockContext ctx;
    ctx.ui_visible = ui::any_document_visible();
#ifdef __SWITCH__
    ctx.benchmark = benchmark::active() || benchmark::matrix_active() || benchmark::prep_active() ||
                    benchmark::tour_active();
#endif
    // Plain gameplay: Link exists, no event or cutscene is running, no menu or map is up.
    ctx.no_gameplay = dComIfGp_getPlayer(0) == nullptr || dComIfGp_event_runCheck() ||
                      dMeter2Info_getWindowStatus() != 0;
    ctx.paused = dComIfGp_isPauseFlag() != 0;
    ctx.calibrating = s_now < s_calibratingUntil;
    return ctx;
}

int clock_index(aurora::device::SensorSide side) {
    return side == aurora::device::SensorSide::Left ? 0 : side == aurora::device::SensorSide::Right ? 1 : 2;
}

// The samples of one sensor since the last tick. Their times come from what each covers (Aurora checks the unit and falls
// back to the sampling number); a backend that cannot say (delta_us 0) gets them spread over the tick.
std::vector<Sample> read_samples(aurora::device::SensorSide side, float dt) {
    aurora::device::MotionSample raw[kMaxSamplesPerTick];
    const int n = aurora::device::motion_samples(side, raw, kMaxSamplesPerTick);
    std::vector<Sample> out;
    out.reserve(static_cast<size_t>(n));
    const int idx = clock_index(side);
    const double step = n > 0 ? static_cast<double>(dt) / n : 0.0;
    for (int i = 0; i < n; ++i) {
        Sample s;
        if (raw[i].delta_us != 0) {
            s_clock[idx] += raw[i].delta_us * 1e-6;
            s.t = s_clock[idx];
        } else {
            s.t = s_now - dt + (i + 1) * step;
        }
        s.accel = {raw[i].accel[0], raw[i].accel[1], raw[i].accel[2]};
        s.gyro = {raw[i].gyro[0], raw[i].gyro[1], raw[i].gyro[2]};
        s.gap = (raw[i].flags & aurora::device::kMotionGapBefore) != 0;
        out.push_back(s);
    }
    // What the console really sends for delta_time, once, and any samples lost, rarely.
    aurora::device::MotionStats st;
    if (aurora::device::motion_stats(side, &st)) {
        if (!s_timingLogged[idx] && st.samples >= 400) {
            s_timingLogged[idx] = true;
            DuskLog.info("wii motion: sensor {} delta_time raw {}..{} read as ns:{} us:{} fallback:{}",
                idx == 0 ? "left" : idx == 1 ? "right" : "primary", st.raw_delta_min, st.raw_delta_max,
                st.as_nanoseconds, st.as_microseconds, st.fallback);
        }
        if (st.dropped > s_lastDropped[idx] && s_now - s_lastDropLog > 2.0) {
            DuskLog.warn("wii motion: sensor {} lost {} sample(s) so far ({} gap event(s)); a movement across a gap is not "
                         "taken as a gesture",
                idx == 0 ? "left" : idx == 1 ? "right" : "primary", st.dropped, st.gaps);
            s_lastDropLog = s_now;
        }
        s_lastDropped[idx] = st.dropped;
    }
    return out;
}

const char* layout_name(Layout l) {
    switch (l) {
    case Layout::DetachedPair: return "Joy-Con pair";
    case Layout::Single: return "single sensor";
    default: return "no sensor";
    }
}

const char* side_name(int side) {
    return side == 0 ? "L" : side == 1 ? "R" : "P";
}

void drop_state() {
    s_engine.reset();
    s_slot.reset();
    s_swordPending = false;
    s_spinUntil = s_bashUntil = -1.0;
}

RawTuning read_raw_tuning() {
    const auto& g = getSettings().game;
    RawTuning r;
    r.accelThresholdG = g.wiiAccelThresholdG.getValue();
    r.gyroThresholdRad = g.wiiGyroThresholdRad.getValue();
    r.dominance = g.wiiDirectionDominance.getValue();
    r.minYawRad = g.wiiMinYawRad.getValue();
    r.lateralAxis[0] = g.wiiAxisLateralLeft.getValue();
    r.forwardAxis[0] = g.wiiAxisForwardLeft.getValue();
    r.forwardSign[0] = g.wiiForwardSignLeft.getValue();
    r.lateralAxis[1] = g.wiiAxisLateralRight.getValue();
    r.forwardAxis[1] = g.wiiAxisForwardRight.getValue();
    r.forwardSign[1] = g.wiiForwardSignRight.getValue();
    return r;
}

bool same_raw(const RawTuning& a, const RawTuning& b) {
    if (!(a.accelThresholdG == b.accelThresholdG && a.gyroThresholdRad == b.gyroThresholdRad &&
            a.dominance == b.dominance && a.minYawRad == b.minYawRad)) {
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        if (a.lateralAxis[i] != b.lateralAxis[i] || a.forwardAxis[i] != b.forwardAxis[i] ||
            a.forwardSign[i] != b.forwardSign[i]) {
            return false;
        }
    }
    return true;
}

// Validates the config.json values when they change (and on first use) and says what is in force, once per change.
void refresh_tuning() {
    const RawTuning raw = read_raw_tuning();
    if (s_haveTuning && same_raw(raw, s_rawTuning)) {
        return;
    }
    std::string notes;
    s_tuning = sanitize(raw, &notes);
    s_rawTuning = raw;
    s_haveTuning = true;
    const auto& g = getSettings().game;
    DuskLog.info("wii motion tuning: accel {:.2f} g, gyro {:.2f} rad/s, dominance {:.2f}, min yaw {:.2f} rad | left Joy-Con as "
                 "Nunchuk: lateral {} forward {} sign {} | right: lateral {} forward {} sign {} | sword hand {}, swing "
                 "cuts {}, swap sides {}, mirror mode {}",
        s_tuning.accelThresholdG, s_tuning.gyroThresholdRad, s_tuning.dominance, s_tuning.minYawRad,
        s_tuning.axes[0].lateralOverride, s_tuning.axes[0].forwardOverride, s_tuning.axes[0].forwardSign,
        s_tuning.axes[1].lateralOverride, s_tuning.axes[1].forwardOverride, s_tuning.axes[1].forwardSign,
        g.wiiMotionSwordHand.getValue() == WiiSwordHand::RIGHT ? "right" : "left", g.wiiMotionSwingCuts.getValue() ? "on" : "off",
        g.wiiMotionSwapSides.getValue() ? "on" : "off", g.enableMirrorMode.getValue() ? "on" : "off");
    if (!notes.empty()) {
        DuskLog.warn("wii motion tuning: {}(axes -1 = the built-in table)", notes);
    }
}

bool line_allowed() {
    if (s_now - s_lineWindowStart >= 1.0) {
        if (s_linesSuppressed != 0) {
            DuskLog.info("wii motion: {} gesture line(s) not shown (rate limit)", s_linesSuppressed);
            s_linesSuppressed = 0;
        }
        s_lineWindowStart = s_now;
        s_linesInWindow = 0;
    }
    if (s_linesInWindow >= kLinesPerSecond) {
        ++s_linesSuppressed;
        return false;
    }
    ++s_linesInWindow;
    return true;
}

void log_report(const Report& r, const GestureEngine& engine) {
    if (!line_allowed()) {
        return;
    }
    const Features& f = r.f;
    const int hand = engine.handedness(r.side);
    if (r.kind == Report::Kind::Sword) {
        if (r.accepted) {
            DuskLog.info("wii sword {}: {} Ev={:.3f} Eh={:.3f} yaw={:.2f} yawPeak={:.2f} aPeak={:.2f}g wPeak={:.2f} act={:.0f}ms "
                         "gravityAxis={} gyroHand={}",
                side_name(r.side), gesture_name(static_cast<SwordGesture>(r.verdict)), f.energyVertical, f.energyHorizontal,
                f.yaw, f.yawPeak, f.accelPeakG, f.gyroPeakRad, f.activeTime * 1000.0, f.gravityAxis, hand);
        } else {
            DuskLog.info("wii sword {} rejected ({}): aPeak={:.2f}g wPeak={:.2f} act={:.0f}ms", side_name(r.side),
                reject_name(r.reason), f.accelPeakG, f.gyroPeakRad, f.activeTime * 1000.0);
        }
    } else {
        const char* what = r.accepted ? (r.verdict == static_cast<int>(NunchukGesture::Spin) ? "spin" : "shield bash")
                                      : reject_name(r.reason);
        DuskLog.info("wii nunchuk {}: {}{} E[x,y,z]=[{:.3f},{:.3f},{:.3f}] first[x,y,z]=[{:.2f},{:.2f},{:.2f}] gravityAxis={} "
                     "aPeak={:.2f}g",
            side_name(r.side), r.accepted ? "" : "none: ", what, f.energyAxis[0], f.energyAxis[1], f.energyAxis[2],
            f.firstPeak[0], f.firstPeak[1], f.firstPeak[2], f.gravityAxis, f.accelPeakG);
    }
}

// The L3 + R3 chord: recenter, then say so -- a short notice and a short vibration. Never reaches the game's pad.
void poll_recenter_combo(float dt) {
    bool left = false, right = false;
    if (!aurora::device::stick_clicks(&left, &right)) {
        s_recenterCombo.reset();  // no controller: nothing in flight, and the next hold waits for a release
        return;
    }
    if (!s_recenterCombo.update(left, right, dt)) {
        return;
    }
    recalibrate();
    DuskLog.info("wii motion controls: recentered with L3 + R3");
    ui::push_toast({.title = "Motion sensors recentered",
        .content = "Hold the controllers still for a moment.",
        .duration = std::chrono::seconds(2)});
    aurora::device::rumble(0x4000, 0x6000, 40);
}

}  // namespace

void sample(float dt) {
    s_now += dt;
    if (s_now > s_spinUntil) s_spinUntil = -1.0;
    if (s_now > s_bashUntil) s_bashUntil = -1.0;

    auto& settings = getSettings().game;
    const bool enabled = settings.enableWiiMotion.getValue();
    if (!enabled) {
        s_recenterCombo.reset();
        if (s_wasEnabled) {
            s_wasEnabled = false;
            drop_state();
            aurora::device::stop_side_sensors();
            s_layout = Layout::None;
            DuskLog.info("wii motion controls off: {} sword, {} spin, {} shield bash, {} rejected", s_swordCount, s_spinCount,
                s_bashCount, s_rejectCount);
        }
        return;
    }
    if (!s_wasEnabled) {
        s_wasEnabled = true;
        s_swordCount = s_spinCount = s_bashCount = s_rejectCount = 0;
        s_loggedSword = s_loggedSpin = s_loggedBash = 0;
        s_haveTuning = false;  // say what is in force
        for (auto& v : s_clock) v = 0.0;
        for (auto& v : s_timingLogged) v = false;
        DuskLog.info("wii motion controls on");
    }
    refresh_tuning();
    poll_recenter_combo(dt);

    s_sensors = aurora::device::sensor_layout(true);
    Layout layout = Layout::None;
    if (s_sensors.detached_pair && s_sensors.left && s_sensors.right) {
        layout = Layout::DetachedPair;
    } else if (s_sensors.primary) {
        layout = Layout::Single;
    }
    // Roles: with a pair, the sword hand's Joy-Con swings the sword and the other is the Nunchuk; one sensor is the sword.
    const bool swordRight = settings.wiiMotionSwordHand.getValue() == WiiSwordHand::RIGHT;
    const int swordSide = layout == Layout::DetachedPair ? (swordRight ? 1 : 0) : 2;
    const int nunchukSide = layout == Layout::DetachedPair ? (swordRight ? 0 : 1) : 2;
    if (layout != s_layout || swordSide != s_swordSide) {
        DuskLog.info("wii motion controls: {} -> {} (sword: {}, Nunchuk: {})", layout_name(s_layout), layout_name(layout),
            side_name(swordSide), layout == Layout::DetachedPair ? side_name(nunchukSide) : "-");
        s_layout = layout;
        s_swordSide = swordSide;
        s_nunchukSide = nunchukSide;
        drop_state();
    }

    const bool blocked = block_context().blocked();
    if (blocked != s_wasBlocked) {
        s_wasBlocked = blocked;
        DuskLog.debug("wii motion gestures {}", blocked ? "held" : "active");
    }

    // Always drain the sensors, so a blocked stretch does not come back as a burst of stale samples. motion_samples() is
    // the only reader of these queues.
    std::vector<Sample> swordSamples, nunchukSamples;
    const auto sideOf = [](int side) {
        return side == 0 ? aurora::device::SensorSide::Left
                         : side == 1 ? aurora::device::SensorSide::Right : aurora::device::SensorSide::Primary;
    };
    if (layout == Layout::DetachedPair) {
        swordSamples = read_samples(sideOf(swordSide), dt);
        nunchukSamples = read_samples(sideOf(nunchukSide), dt);
    } else if (layout == Layout::Single) {
        swordSamples = read_samples(aurora::device::SensorSide::Primary, dt);
    }

    Inputs in;
    in.enabled = true;
    in.blocked = blocked;
    in.layout = layout;
    in.sensitivity = settings.wiiMotionSensitivity.getValue();
    in.swordSide = swordSide;
    in.nunchukSide = std::min(nunchukSide, 1);
    in.tuning = s_tuning;
    const Pulses pulses =
        s_engine.update(in, {swordSamples.data(), swordSamples.size()}, {nunchukSamples.data(), nunchukSamples.size()});
    for (int i = 0; i < pulses.reportCount; ++i) {
        const Report& r = pulses.reports[i];
        if (!r.accepted) {
            ++s_rejectCount;
        }
        log_report(r, s_engine);
    }
    if (pulses.sword) {
        s_swordPending = true;
        ++s_swordCount;
        if (settings.wiiMotionSwingCuts.getValue()) {
            s_slot.offer(pulses.serial, pulses.gesture, s_now);
        }
    }
    if (pulses.spin) {
        s_spinUntil = s_now + kLatchSeconds;
        ++s_spinCount;
    }
    if (pulses.shield_bash) {
        s_bashUntil = s_now + kLatchSeconds;
        ++s_bashCount;
    }
    // Near misses: the strongest movement that did not open a window, when it was at least a good part of the way there.
    s_idlePeak = std::max({s_idlePeak, s_engine.take_sword_idle_peak(), s_engine.take_nunchuk_idle_peak()});
    const float openAt = s_tuning.accelThresholdG / clamp_sensitivity(in.sensitivity);
    if (!blocked && s_idlePeak >= 0.45f * openAt && s_now - s_lastIdleLog > 3.0) {
        DuskLog.info("wii motion: strongest unclaimed movement {:.2f} g (a gesture opens at {:.2f} g)", s_idlePeak, openAt);
        s_lastIdleLog = s_now;
        s_idlePeak = 0.0f;
    } else if (s_now - s_lastIdleLog > 3.0) {
        s_idlePeak = 0.0f;
    }
    // A summary when the totals moved and a while has passed -- not per gesture, and never per frame.
    const unsigned total = s_swordCount + s_spinCount + s_bashCount;
    if (total >= s_loggedSword + s_loggedSpin + s_loggedBash + 25) {
        s_loggedSword = s_swordCount;
        s_loggedSpin = s_spinCount;
        s_loggedBash = s_bashCount;
        DuskLog.info("wii motion gestures so far: {} sword, {} spin, {} shield bash, {} rejected; gesture slot: {} offered, {} "
                     "used, {} discarded, {} expired, {} replaced",
            s_swordCount, s_spinCount, s_bashCount, s_rejectCount, s_slot.offered(), s_slot.taken(), s_slot.discarded(),
            s_slot.expired(), s_slot.replaced());
    }
}

bool take_sword_pulse() {
    const bool pending = s_swordPending;
    s_swordPending = false;
    return pending;
}

bool consume_spin() {
    if (s_spinUntil < 0.0) return false;
    s_spinUntil = -1.0;
    return true;
}

bool consume_shield_bash() {
    if (s_bashUntil < 0.0) return false;
    s_bashUntil = -1.0;
    return true;
}

GestureSlot::Taken take_sword_gesture() {
    return s_slot.take(s_now);
}

CutStyle cut_style_for(const GestureSlot::Taken& taken, CutBranch branch) {
    if (!taken.valid()) {
        return CutStyle::Keep;
    }
    const auto& g = getSettings().game;
    return select_cut(taken.gesture, branch, g.enableMirrorMode.getValue(), g.wiiMotionSwapSides.getValue());
}

void discard_sword_gesture() {
    s_slot.discard();
}

void recalibrate() {
    drop_state();
    s_calibratingUntil = s_now + kCalibrationSeconds;
    DuskLog.info("wii motion controls: recentered");
}

bool available() {
    return s_layout != Layout::None;
}

std::string status_text() {
    if (!getSettings().game.enableWiiMotion.getValue()) {
        return "Off.";
    }
    const bool swordRight = getSettings().game.wiiMotionSwordHand.getValue() == WiiSwordHand::RIGHT;
    std::string s = swordRight ? "Right Joy-Con (sword): " : "Left Joy-Con (sword): ";
    const bool swordDetected = s_sensors.detached_pair ? (swordRight ? s_sensors.right : s_sensors.left) : s_sensors.primary;
    s += swordDetected ? "detected" : "not detected";
    s += swordRight ? ". Left Joy-Con (Nunchuk): " : ". Right Joy-Con (Nunchuk): ";
    s += s_sensors.detached_pair ? ((swordRight ? s_sensors.left : s_sensors.right) ? "detected" : "not detected")
                                 : "not detected";
    s += ".\n";
    switch (s_layout) {
    case Layout::DetachedPair:
        s += "Available: swing for the sword (the direction picks the cut), sideways for the spin attack, push forward for "
             "the shield bash.";
        break;
    case Layout::Single:
        s += "Available: swing for the sword. Spin and shield bash need two detached Joy-Con (they stay on the buttons).";
        break;
    default:
        s += "No motion sensor found: the buttons work as usual.";
        break;
    }
    if (s_now < s_calibratingUntil) {
        s += " Recentering...";
    }
    s += "\nRecenter shortcut: hold L3 + R3 together for half a second.";
    return s;
}

}  // namespace dusk::wii
