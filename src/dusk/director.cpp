#include "dusk/director.hpp"

#include "dusk/achievement_signals.hpp"
#include "dusk/achievements.h"
#include "dusk/benchmark.hpp"
#include "dusk/dev_mode.hpp"
#include "dusk/logging.h"
#include "dusk/perf_profile_apply.hpp"
#include "dusk/settings.h"

#include "dusk/session_log.hpp"
#include "dusk/ui/document.hpp"
#include "dusk/ui/ui.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_camera.h"
#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_overlap_mng.h"
#include "m_Do/m_Do_controller_pad.h"

#include <fmt/format.h>
#include <aurora/aurora.h>

#ifdef __SWITCH__
#include <switch.h>
#endif
#include <ctime>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace dusk::director {
namespace {

inline constexpr borealis::Log Log{"director"};
using clock = std::chrono::steady_clock;
constexpr const char* kTakesPath = "sdmc:/switch/dusklight/director.txt";
constexpr const char* kKeysPath = "sdmc:/switch/dusklight/director-keys.txt";
constexpr const char* kCheckPath = "sdmc:/switch/dusklight/check.txt";
constexpr const char* kPadDir = "sdmc:/switch/dusklight/director";
constexpr double kCountdown = 3.0;
constexpr double kReelCountdown = 1.5;  // the reel: a short settle before each take (no one is waiting to press R)
constexpr double kCheckLoadTimeout = 90.0;  // V1-rc12: a check spot that never settles is skipped (the reel too)
constexpr char kPadMagic[8] = {'D', 'K', 'P', 'A', 'D', '1', 0, 0};

struct Key {
    float t = 0.0f;
    cXyz eye, center;
    float fovy = 60.0f;
};

// cam_follow=dist,height,yaw_speed[,fovy]: an orbit around Link. Starts behind him, turns yaw_speed degrees a second.
struct Follow {
    bool on = false;
    float dist = 400.0f;
    float height = 150.0f;
    float yawSpeed = 0.0f;
    float fovy = 60.0f;
};

struct Spawn {
    s16 proc = 0;
    cXyz pos;
    u32 params = 0;
};

enum class Form : u8 { Link, Wolf, Epona };
enum class Controls : u8 { Locked, Player, Recorded };

struct Take {
    std::string id, stage;
    int room = 0, point = 0, layer = -1;
    float hour = -1.0f;  ///< 0..24, < 0 leaves the save's time
    bool rain = false;
    bool hud = false;
    Form form = Form::Link;
    Controls controls = Controls::Locked;
    float duration = 10.0f;
    std::vector<Key> keys;  ///< empty: cam_follow, or the game's own camera
    Follow follow;
    std::vector<Spawn> spawns;
    // The look, for this take only (restored when it ends). -1: leave as the player has it.
    bool recordingMode = true;
    int music = -1;   ///< 0 off, 1 on
    int preset = -1;  ///< kPerfPresetQuality / Balanced / Performance
    int bloom = -1;   ///< BloomMode
    int dof = -1;     ///< DepthOfFieldMode
};

enum class Phase : u8 { Idle, Armed, Loading, Countdown, Running, Restoring };
enum class CamMethod : u8 { None, Set, Stopped };

struct Session {
    Phase phase = Phase::Idle;
    std::vector<Take> takes;
    int current = -1;
    clock::time_point phaseStart;
    // Restored at the end, as the tour does.
    std::string originalStage;
    int originalRoom = 0, originalPoint = 0, originalLayer = -1;
    std::unique_ptr<dSv_info_c> savedInfo;
    std::unique_ptr<ScopedAchievementSuspension<AchievementSystem>> achievements;
    u8 transformAtStart = 0;
    float timeAtStart = 0.0f;
    bool spawned = false;
    bool autoRun = false;  // V1-rc12: Check Tour
    bool reel = false;     // trailer: every take of director.txt, one after the other
    bool autoWaitingForUi = false;  // armed from Settings: the first warp waits for the menu to close
    std::string checkFile;
    // The take that is rolling.
    CamMethod cam = CamMethod::None;
    cXyz lastEye;
    bool lastEyeSet = false;
    s16 followYaw0 = 0;
    std::vector<interface_of_controller_pad> recording;  // controles=jugador: every frame, for L+R+Up
    std::vector<interface_of_controller_pad> playback;   // controles=grabado
    std::size_t playIndex = 0;
    std::vector<std::function<void()>> lookRestore;
    bool checkedForm = false;
    int rainLowTicks = 0;
    bool rainLogged = false;
};

Session g;

double since(clock::time_point t) { return std::chrono::duration<double>(clock::now() - t).count(); }

void set_phase(Phase p) {
    g.phase = p;
    g.phaseStart = clock::now();
}

bool parse_vec(std::istringstream& in, cXyz& v) {
    char c;
    return static_cast<bool>(in >> v.x >> c >> v.y >> c >> v.z);
}

int parse_mode(const std::string& s) {  // off / classic / dusk, as BloomMode and DepthOfFieldMode
    return s == "off" ? 0 : s == "classic" ? 1 : s == "dusk" ? 2 : -1;
}

// director.txt: "[id]" starts a take, then key=value lines (see exportar_director.py). The format is in director.hpp.
std::vector<Take> load_takes(const char* path = kTakesPath) {
    std::vector<Take> takes;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            takes.push_back({});
            takes.back().id = line.substr(1, line.find(']') - 1);
            continue;
        }
        if (takes.empty()) continue;
        auto& t = takes.back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        std::istringstream v(line.substr(eq + 1));
        char c;
        if (k == "stage") v >> t.stage;
        else if (k == "room") v >> t.room;
        else if (k == "point") v >> t.point;
        else if (k == "layer") v >> t.layer;
        else if (k == "hora") v >> t.hour;
        else if (k == "lluvia") v >> t.rain;
        else if (k == "hud") v >> t.hud;
        else if (k == "dur") v >> t.duration;
        else if (k == "forma") {
            std::string s; v >> s;
            t.form = s == "lobo" ? Form::Wolf : s == "epona" ? Form::Epona : Form::Link;
        } else if (k == "controles") {
            std::string s; v >> s;
            t.controls = s == "jugador" ? Controls::Player : s == "grabado" ? Controls::Recorded : Controls::Locked;
        } else if (k == "cam") {  // t,ex,ey,ez,cx,cy,cz,fovy
            Key key;
            v >> key.t >> c;
            if (parse_vec(v, key.eye) && v >> c && parse_vec(v, key.center)) {
                v >> c >> key.fovy;
                t.keys.push_back(key);
            }
        } else if (k == "cam_follow") {  // dist,height,yaw_speed[,fovy]
            Follow fo;
            if (v >> fo.dist >> c >> fo.height >> c >> fo.yawSpeed) {
                if (!(v >> c >> fo.fovy)) fo.fovy = 60.0f;
                fo.on = true;
                t.follow = fo;
            }
        } else if (k == "spawn") {  // proc,x,y,z,params(hex)
            Spawn s;
            v >> s.proc >> c;
            if (parse_vec(v, s.pos)) {
                v >> c >> std::hex >> s.params;
                t.spawns.push_back(s);
            }
        } else if (k == "grabacion") v >> t.recordingMode;
        else if (k == "musica") v >> t.music;
        else if (k == "preset") {
            std::string s; v >> s;
            t.preset = s == "quality" ? kPerfPresetQuality : s == "balanced" ? kPerfPresetBalanced :
                       s == "performance" ? kPerfPresetPerformance : -1;
        } else if (k == "bloom") {
            std::string s; v >> s;
            t.bloom = parse_mode(s);
        } else if (k == "dof") {
            std::string s; v >> s;
            t.dof = parse_mode(s);
        }
    }
    Log.info("director: {} takes read from {}", takes.size(), path);
    return takes;
}

