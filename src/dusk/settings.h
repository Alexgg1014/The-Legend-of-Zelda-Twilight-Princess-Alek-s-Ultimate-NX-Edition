#pragma once

#include <array>

#include "dusk/config_var.hpp"
#include "dusk/resolution_ladder.hpp"
#include "dusk/ui/controls.hpp"

namespace dusk {

using config::ConfigVar;
using config::ActionBindConfigVar;

// aleks2 (companion): shared constants -----------------------------------
// video.fpsOverlayCorner: 0-3 are screen corners, 4 means the companion
// screen draws the counter instead. Shared by the settings UI, the RmlUi
// overlay and the companion dashboard.
constexpr int kFpsCornerCompanion = 4;

// game.dualScreenHudMode: which HUD split the second screen uses.
//   Cinematic  - the whole status HUD lives on the companion (original).
//   Functional - the gameplay-critical HUD MOVES BACK to the main screen and
//                the companion becomes a control surface (map/items/collect
//                centred, item buttons right, utility buttons left).
// Elements never render on both screens; each mode is a partition.
constexpr int kDualHudCinematic = 0;
constexpr int kDualHudFunctional = 1;

// game.dualScreenLayout: how the Switch's single panel is split.
// Normal keeps the port's existing full-screen presentation and summons the
// companion on demand; Flip Grip stacks game over companion on a portrait
// canvas, which needs the console physically turned, so it is handheld only.
constexpr int kDualLayoutNormal = 0;
constexpr int kDualLayoutFlip = 1;

// aleks2 (performance): "Optimized Settings" presets. Not stored: the row
// shows whichever preset the current knobs match, or Custom.
constexpr int kPerfPresetCustom = -1;
constexpr int kPerfPresetQuality = 0;
constexpr int kPerfPresetBalanced = 1;
constexpr int kPerfPresetPerformance = 2;

enum class BloomMode : int {
    Off = 0,
    Classic = 1,
    Dusk = 2,
};

enum class DepthOfFieldMode : int {
    Off = 0,
    Classic = 1,
    Dusk = 2,
};

enum class Resampler : int {
    Bilinear = 0,
    Area = 1,
};

enum class GameLanguage : u8 {
    English = OS_LANGUAGE_ENGLISH,
    German = OS_LANGUAGE_GERMAN,
    French = OS_LANGUAGE_FRENCH,
    Spanish = OS_LANGUAGE_SPANISH,
    Italian = OS_LANGUAGE_ITALIAN,
    Japanese = 6,
};

enum class DiscVerificationState : u8 {
    Unknown = 0,
    Success,
    HashMismatch,
};

enum class FrameInterpMode : u8 {
    Off = 0,
    Capped = 1,
    Unlimited = 2,
};

enum class InterpQuality : u8 {
    Fast = 0,
    Fancy = 1,
};

enum class LetterboxMode : u8 {
    Off = 0,
    On = 1,
    GameplayOnly = 2,
    CutsceneOnly = 3,
};

enum class TouchTargeting : u8 {
    Hybrid = 0,
    Hold = 1,
    Switch = 2,
};

enum class MenuScaling : u8 {
    GameCube = 0,
    Wii = 1,
    Dusklight = 2,
};

// aleks2 (H38-c3): which Joy-Con swings the sword with Wii-style motion controls; the other is the Nunchuk.
enum class WiiSwordHand : u8 {
    RIGHT = 0,
    LEFT = 1,
};

enum class AlwaysGreatspinMode : u8 {
    OFF = 0,
    AFTER_SKILL = 1,
    ALWAYS = 2,
};

enum class MagicArmorMode : u8 {
    NORMAL = 0,
    ON_DAMAGE = 1,
    DOUBLE_DEFENSE = 2,
    INVINCIBLE = 3,
    COSMETIC = 4,
};

enum class AudioOutputMode : u8 {
    StereoSpeakers = 0,
    StereoHeadphones = 1,   // spatial audio
    Surround6ch = 2,        // discrete 5.1
    Surround8ch = 3,        // discrete 7.1
};

namespace config {
template <>
struct ConfigEnumRange<BloomMode> {
    static constexpr auto min = BloomMode::Off;
    static constexpr auto max = BloomMode::Dusk;
};

template <>
struct ConfigEnumRange<DepthOfFieldMode> {
    static constexpr auto min = DepthOfFieldMode::Off;
    static constexpr auto max = DepthOfFieldMode::Dusk;
};

template <>
struct ConfigEnumRange<Resampler> {
    static constexpr auto min = Resampler::Bilinear;
    static constexpr auto max = Resampler::Area;
};

template <>
struct ConfigEnumRange<GameLanguage> {
    static constexpr auto min = GameLanguage::English;
    static constexpr auto max = GameLanguage::Japanese;
};

template <>
struct ConfigEnumRange<DiscVerificationState> {
    static constexpr auto min = DiscVerificationState::Unknown;
    static constexpr auto max = DiscVerificationState::HashMismatch;
};

template <>
struct ConfigEnumRange<FrameInterpMode> {
    static constexpr auto min = FrameInterpMode::Off;
    static constexpr auto max = FrameInterpMode::Unlimited;
};

template <>
struct ConfigEnumRange<InterpQuality> {
    static constexpr auto min = InterpQuality::Fast;
    static constexpr auto max = InterpQuality::Fancy;
};

template <>
struct ConfigEnumRange<LetterboxMode> {
    static constexpr auto min = LetterboxMode::Off;
    static constexpr auto max = LetterboxMode::CutsceneOnly;
};

template <>
struct ConfigEnumRange<TouchTargeting> {
    static constexpr auto min = TouchTargeting::Hybrid;
    static constexpr auto max = TouchTargeting::Switch;
};

template <>
struct ConfigEnumRange<MenuScaling> {
    static constexpr auto min = MenuScaling::GameCube;
    static constexpr auto max = MenuScaling::Dusklight;
};

template <>
struct ConfigEnumRange<WiiSwordHand> {
    static constexpr auto min = WiiSwordHand::RIGHT;
    static constexpr auto max = WiiSwordHand::LEFT;
};

template <>
struct ConfigEnumRange<AlwaysGreatspinMode> {
    static constexpr auto min = AlwaysGreatspinMode::OFF;
    static constexpr auto max = AlwaysGreatspinMode::ALWAYS;
};

template <>
struct ConfigEnumRange<MagicArmorMode> {
    static constexpr auto min = MagicArmorMode::NORMAL;
    static constexpr auto max = MagicArmorMode::COSMETIC;
};

template <>
struct ConfigEnumRange<AudioOutputMode> {
    static constexpr auto min = AudioOutputMode::StereoSpeakers;
    static constexpr auto max = AudioOutputMode::Surround8ch;
};

template <>
struct ConfigValueTraits<ui::ControlLayout> {
    static constexpr bool enabled = true;
};
}  // namespace config

// Persistent user settings

struct UserSettings {
    // Program settings

