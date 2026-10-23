// V1-rc9: the sword classification replayed on what the console logged (test-wii-gestures.sh). Each "wii sword"
// line carries the window's features; classify_sword runs again on them with the current rules and default tuning.
//   data/wii_rc8_111711.txt  rc8, tester swinging for real: strong windows were rejected as wind-ups
//   data/wii_rc7_000552.txt  rc7, the session where raising the arm attacked
// Rejected lines do not log yawPeak or the gyro handedness: those replay with yawPeak 0 and the handedness known, which
// is enough for the wind-up rules (they read neither).
#include "dusk/wii_gesture_detector.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>

using namespace dusk::wii;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

bool field(const std::string& line, const char* key, float& out) {
    const auto at = line.find(std::string(" ") + key + "=");
    if (at == std::string::npos) {
        return false;
    }
    out = std::strtof(line.c_str() + at + std::strlen(key) + 2, nullptr);
    return true;
}

struct Replayed {
    std::string logged;  // what the console said: a gesture or "rejected (reason)"
    SwordVerdict now;
    Features f;
};

bool parse(const std::string& line, Replayed& r) {
    const auto at = line.find("wii sword ");
    if (at == std::string::npos) {
        return false;
    }
    const auto colon = line.find(':', at);
    const auto rejected = line.find("rejected (", at);
    if (rejected != std::string::npos && rejected < colon) {
        r.logged = line.substr(rejected, line.find(')', rejected) + 1 - rejected);
    } else {
        const auto start = colon + 2;
        r.logged = line.substr(start, line.find(' ', start) - start);
    }
    float ev = 0, eh = 0, yaw = 0, yawPeak = 0, a = 0, w = 0, act = 0, fv = 0, hand = 1;
    if (!field(line, "Ev", ev) || !field(line, "Eh", eh) || !field(line, "aPeak", a) || !field(line, "wPeak", w) ||
        !field(line, "firstVert", fv)) {
        return false;
    }
    field(line, "yaw", yaw);
    field(line, "yawPeak", yawPeak);
    field(line, "act", act);
    field(line, "gyroHand", hand);
    r.f.energyVertical = ev;
    r.f.energyHorizontal = eh;
    r.f.yaw = yaw;
    r.f.yawPeak = yawPeak;
    r.f.accelPeakG = a;
    r.f.gyroPeakRad = w;
    r.f.activeTime = act / 1000.0;
    r.f.duration = act / 1000.0;
    r.f.firstVert = fv;
    r.f.handednessKnown = hand != 0.0f;
    return true;
}

const char* name(SwordGesture g) {
    switch (g) {
    case SwordGesture::None: return "none";
    case SwordGesture::Generic: return "generic";
    case SwordGesture::Vertical: return "vertical";
    case SwordGesture::LeftToRight: return "left-to-right";
    case SwordGesture::RightToLeft: return "right-to-left";
    case SwordGesture::Thrust: return "thrust";
    }
    return "?";
}

std::string verdict_name(const SwordVerdict& v) {
    return v.gesture != SwordGesture::None ? name(v.gesture)
                                           : std::string("rejected (") + reject_name(v.reason) + ")";
}

void replay(const char* path, bool rc8) {
    std::ifstream in(path);
    CHECK(static_cast<bool>(in));
    const Tuning t = sanitize(RawTuning{});
    std::map<std::string, std::map<std::string, int>> table;  // logged -> now -> count
    int lines = 0;
    std::string line;
    while (std::getline(in, line)) {
        Replayed r;
        if (!parse(line, r)) {
            continue;
        }
        if (r.logged == "rejected (rebound)" || r.logged == "rejected (gap)") {
            continue;  // decided by the engine from the windows around it, not by classify_sword
        }
        ++lines;
        r.now = classify_sword(r.f, t, tuning::kAccelThresholdG);
        ++table[r.logged][verdict_name(r.now)];
        const float sv = std::sqrt(r.f.energyVertical), sh = std::sqrt(r.f.energyHorizontal);
        const bool strong = r.f.accelPeakG >= t.windupMaxG || r.f.gyroPeakRad >= t.windupMaxGyroRad;
        // a strong window is never a wind-up, whichever way it started
        if (strong) {
            CHECK(r.now.reason != RejectReason::Windup);
        }
        // a weak one that rose first, with enough vertical in it, still is (the rc7 complaint)
        if (!strong && r.f.firstVert > tuning::kWindupMinG && sv >= tuning::kWindupShare * sh &&
            r.f.accelPeakG >= tuning::kAccelThresholdG * tuning::kRotationOnlyAccelRatio && r.f.activeTime >=
            tuning::kMinActiveS) {
            CHECK(r.now.reason == RejectReason::Windup);
        }
        // nothing logged as an attack in rc8 is lost (rc7's included the rises that attacked)
        if (rc8 && r.logged.rfind("rejected", 0) != 0) {
            CHECK(r.now.gesture != SwordGesture::None);
        }
    }
    std::printf("%s: %d windows replayed (logged -> now)\n", path, lines);
    for (const auto& [logged, now] : table) {
        for (const auto& [verdict, count] : now) {
            if (logged != verdict) {
                std::printf("   %3d  %-26s -> %s\n", count, logged.c_str(), verdict.c_str());
            }
        }
    }
    if (rc8) {
        // the rc8 session's wind-up rejections were real swings (18 of 19 over 14 rad/s): only a weak one may stay
        CHECK(table["rejected (windup)"]["rejected (windup)"] <= 1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "data";
    replay((dir + "/wii_rc8_111711.txt").c_str(), true);
    replay((dir + "/wii_rc7_000552.txt").c_str(), false);
    if (g_failures == 0) {
        std::puts("wii replay: rc8 strong windows never wind-ups, rc7 weak rises still are, no logged attack lost: ok");
    }
    return g_failures == 0 ? 0 : 1;
}