// V1-rc12: the Check Tour's spots. Fixed so every build and both backends (and the emulator) see the same frames;
// sdmc:/switch/dusklight/check.txt replaces the list (director.txt's format: add cam= keys to pin a camera).
// No cam keys: the game's own camera behind a Link who does not move.
std::vector<Take> check_takes() {
    struct Spot {
        const char* id;
        const char* stage;
        int room, point;
        float hour, hold;
    };
    static constexpr Spot kSpots[] = {
        {"gorge-night", "F_SP121", 3, 5, 23.0f, 25.0f},     // black ground (deko3d repro: black from ~90 s with a Kargarok)
        {"gorge-day", "F_SP121", 3, 5, 12.0f, 15.0f},
        {"ordon-pumpkins", "F_SP103", 0, 0, 12.0f, 10.0f},  // dark squares; VERIFY the point faces the patch
        {"field-castle", "F_SP121", 0, 0, 12.0f, 12.0f},    // heavy (the tour's Hyrule Field)
        {"castle-west", "F_SP122", 8, 0, 12.0f, 12.0f},
        {"ordon-village", "F_SP103", 0, 1, 12.0f, 10.0f},
        {"links-house", "R_SP01", 4, 0, -1.0f, 8.0f},
        {"mirror-chamber", "F_SP125", 4, 0, 12.0f, 12.0f},  // portal flicker; may need the story to have reached it
    };
    std::vector<Take> takes;
    for (const auto& s : kSpots) {
        Take t;
        t.id = s.id;
        t.stage = s.stage;
        t.room = s.room;
        t.point = s.point;
        t.hour = s.hour;
        t.hud = true;  // the minimap and the HUD are part of what is measured
        t.recordingMode = false;
        t.duration = s.hold;
        takes.push_back(std::move(t));
    }
    return takes;
}