    struct {
        // Video
        ConfigVar<bool> enableFullscreen;
        ConfigVar<bool> enableVsync;
        ConfigVar<bool> lockAspectRatio;
        ConfigVar<bool> enableFpsOverlay;
        ConfigVar<int> fpsOverlayCorner;
        ConfigVar<int> maxFrameRate;
        ConfigVar<bool> rememberWindowSize;
        ConfigVar<int> lastWindowWidth;
        ConfigVar<int> lastWindowHeight;
        ConfigVar<int> uiScale;
    } video;

    struct {
        // Audio
        ConfigVar<AudioOutputMode> outputMode;
        ConfigVar<int> masterVolume;
        ConfigVar<int> mainMusicVolume;
        ConfigVar<int> subMusicVolume;
        ConfigVar<int> soundEffectsVolume;
        ConfigVar<int> fanfareVolume;
        ConfigVar<bool> enableReverb;
        ConfigVar<bool> menuSounds;
    } audio;

    // Game settings

    struct {
        ConfigVar<GameLanguage> language;

        // QoL
        ConfigVar<bool> enableQuickTransform;
        ConfigVar<bool> hideTvSettingsScreen;
        ConfigVar<bool> biggerWallets;
        ConfigVar<bool> noReturnRupees;
        ConfigVar<bool> disableRupeeCutscenes;
        ConfigVar<bool> fastTransitions;
        ConfigVar<bool> noSwordRecoil;
        ConfigVar<int> damageMultiplier;
        ConfigVar<bool> noHeartDrops;
        ConfigVar<bool> instantDeath;
        ConfigVar<bool> fastClimbing;
        ConfigVar<bool> noMissClimbing;
        ConfigVar<bool> fastTears;
        ConfigVar<bool> no2ndFishForCat;
        ConfigVar<bool> buttonFishing;
        ConfigVar<bool> instantSaves;

