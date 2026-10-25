#include "dusk/settings.h"
#include "dusk/dualscreen.h"

#include "dusk/config.hpp"
#include "dusk/game_mode.hpp"
#include "dusk/logging.h"
#include "dusk/perf_profile_apply.hpp"
#include "dusk/resolution_policy.hpp"
#include "dusk/switch_display.hpp"
#include "dusk/texture_replacements.hpp"
#include "dusk/ui/ui.hpp"

#include <aurora/aurora.h>
#include <aurora/tuning.h>
#include <dolphin/vi.h>

#include <utility>

namespace dusk {

UserSettings g_userSettings = {
    .video = {
        .enableFullscreen {"video.enableFullscreen", false},
        .enableVsync {"video.enableVsync", true},
        .lockAspectRatio {"video.lockAspectRatio", false},
        .enableFpsOverlay {"game.enableFpsOverlay", false},
        .fpsOverlayCorner {"game.fpsOverlayCorner", 0},
        .maxFrameRate {"video.maxFrameRate", 240},
        .rememberWindowSize {"video.rememberWindowSize", false},
        .lastWindowWidth {"video.lastWindowWidth", 0},
        .lastWindowHeight {"video.lastWindowHeight", 0},
        .uiScale {"video.uiScale", 100},
    },

    .audio = {
        .outputMode {"audio.outputMode", AudioOutputMode::StereoSpeakers},
        .masterVolume {"audio.masterVolume", 60},
        .mainMusicVolume {"audio.mainMusicVolume", 100},
        .subMusicVolume {"audio.subMusicVolume", 100},
        .soundEffectsVolume {"audio.soundEffectsVolume", 100},
        .fanfareVolume {"audio.fanfareVolume", 100},
        .enableReverb {"audio.enableReverb", true},
        .menuSounds {"audio.menuSounds", true},
    },

    .game = {
        .language { "game.language", GameLanguage::English },

        // Quality of Life
        .enableQuickTransform {"game.enableQuickTransform", false},
        .hideTvSettingsScreen {"game.hideTvSettingsScreen", true},
        .biggerWallets {"game.biggerWallets", false},
        .noReturnRupees {"game.noReturnRupees", false},
        .disableRupeeCutscenes {"game.disableRupeeCutscenes", false},
        .fastTransitions {"game.fastTransitions", false},
        .noSwordRecoil {"game.noSwordRecoil", false},
        .damageMultiplier {"game.damageMultiplier", 1},
        .noHeartDrops {"game.noHeartDrops", false},
        .instantDeath {"game.instantDeath", false},
        .fastClimbing {"game.fastClimbing", false},
        .noMissClimbing {"game.noMissClimbing", false},
        .fastTears {"game.fastTears", false},
        .no2ndFishForCat {"game.no2ndFishForCat", false},
        .buttonFishing {"game.buttonFishing", false},
        .instantSaves {"game.instantSaves", false},
        .instantText {"game.instantText", false},
        .holdToMash {"game.holdToMash", false},
        .sunsSong {"game.sunsSong", false},
        .autoSave {"game.autoSave", false},
        .enhancedMapMenus {"game.enhancedMapMenus", false},
        .aimingReticle {"game.aimingReticle", false},

        // Preferences
        .enableMirrorMode {"game.enableMirrorMode", false},
        .minimalHUD {"game.minimalHUD", false},
        .showMinimap {"game.showMinimap", true},
        .hudScale {"game.hudScale", 1.0f},
        .pauseOnFocusLost {"game.pauseOnFocusLost", false},
        .enableLinkDollRotation {"game.enableLinkDollRotation", false},
        .enableAchievementToasts {"game.enableAchievementToasts", true},
        .enableControllerToasts {"game.enableControllerToasts", true},
        .enableDiscordPresence {"game.enableDiscordPresence", true},
        .menuScalingMode {"game.menuScalingMode", MenuScaling::Wii},

        // Graphics
        .bloomMode {"game.bloomMode", BloomMode::Dusk},
        .bloomMultiplier {"game.bloomMultiplier", 1.0f},
        .depthOfFieldMode{"game.depthOfFieldMode", DepthOfFieldMode::Dusk},
        .disableWaterRefraction {"game.disableWaterRefraction", false},
        .enableTextureReplacements {"game.enableTextureReplacements", true},
        .enableFrameInterpolation {"game.enableFrameInterpolation", FrameInterpMode::Off},
        .internalResolutionScale {"game.internalResolutionScale", 0},
        .shadowResolutionMultiplier {"game.shadowResolutionMultiplier", 1},
        .resampler {"game.resampler", Resampler::Bilinear},
        .enableMapBackground {"game.enableMapBackground", true},
        .disableCutscenePillarboxing {"game.disableCutscenePillarboxing", false},
        .disableLetterboxing {"game.disableLetterboxing", LetterboxMode::Off},
        .enableHighQualityMinimapTextures {"game.enableHighQualityMinimapTextures", true},

        // Audio
        .noLowHpSound {"game.noLowHpSound", false},
        .midnasLamentNonStop {"game.midnasLamentNonStop", false},

        // Input
        .enableGyroAim {"game.enableGyroAim", false},
        .enableGyroRollgoal {"game.enableGyroRollgoal", false},
        .enableWiiMotion {"game.enableWiiMotion", false},
        .wiiMotionSensitivity {"game.wiiMotionSensitivity", 1.0f},
        .wiiMotionSwordHand {"game.wiiMotionSwordHand", WiiSwordHand::RIGHT},
        .wiiMotionSwingCuts {"game.wiiMotionSwingCuts", true},
        .wiiMotionSwapSides {"game.wiiMotionSwapSides", false},
        .wiiAccelThresholdG {"game.wiiAccelThresholdG", 1.3f},
        .wiiGyroThresholdRad {"game.wiiGyroThresholdRad", 3.0f},
        .wiiDirectionDominance {"game.wiiDirectionDominance", 1.5f},
        .wiiMinYawRad {"game.wiiMinYawRad", 0.25f},
        .wiiGyroWeight {"game.wiiGyroWeight", 0.5f},
        .wiiCooldownS {"game.wiiCooldownS", 0.12f},
        .wiiWindupFilter {"game.wiiWindupFilter", true},
        .wiiWindupMaxG {"game.wiiWindupMaxG", 4.0f},
        .wiiWindupMaxGyroRad {"game.wiiWindupMaxGyroRad", 12.0f},
        .wiiThrustMaxGyroRad {"game.wiiThrustMaxGyroRad", 4.0f},
        .wiiThrustAxis {"game.wiiThrustAxis", -1},
        .wiiAxisLateralLeft {"game.wiiAxisLateralLeft", -1},
        .wiiAxisForwardLeft {"game.wiiAxisForwardLeft", -1},
        .wiiForwardSignLeft {"game.wiiForwardSignLeft", 0},
        .wiiAxisLateralRight {"game.wiiAxisLateralRight", -1},
        .wiiAxisForwardRight {"game.wiiAxisForwardRight", -1},
        .wiiForwardSignRight {"game.wiiForwardSignRight", 0},
        .gyroSensitivityX {"game.gyroSensitivityX", 1.0f},
        .gyroSensitivityY {"game.gyroSensitivityY", 1.0f},
        .gyroSensitivityRollgoal {"game.gyroSensitivityRollgoal", 1.0f},
        .gyroSmoothing {"game.gyroSmoothing", 0.65f},
        .gyroDeadband {"game.gyroDeadband", 0.04f},
        .gyroInvertPitch {"game.gyroInvertPitch", false},
        .gyroInvertYaw {"game.gyroInvertYaw", false},
        .enableMouseCamera {"game.enableMouseCamera", false},
        .enableMouseAim {"game.enableMouseAim", false},
        .mouseAimSensitivity {"game.mouseAimSensitivity", 1.0f},
        .mouseCameraSensitivity {"game.mouseCameraSensitivity", 1.0f},
        .invertMouseY {"game.invertMouseY", false},
        .freeCamera {"game.freeCamera", false},
        .enableTouchControls {"game.enableTouchControls", false},
        .touchTargeting {"game.touchTargeting", TouchTargeting::Hybrid},
        .enableMenuPointer {"game.enableMenuPointer", true},
        .touchControlsLayout {"game.touchControlsLayout", ui::ControlLayout{}},
        .invertCameraXAxis {"game.invertCameraXAxis", false},
        .invertCameraYAxis {"game.invertCameraYAxis", false},
        .invertFirstPersonXAxis {"game.invertFirstPersonXAxis", false},
        .invertFirstPersonYAxis {"game.invertFirstPersonYAxis", false},
        .invertAirSwimX {"game.invertAirSwimX", false},
        .invertAirSwimY {"game.invertAirSwimY", false},
        .freeCameraXSensitivity {"game.freeCameraXSensitivity", 1.0f},
        .freeCameraYSensitivity {"game.freeCameraYSensitivity", 1.0f},
        .touchCameraXSensitivity {"game.touchCameraXSensitivity", 1.0f},
        .touchCameraYSensitivity {"game.touchCameraYSensitivity", 1.0f},
        .debugFlyCam {"game.debugFlyCam", false},
        .debugFlyCamLockEvents {"game.debugFlyCamLockEvents", true},
        .allowBackgroundInput {"game.allowBackgroundInput", true},
        .enableLED {
            ConfigVar<bool>{"game.enableLED_port0", true},
            ConfigVar<bool>{"game.enableLED_port1", true},
            ConfigVar<bool>{"game.enableLED_port2", true},
            ConfigVar<bool>{"game.enableLED_port3", true},
        },
        .swapDirectSelect {"game.swapDirectSelect", false},

        // Cheats
        .infiniteHearts {"game.infiniteHearts", false},
        .infiniteArrows {"game.infiniteArrows", false},
        .infiniteSeeds {"game.infiniteSeeds", false},
        .infiniteBombs {"game.infiniteBombs", false},
        .infiniteOil {"game.infiniteOil", false},
        .infiniteOxygen {"game.infiniteOxygen", false},
        .infiniteRupees {"game.infiniteRupees", false},
        .enableIndefiniteItemDrops {"game.enableIndefiniteItemDrops", false},
        .moonJump {"game.moonJump", false},
        .superClawshot {"game.superClawshot", false},
        .alwaysGreatspin {"game.alwaysGreatspin", AlwaysGreatspinMode::OFF},
        .enableFastIronBoots {"game.enableFastIronBoots", false},
        .canTransformAnywhere {"game.canTransformAnywhere", false},
        .fastRoll {"game.fastRoll", false},
        .fastSpinner {"game.fastSpinner", false},
        .armorRupeeDrain {"game.armorRupeeDrain", MagicArmorMode::NORMAL},
        .invincibleEnemies {"game.invincibleEnemies", false},
        .easyQuickSpin {"game.easyQuickSpin", false},

        // Technical
        .restoreWiiGlitches {"game.restoreWiiGlitches", false},

        // Controls
        .enableTurboKeybind {"game.enableTurboKeybind", false},
        .enableResetKeybind {"game.enableResetKeybind", false},

        // Tools
        .speedrunMode {"game.speedrunMode", false},
        .liveSplitEnabled {"game.liveSplitEnabled", false},
        .showSpeedrunRTATimer {"game.showSpeedrunRTATimer", true},
        .recordingMode {"game.recordingMode", false},
        .removeQuestMapMarkers {"game.removeQuestMapMarkers", false},
        .showInputViewer {"game.showInputViewer", false},
        .showInputViewerGyro {"game.showInputViewerGyro", false},
        .enableMoveLinkCombo {"game.enableMoveLinkCombo", false},
        .enableTeleportCombo {"game.enableTeleportCombo", false},
        .lastSelectedGameModeId {"game.lastSelectedGameModeId", gamemode::kVanillaGameModeId},

        // Fork-added settings, in the same order as settings.h: C++20 wants
        // designated initializers in declaration order, and the declarations
        // all sit at the end of the struct so mod ABI stays upstream's.
        // aleks2 (1.0.1): companion OFF by default, the normal single-screen HUD; the player opts in
        // (Screen Layout). With it on out of the box, the 3DS-style HUD moved rupees and X/Y/Z to a
        // second screen nobody had opened (BloodFalled, GBAtemp). Only a value the player set is saved
        // (config.cpp save(): the Value layer), so untouched installs follow this default and a choice
        // made in the menu stays.
        .dualScreen {"game.dualScreen", false},
        .dualScreenHudMode {"game.dualScreenHudMode", kDualHudFunctional},
        .dualScreenLayout {"game.dualScreenLayout", kDualLayoutNormal},
        .dualScreenRestoreMinimalHud {"game.dualScreenRestoreMinimalHud", false},
        .dualScreenRedrawDivisor {"game.dualScreenRedrawDivisor", 3},
        .minimapRedrawDivisor {"game.minimapRedrawDivisor", 1},
        .shaderSceneWaitMs {"game.shaderSceneWaitMs", 2500},  // V1-rc9: was 400
        .dualScreenResolution {"game.dualScreenResolution", 640},
        .dualScreenSwap {"game.dualScreenSwap", false},
        .dualScreenDisplay {"game.dualScreenDisplay", -1},
        .dualScreenPosX {"game.dualScreenPosX", -1},
        .dualScreenPosY {"game.dualScreenPosY", -1},
        .dualScreenHaptics {"game.dualScreenHaptics", true},
        .configMigration {"game.configMigration", 0},
        .guideEnabled {"game.guideEnabled", true},
        .dynamicResolution {"game.dynamicResolution", false},
        .dynamicResolutionTarget {"game.dynamicResolutionTarget", 60},
        .predictiveInterpolation {"game.predictiveInterpolation", true},
        .interpolationQuality {"game.interpolationQuality", InterpQuality::Fast},
#ifdef __SWITCH__
        .maxTextureAnisotropy {"game.maxTextureAnisotropy", 4},
#else
        .maxTextureAnisotropy {"game.maxTextureAnisotropy", 16},
#endif
        .pipelineMemo {"game.pipelineMemo", true},
        .autoResolutionPolicy {"game.autoResolutionPolicy", respol::kPolicyLegacy},
        .hdRumble {"game.hdRumble", true},
        .rumbleIntensity {"game.rumbleIntensity", 70},
        .gpuBoost {"game.gpuBoost", false},
    },

    .backend = {
        .isoPath {"backend.isoPath", ""},
        .isoVerification {"backend.isoVerification", DiscVerificationState::Unknown},
        .graphicsBackend {"backend.graphicsBackend", "auto"},
        .skipPreLaunchUI {"backend.skipPreLaunchUI", false},
        .showPipelineCompilation {"backend.showPipelineCompilation", true},
        .wasPresetChosen {"backend.wasPresetChosen", false},
        .checkForUpdates {"backend.checkForUpdates", true},
        .checkForModUpdates {"backend.checkForModUpdates", true},
        .cardFileType {"backend.cardFileType", static_cast<int>(CARD_GCIFOLDER)},
        .enableAdvancedSettings {"backend.enableAdvancedSettings", false},
        .showProfilerOverlay {"backend.showProfilerOverlay", false},
    },

    // Not sure if there's a better way to declare this
    .actionBindings = {
        .firstPersonCamera {
            ActionBindConfigVar{"actionBindings.firstPersonCamera_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.firstPersonCamera_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.firstPersonCamera_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.firstPersonCamera_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .callMidna {
            ActionBindConfigVar{"actionBindings.callMidna_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.callMidna_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.callMidna_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.callMidna_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .openMapScreen {
            ActionBindConfigVar{"actionBindings.openMapScreen_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openMapScreen_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openMapScreen_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openMapScreen_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .toggleMinimap {
            ActionBindConfigVar{"actionBindings.toggleMinimap_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.toggleMinimap_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.toggleMinimap_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.toggleMinimap_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .openDusklightMenu {
            ActionBindConfigVar{"actionBindings.openDusklightMenu_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openDusklightMenu_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openDusklightMenu_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openDusklightMenu_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .turboSpeedButton {
            ActionBindConfigVar{"actionBindings.turboButton_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.turboButton_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.turboButton_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.turboButton_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .useSlotItem1 {
            ActionBindConfigVar{"actionBindings.useSlotItem1_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.useSlotItem1_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.useSlotItem1_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.useSlotItem1_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .useSlotItem2 {
            ActionBindConfigVar{"actionBindings.useSlotItem2_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.useSlotItem2_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.useSlotItem2_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.useSlotItem2_port3", PAD_NATIVE_BUTTON_INVALID},
        },
    }
};

UserSettings& getSettings() {
    return g_userSettings;
}

namespace {
std::string s_repairNotice;
}  // namespace

std::string takeSettingsRepairNotice() {
    return std::exchange(s_repairNotice, {});
}

// H33: the one place that decides which internal resolution reaches the renderer. Every route
// ends here -- startup, the setting's callback, a profile, dock/undock, dynamic resolution --
// so nothing a setting says, old or corrupt, can allocate a framebuffer the console cannot
// show. See resolution_policy.hpp. Aurora clamps once more as a second barrier.
void applyInternalResolutionScale(int scale) {
#ifdef __SWITCH__
    const auto mode = display::mode();
    const auto resolved =
        respol::resolve(scale, getSettings().game.autoResolutionPolicy.getValue(), mode);
    // One line per change of what is actually applied, not per call.
    static int s_lastLoggedRows = -1;
    if (resolved.rows != s_lastLoggedRows) {
        s_lastLoggedRows = resolved.rows;
        if (resolved.clamped) {
            DuskLog.info("Internal resolution: setting {} limited to {} rows in {} mode", scale,
                         resolved.rows, mode == respol::Mode::Docked ? "docked" : "handheld");
        } else if (resolved.repaired) {
            DuskLog.warn("Internal resolution: setting {} is not usable on this console, using Auto ({} rows)",
                         scale, resolved.rows);
        }
    }
    if (const int rows = ladder::exact_height(resolved.value); rows > 0) {
        VISetFrameBufferHeight(static_cast<uint32_t>(rows));
    } else {
        VISetFrameBufferScale(static_cast<float>(resolved.value));
    }
#else
    if (scale <= 0) {
        VISetFrameBufferScale(0.0f);
        return;
    }
    if (const int rows = ladder::exact_height(scale); rows > 0) {
        VISetFrameBufferHeight(static_cast<uint32_t>(rows));
        return;
    }
    VISetFrameBufferScale(static_cast<float>(scale));
#endif
}

void applyResampler(Resampler resampler) {
    switch (resampler) {
    case Resampler::Area:
        aurora_set_resampler(SAMPLER_AREA);
        break;
    case Resampler::Bilinear:
    default:
        aurora_set_resampler(SAMPLER_BILINEAR);
        break;
    }
}

void registerSettings() {
    // Video
    Register(g_userSettings.video.enableFullscreen);
    Register(g_userSettings.video.enableVsync);
    Register(g_userSettings.video.lockAspectRatio);
    Register(g_userSettings.video.enableFpsOverlay);
    Register(g_userSettings.video.fpsOverlayCorner);
    Register(g_userSettings.video.maxFrameRate);
    Register(g_userSettings.video.rememberWindowSize);
    Register(g_userSettings.video.lastWindowWidth);
    Register(g_userSettings.video.lastWindowHeight);
    Register(g_userSettings.video.uiScale, 
        [](const int&, const int&) { dusk::ui::apply_scale(); });

    // Audio
    Register(g_userSettings.audio.outputMode);
    Register(g_userSettings.audio.masterVolume);
    Register(g_userSettings.audio.mainMusicVolume);
    Register(g_userSettings.audio.subMusicVolume);
    Register(g_userSettings.audio.soundEffectsVolume);
    Register(g_userSettings.audio.fanfareVolume);
    Register(g_userSettings.audio.enableReverb);
    Register(g_userSettings.audio.menuSounds);

    // Game
    Register(g_userSettings.game.language);
    Register(g_userSettings.game.enableQuickTransform);
    Register(g_userSettings.game.hideTvSettingsScreen);
    Register(g_userSettings.game.biggerWallets);
    Register(g_userSettings.game.noReturnRupees);
    Register(g_userSettings.game.disableRupeeCutscenes);
    Register(g_userSettings.game.fastTransitions);
    Register(g_userSettings.game.noSwordRecoil);
    Register(g_userSettings.game.damageMultiplier);
    Register(g_userSettings.game.noHeartDrops);
    Register(g_userSettings.game.instantDeath);
    Register(g_userSettings.game.fastClimbing);
    Register(g_userSettings.game.fastTears);
    Register(g_userSettings.game.no2ndFishForCat);
    Register(g_userSettings.game.buttonFishing);
    Register(g_userSettings.game.instantSaves);
    Register(g_userSettings.game.dualScreen);
    Register(g_userSettings.game.dualScreenHudMode);
    Register(g_userSettings.game.dualScreenLayout);
    Register(g_userSettings.game.dualScreenRestoreMinimalHud);
    Register(g_userSettings.game.dualScreenRedrawDivisor);
    Register(g_userSettings.game.minimapRedrawDivisor);
    Register(g_userSettings.game.shaderSceneWaitMs);
    Register(g_userSettings.game.dualScreenResolution);
    Register(g_userSettings.game.dualScreenSwap);
    Register(g_userSettings.game.dualScreenDisplay);
    Register(g_userSettings.game.dualScreenPosX);
    Register(g_userSettings.game.dualScreenPosY);
    Register(g_userSettings.game.dualScreenHaptics);
    Register(g_userSettings.game.configMigration);
    Register(g_userSettings.game.guideEnabled);
    Register(g_userSettings.game.dynamicResolution);
    Register(g_userSettings.game.dynamicResolutionTarget);
    Register(g_userSettings.game.predictiveInterpolation);
    Register(g_userSettings.game.instantText);
    Register(g_userSettings.game.holdToMash);
    Register(g_userSettings.game.sunsSong);
    Register(g_userSettings.game.autoSave);
    Register(g_userSettings.game.enhancedMapMenus);
    Register(g_userSettings.game.aimingReticle);
    Register(g_userSettings.game.enableMirrorMode);
    Register(g_userSettings.game.invertCameraXAxis);
    Register(g_userSettings.game.invertCameraYAxis);
    Register(g_userSettings.game.invertFirstPersonXAxis);
    Register(g_userSettings.game.invertFirstPersonYAxis);
    Register(g_userSettings.game.invertAirSwimX);
    Register(g_userSettings.game.invertAirSwimY);
    Register(g_userSettings.game.freeCameraXSensitivity);
    Register(g_userSettings.game.freeCameraYSensitivity);
    Register(g_userSettings.game.touchCameraXSensitivity);
    Register(g_userSettings.game.touchCameraYSensitivity);
    Register(g_userSettings.game.minimalHUD);
    Register(g_userSettings.game.showMinimap);
    Register(g_userSettings.game.hudScale);
    Register(g_userSettings.game.pauseOnFocusLost,
        [](const bool& value, const bool&) { aurora_set_pause_on_focus_lost(value); });
    Register(g_userSettings.game.enableDiscordPresence);
    Register(g_userSettings.game.bloomMode);
    Register(g_userSettings.game.bloomMultiplier);
    Register(g_userSettings.game.depthOfFieldMode);
    Register(g_userSettings.game.disableWaterRefraction);
    Register(g_userSettings.game.enableTextureReplacements,
        [](const bool&, const bool&) { texture_replacements::reload(); });
    Register(g_userSettings.game.internalResolutionScale,
        [](const int& value, const int&) { applyInternalResolutionScale(value); });
    Register(g_userSettings.game.resampler,
        [](const Resampler& value, const Resampler&) { applyResampler(value); });
    Register(g_userSettings.game.shadowResolutionMultiplier);
    Register(g_userSettings.game.maxTextureAnisotropy);
    // Applied on change and once here, so a saved "off" is in force from the first frame.
    Register(g_userSettings.game.pipelineMemo,
        [](const bool& value, const bool&) { aurora_gx_set_pipeline_memo(value); });
    aurora_gx_set_pipeline_memo(g_userSettings.game.pipelineMemo.getValue());
    // A new base policy changes what Auto means: apply it at once (poll() also notices).
    Register(g_userSettings.game.autoResolutionPolicy, [](const int&, const int&) {
        applyInternalResolutionScale(g_userSettings.game.internalResolutionScale.getValue());
    });
    Register(g_userSettings.game.hdRumble);         // V1-rc16
    Register(g_userSettings.game.rumbleIntensity);  // V1-rc16
    Register(g_userSettings.game.gpuBoost);         // V1-rc16
    Register(g_userSettings.game.enableMapBackground);
    Register(g_userSettings.game.disableCutscenePillarboxing);
    Register(g_userSettings.game.disableLetterboxing);
    Register(g_userSettings.game.enableHighQualityMinimapTextures);
    Register(g_userSettings.game.enableFastIronBoots);
    Register(g_userSettings.game.canTransformAnywhere);
    Register(g_userSettings.game.fastRoll);
    Register(g_userSettings.game.armorRupeeDrain);
    Register(g_userSettings.game.restoreWiiGlitches);
    Register(g_userSettings.game.enableLinkDollRotation);
    Register(g_userSettings.game.enableAchievementToasts);
    Register(g_userSettings.game.enableControllerToasts);
    Register(g_userSettings.game.noMissClimbing);
    Register(g_userSettings.game.noLowHpSound);
    Register(g_userSettings.game.midnasLamentNonStop);
    Register(g_userSettings.game.enableTurboKeybind);
    Register(g_userSettings.game.enableResetKeybind);
    Register(g_userSettings.game.speedrunMode);
    Register(g_userSettings.game.liveSplitEnabled);
    Register(g_userSettings.game.showSpeedrunRTATimer);
    Register(g_userSettings.game.recordingMode);
    Register(g_userSettings.game.menuScalingMode);
    Register(g_userSettings.game.removeQuestMapMarkers);
    Register(g_userSettings.game.showInputViewer);
    Register(g_userSettings.game.showInputViewerGyro);
    Register(g_userSettings.game.enableMoveLinkCombo);
    Register(g_userSettings.game.enableTeleportCombo);
    Register(g_userSettings.game.lastSelectedGameModeId);
    Register(g_userSettings.game.fastSpinner);
    Register(g_userSettings.game.infiniteHearts);
    Register(g_userSettings.game.infiniteArrows);
    Register(g_userSettings.game.infiniteSeeds);
    Register(g_userSettings.game.infiniteBombs);
    Register(g_userSettings.game.infiniteOil);
    Register(g_userSettings.game.infiniteOxygen);
    Register(g_userSettings.game.infiniteRupees);
    Register(g_userSettings.game.enableIndefiniteItemDrops);
    Register(g_userSettings.game.moonJump);
    Register(g_userSettings.game.superClawshot);
    Register(g_userSettings.game.alwaysGreatspin);
    Register(g_userSettings.game.invincibleEnemies);
    Register(g_userSettings.game.easyQuickSpin);

    Register(g_userSettings.game.enableFrameInterpolation);
    Register(g_userSettings.game.interpolationQuality);
    Register(g_userSettings.game.enableGyroAim);
    Register(g_userSettings.game.enableGyroRollgoal);
    Register(g_userSettings.game.enableWiiMotion);
    Register(g_userSettings.game.wiiMotionSensitivity);
    Register(g_userSettings.game.wiiMotionSwordHand);
    Register(g_userSettings.game.wiiMotionSwingCuts);
    Register(g_userSettings.game.wiiMotionSwapSides);
    Register(g_userSettings.game.wiiAccelThresholdG);
    Register(g_userSettings.game.wiiGyroThresholdRad);
    Register(g_userSettings.game.wiiDirectionDominance);
    Register(g_userSettings.game.wiiMinYawRad);
    Register(g_userSettings.game.wiiGyroWeight);
    Register(g_userSettings.game.wiiCooldownS);
    Register(g_userSettings.game.wiiWindupFilter);
    Register(g_userSettings.game.wiiWindupMaxG);
    Register(g_userSettings.game.wiiWindupMaxGyroRad);
    Register(g_userSettings.game.wiiThrustMaxGyroRad);
    Register(g_userSettings.game.wiiThrustAxis);
    Register(g_userSettings.game.wiiAxisLateralLeft);
    Register(g_userSettings.game.wiiAxisForwardLeft);
    Register(g_userSettings.game.wiiForwardSignLeft);
    Register(g_userSettings.game.wiiAxisLateralRight);
    Register(g_userSettings.game.wiiAxisForwardRight);
    Register(g_userSettings.game.wiiForwardSignRight);
    Register(g_userSettings.game.gyroSensitivityX);
    Register(g_userSettings.game.gyroSensitivityY);
    Register(g_userSettings.game.gyroSensitivityRollgoal);
    Register(g_userSettings.game.gyroDeadband);
    Register(g_userSettings.game.gyroSmoothing);
    Register(g_userSettings.game.gyroInvertPitch);
    Register(g_userSettings.game.gyroInvertYaw);
    Register(g_userSettings.game.enableMouseCamera);
    Register(g_userSettings.game.enableMouseAim);
    Register(g_userSettings.game.mouseAimSensitivity);
    Register(g_userSettings.game.mouseCameraSensitivity);
    Register(g_userSettings.game.invertMouseY);
    Register(g_userSettings.game.freeCamera);
    Register(g_userSettings.game.enableTouchControls);
    Register(g_userSettings.game.touchTargeting);
    Register(g_userSettings.game.enableMenuPointer);
    Register(g_userSettings.game.touchControlsLayout);
    Register(g_userSettings.game.debugFlyCam);
    Register(g_userSettings.game.debugFlyCamLockEvents);
    Register(g_userSettings.game.allowBackgroundInput);
    Register(g_userSettings.game.enableLED[0]);
    Register(g_userSettings.game.enableLED[1]);
    Register(g_userSettings.game.enableLED[2]);
    Register(g_userSettings.game.enableLED[3]);
    Register(g_userSettings.game.swapDirectSelect);

    Register(g_userSettings.backend.isoPath);
    Register(g_userSettings.backend.isoVerification);
    Register(g_userSettings.backend.graphicsBackend);
    Register(g_userSettings.backend.skipPreLaunchUI);
    Register(g_userSettings.backend.showPipelineCompilation);
    Register(g_userSettings.backend.wasPresetChosen);
    Register(g_userSettings.backend.checkForUpdates);
    Register(g_userSettings.backend.checkForModUpdates);
    Register(g_userSettings.backend.cardFileType);
    Register(g_userSettings.backend.enableAdvancedSettings);
    Register(g_userSettings.backend.showProfilerOverlay);

    Register(g_userSettings.actionBindings.firstPersonCamera[0]);
    Register(g_userSettings.actionBindings.firstPersonCamera[1]);
    Register(g_userSettings.actionBindings.firstPersonCamera[2]);
    Register(g_userSettings.actionBindings.firstPersonCamera[3]);
    Register(g_userSettings.actionBindings.callMidna[0]);
    Register(g_userSettings.actionBindings.callMidna[1]);
    Register(g_userSettings.actionBindings.callMidna[2]);
    Register(g_userSettings.actionBindings.callMidna[3]);
    Register(g_userSettings.actionBindings.openMapScreen[0]);
    Register(g_userSettings.actionBindings.openMapScreen[1]);
    Register(g_userSettings.actionBindings.openMapScreen[2]);
    Register(g_userSettings.actionBindings.openMapScreen[3]);
    Register(g_userSettings.actionBindings.toggleMinimap[0]);
    Register(g_userSettings.actionBindings.toggleMinimap[1]);
    Register(g_userSettings.actionBindings.toggleMinimap[2]);
    Register(g_userSettings.actionBindings.toggleMinimap[3]);
    Register(g_userSettings.actionBindings.openDusklightMenu[0]);
    Register(g_userSettings.actionBindings.openDusklightMenu[1]);
    Register(g_userSettings.actionBindings.openDusklightMenu[2]);
    Register(g_userSettings.actionBindings.openDusklightMenu[3]);
    Register(g_userSettings.actionBindings.turboSpeedButton[0]);
    Register(g_userSettings.actionBindings.turboSpeedButton[1]);
    Register(g_userSettings.actionBindings.turboSpeedButton[2]);
    Register(g_userSettings.actionBindings.turboSpeedButton[3]);
    Register(g_userSettings.actionBindings.useSlotItem1[0]);
    Register(g_userSettings.actionBindings.useSlotItem1[1]);
    Register(g_userSettings.actionBindings.useSlotItem1[2]);
    Register(g_userSettings.actionBindings.useSlotItem1[3]);
    Register(g_userSettings.actionBindings.useSlotItem2[0]);
    Register(g_userSettings.actionBindings.useSlotItem2[1]);
    Register(g_userSettings.actionBindings.useSlotItem2[2]);
    Register(g_userSettings.actionBindings.useSlotItem2[3]);
}

// Transient settings

static TransientSettings g_transientSettings = {
    .collisionView = {
        .enableTerrainView = false,
        .enableWireframe = false,
        .enableAtView = false,
        .enableTgView = false,
        .enableCoView = false,
        .terrainViewOpacity = 50.0f,
        .colliderViewOpacity = 50.0f,
        .drawRange = 100.0f,
    },
    .triggerView = {
        .loadZones = false,
        .eventAreas = false,
        .switchAreas = false,
        .eventTags = false,
        .midnaStops = false,
        .twilightGates = false,
        .checkpoints = false,
        .paths = false,
        .transformDists = false,
        .attentionDists = false,
        .purpleMistAvoid = false,
        .leevers = false,
        .opacity = 75.0f,
    },
    .turboMode = false,
};

TransientSettings& getTransientSettings() {
    return g_transientSettings;
}

// H33: repairs what a config file could hold that the console cannot show. Returns true when
// something was rewritten (the caller saves once for all of it).
//
//   * corrupt: a resolution outside 0..18, or an unknown base policy;
//   * unsafe on this console: inside the domain but above 1080 rows (factors 3 to 12, 1440p,
//     2160p). A x12 saved by accident allocates a 6336-row framebuffer and ran the device out
//     of memory at startup, so it would crash on every launch until the file was edited.
// Both go back to Auto (a base policy back to Legacy) and the user is told once. A value that
// is merely too big for the current mode (1080p while handheld) is left alone: it is applied
// limited at run time and comes back when the console is docked.
static bool sanitizeLoadedSettings() {
    bool changed = false;
    auto& game = g_userSettings.game;

    // The persisted value, not whatever an override layer says right now.
    const int resolution = game.internalResolutionScale.getValueForSave();
    bool repair = false;
    switch (respol::classify(resolution)) {
    case respol::StoredClass::Corrupt:
        repair = true;
        break;
    case respol::StoredClass::UnsafeSwitch:
#ifdef __SWITCH__
        repair = true;
#endif
        break;
    default:
        break;
    }
    if (repair) {
        DuskLog.warn("Config: game.internalResolutionScale = {} is not usable here, resetting it to Auto",
                     resolution);
        game.internalResolutionScale.setValue(0, false);
        s_repairNotice = "The saved internal resolution was not valid for this console and was reset to Auto.";
        changed = true;
    }

    const int policy = game.autoResolutionPolicy.getValueForSave();
    if (respol::sanitize_policy(policy) != policy) {
        DuskLog.warn("Config: game.autoResolutionPolicy = {} is not valid, resetting it", policy);
        game.autoResolutionPolicy.setValue(respol::kPolicyLegacy, false);
        changed = true;
    }
    return changed;
}

// aleks2 (companion): from Aleks' settings.cpp.
void runConfigMigrations() {
    // Bump this and add a numbered block below when an update needs to
    // rewrite a value stored by older installs.
    constexpr int kConfigMigration = 4;
    const bool repaired = sanitizeLoadedSettings();
    auto& applied = g_userSettings.game.configMigration;
    if (applied.getValue() >= kConfigMigration) {
        if (repaired) {
            config::save();
        }
        return;
    }
    if (applied.getValue() < 1) {
        // 1: "3DS Style" (Functional) became the intended mode for updated
        // installs; older configs carry an explicit Cinematic value from
        // when that was the only/default mode.
        g_userSettings.game.dualScreenHudMode.setValue(kDualHudFunctional);
    }
    if (applied.getValue() < 2) {
        // 2: dual-screen baseline -- the FPS counter homes on the companion
        // (safe on single-screen: the overlay falls back to a main corner)
        // and Minimal HUD off (the companion replaces its purpose).
        g_userSettings.video.fpsOverlayCorner.setValue(kFpsCornerCompanion);
        g_userSettings.game.minimalHUD.setValue(false);
    }
    if (applied.getValue() < 3) {
        // 3 (aleks2): used to turn Lock Aspect Ratio on for configs already running Flip
        // Grip. H33 retired that: Flip Grip forces 4:3 while it is in effect and never
        // writes the preference (dualscreen::effectiveAspectLock), so there is nothing to
        // migrate. A value already stored by an earlier build is the user's now and stays.
    }
    if (applied.getValue() < 4) {
        // 4 (H33): "Auto" resolution gets a remembered base policy. A config that matches a
        // pre-H33 profile EXACTLY becomes that profile's policy, with Auto resolution (the old
        // Balanced and Performance stored a fixed 540). Anything else -- every Custom config,
        // including Auto ones -- stays Legacy and keeps the behavior it had.
        if (const int legacy = profile::detect_legacy(profile::currentKnobs());
            legacy != profile::kCustom)
        {
            g_userSettings.game.autoResolutionPolicy.setValue(profile::policy_for(legacy));
            g_userSettings.game.internalResolutionScale.setValue(0);
        }
    }
    applied.setValue(kConfigMigration);
    config::save();
}

}