const char* backend_name() {
    switch (aurora_get_backend()) {
    case BACKEND_DEKO3D:
        return "deko3d";
    case BACKEND_VULKAN:
        return "vulkan";
    default:
        return "other";
    }
}

std::string local_time_text() {
    const std::time_t now = std::time(nullptr);
    char buf[32] = "?";
    if (const std::tm* tm = std::localtime(&now)) {
        std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", tm);
    }
    return buf;
}

// The console's own screenshot (as the Capture button): the final frame whatever the backend, in the Album.
bool request_album_screenshot() {
#ifdef __SWITCH__
    static bool permitted = false;
    if (!permitted) {
        appletSetScreenShotPermission(AppletScreenShotPermission_Enable);
        permitted = true;
    }
    return R_SUCCEEDED(appletPerformSystemButtonPressingIfInFocus(AppletSystemButtonType_CaptureButtonShortPressing));
#else
    return false;
#endif
}

void check_spot_done(const Take& t, bool skipped) {
    const std::string when = local_time_text();
    const bool shot = !skipped && request_album_screenshot();
    session_log::requestHeartbeat();
    const char* stage = dComIfGp_getStartStageName();
    const std::string line = fmt::format("{} spot={} stage={} room={} point={} hour={:.0f} at={} shot={}\n", backend_name(),
        t.id, stage != nullptr ? stage : "?", dComIfGp_getStartStageRoomNo(), t.point, t.hour, when,
        skipped ? "skipped (did not load)" : shot ? "album" : "failed");
    if (FILE* f = std::fopen(g.checkFile.c_str(), "a")) {
        std::fputs(line.c_str(), f);
        std::fclose(f);
    }
    Log.info("check: {}", line.substr(0, line.size() - 1));
}

// Catmull-Rom through the key frames, so the camera eases between them with no corners.
cXyz catmull(const cXyz& p0, const cXyz& p1, const cXyz& p2, const cXyz& p3, float u) {
    const float u2 = u * u, u3 = u2 * u;
    auto f = [&](float a, float b, float c, float d) {
        return 0.5f * ((2 * b) + (-a + c) * u + (2 * a - 5 * b + 4 * c - d) * u2 + (-a + 3 * b - 3 * c + d) * u3);
    };
    return cXyz(f(p0.x, p1.x, p2.x, p3.x), f(p0.y, p1.y, p2.y, p3.y), f(p0.z, p1.z, p2.z, p3.z));
}

// Where the take's camera is at `time`: the keys if it has any, else the orbit around Link. False: the game's own.
bool camera_at(const Take& t, float time, cXyz& eye, cXyz& center, float& fovy) {
    if (!t.keys.empty()) {
        const auto& k = t.keys;
        std::size_t i = 0;
        while (i + 1 < k.size() && k[i + 1].t <= time) ++i;
        if (i + 1 >= k.size()) {
            eye = k.back().eye;
            center = k.back().center;
            fovy = k.back().fovy;
            return true;
        }
        const float span = k[i + 1].t - k[i].t;
        float u = span > 0 ? (time - k[i].t) / span : 1.0f;
        u = u * u * (3 - 2 * u);  // ease in and out of every key
        const auto& a = k[i > 0 ? i - 1 : i];
        const auto& d = k[i + 2 < k.size() ? i + 2 : i + 1];
        eye = catmull(a.eye, k[i].eye, k[i + 1].eye, d.eye, u);
        center = catmull(a.center, k[i].center, k[i + 1].center, d.center, u);
        fovy = k[i].fovy + (k[i + 1].fovy - k[i].fovy) * u;
        return true;
    }
    if (t.follow.on) {
        daAlink_c* link = daAlink_getAlinkActorClass();
        if (link == nullptr) return false;
        const cXyz& p = link->current.pos;
        // The turn eases in over the first second (its speed ramps from 0), so the orbit does not jerk into motion.
        const float turnedTime = time < 1.0f ? 0.5f * time * time : time - 0.5f;
        constexpr float kPi = 3.14159265f;
        const float yaw = static_cast<float>(g.followYaw0) * (kPi / 32768.0f) + t.follow.yawSpeed * turnedTime * (kPi / 180.0f);
        center = cXyz(p.x, p.y + t.follow.height * 0.5f, p.z);
        eye = cXyz(p.x + std::sin(yaw) * t.follow.dist, p.y + t.follow.height, p.z + std::cos(yaw) * t.follow.dist);
        fovy = t.follow.fovy;
        return true;
    }
    return false;
}