        ConfigVar<bool> instantText;
        ConfigVar<bool> holdToMash;
        ConfigVar<bool> sunsSong;
        ConfigVar<bool> autoSave;
        ConfigVar<bool> enhancedMapMenus;
        ConfigVar<bool> aimingReticle;

        // Preferences
        ConfigVar<bool> enableMirrorMode;
        ConfigVar<bool> minimalHUD;
        ConfigVar<bool> showMinimap;  // aleks2 (V1-rc10): the corner map alone, without the rest of the HUD
        ConfigVar<float> hudScale;
        ConfigVar<bool> pauseOnFocusLost;
        ConfigVar<bool> enableLinkDollRotation;
        ConfigVar<bool> enableAchievementToasts;
        ConfigVar<bool> enableControllerToasts;
        ConfigVar<bool> enableDiscordPresence;
        ConfigVar<MenuScaling> menuScalingMode;

        // Graphics
        ConfigVar<BloomMode> bloomMode;
        ConfigVar<float> bloomMultiplier;
        ConfigVar<DepthOfFieldMode> depthOfFieldMode;
        ConfigVar<bool> disableWaterRefraction;
        ConfigVar<bool> enableTextureReplacements;
        ConfigVar<FrameInterpMode> enableFrameInterpolation;
        ConfigVar<int> internalResolutionScale;
        ConfigVar<int> shadowResolutionMultiplier;
        ConfigVar<Resampler> resampler;
        ConfigVar<bool> enableMapBackground;
        ConfigVar<bool> disableCutscenePillarboxing;
        ConfigVar<LetterboxMode> disableLetterboxing;
        ConfigVar<bool> enableHighQualityMinimapTextures;

        // Audio
        ConfigVar<bool> noLowHpSound;
        ConfigVar<bool> midnasLamentNonStop;

        // Input
        ConfigVar<bool> enableGyroAim;
        ConfigVar<bool> enableGyroRollgoal;
        ConfigVar<bool> enableWiiMotion;
        ConfigVar<float> wiiMotionSensitivity;
        ConfigVar<WiiSwordHand> wiiMotionSwordHand;   // Motion Sword Hand
        ConfigVar<bool> wiiMotionSwingCuts;           // Swing Direction Animations
        ConfigVar<bool> wiiMotionSwapSides;           // Swap Swing Sides
        // Read from config.json when present (no UI); out-of-range values fall back to the defaults, logged once
        // (dusk/wii_gesture_detector.hpp: RawTuning / sanitize). Axes are 0..2 (x, y, z), -1 = the built-in table.
        ConfigVar<float> wiiAccelThresholdG;          // 0.6 .. 3.0, default 1.3
        ConfigVar<float> wiiGyroThresholdRad;         // 1.0 .. 12.0, default 3.0
        ConfigVar<float> wiiDirectionDominance;       // 1.1 .. 3.0, default 1.5
        ConfigVar<float> wiiMinYawRad;                // 0.05 .. 1.5, default 0.25
        // V1-rc7 (wii_gesture_detector.hpp): gyro's weight in opening a window, cooldown after one, the wind-up
        // filter, the most a thrust may turn, and the sensor axis a thrust runs along (-1 any).
        ConfigVar<float> wiiGyroWeight;               // 0 .. 1, default 0.5
        ConfigVar<float> wiiCooldownS;                // 0.05 .. 0.5, default 0.12
        ConfigVar<bool> wiiWindupFilter;              // default on
        ConfigVar<float> wiiWindupMaxG;               // V1-rc9: 1 .. 8, default 4.0 (a wind-up is weaker)
        ConfigVar<float> wiiWindupMaxGyroRad;         // V1-rc9: 2 .. 40, default 12
        ConfigVar<float> wiiThrustMaxGyroRad;         // 0.5 .. 12, default 4.0 (V1-rc8)
        ConfigVar<int> wiiThrustAxis;                 // -1 .. 2, default -1
        ConfigVar<int> wiiAxisLateralLeft, wiiAxisForwardLeft, wiiForwardSignLeft;     // the left Joy-Con as Nunchuk
        ConfigVar<int> wiiAxisLateralRight, wiiAxisForwardRight, wiiForwardSignRight;  // the right Joy-Con as Nunchuk
        ConfigVar<float> gyroSensitivityX;
        ConfigVar<float> gyroSensitivityY;
        ConfigVar<float> gyroSensitivityRollgoal;
        ConfigVar<float> gyroSmoothing;
        ConfigVar<float> gyroDeadband;
        ConfigVar<bool> gyroInvertPitch;
        ConfigVar<bool> gyroInvertYaw;
        ConfigVar<bool> enableMouseCamera;
        ConfigVar<bool> enableMouseAim;
        ConfigVar<float> mouseAimSensitivity;
        ConfigVar<float> mouseCameraSensitivity;
        ConfigVar<bool> invertMouseY;
        ConfigVar<bool> freeCamera;
        ConfigVar<bool> enableTouchControls;
        ConfigVar<TouchTargeting> touchTargeting;
        ConfigVar<bool> enableMenuPointer;
        ConfigVar<ui::ControlLayout> touchControlsLayout;
        ConfigVar<bool> invertCameraXAxis;
        ConfigVar<bool> invertCameraYAxis;
        ConfigVar<bool> invertFirstPersonXAxis;
        ConfigVar<bool> invertFirstPersonYAxis;
        ConfigVar<bool> invertAirSwimX;
        ConfigVar<bool> invertAirSwimY;
        ConfigVar<float> freeCameraXSensitivity;
        ConfigVar<float> freeCameraYSensitivity;
        ConfigVar<float> touchCameraXSensitivity;
        ConfigVar<float> touchCameraYSensitivity;
        ConfigVar<bool> debugFlyCam;
        ConfigVar<bool> debugFlyCamLockEvents;
        ConfigVar<bool> allowBackgroundInput;
        std::array<ConfigVar<bool>, 4> enableLED;
        ConfigVar<bool> swapDirectSelect;

        // Cheats
        ConfigVar<bool> infiniteHearts;
        ConfigVar<bool> infiniteArrows;
        ConfigVar<bool> infiniteSeeds;
        ConfigVar<bool> infiniteBombs;
        ConfigVar<bool> infiniteOil;
        ConfigVar<bool> infiniteOxygen;
        ConfigVar<bool> infiniteRupees;
        ConfigVar<bool> enableIndefiniteItemDrops;
        ConfigVar<bool> moonJump;
        ConfigVar<bool> superClawshot;
        ConfigVar<AlwaysGreatspinMode> alwaysGreatspin;
        ConfigVar<bool> enableFastIronBoots;
        ConfigVar<bool> canTransformAnywhere;
        ConfigVar<bool> fastRoll;
        ConfigVar<bool> fastSpinner;
        ConfigVar<MagicArmorMode> armorRupeeDrain;
        ConfigVar<bool> invincibleEnemies;
        ConfigVar<bool> easyQuickSpin;

        // Technical
        ConfigVar<bool> restoreWiiGlitches;

        // Controls
        ConfigVar<bool> enableTurboKeybind;
        ConfigVar<bool> enableResetKeybind;

        // Tools
        ConfigVar<bool> speedrunMode;
        ConfigVar<bool> liveSplitEnabled;
        ConfigVar<bool> showSpeedrunRTATimer;
        ConfigVar<bool> recordingMode;
        ConfigVar<bool> removeQuestMapMarkers;
        ConfigVar<bool> showInputViewer;
        ConfigVar<bool> showInputViewerGyro;
        ConfigVar<bool> enableMoveLinkCombo;
        ConfigVar<bool> enableTeleportCombo;