// Every frame. dCamera_c::Set alone writes the camera's eye and centre, but the camera's own mode may recompute them
// before the frame is drawn. So: Set, and the next frame look whether the camera still has what was set; if it moved
// away, stop the game camera (dCamera_c::Stop: it holds still and takes what Set gives it) for the rest of the take.
void apply_camera(const Take& t, float time) {
    cXyz eye, center;
    float fovy = 60.0f;
    if (!camera_at(t, time, eye, center, fovy)) return;
    dCamera_c* cam = dCam_getBody();
    if (cam == nullptr) return;
    if (g.cam == CamMethod::None) {
        g.cam = CamMethod::Set;
        Log.info("director: take {} camera: dCamera_c::Set every frame", t.id);
    } else if (g.cam == CamMethod::Set && g.lastEyeSet) {
        const cXyz now = cam->Eye();
        const float dx = now.x - g.lastEye.x, dy = now.y - g.lastEye.y, dz = now.z - g.lastEye.z;
        if (dx * dx + dy * dy + dz * dz > 25.0f * 25.0f) {
            cam->Stop();
            g.cam = CamMethod::Stopped;
            Log.info("director: take {} camera: Set alone lost to the game camera ({:.0f} units away); "
                     "game camera stopped, Set every frame",
                t.id, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
    }
    if (g.cam == CamMethod::Stopped) cam->Stop();  // kept stopped: an event may start it again
    cam->Set(center, eye, fovy, 0);
    g.lastEye = eye;
    g.lastEyeSet = true;
}

void release_camera() {
    if (g.cam == CamMethod::Stopped) {
        if (dCamera_c* cam = dCam_getBody()) cam->Start();
    }
    g.cam = CamMethod::None;
    g.lastEyeSet = false;
}

void apply_environment(const Take& t) {
    if (t.hour >= 0.0f) {
        dComIfGs_setTime(t.hour * 15.0f);  // the save keeps time as degrees, 360 = 24 h
    }
    // Rain: raincnt is the rain strength the kankyo code reads every frame. Whether it held since the last frame is
    // the check: if the stage keeps zeroing it (an interior, an event), the take goes on dry and the log says so.
    if (t.rain && g.phase == Phase::Running && !g.rainLogged) {
        g.rainLowTicks = g_env_light.raincnt < 200 ? g.rainLowTicks + 1 : 0;
        if (g.rainLowTicks > 60) {
            g.rainLogged = true;
            Log.warn("director: take {}: rain does not hold here (the stage resets it); the take goes on", t.id);
        }
    }
    g_env_light.raincnt = t.rain ? 250 : 0;
    if (t.hud) dComIfGp_2dShowOn(); else dComIfGp_2dShowOff();
}

// Wolf and Epona are asked for before the warp; once the take is up, whether they took is checked once and logged.
void check_form(const Take& t) {
    if (g.checkedForm) return;
    g.checkedForm = true;
    if (t.form == Form::Wolf && !daPy_py_c::checkNowWolf()) {
        Log.warn("director: take {}: Link is not a wolf here (the stage or the story does not allow it); the take goes "
                 "on as Link", t.id);
    } else if (t.form == Form::Epona) {
        daAlink_c* link = daAlink_getAlinkActorClass();
        if (link == nullptr || !link->checkHorseRide()) {
            Log.warn("director: take {}: Link is not on Epona at {} point {} (that start point does not seat him); the "
                     "take goes on on foot", t.id, t.stage, t.point);
        }
    }
}

// The look: each setting on its Override layer, which is never saved and is cleared when the take ends. A setting
// still on its Default layer is first set to that default on the Value layer: clearOverride() hands back the Value
// layer, which for a never-set variable holds a zero (perf_profile_apply.hpp has the same trap).
template <typename T>
void override_look(ConfigVar<T>& var, T value) {
    if (var.getLayer() == config::ConfigVarLayer::Override || var.getLayer() == config::ConfigVarLayer::Speedrun) {
        return;  // someone else holds it: leave it
    }
    if (var.getValue() == value) return;
    if (var.getLayer() == config::ConfigVarLayer::Default) var.setValue(var.getDefaultValue());
    var.setOverrideValue(value);
    g.lookRestore.push_back([&var] { var.clearOverride(); });
}

void apply_look(const Take& t) {
    auto& s = getSettings();
    if (t.recordingMode) override_look(s.game.recordingMode, true);
    if (t.music == 0) {
        override_look(s.audio.mainMusicVolume, 0);
        override_look(s.audio.subMusicVolume, 0);
    }
    if (t.preset >= 0) {
        std::string why;
        if (profile::applyOverride(t.preset, why)) {
            g.lookRestore.push_back([] { profile::clearOverrides(); });
        } else {
            Log.warn("director: take {}: preset {} not applied ({}); the take goes on", t.id, profile::name(t.preset), why);
        }
    }
    if (t.bloom >= 0) override_look(s.game.bloomMode, static_cast<BloomMode>(t.bloom));
    if (t.dof >= 0) override_look(s.game.depthOfFieldMode, static_cast<DepthOfFieldMode>(t.dof));
}

void restore_look() {
    for (auto it = g.lookRestore.rbegin(); it != g.lookRestore.rend(); ++it) (*it)();
    g.lookRestore.clear();
}

std::string pad_path(const Take& t) { return fmt::format("{}/{}.pad", kPadDir, t.id); }

// controles=jugador: L+R+Up saves what was played so far as <take>.pad (the L+R+Up press itself left out).
void save_recording(const Take& t) {
    auto frames = g.recording;
    while (!frames.empty()) {
        const u32 b = frames.back().mButtonFlags;
        if (!((b & PAD_TRIGGER_L) && (b & PAD_TRIGGER_R))) break;
        frames.pop_back();
    }
    mkdir(kPadDir, 0777);
    const std::string path = pad_path(t);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        Log.warn("director: take {}: cannot write {}", t.id, path);
        return;
    }
    const u32 count = static_cast<u32>(frames.size()), size = sizeof(interface_of_controller_pad);
    std::fwrite(kPadMagic, 1, sizeof kPadMagic, f);
    std::fwrite(&count, sizeof count, 1, f);
    std::fwrite(&size, sizeof size, 1, f);
    if (count != 0) std::fwrite(frames.data(), size, count, f);
    std::fclose(f);
    Log.info("director: take {}: {} frames of input recorded to {}", t.id, count, path);
}

void load_playback(const Take& t) {
    g.playback.clear();
    g.playIndex = 0;
    const std::string path = pad_path(t);
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        Log.warn("director: take {}: controles=grabado but no {}; the take runs with the controls locked", t.id, path);
        return;
    }
    char magic[8];
    u32 count = 0, size = 0;
    const bool ok = std::fread(magic, 1, sizeof magic, f) == sizeof magic && std::memcmp(magic, kPadMagic, 8) == 0 &&
                    std::fread(&count, sizeof count, 1, f) == 1 && std::fread(&size, sizeof size, 1, f) == 1 &&
                    size == sizeof(interface_of_controller_pad) && count < 100000;
    if (ok) {
        g.playback.resize(count);
        if (count != 0 && std::fread(g.playback.data(), size, count, f) != count) g.playback.clear();
    }
    std::fclose(f);
    if (g.playback.empty()) {
        Log.warn("director: take {}: {} is not a recording of this build; the take runs with the controls locked", t.id,
            path);
    } else {
        Log.info("director: take {}: playing back {} frames from {}", t.id, g.playback.size(), path);
    }
}