        ConfigVar<std::string> lastSelectedGameModeId;

        // --- Fork-added settings ---------------------------------------
        // Everything this fork adds to UserSettings lives at the END of the
        // struct, never in the middle. Third-party mods are compiled against
        // upstream's layout and read members by hardcoded byte offset, so a
        // ConfigVar inserted above shifts every setting after it and makes
        // those mods read the wrong variable -- a crash, not a glitch.
        // (Diagnosed and documented in igawa6/dusklight v2.0.0-ds-3.0.2.)
        // Registration order and the JSON keys are unaffected: ConfigVars are
        // keyed by name, so existing config.json files keep working.
        // aleks2 (companion) ------------------------------------------------
        ConfigVar<bool> dualScreen;
        ConfigVar<int> dualScreenHudMode;
        // Switch only: how the single panel is split. 0 = Normal (game full
        // screen, companion summoned), 1 = Flip Grip (portrait, stacked).
        // There is deliberately no horizontal dual mode.
        ConfigVar<int> dualScreenLayout;
        // V1-rc1: turning the second screen on clears Minimal HUD; this remembers that it was on,
        // so turning the second screen off gives it back (it used to stay off: the full HUD kept
        // drawing on the main screen, ~430 -> 1300 draws a frame).
        ConfigVar<bool> dualScreenRestoreMinimalHud;
        // Switch only: redraw the companion every Nth frame (1 = every frame).
        // The dashboard is a status surface, not animation: at 30 fps a
        // divisor of 3 still updates it 10x a second while cutting the GX
        // draw work to a third. Interaction bypasses it via
        // dualscreen::requestRedraw().
        ConfigVar<int> dualScreenRedrawDivisor;
        // aleks2 (V1-rc6): how often the field minimap is rendered, in game frames (1: every frame, as the game
        // does). It re-renders its whole texture each time: ~800 GX draws a frame in Hyrule Field and Castle Town
        // with the full HUD. 1 by default until a console run shows the map keeps its picture between renders.
        ConfigVar<int> minimapRedrawDivisor;
        // aleks2 (V1-rc7): the most a stage's loading screen waits, in ms, for that stage's shaders (0: never waits).
        ConfigVar<int> shaderSceneWaitMs;
        // Switch only: the companion's render resolution, as pixels on its
        // longest side. 720 is the panel's own size (no downscale); 640 is
        // the default. The aspect is always preserved.
        ConfigVar<int> dualScreenResolution;
        // Companion on the main screen, game on the second (desktop/Android).
        ConfigVar<bool> dualScreenSwap;
        ConfigVar<int> dualScreenDisplay;
        ConfigVar<int> dualScreenPosX;
        ConfigVar<int> dualScreenPosY;
        ConfigVar<bool> dualScreenHaptics;
        // Highest one-shot migration already applied to this config file.
        ConfigVar<int> configMigration;
        // Guide reader (companion page).
        ConfigVar<bool> guideEnabled;
        // aleks2 (performance). Stepped dynamic resolution: drops the internal resolution one rung
        // at a time while the measured frame rate misses the target, and
        // climbs back when there is headroom. Never goes above
        // internalResolutionScale, never below 1x.
        ConfigVar<bool> dynamicResolution;
        ConfigVar<int> dynamicResolutionTarget;
        // Skip an interpolated frame that would overrun the next sim tick
        // (see game_clock.cpp). 60 where it fits, a clean 30 where not.
        ConfigVar<bool> predictiveInterpolation;
        // Fast skips the particle/rain/snow/cloud/material/vertex capture.
        ConfigVar<InterpQuality> interpolationQuality;
        ConfigVar<int> maxTextureAnisotropy;
        // aleks2 (H31 diagnostic, not a graphics setting): replay stored pipeline
        // lookups in the GX draw path. Off = the H30 path. See aurora/tuning.h.
        ConfigVar<bool> pipelineMemo;
        // aleks2 (H33): what "Auto" internal resolution means: 0 Legacy (1080 docked / 720
        // handheld / 540 Flip Grip), 1 Quality, 2 Balanced, 3 Performance. Remembers the last
        // profile the user chose, so a hand-edited knob (the row then reads Custom) does not
        // move the docked resolution. See resolution_policy.hpp. Appended last: mod ABI.
        ConfigVar<int> autoResolutionPolicy;
        // aleks2 (V1-rc16): HD Rumble on the Switch's own controllers (src/dusk/hd_rumble.cpp); off falls back to the
        // classic on/off rumble. Appended last: mod ABI.
        ConfigVar<bool> hdRumble;
        // aleks2 (V1-rc16): 0-100 %, every vibration (HD and classic) on the Switch's own controllers.
        ConfigVar<int> rumbleIntensity;
        // aleks2 (V1-rc16): handheld only, opt-in, any preset: the system's official 460 MHz GPU configuration
        // (src/dusk/gpu_boost.cpp). Presets never touch the clocks. Appended last: mod ABI.
        ConfigVar<bool> gpuBoost;
    } game;