// Everything a take changed, back as it was; called when a take ends however it ends. Safe to call twice.
void finish_take() {
    release_camera();
    restore_look();
    g.recording.clear();
    g.playback.clear();
    g.playIndex = 0;
}

void spawn_enemies(const Take& t) {
    for (const auto& s : t.spawns) {
        fopAcM_create(s.proc, s.params, &s.pos, t.room, nullptr, nullptr, -1);
    }
    g.spawned = true;
}

void warp_to(const Take& t) {
    finish_take();
    if ((g.autoRun || g.reel) && dComIfGs_getLife() < dComIfGs_getMaxLife()) {
        dComIfGs_setLife(dComIfGs_getMaxLife());  // V1-rc12: as the tour, never a game over mid-take
    }
    // Form is chosen before the warp: Link spawns already transformed. Epona takes need a start point that puts
    // Link on the horse (the warp table has some); check_form() logs it when the point does not.
    dComIfGs_setTransformStatus(t.form == Form::Wolf ? 1 : 0);
    dComIfGp_setNextStage(t.stage.c_str(), static_cast<s16>(t.point), static_cast<s8>(t.room),
        static_cast<s8>(t.layer));
    g.spawned = false;
    g.checkedForm = false;
    g.rainLowTicks = 0;
    g.rainLogged = false;
    set_phase(Phase::Loading);
    Log.info("director: take {} -> {} room {} point {}", t.id, t.stage, t.room, t.point);
}

bool settled(const Take& t) {
    const char* stage = dComIfGp_getStartStageName();
    return stage != nullptr && t.stage == stage && dComIfGp_getStartStageRoomNo() == t.room &&
           !dComIfGp_isEnableNextStage() && !fopOvlpM_IsDoingReq() && daAlink_getAlinkActorClass() != nullptr;
}

void arm(std::vector<Take> takes) {
    g = Session{};
    g.takes = std::move(takes);
    if (g.takes.empty()) return;
    g.originalStage = dComIfGp_getStartStageName();
    g.originalRoom = dComIfGp_getStartStageRoomNo();
    g.originalPoint = dComIfGp_getStartStagePoint();
    g.originalLayer = dComIfGp_getStartStageLayer();
    g.savedInfo = std::make_unique<dSv_info_c>(*dComIfGs_getSaveInfo());
    g.transformAtStart = dComIfGs_getTransformStatus();
    g.timeAtStart = dComIfGs_getTime();
    g.achievements = std::make_unique<ScopedAchievementSuspension<AchievementSystem>>(AchievementSystem::get());
    set_phase(Phase::Armed);
    Log.info("director: armed, saves blocked, achievements suspended");
}

void end_session() {
    finish_take();
    dComIfGp_2dShowOn();
    g_env_light.raincnt = 0;
    if (g.savedInfo) *dComIfGs_getSaveInfo() = *g.savedInfo;
    dComIfGs_setTransformStatus(g.transformAtStart);
    dComIfGs_setTime(g.timeAtStart);
    dComIfGp_setNextStage(g.originalStage.c_str(), static_cast<s16>(g.originalPoint),
        static_cast<s8>(g.originalRoom), static_cast<s8>(g.originalLayer));
    set_phase(Phase::Restoring);
    Log.info("director: session ended, restoring");
}

bool ui_window_visible() {  // as benchmark.cpp: the tour also waits for Settings to close
    for (const auto scope : {ui::DocumentScope::Window, ui::DocumentScope::MenuBar}) {
        if (const auto* doc = ui::find_document(scope); doc != nullptr && doc->visible()) {
            return true;
        }
    }
    return false;
}

// V1-rc12: Check Tour (and the reel) -- the take after this one, or the end.
void check_next() {
    if (g.current + 1 < static_cast<int>(g.takes.size())) {
        warp_to(g.takes[++g.current]);
    } else {
        if (g.autoRun) Log.info("check: all {} spots done; manifest {}", g.takes.size(), g.checkFile);
        else Log.info("director: reel done, {} takes", g.takes.size());
        end_session();
    }
}

void log_key() {
    dCamera_c* cam = dCam_getBody();
    if (cam == nullptr) return;
    const cXyz e = cam->Eye(), c = cam->Center();
    const float t = g.phase == Phase::Running ? static_cast<float>(since(g.phaseStart)) : 0.0f;
    const std::string line = fmt::format("cam={:.2f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f}  # {} room {}\n",
        t, e.x, e.y, e.z, c.x, c.y, c.z, cam->Fovy(), dComIfGp_getStartStageName(), dComIfGp_getStartStageRoomNo());
    if (FILE* f = std::fopen(kKeysPath, "a")) {
        std::fputs(line.c_str(), f);
        std::fclose(f);
    }
    Log.info("director: key {}", line);
}

const Take* running_take() {
    return g.phase == Phase::Running && g.current >= 0 ? &g.takes[g.current] : nullptr;
}

// What the running take does with the pad this frame: record it, replace it with the recording, or nothing.
void take_input(interface_of_controller_pad* pads, int count) {
    const Take* t = running_take();
    if (t == nullptr) return;
    if (t->controls == Controls::Player) {
        g.recording.push_back(pads[0]);
    } else if (t->controls == Controls::Recorded && !g.playback.empty()) {
        for (int i = 0; i < count; ++i) std::memset(&pads[i], 0, sizeof(interface_of_controller_pad));
        if (g.playIndex < g.playback.size()) pads[0] = g.playback[g.playIndex++];
    }
}

}  // namespace

bool available() {
    if (!dev_mode()) return false;
    static const bool present = [] { std::ifstream f(kTakesPath); return f.good(); }();
    return present;
}

bool active() { return g.phase != Phase::Idle; }

bool can_start_check(std::string& whyNot) {
    if (!dev_mode()) {
        whyNot = "Developer mode only (dev.txt).";
        return false;
    }
    if (active() || benchmark::active()) {
        whyNot = "A tour, benchmark or director session is running.";
        return false;
    }
    if (daAlink_getAlinkActorClass() == nullptr) {
        whyNot = "Load a save first.";
        return false;
    }
    if (daPy_py_c::checkNowWolf()) {
        whyNot = "Link is a wolf: change back to human form, then start the check.";
        return false;
    }
    return true;
}

bool start_check(std::string& error) {
    if (!can_start_check(error)) return false;
    std::ifstream custom(kCheckPath);
    const bool fromFile = custom.good();
    custom.close();
    auto takes = fromFile ? load_takes(kCheckPath) : check_takes();
    if (takes.empty()) {
        error = "check.txt has no spots.";
        return false;
    }
    arm(std::move(takes));
    g.autoRun = true;
    std::string build = benchmark::build_id();
    for (char& c : build) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') c = '_';
    }
    g.checkFile = fmt::format("sdmc:/switch/dusklight/cache/logs/check-{}-{}.txt", build, backend_name());
    if (FILE* f = std::fopen(g.checkFile.c_str(), "a")) {
        std::fprintf(f, "# check tour: build %s, backend %s, started %s, %zu spots (%s); screenshots in the Album at the "
                        "times below; the session log has a heartbeat after each spot\n",
            benchmark::build_id().c_str(), backend_name(), local_time_text().c_str(), g.takes.size(),
            fromFile ? "check.txt" : "built-in list");
        std::fclose(f);
    }
    Log.info("check: armed, {} spots, backend {}; starts when the menu closes", g.takes.size(), backend_name());
    g.autoWaitingForUi = true;
    return true;
}