    struct {
        ConfigVar<std::string> isoPath;
        ConfigVar<DiscVerificationState> isoVerification;
        ConfigVar<std::string> graphicsBackend;
        ConfigVar<bool> skipPreLaunchUI;
        ConfigVar<bool> showPipelineCompilation;
        ConfigVar<bool> wasPresetChosen;
        ConfigVar<bool> checkForUpdates;
        ConfigVar<bool> checkForModUpdates;
        ConfigVar<int> cardFileType;
        ConfigVar<bool> enableAdvancedSettings;
        ConfigVar<bool> showProfilerOverlay;
    } backend;

    // Arrays of size 4 for 4 ports
    struct {
        std::array<ActionBindConfigVar, 4> firstPersonCamera;
        std::array<ActionBindConfigVar, 4> callMidna;
        std::array<ActionBindConfigVar, 4> openMapScreen;
        std::array<ActionBindConfigVar, 4> toggleMinimap;
        std::array<ActionBindConfigVar, 4> openDusklightMenu;
        std::array<ActionBindConfigVar, 4> turboSpeedButton;
        // aleks2 (companion): slot buttons I/II (item buttons 2/3), physical
        // key triggers for the companion's slot items.
        std::array<ActionBindConfigVar, 4> useSlotItem1;
        std::array<ActionBindConfigVar, 4> useSlotItem2;
    } actionBindings;
};

// aleks2 (companion): one-shot config migrations for EXISTING installs, run
// right after the config file loads. Each migration runs exactly once per
// config file, tracked by game.configMigration.
void runConfigMigrations();


UserSettings& getSettings();

void registerSettings();

// Applies an internal resolution setting value through the one place that decides what is
// safe (resolution_policy.hpp): manual wins over Auto, Auto follows autoResolutionPolicy, and
// the result never exceeds the console's limit for its current mode.
void applyInternalResolutionScale(int scale);

// H33: a repair made at startup that the user should be told about (empty when none).
// Returns it once and clears it.
std::string takeSettingsRepairNotice();


void applyResampler(Resampler resampler);

inline bool isLetterboxingDisabled(bool inCutscene) {
    const auto mode = getSettings().game.disableLetterboxing.getValue();
    return mode == LetterboxMode::On ||
           (mode == LetterboxMode::CutsceneOnly && inCutscene) ||
           (mode == LetterboxMode::GameplayOnly && !inCutscene);
}

// Transient settings

struct CollisionViewSettings {
    bool enableTerrainView;
    bool enableWireframe;
    bool enableAtView;
    bool enableTgView;
    bool enableCoView;
    float terrainViewOpacity;
    float colliderViewOpacity;
    float drawRange;
};

struct TriggerViewSettings {
    bool loadZones;
    bool eventAreas;
    bool switchAreas;
    bool eventTags;
    bool midnaStops;
    bool twilightGates;
    bool checkpoints;
    bool paths;
    bool transformDists;
    bool attentionDists;
    bool purpleMistAvoid;
    bool leevers;
    float opacity;
};

struct TransientSettings {
    CollisionViewSettings collisionView;
    TriggerViewSettings triggerView;
    bool turboMode;
    bool moveLinkActive;
    bool stateShareLoadActive;
};

TransientSettings& getTransientSettings();

}  // namespace dusk