std::string check_description() {
    std::string s = "Visits fixed spots one after the other and holds each for a few seconds with the controls "
                    "locked: Kakariko Gorge road at night and by day, Ordon (pumpkin patch), Hyrule Field near "
                    "Castle Town, Outside Castle Town (west), Ordon Village, Link's House, the Mirror Chamber. "
                    "At the end of each hold it takes a console screenshot (Album) and logs a full heartbeat; "
                    "cache/logs/check-<build>-<backend>.txt says which screenshot is which spot. "
                    "sdmc:/switch/dusklight/check.txt (director.txt's format) replaces the list.";
    return s;
}

bool can_start_reel(std::string& whyNot) {
    if (!available()) {
        whyNot = "Developer mode (dev.txt) and sdmc:/switch/dusklight/director.txt are needed.";
        return false;
    }
    if (active() || benchmark::active()) {
        whyNot = "A tour, benchmark or director session is running.";
        return false;
    }
    if (daAlink_getAlinkActorClass() == nullptr) {
        whyNot = "Load a save first.";
        return false;
    }
    if (daPy_py_c::checkNowWolf()) {
        whyNot = "Link is a wolf: change back to human form, then start the takes.";
        return false;
    }
    return true;
}

bool start_reel(std::string& error) {
    if (!can_start_reel(error)) return false;
    auto takes = load_takes();
    if (takes.empty()) {
        error = "director.txt has no takes.";
        return false;
    }
    arm(std::move(takes));
    g.reel = true;
    g.autoWaitingForUi = true;
    Log.info("director: reel armed, {} takes; starts when the menu closes", g.takes.size());
    return true;
}

std::string reel_description() {
    return "Plays every take of sdmc:/switch/dusklight/director.txt one after the other: the warp, the time of day, "
           "the weather, Link's form, the camera path or the orbit around Link, the look of the take (HUD, recording "
           "mode, music, preset, bloom, depth of field) and the input (locked, yours, or recorded in "
           "sdmc:/switch/dusklight/director/<take>.pad). Start the console's video capture before choosing it.";
}

bool locks_input() {
    if (g.autoRun && !g.autoWaitingForUi && g.phase != Phase::Idle && g.phase != Phase::Restoring) return true;
    if (g.phase == Phase::Loading || g.phase == Phase::Countdown) return true;
    return g.phase == Phase::Running && g.current >= 0 && g.takes[g.current].controls == Controls::Locked;
}

void filter_pad(interface_of_controller_pad* pads, int count) {
    if (!(available() || g.autoRun) || benchmark::active() || pads == nullptr || count <= 0) return;
    const u32 hold = pads[0].mButtonFlags, trig = pads[0].mPressedButtonFlags;
    if (g.autoRun) {  // V1-rc12: Check Tour -- only Start, which ends it and restores
        if ((trig & PAD_BUTTON_START) && g.phase != Phase::Restoring && g.phase != Phase::Idle) {
            Log.info("check: cancelled with Start");
            end_session();
        }
        if (locks_input()) {
            for (int i = 0; i < count; ++i) std::memset(&pads[i], 0, sizeof(interface_of_controller_pad));
        }
        return;
    }
    const bool lr = (hold & PAD_TRIGGER_L) && (hold & PAD_TRIGGER_R);
    const Take* rolling = running_take();
    if (g.reel) {  // the reel: Start stops it; L+R+Up still records a controles=jugador take
        if ((trig & PAD_BUTTON_START) && g.phase != Phase::Restoring && g.phase != Phase::Idle) {
            Log.info("director: reel stopped with Start");
            end_session();
            pads[0].mPressedButtonFlags &= ~PAD_BUTTON_START;
            pads[0].mButtonFlags &= ~PAD_BUTTON_START;
        } else if (lr && (trig & PAD_BUTTON_UP) && rolling != nullptr && rolling->controls == Controls::Player) {
            save_recording(*rolling);
        } else {
            take_input(pads, count);
        }
        if (locks_input()) {
            for (int i = 0; i < count; ++i) std::memset(&pads[i], 0, sizeof(interface_of_controller_pad));
        }
        return;
    }
    if (lr && (trig & PAD_BUTTON_RIGHT)) {
        if (g.phase == Phase::Idle) arm(load_takes());
        if (g.phase == Phase::Armed || g.phase == Phase::Running) {
            if (g.current + 1 < static_cast<int>(g.takes.size())) warp_to(g.takes[++g.current]);
        }
    } else if (lr && (trig & PAD_BUTTON_LEFT) && g.current >= 0 && g.phase != Phase::Restoring) {
        warp_to(g.takes[g.current]);
    } else if (lr && (trig & PAD_BUTTON_UP)) {
        if (rolling != nullptr && rolling->controls == Controls::Player) save_recording(*rolling);
        else log_key();
    } else if (lr && (trig & PAD_BUTTON_DOWN) && active() && g.phase != Phase::Restoring) {
        end_session();
    } else if ((trig & PAD_BUTTON_START) && g.phase == Phase::Running) {
        finish_take();
        set_phase(Phase::Armed);  // cancel the take, stay armed
    } else {
        take_input(pads, count);
    }
    if (locks_input()) {
        for (int i = 0; i < count; ++i) std::memset(&pads[i], 0, sizeof(interface_of_controller_pad));
    }
}

void tick() {
    if ((g.autoRun || g.reel) && g.autoWaitingForUi && g.phase == Phase::Armed) {
        if (!ui_window_visible()) {
            g.autoWaitingForUi = false;
            check_next();
        }
        return;
    }
    if (g.phase == Phase::Idle || g.phase == Phase::Armed) return;
    if (g.phase == Phase::Restoring) {
        if (!dComIfGp_isEnableNextStage() && !fopOvlpM_IsDoingReq()) {
            g.achievements.reset();  // after the save state is back, as the tour does
            g = Session{};
            Log.info("director: restored");
        }
        return;
    }
    const Take& t = g.takes[g.current];
    apply_environment(t);  // every frame: the game's own systems would drift it back
    if ((g.autoRun || g.reel) && g.phase == Phase::Loading && since(g.phaseStart) >= kCheckLoadTimeout) {
        Log.warn("{}: {} did not load in {:.0f} s; skipped", g.autoRun ? "check" : "director", t.id, kCheckLoadTimeout);
        if (g.autoRun) check_spot_done(t, true);
        check_next();
        return;
    }
    if (g.phase == Phase::Loading && settled(t)) {
        if (daAlink_c* link = daAlink_getAlinkActorClass()) {
            g.followYaw0 = static_cast<s16>(link->shape_angle.y + 0x8000);  // behind Link
        }
        apply_look(t);
        set_phase(Phase::Countdown);
    } else if (g.phase == Phase::Countdown) {
        apply_camera(t, 0.0f);
        if (since(g.phaseStart) >= (g.reel ? kReelCountdown : kCountdown)) {
            if (!g.spawned) spawn_enemies(t);
            check_form(t);
            if (t.controls == Controls::Recorded) load_playback(t);
            g.recording.clear();
            set_phase(Phase::Running);
            Log.info("director: take {} rolling ({:.1f} s)", t.id, t.duration);
        }
    } else if (g.phase == Phase::Running) {
        const double s = since(g.phaseStart);
        apply_camera(t, static_cast<float>(s));
        if (s >= t.duration) {
            finish_take();
            set_phase(Phase::Armed);
            Log.info("director: take {} done", t.id);
            if (g.autoRun) {  // V1-rc12: Check Tour -- evidence, then the next spot
                check_spot_done(t, false);
                check_next();
            } else if (g.reel) {
                check_next();
            }
        }
    }
}

std::string check_status_text() {
    if (g.reel) {  // the reel: a line only between takes, never over a frame being recorded
        const int n = static_cast<int>(g.takes.size());
        switch (g.phase) {
        case Phase::Loading:
            return fmt::format("Takes {}/{}: {} loading - Start stops", g.current + 1, n, g.takes[g.current].id);
        case Phase::Armed:
            return g.autoWaitingForUi ? std::string{"Takes: close the menu to start"} : std::string{};
        case Phase::Restoring: return "Takes: restoring";
        default: return {};
        }
    }
    return g.autoRun ? status_text() : std::string{};
}

std::string status_text() {
    if (g.phase == Phase::Idle) return {};
    const char* id = g.current >= 0 ? g.takes[g.current].id.c_str() : "-";
    if (g.autoRun) {
        const int n = static_cast<int>(g.takes.size());
        switch (g.phase) {
        case Phase::Loading: return fmt::format("Check {}/{}: {} loading - Start cancels", g.current + 1, n, id);
        case Phase::Countdown:
        case Phase::Running: return {};  // nothing on screen while the frame is the evidence
        case Phase::Restoring: return "Check: restoring";
        default: return g.autoWaitingForUi ? std::string{"Check: close the menu to start"} :
                                             fmt::format("Check {}/{}", g.current + 1, n);
        }
    }
    switch (g.phase) {
    case Phase::Armed: return fmt::format("Director: {} ready (L+R+Right next)", id);
    case Phase::Loading: return fmt::format("Director: {} loading", id);
    case Phase::Countdown: return fmt::format("Director: {} in {:.0f}", id, kCountdown - since(g.phaseStart));
    case Phase::Running: return fmt::format("Director: {} REC {:.1f}", id, since(g.phaseStart));
    default: return "Director: restoring";
    }
}

}  // namespace dusk::director
