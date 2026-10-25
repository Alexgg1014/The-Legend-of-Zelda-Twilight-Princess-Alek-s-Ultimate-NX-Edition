#include "settings.hpp"

#include "bool_button.hpp"
#include "controller_config.hpp"
#include "graphics_tuner.hpp"
#include "menu_bar.hpp"
#include "modal.hpp"
#include "number_button.hpp"
#include "pane.hpp"
#include "prelaunch.hpp"
#include "saves_window.hpp"
#include "touch_controls_editor.hpp"
#include "ui.hpp"
#include "dusk/dev_mode.hpp"
#include <aurora/shader_cache.h>

#include "dusk/app_info.hpp"
#include "dusk/audio/DuskAudioSystem.h"
#include "dusk/audio/DuskDsp.hpp"
#include "dusk/config.hpp"
#include "dusk/deko3d_policy.hpp"
#include "dusk/data.hpp"
#include "dusk/dualscreen.h"
#include "dusk/perf_profile_apply.hpp"
#include "dusk/drs_stress.hpp"
#include "dusk/dynres.hpp"
#ifdef __SWITCH__
#include "dusk/switch_net.h"
#endif
#include "dusk/frame_telemetry.hpp"
#include "dusk/discord_presence.hpp"
#include "dusk/hotkeys.h"
#include "dusk/imgui/ImGuiEngine.hpp"
#include "dusk/language.hpp"
#include "dusk/livesplit.h"
#include "dusk/presentation.hpp"
#include "dusk/speedrun.h"
#include "dusk/wii_controls.hpp"
#ifdef __SWITCH__
#include "dusk/benchmark.hpp"
#include "dusk/director.hpp"
#include "dusk/switch_jit.h"
#endif
#if ALEKS2_NATIVE_MOD_LOADER
#include "dusk/main.h"
#include "dusk/switch_native_loader.hpp"
#ifdef __SWITCH__
#include "dusk/self_update.hpp"
#endif
// platforms/switch/source/switch_startup.cpp (V1-rc4): start this NRO again when the program exits.
extern "C" bool aleks2_relaunch_on_exit(void);
#endif
#if defined(__SWITCH__) && DUSK_HAS_FUNCHOOK
#include "dusk/switch_code_arena.h"
#endif

#include <aurora/gfx.h>
#include <aurora/tuning.h>
#include <aurora/lib/window.hpp>
#include <borealis/file_select.hpp>
#include <borealis/io.hpp>
#if BOREALIS_HAS_SENTRY
#include <borealis/sentry.hpp>
#endif
#include <fmt/format.h>
#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <filesystem>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(TARGET_ANDROID) || defined(__ANDROID__) ||                                             \
    (defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_MACCATALYST)
#define TOUCH_CONTROLS_AVAILABLE true
#else
#define TOUCH_CONTROLS_AVAILABLE false
#endif

namespace dusk::ui {
namespace {

constexpr std::array kCardFileTypes = {
    "Card Image",
    "GCI Folder",
};

constexpr std::array kFpsOverlayCornerNames = {
    "Top Left",
    "Top Right",
    "Bottom Left",
    "Bottom Right",
    // aleks2 (companion): index 4 == dusk::kFpsCornerCompanion.
    "Companion Screen",
};
static_assert(kFpsOverlayCornerNames.size() == dusk::kFpsCornerCompanion + 1);

// aleks2 (companion): from the Aleks settings UI, unchanged wording.
constexpr std::array kDualScreenHudModeNames = {
    "Wii U Style",
    "3DS Style",
};

// How the Switch's single panel is split. Deliberately only two: there is no
// horizontal dual mode. Flip Grip needs the console physically turned, so it is
// handheld-only and falls back to Normal while docked.
constexpr std::array kDualScreenLayoutNames = {
    "Normal",
    "Flip Grip",
};

// Shared by every dual-screen button: turning the feature on points the FPS
// counter at the companion and clears Minimal HUD; turning it off hands the
// counter back. Callers invoke this only on an Off <-> On transition.
void applyDualScreenSideEffects(bool enabled) {
    auto& corner = getSettings().video.fpsOverlayCorner;
    auto& minimal = getSettings().game.minimalHUD;
    if (enabled && dusk::dualscreen::hudOnCompanion()) {
        if (corner.getValue() != dusk::kFpsCornerCompanion) {
            corner.setValue(dusk::kFpsCornerCompanion);
        }
        if (minimal.getValue()) {
            minimal.setValue(false);
            getSettings().game.dualScreenRestoreMinimalHud.setValue(true);
        }
    } else if (!enabled) {
        if (corner.getValue() == dusk::kFpsCornerCompanion) {
            // Explicit main corner, NOT getDefaultValue(): the default itself may
            // be Companion, which would leave this hand-back a no-op.
            corner.setValue(0);
        }
        // V1-rc1: hand Minimal HUD back if turning the second screen on took it away.
        auto& restore = getSettings().game.dualScreenRestoreMinimalHud;
        if (restore.getValue()) {
            restore.setValue(false);
            if (!minimal.getValue()) {
                minimal.setValue(true);
            }
        }
    }
}

// aleks2 (performance): "Optimized Settings". Three fixed bundles of the knobs that move the
// frame rate on this console. H33: the bundles themselves (perf_profile.hpp) and the binding to
// the settings (perf_profile_apply.cpp) live outside this file so they can be tested and so the
// benchmark's Run Profile Matrix can apply the same bundles as non-persistent overrides.
// Choosing one also remembers its Auto-resolution policy (game.autoResolutionPolicy); the row
// shows Custom as soon as a knob is changed by hand, but the policy stays.
void applyPerfPreset(int index) {
    dusk::profile::applyPersistent(index);
}

int detectPerfPreset() {
    return dusk::profile::detectCurrent();
}

constexpr std::array kInterpolationModes = {
    "Off",
    "Capped",
    "Unlimited",
};

constexpr std::array<const char*, 2> kInterpQualityModes = {"Fast", "Fancy"};

constexpr std::array kAudioOutputModeNames = {
    "Stereo (Speakers)",
    "Stereo (Headphones)",
    "5.1 Surround",
    "7.1 Surround",
};

constexpr std::array kLetterboxModes = {
    "Off",
    "On",
    "Only During Gameplay",
    "Only During Cutscenes",
};

constexpr std::array kTouchTargetingLabels = {
    "Hybrid",
    "Hold",
    "Switch",
};

constexpr std::array kTouchTargetingDescriptions = {
    "Tap once to lock on when a target is found. Double-tap when none is found to hold L.",
    "L stays held only while your finger is on the button.",
    "Tap L to keep it held. Tap again to release it.",
};

constexpr std::array kGyroInputModeLabels = {
    "Sensor",
    "Mouse",
};

constexpr std::array kMenuScalingModeLabels = {
    "GameCube",
    "Wii",
    "Dusklight",
};

constexpr std::array kWiiSwordHands = {
    "Right Joy-Con",
    "Left Joy-Con",
};

constexpr std::array kAlwaysGreatspinModes = {
    "Off",
    "After Learning Skill",
    "Always",
};

constexpr std::array kMagicArmorModes = {
    "Normal",
    "On Damage",
    "Double Defense",
    "Invincible",
    "Cosmetic",
};

bool try_parse_backend(std::string_view backend, AuroraBackend& outBackend) {
    if (backend == "auto") {
        outBackend = BACKEND_AUTO;
        return true;
    }
    if (backend == "d3d11") {
        outBackend = BACKEND_D3D11;
        return true;
    }
    if (backend == "d3d12") {
        outBackend = BACKEND_D3D12;
        return true;
    }
    if (backend == "metal") {
        outBackend = BACKEND_METAL;
        return true;
    }
    if (backend == "vulkan") {
        outBackend = BACKEND_VULKAN;
        return true;
    }
    if (backend == "opengl") {
        outBackend = BACKEND_OPENGL;
        return true;
    }
    if (backend == "opengles") {
        outBackend = BACKEND_OPENGLES;
        return true;
    }
    if (backend == "webgpu") {
        outBackend = BACKEND_WEBGPU;
        return true;
    }
    if (backend == "null") {
        outBackend = BACKEND_NULL;
        return true;
    }
    // aleks2 (deko3d P1): the experimental deko3d renderer (Switch lab builds only)
    if (backend == "deko3d") {
        outBackend = BACKEND_DEKO3D;
        return true;
    }

    return false;
}

std::string_view backend_name(AuroraBackend backend) {
    switch (backend) {
    default:
        return "Auto";
    case BACKEND_D3D12:
        return "D3D12";
    case BACKEND_D3D11:
        return "D3D11";
    case BACKEND_METAL:
        return "Metal";
    case BACKEND_VULKAN:
        // aleks2 (deko3d rc12): on Switch, Vulkan is Dawn/NVK next to deko3d
        return dusk::deko3d_policy::deko3d_default() ? "Vulkan — best mod compatibility" : "Vulkan";
    case BACKEND_OPENGL:
        return "OpenGL";
    case BACKEND_OPENGLES:
        return "OpenGL ES";
    case BACKEND_WEBGPU:
        return "WebGPU";
    case BACKEND_NULL:
        return "Null";
    case BACKEND_DEKO3D:
        return dusk::deko3d_policy::deko3d_default() ? "deko3d (recommended)" : "deko3d (Experimental)";
    }
}

std::string_view backend_id(AuroraBackend backend) {
    switch (backend) {
    default:
        return "auto";
    case BACKEND_D3D12:
        return "d3d12";
    case BACKEND_D3D11:
        return "d3d11";
    case BACKEND_METAL:
        return "metal";
    case BACKEND_VULKAN:
        return "vulkan";
    case BACKEND_OPENGL:
        return "opengl";
    case BACKEND_OPENGLES:
        return "opengles";
    case BACKEND_WEBGPU:
        return "webgpu";
    case BACKEND_NULL:
        return "null";
    case BACKEND_DEKO3D:
        return "deko3d";
    }
}

std::vector<AuroraBackend> available_backends() {
    std::vector<AuroraBackend> backends;
    backends.emplace_back(BACKEND_AUTO);
    size_t backendCount = 0;
    const AuroraBackend* raw = aurora_get_available_backends(&backendCount);
    for (size_t i = 0; i < backendCount; ++i) {
        // Do not expose NULL
        if (raw[i] != BACKEND_NULL) {
            backends.emplace_back(raw[i]);
        }
    }
    return backends;
}

AuroraBackend configured_backend() {
    AuroraBackend configuredBackend = BACKEND_AUTO;
    const auto configuredId = getSettings().backend.graphicsBackend.getValue();
    if (!try_parse_backend(configuredId, configuredBackend)) {
        configuredBackend = BACKEND_AUTO;
    }
    return configuredBackend;
}

bool is_graphics_backend_restart_pending() {
    return getSettings().backend.graphicsBackend.getValue() !=
           prelaunch_state().initialGraphicsBackend;
}

Rml::String graphics_backend_display_name() {
    if (is_graphics_backend_restart_pending()) {
        return Rml::String{backend_name(configured_backend())};
    }
    return Rml::String{backend_name(aurora_get_backend())};
}

Rml::String configured_data_path_display_name() {
    const auto path = data::abbreviated_path_string(data::configured_data_path());
    if (path.empty()) {
        return "(none)";
    }

    auto display = borealis::io::display_name(path);
    if (display.empty()) {
        return path;
    }
    return display;
}

class DataFolderPathText : public Component {
public:
    explicit DataFolderPathText(Rml::Element* parent)
        : Component(append(parent, "data-folder-path")) {
        append_text_element(mRoot, "small", "Current data folder:");
        mPath = append(mRoot, "file-path");
    }

    void update() override {
        const Rml::String path = data::abbreviated_path_string(data::configured_data_path());
        if (path != mCurrentPath) {
            set_text_content(mPath, path);
            mCurrentPath = path;
        }
        Component::update();
    }

private:
    Rml::Element* mPath = nullptr;
    Rml::String mCurrentPath;
};

void show_data_folder_error_modal(std::string_view message) {
    auto dismiss = [](Modal& modal) {
        mDoAud_seStartMenu(kSoundWindowClose);
        modal.pop();
    };
    push_document(std::make_unique<Modal>(Modal::Props{
        .title = "Data Folder Not Changed",
        .bodyText = Rml::String{message},
        .actions =
            {
                ModalAction{
                    .label = "OK",
                    .onPressed = dismiss,
                },
            },
        .onDismiss = dismiss,
        .icon = "warning",
    }));
    if (auto* doc = top_document()) {
        doc->focus();
    }
}

void data_folder_dialog_callback(borealis::file_select::Result result) {
    if (result.status == borealis::file_select::Status::Canceled) {
        return;
    }
    if (result.status != borealis::file_select::Status::Selected || result.locations.empty()) {
        show_data_folder_error_modal("Dusklight could not open the folder picker.");
        return;
    }

    std::string dataPathError;
    if (data::set_custom_data_path(result.locations.front(), &dataPathError)) {
        mDoAud_seStartMenu(kSoundItemChange);
        return;
    }

    if (dataPathError.empty()) {
        dataPathError =
            fmt::format("{} could not use the selected folder as its data folder.", AppName);
    }
    show_data_folder_error_modal(dataPathError);
}

const Rml::String kInternalResolutionHelpText =
    "Configure the resolution used for rendering the game. Higher values are more demanding on "
    "your graphics hardware.";
const Rml::String kShadowResolutionHelpText =
    "Configure the shadow-map resolution. Higher values improve shadow quality but increase GPU "
    "and memory usage.";
const Rml::String kResamplerHelpText =
    "Configure the sampling method used when scaling the internal resolution for final presentation.";
const Rml::String kBloomHelpText =
    "Configure the post-processing bloom effect. Classic uses the original bloom pass; Dusklight uses "
    "a higher-quality bloom pass.";
const Rml::String kBloomBrightnessHelpText =
    "Configure bloom intensity. Higher values make bright areas glow more strongly.";
const Rml::String kDepthOfFieldHelpText =
    "Configure the post-processing depth-of-field effect. Classic uses the original depth-of-field pass;"
    " Dusklight uses a higher-quality depth-of-field pass.";
const Rml::String kUnlockFramerateHelpText =
    "<br/>Uses inter-frame interpolation to enable higher frame rates.<br/><br/>May introduce minor "
    "visual artifacts or animation glitches.";
const Rml::String kInterpolationQualityHelpText =
    "<br/>Fast leaves particles, rain, snow, clouds, material effects and model deformation out of"
    " interpolation, so they only change on game ticks.<br/><br/>Fancy interpolates them too:"
    " smoother, and more CPU.";
const Rml::String kTextureReplacementHelpText =
    "Enable installed texture replacements.";

int float_setting_percent(ConfigVar<float>& var) {
    return static_cast<int>(var.getValue() * 100.0f + 0.5f);
}

bool gyro_enabled() {
    return getSettings().game.enableGyroAim || getSettings().game.enableGyroRollgoal;
}

Rml::String touch_targeting_label(TouchTargeting targeting) {
    const auto index = static_cast<size_t>(targeting);
    if (index >= kTouchTargetingLabels.size()) {
        return "Unknown";
    }
    return kTouchTargetingLabels[index];
}

struct ConfigBoolProps {
    Rml::String key;
    Rml::String icon;
    Rml::String helpText;
    std::function<void(bool)> onChange;
    std::function<bool()> isDisabled;
};

SelectButton& config_bool_select(
    Pane& leftPane, Pane& rightPane, ConfigVar<bool>& var, ConfigBoolProps props) {
    auto& button = leftPane.add_child<BoolButton>(BoolButton::Props{
        .key = std::move(props.key),
        .icon = std::move(props.icon),
        .getValue = [&var] { return var.getValue(); },
        .setValue =
            [&var, callback = std::move(props.onChange)](bool value) {
                if (value == var.getValue()) {
                    return;
                }
                var.setValue(value);
                config::save();
                if (callback) {
                    callback(value);
                }
            },
        .isDisabled = std::move(props.isDisabled),
        .isModified = [&var] { return var.getValue() != var.getDefaultValue(); },
    });
    leftPane.register_control(
        button, rightPane, [helpText = std::move(props.helpText)](Pane& pane) {
            pane.clear();
            pane.add_rml(helpText);
        });
    return button;
}

void add_speedrun_disabled_option(Pane& leftPane, Pane& rightPane, ConfigVar<bool>& var,
    const Rml::String& key, const Rml::String& helpText) {
    config_bool_select(leftPane, rightPane, var, {
        .key = key,
        .helpText = helpText,
        .isDisabled = [] { return speedrun::isActive(); },
    });
}

// A row with one button per value of an enum, for a setting with a few named
// choices. `names` is indexed by the enum's numeric value and must outlive the row.
template <typename E, std::size_t N>
void config_enum_select(Pane& leftPane, Pane& rightPane, ConfigVar<E>& var, Rml::String key,
    const std::array<const char*, N>& names, Rml::String helpText) {
    leftPane.register_control(
        leftPane.add_select_button({
            .key = std::move(key),
            .getValue = [&var, &names] { return names[static_cast<std::size_t>(var.getValue())]; },
            .isModified = [&var] { return var.getValue() != var.getDefaultValue(); },
        }),
        rightPane, [&var, &names, helpText = std::move(helpText)](Pane& pane) {
            for (std::size_t i = 0; i < N; ++i) {
                pane.add_button({
                              .text = names[i],
                              .isSelected = [&var, i] { return var.getValue() == static_cast<E>(i); },
                          })
                    .on_pressed([&var, i] {
                        mDoAud_seStartMenu(kSoundItemChange);
                        var.setValue(static_cast<E>(i));
                        config::save();
                    });
            }
            pane.add_rml(helpText);
        });
}

SelectButton& config_percent_select(Pane& leftPane, Pane& rightPane, ConfigVar<float>& var,
    Rml::String key, Rml::String helpText, int min, int max, int step = 5,
    std::function<bool()> isDisabled = {}) {
    auto& button = leftPane.add_child<NumberButton>(NumberButton::Props{
        .key = std::move(key),
        .getValue = [&var] { return float_setting_percent(var); },
        .setValue =
            [&var, min, max](int value) {
                var.setValue(std::clamp(value, min, max) / 100.0f);
                config::save();
            },
        .isDisabled = std::move(isDisabled),
        .isModified = [&var] { return var.getValue() != var.getDefaultValue(); },
        .min = min,
        .max = max,
        .step = step,
        .suffix = "%",
    });
    leftPane.register_control(button, rightPane, [helpText = std::move(helpText)](Pane& pane) {
        pane.clear();
        pane.add_rml(helpText);
    });
    return button;
}

SelectButton& config_int_select(Pane& leftPane, Pane& rightPane, ConfigVar<int>& var,
    Rml::String key, Rml::String helpText, int min, int max, int step = 5,
    std::function<bool()> isDisabled = {}, std::function<void(int)> onChange = {},
    std::string suffix = "") {
    auto& button = leftPane.add_child<NumberButton>(NumberButton::Props{
        .key = std::move(key),
        .getValue = [&var] { return var.getValue(); },
        .setValue =
            [&var, min, max, callback = std::move(onChange)](int value) {
                const int clampedValue = std::clamp(value, min, max);
                var.setValue(clampedValue);
                config::save();
                if (callback) {
                    callback(clampedValue);
                }
            },
        .isDisabled = std::move(isDisabled),
        .isModified = [&var] { return var.getValue() != var.getDefaultValue(); },
        .min = min,
        .max = max,
        .step = step,
        .suffix = suffix,
    });
    leftPane.register_control(button, rightPane, [helpText = std::move(helpText)](Pane& pane) {
        pane.clear();
        pane.add_text(helpText);
    });
    return button;
}

void graphics_tuner_control(Window& window, Pane& leftPane, Pane& rightPane,
    const GraphicsTunerProps& props) {
    const auto setting = GraphicsSetting::of(props.option);
    leftPane.register_control(
        leftPane
            .add_select_button({
                .key = props.title,
                .getValue = [setting] { return setting.text(); },
                .isModified = [setting] { return setting.isModified(); },
                .submit = false,
            })
            .on_nav_command([&window, props](Rml::Event&, NavCommand cmd) {
                if (cmd == NavCommand::Confirm || cmd == NavCommand::Left ||
                    cmd == NavCommand::Right) {
                    window.push(std::make_unique<GraphicsTuner>(props));
                    return true;
                }
                return false;
            }),
        rightPane, [helpText = props.helpText](Pane& pane) {
            pane.clear();
            pane.add_text(helpText);
        });
}

}  // namespace

SettingsWindow::SettingsWindow(bool prelaunch) : mPrelaunch(prelaunch) {
    if (prelaunch) {
        add_tab("Prelaunch", [this](Rml::Element* content) {
            auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
            auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

            leftPane.register_control(
                leftPane
                    .add_select_button({
                        .key = "Disc Image",
                        .getValue =
                            [] {
                                const auto& path = prelaunch_state().configuredDiscPath;
                                std::string display;
                                if (path.empty()) {
                                    display = "(none)";
                                } else {
                                    display = borealis::io::display_name(path);
                                    if (display.empty()) {
                                        display = path;
                                    }
                                }
                                return display;
                            },
                        .isModified =
                            [] {
                                const auto& state = prelaunch_state();
                                const auto& active = state.activeDiscPath;
                                return !active.empty() && state.configuredDiscPath != active;
                            },
                    })
                    .on_pressed([] { open_iso_picker(); }),
                rightPane, [](Pane& pane) {
                    pane.add_rml("Set the disc image that Dusklight uses to launch the game.<br/><br/>"
                                 "Changes require a restart.");
                });
            if (data::manager().capabilities().canChangeLocation &&
                borealis::file_select::capabilities().canOpenFolder)
            {
                leftPane.register_control(
                    leftPane.add_select_button({
                        .key = "Data Folder",
                        .getValue = [] { return configured_data_path_display_name(); },
                        .isModified = [] { return data::is_data_path_restart_pending(); },
                    }),
                    rightPane, [](Pane& pane) {
                        pane.add_text("The data folder is where Dusklight stores settings, saves, "
                                      "logs, texture replacements, and other app data.");
                        pane.add_child<DataFolderPathText>();
#if DUSK_CAN_OPEN_DATA_FOLDER
                        pane.add_button("Open Data Folder").on_pressed([] {
                            if (data::open_data_path()) {
                                mDoAud_seStartMenu(kSoundClick);
                            }
                        });
#endif
                        pane.add_button("Change Data Folder").on_pressed([] {
                            const auto defaultLocation =
                                borealis::io::fs_path_to_string(data::configured_data_path());
                            borealis::file_select::open_folder(
                                {
                                    .parentWindow = aurora::window::get_sdl_window(),
                                    .defaultLocation = defaultLocation,
                                    .requireRealPath = true,
                                },
                                &data_folder_dialog_callback);
                        });
#if defined(_WIN32)
                        pane.add_button("Portable Mode").on_pressed([] {
                            if (data::set_portable_data_path()) {
                                mDoAud_seStartMenu(kSoundItemChange);
                            }
                        });
#endif
                        pane.add_button(
                                {
                                    .text = "Reset to Default",
                                    .isDisabled = [] { return data::is_default_data_path(); },
                                })
                            .on_pressed([] {
                                if (data::reset_data_path()) {
                                    mDoAud_seStartMenu(kSoundItemChange);
                                }
                            });
                        pane.add_rml("Data will be migrated automatically on restart.");
                    });
            }
            leftPane.register_control(
                leftPane.add_select_button({
                    .key = "Language",
                    .getValue =
                        [] {
                            return language::language_name(getSettings().game.language.getValue());
                        },
                    .isDisabled =
                        [] {
                            const auto& state = prelaunch_state();
                            if (!state.configuredDiscCanLaunch) {
                                return true;
                            }
                            return language::available_languages(state.configuredDiscInfo).size() <= 1;
                        },
                    .isModified =
                        [] {
                            return getSettings().game.language.getValue() !=
                                   prelaunch_state().initialLanguage;
                        },
                }),
                rightPane, [](Pane& pane) {
                    const auto& state = prelaunch_state();
                    const auto languages = state.configuredDiscCanLaunch
                                               ? language::available_languages(state.configuredDiscInfo)
                                               : language::available_languages({});
                    for (const GameLanguage language : languages) {
                        pane.add_button({
                                            .text = language::language_name(language),
                                            .isSelected =
                                                [language] {
                                                    return getSettings().game.language.getValue() ==
                                                           language;
                                                },
                                        })
                            .on_pressed([language] {
                                mDoAud_seStartMenu(kSoundItemChange);
                                getSettings().game.language.setValue(language);
                                config::save();
                            });
                    }
                    pane.add_rml("<br/>Changes require a restart.");
                });
            leftPane.register_control(
                leftPane.add_select_button({
                    .key = "Graphics Backend",
                    .getValue = [] { return graphics_backend_display_name(); },
                    .isModified = [] { return is_graphics_backend_restart_pending(); },
                }),
                rightPane, [](Pane& pane) {
                    const auto availableBackends = available_backends();
                    for (const auto backend : availableBackends) {
                        pane
                            .add_button({
                                .text = Rml::String{backend_name(backend)},
                                .isSelected = [backend] { return configured_backend() == backend; },
                            })
                            .on_pressed([backend] {
                                mDoAud_seStartMenu(kSoundItemChange);
                                getSettings().backend.graphicsBackend.setValue(
                                    std::string{backend_id(backend)});
                                config::save();
                                dusk::deko3d_policy::on_backend_chosen(backend);
                            });
                    }
#ifdef __SWITCH__
                    // aleks2 (deko3d rc12): the restart the backend change (and deko3d's prompts) ask for
                    pane.add_button({.text = "Restart now"}).on_pressed([] {
                        mDoAud_seStartMenu(kSoundClick);
                        if (aleks2_relaunch_on_exit()) {
                            IsRunning = false;
                        } else {
                            dusk::ui::push_toast({.title = "Restart by hand",
                                .content = "Quit Dusklight and open it again to apply the change.",
                                .duration = std::chrono::seconds(6)});
                        }
                    });
#endif
                    pane.add_rml("<br/>Changes require a restart.");
                    // aleks2 (deko3d rc12)
                    pane.add_rml("<br/>deko3d: the console's own graphics API. Mods that draw with the gfx "
                                 "service need Vulkan: with such a mod enabled, Vulkan is used and a message says "
                                 "why. If deko3d fails to start twice, Vulkan is used from then on.");
                });
            leftPane.register_control(
                leftPane.add_select_button({
                    .key = "Save File Type",
                    .getValue =
                        [] {
                            return kCardFileTypes[getSettings().backend.cardFileType.getValue()];
                        },
                    .isModified =
                        [] {
                            return getSettings().backend.cardFileType.getValue() !=
                                   prelaunch_state().initialCardFileType;
                        },
                }),
                rightPane, [](Pane& pane) {
                    for (int i = 0; i < kCardFileTypes.size(); i++) {
                        pane
                            .add_button({
                                .text = kCardFileTypes[i],
                                .isSelected =
                                    [i] {
                                        return getSettings().backend.cardFileType.getValue() == i;
                                    },
                            })
                            .on_pressed([i] {
                                mDoAud_seStartMenu(kSoundItemChange);
                                getSettings().backend.cardFileType.setValue(i);
                                config::save();
                            });
                    }
                });
            add_save_files_control(leftPane, rightPane);
        });
    }

    add_tab("Video", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Display");
        #ifndef __SWITCH__
        leftPane.register_control(leftPane.add_button("Toggle Fullscreen").on_pressed([] {
            mDoAud_seStartMenu(kSoundItemChange);
            getSettings().video.enableFullscreen.setValue(!getSettings().video.enableFullscreen);
            VISetWindowFullscreen(getSettings().video.enableFullscreen);
            config::save();
        }),
            rightPane, [](Pane& pane) { pane.clear(); });
        leftPane.register_control(leftPane.add_button("Restore Default Window Size").on_pressed([] {
            mDoAud_seStartMenu(kSoundItemChange);
            getSettings().video.enableFullscreen.setValue(false);
            VISetWindowFullscreen(false);
            VISetWindowSize(FB_WIDTH * 2, FB_HEIGHT * 2);
            VICenterWindow();
        }),
            rightPane, [](Pane& pane) { pane.clear(); });
        #endif
        config_bool_select(leftPane, rightPane, getSettings().video.enableVsync,
            {
                .key = "Enable VSync",
                #ifdef __SWITCH__
                .helpText = "Waits for the display before showing each frame (60 Hz on the Switch), so frames never tear.",
                #else
                .helpText = "Synchronizes the frame rate to your monitor's refresh rate.",
                #endif
                .onChange = [](bool value) { aurora_enable_vsync(value); },
            });
        config_bool_select(leftPane, rightPane, getSettings().video.lockAspectRatio,
            {
                .key = "Lock 4:3 Aspect Ratio",
                .helpText = "Lock the game's aspect ratio to the original. Flip Grip forces 4:3 "
                            "while it is in effect whatever this says; this setting is untouched "
                            "and applies again when you leave it.",
                .onChange =
                    [](bool) {
                        // H33: through the one place that also knows Flip Grip forces 4:3.
                        dusk::dualscreen::applyAspectPolicy();
                    },
            });
        config_bool_select(leftPane, rightPane, getSettings().game.pauseOnFocusLost,
            {
                .key = "Pause on Focus Lost",
                .helpText = "Pause the game when window focus is lost.",
                .isDisabled = [] { return IsMobile || speedrun::isActive(); },
            });
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Show FPS Counter",
                .getValue =
                    [] {
                        if (!getSettings().video.enableFpsOverlay.getValue()) {
                            return Rml::String{"Off"};
                        }
                        int idx = getSettings().video.fpsOverlayCorner.getValue();
                        if (idx < 0 || idx >= static_cast<int>(kFpsOverlayCornerNames.size())) {
                            idx = 0;
                        }
                        return Rml::String{kFpsOverlayCornerNames[idx]};
                    },
                .isModified =
                    [] {
                        const auto& enable = getSettings().video.enableFpsOverlay;
                        const auto& corner = getSettings().video.fpsOverlayCorner;
                        return enable.getValue() != enable.getDefaultValue() ||
                               (enable.getValue() && corner.getValue() != corner.getDefaultValue());
                    },
            }),
            rightPane, [](Pane& pane) {
                pane.add_button(
                        {
                            .text = "Off",
                            .isSelected =
                                [] { return !getSettings().video.enableFpsOverlay.getValue(); },
                        })
                    .on_pressed([] {
                        mDoAud_seStartMenu(kSoundItemChange);
                        getSettings().video.enableFpsOverlay.setValue(false);
                        config::save();
                    });
                for (int i = 0; i < static_cast<int>(kFpsOverlayCornerNames.size()); ++i) {
                    pane.add_button(
                            {
                                .text = kFpsOverlayCornerNames[i],
                                .isSelected =
                                    [i] {
                                        return getSettings().video.enableFpsOverlay.getValue() &&
                                               getSettings().video.fpsOverlayCorner.getValue() == i;
                                    },
                            })
                        .on_pressed([i] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            getSettings().video.enableFpsOverlay.setValue(true);
                            getSettings().video.fpsOverlayCorner.setValue(i);
                            config::save();
                        });
                }
                pane.add_rml(
                    "<br/>Display the current framerate in a corner of the screen while playing.");
            });
        #ifndef __SWITCH__
        config_bool_select(leftPane, rightPane, getSettings().video.rememberWindowSize,
            {
                .key = "Remember Window Size",
                .helpText = "Save and restore the previous session's window size when opening Dusklight.",
                .onChange =
                    [](bool value) {
                        if (value && !getSettings().video.enableFullscreen) {
                            const auto windowSize = aurora::window::get_window_size();
                            getSettings().video.lastWindowWidth.setValue(windowSize.width);
                            getSettings().video.lastWindowHeight.setValue(windowSize.height);
                            config::save();
                        }
                    },
                .isDisabled = [] { return IsMobile; },
            });
        #endif
        config_int_select(leftPane, rightPane, getSettings().video.uiScale,
            "UI Scale", 
            "Scales the Dusklight interface relative to the display's DPI scale. Has no effect on the game's UI and HUD.",
            50, 200, 25, {}, {}, "%");

        leftPane.add_section("Resolution");
        graphics_tuner_control(*this, leftPane, rightPane,
            GraphicsTunerProps{
                .option = GraphicsOption::InternalResolution,
                .title = "Internal Resolution",
                .helpText = kInternalResolutionHelpText,
            });
        graphics_tuner_control(*this, leftPane, rightPane,
            GraphicsTunerProps{
                .option = GraphicsOption::ShadowResolution,
                .title = "Shadow Resolution",
                .helpText = kShadowResolutionHelpText,
            });
        graphics_tuner_control(*this, leftPane, rightPane,
            GraphicsTunerProps{
                .option = GraphicsOption::Resampler,
                .title = "Output Resampling",
                .helpText = kResamplerHelpText,
            });

        config_int_select(leftPane, rightPane, getSettings().game.maxTextureAnisotropy,
            "Max Anisotropy",
            "Upper limit for anisotropic texture filtering. Lower values cost less GPU time; textures seen at an angle look softer.",
            1, 16, 1, {}, {}, "x");
        leftPane.add_section("Post-Processing");
        graphics_tuner_control(*this, leftPane, rightPane,
            GraphicsTunerProps{
                .option = GraphicsOption::BloomMode,
                .title = "Bloom",
                .helpText = kBloomHelpText,
            });
        graphics_tuner_control(*this, leftPane, rightPane,
            GraphicsTunerProps{
                .option = GraphicsOption::BloomMultiplier,
                .title = "Bloom Brightness",
                .helpText = kBloomBrightnessHelpText,
            });
        graphics_tuner_control(*this, leftPane, rightPane,
            GraphicsTunerProps{
                .option = GraphicsOption::DepthOfFieldMode,
                .title = "Depth of Field",
                .helpText = kDepthOfFieldHelpText,
            });

        leftPane.add_section("Rendering");
        graphics_tuner_control(*this, leftPane, rightPane,
            GraphicsTunerProps{
                .option = GraphicsOption::TextureReplacements,
                .title = "Enable Texture Replacements",
                .helpText = kTextureReplacementHelpText,
            });
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Unlock Framerate",
                .getValue =
                    [] {
                        return kInterpolationModes[static_cast<u8>(
                            getSettings().game.enableFrameInterpolation.getValue())];
                    },
                .isModified =
                    [] {
                        return getSettings().game.enableFrameInterpolation.getValue() !=
                               getSettings().game.enableFrameInterpolation.getDefaultValue();
                    },
            }),
            rightPane, [](Pane& pane) {
                for (int i = 0; i < kInterpolationModes.size(); i++) {
                    pane.add_button({
                            .text = kInterpolationModes[i],
                            .isSelected =
                                [i] {
                                    return getSettings().game.enableFrameInterpolation.getValue() == static_cast<FrameInterpMode>(i);
                                },
                        })
                        .on_pressed([i] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            getSettings().game.enableFrameInterpolation.setValue(static_cast<FrameInterpMode>(i));
                            presentation::update_frame_rate_preference();
                            config::save();
                        });
                }
                pane.add_rml(kUnlockFramerateHelpText);
            });
        config_int_select(leftPane, rightPane, getSettings().video.maxFrameRate,
            "Framerate Cap", "Limit the framerate to the specified value.", 30, 540, 1,
            [] { return getSettings().game.enableFrameInterpolation.getValue() != FrameInterpMode::Capped; },
            [](int) { presentation::update_frame_rate_preference(); });
        config_enum_select(leftPane, rightPane, getSettings().game.interpolationQuality,
            "Interpolation Quality", kInterpQualityModes, kInterpolationQualityHelpText);
        config_bool_select(leftPane, rightPane, getSettings().game.enableMapBackground,
            {
                .key = "Enable Mini-Map Shadows",
                .helpText = "Render a thick shadow around the mini-map. May impact performance."
            });
        config_bool_select(leftPane, rightPane, getSettings().game.disableCutscenePillarboxing,
            {
                .key = "Disable Cutscene Pillarboxing",
                .helpText = "Disable black bars on the left and right sides of the screen "
                            "during some cutscenes, particularly on ultra-wide displays. "
                            "Visuals beyond the original intended framing may appear buggy.",
            });
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Disable Letterboxing",
                .getValue =
                    [] {
                        return kLetterboxModes[static_cast<u8>(getSettings().game.disableLetterboxing.getValue())];
                    },
                .isModified =
                    [] {
                        return getSettings().game.disableLetterboxing.getValue() !=
                               getSettings().game.disableLetterboxing.getDefaultValue();
                    },
            }),
            rightPane, [](Pane& pane) {
                for (int i = 0; i < static_cast<int>(kLetterboxModes.size()); i++) {
                    pane.add_button({
                            .text = kLetterboxModes[i],
                            .isSelected =
                                [i] {
                                    return getSettings().game.disableLetterboxing.getValue() == static_cast<LetterboxMode>(i);
                                },
                        })
                        .on_pressed([i] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            getSettings().game.disableLetterboxing.setValue(static_cast<LetterboxMode>(i));
                            config::save();
                        });
                }
                pane.add_rml(
                    "<br/>Disable the top and bottom black bars during L-targeting, aiming, "
                    "cutscenes, dialogue, etc.");
            });
    });

#ifdef __SWITCH__
    // aleks2: everything this fork adds, in one place instead of buried at the
    // bottom of Interface. These are the settings that get changed most -- the
    // performance bundles and the companion -- and they were the hardest to
    // find. Keeping them in a tab of their own also keeps the fork's surface
    // separate from upstream's, which is worth something every time Dusklight
    // is updated.
    add_tab("Aleks", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);
#ifdef __SWITCH__
        // V1-rc8: the in-app updater (self_update.hpp). V1-rc9: first in the Aleks tab (it was in Interface, after
        // the other Dusklight settings, where testers did not find it).
        leftPane.add_section("Updates");
        config_bool_select(leftPane, rightPane, getSettings().backend.checkForUpdates,
            {
                .key = "Check for Updates at Start",
                .helpText = "Checks this port's GitHub releases for a new version when Dusklight starts, at most "
                            "once a day, and says so if there is one. Nothing is downloaded until you choose to."
                            "<br/><br/>No personal information is transmitted or collected.",
            });
        config_bool_select(leftPane, rightPane, dusk::self_update::include_betas(),
            {
                .key = "Include Beta Versions",
                .helpText = "Also offer versions GitHub marks as pre-releases (betas and release candidates).",
            });
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Dusklight Update",
                .getValue = [] { return Rml::String{dusk::self_update::status_value()}; },
            }),
            rightPane, [](Pane& pane) {
                pane.add_button({.text = "Check now"}).on_pressed([] {
                    mDoAud_seStartMenu(kSoundClick);
                    dusk::self_update::check_now();
                });
                pane.add_button({.text = "Download and install"}).on_pressed([] {
                    mDoAud_seStartMenu(kSoundClick);
                    dusk::self_update::install();
                });
                pane.add_button({.text = "Restart now"}).on_pressed([] {
                    mDoAud_seStartMenu(kSoundClick);
                    dusk::self_update::restart_now();
                });
                pane.add_rml(Rml::String{
                    "Looks for a newer version on this port's GitHub releases. One that ships its NRO is "
                    "downloaded to the SD card, checked against the SHA-256 GitHub publishes, and installed when "
                    "Dusklight closes: <b>save first</b>, then <b>Restart now</b>. The previous NRO stays next to it "
                    "as <b>.nro.bak</b> until the new one has run for a minute; if the new one does not start, "
                    "delete it and rename the .bak back. A version that needs more than the NRO is installed by "
                    "hand from the release page. Your saves, mods and shader caches are not touched."});
            });
#endif
        // aleks2 (performance): the three bundles, then the dynamic
        // resolution knobs they set, exposed so a bundle can be adjusted.
        leftPane.add_section("Performance (Aleks)");
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Optimized Settings",
                .getValue =
                    [] {
                        const int idx = detectPerfPreset();
                        return Rml::String{idx < 0 ? "Custom" : dusk::profile::name(idx)};
                    },
                .isModified = [] { return false; },
            }),
            rightPane, [](Pane& pane) {
                for (int i = 0; i < static_cast<int>(dusk::profile::kCount); ++i) {
                    pane.add_button({
                            .text = dusk::profile::name(i),
                            .isSelected = [i] { return detectPerfPreset() == i; },
                        })
                        .on_pressed([i] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            applyPerfPreset(i);
                            config::save();
                        });
                }
                pane.add_rml(
                    "<br/><b>Quality</b>: everything at its best at the game's own 30. 4x shadows, "
                    "Dusk bloom and depth of field. Auto resolution: 900p docked, 720p handheld, "
                    "540p in Flip Grip."
                    "<br/><b>Balanced</b>: Fancy interpolation capped at 60 with predictive "
                    "skipping; where 60 is out of reach it keeps interpolating below it, and only "
                    "falls back to 30 where that would look worse than 30. "
                    "2x shadows, Classic depth of field. Auto resolution: 720p docked, 720p "
                    "handheld with deko3d (540p with Vulkan), 540p in Flip Grip."
                    "<br/><b>Performance</b>: Fast interpolation capped at 60 with predictive "
                    "skipping, 1x shadows, Classic bloom, no depth of field or water refraction. "
                    "Auto resolution: 720p docked, 540p handheld."
                    "<br/><br/>Every profile runs at the console's stock clocks; GPU Boost (below) is "
                    "a separate choice."
                    "<br/><br/>These are goals, not guarantees: 60 is only reached where the "
                    "console keeps up. The render thread at stock clocks is the limit in towns, "
                    "caves and Castle Town; no profile changes that, a CPU overclock does. "
                    "Changing any of these knobs by hand shows Custom, and a resolution you "
                    "choose yourself always replaces Auto. The profile's Auto resolution "
                    "follows the dock without changing your settings.");
            });

#ifdef __SWITCH__
        // aleks2 (V1-rc16): src/dusk/gpu_boost.cpp. Opt-in, any profile; profiles never touch the clocks.
        config_bool_select(leftPane, rightPane, getSettings().game.gpuBoost,
            {
                .key = "GPU Boost (handheld)",
                .helpText = "In handheld mode, asks the console for its official 460 MHz GPU profile instead of "
                            "the usual 307 MHz. This is not an overclock -- the system offers it -- but it uses "
                            "more battery and the console runs warmer.<br/><br/>Not recommended for most players: "
                            "the profiles are designed for stock clocks. Docked play is unaffected. If a clock "
                            "tool such as sys-clk is setting the clocks, this steps aside.",
            });
#endif

        config_bool_select(leftPane, rightPane, getSettings().game.predictiveInterpolation,
            {
                .key = "Predictive Interpolation",
                .helpText = "With Unlock Framerate on, skip an interpolated frame that would not "
                            "finish before the next game tick, so heavy places give a clean 30 "
                            "instead of 33/50 ms judder. Costs nothing where 60 already fits.",
            });
        // aleks2 (V1-c1): one choice, Off or Auto. Auto moves only when the GPU is the limit
        // (dynres.hpp); the target is 30 (perf-lab.ini drs_target_fps changes it for tests).
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Dynamic Resolution",
                .getValue =
                    [] {
                        return Rml::String{getSettings().game.dynamicResolution.getValue()
                                               ? "Auto (only when the GPU is tight)"
                                               : "Off"};
                    },
                .isModified = [] { return getSettings().game.dynamicResolution.getValue(); },
            }),
            rightPane, [](Pane& pane) {
                for (const bool on : {false, true}) {
                    pane.add_button({
                            .text = on ? "Auto (only when the GPU is tight)" : "Off",
                            .isSelected = [on] { return getSettings().game.dynamicResolution.getValue() == on; },
                        })
                        .on_pressed([on] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            getSettings().game.dynamicResolution.setValue(on);
                            dusk::dynres::reset();
                            config::save();
                        });
                }
                const auto st = dusk::dynres::status();
                pane.add_rml(Rml::String{
                    "<br/><b>Off</b> (default): the resolution you chose above, always."
                    "<br/><br/><b>Auto</b>: lowers the internal resolution in a few fixed steps (85 % and 75 % "
                    "of your resolution) only when frames are being missed <b>and the GPU is the reason</b>, "
                    "measured every frame. Where the CPU or the render thread is the limit (towns, caves, "
                    "Castle Town at stock clocks) it leaves the resolution alone, because fewer pixels would "
                    "not buy frames there. It waits a few seconds before each step, never changes during "
                    "loads or shader compiles, and climbs back when there is room. The aim is 30 FPS."
                    "<br/><br/>Experimental: the step-change crash seen in earlier builds is what the "
                    "<b>DRS stress test</b> in Diagnostics checks. Keep it Off unless you are testing."
                    "<br/><br/>Now: " + dusk::ui::escape(st.state) + " (" + std::to_string(st.rows) + " of " +
                    std::to_string(st.ceilingRows) + " rows)."});
            });

        // aleks2 (companion): the second screen composited on the panel.
        leftPane.add_section("Companion (Aleks)");
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Dual Screen HUD",
                .getValue =
                    [] {
                        if (!getSettings().game.dualScreen.getValue()) {
                            return Rml::String{"Off"};
                        }
                        int idx = getSettings().game.dualScreenHudMode.getValue();
                        if (idx < 0 || idx >= static_cast<int>(kDualScreenHudModeNames.size())) {
                            idx = dusk::kDualHudFunctional;
                        }
                        return Rml::String{kDualScreenHudModeNames[idx]};
                    },
                .isModified = [] { return false; },
            }),
            rightPane, [](Pane& pane) {
                pane.add_button(
                        {
                            .text = "Off",
                            .isSelected = [] { return !getSettings().game.dualScreen.getValue(); },
                        })
                    .on_pressed([] {
                        mDoAud_seStartMenu(kSoundItemChange);
                        getSettings().game.dualScreen.setValue(false);
                        applyDualScreenSideEffects(false);
                        config::save();
                    });
                for (int i = 0; i < static_cast<int>(kDualScreenHudModeNames.size()); ++i) {
                    pane.add_button(
                            {
                                .text = kDualScreenHudModeNames[i],
                                .isSelected =
                                    [i] {
                                        return getSettings().game.dualScreen.getValue() &&
                                               getSettings().game.dualScreenHudMode.getValue() == i;
                                    },
                            })
                        .on_pressed([i] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            const bool wasOn = getSettings().game.dualScreen.getValue();
                            getSettings().game.dualScreen.setValue(true);
                            getSettings().game.dualScreenHudMode.setValue(i);
                            // Only on the Off -> On edge: a plain mode switch
                            // must not re-force the FPS corner or Minimal HUD.
                            if (!wasOn) {
                                applyDualScreenSideEffects(true);
                                dusk::deko3d_policy::on_dual_screen_enabled();  // aleks2 (deko3d rc12)
                            }
                            dusk::dualscreen::requestRedraw();
                            config::save();
                        });
                }
                pane.add_rml(
                    "<br/>Shows the HUD, map and menus on a second screen. "
                    "Wii U Style moves the whole HUD there, leaving the game view clear. "
                    "3DS Style keeps the hearts, the Vessel of Light and A/B on the main screen; X/Y/Z, "
                    "Midna, the d-pad, rupees, keys and the magic meter move to the second screen, which "
                    "becomes a touch control surface."
                    "<br/>Enabling moves the FPS counter to the companion screen and turns "
                    "off Minimal HUD.");
            });

        // The panel split. Sits under the style row because the two answer
        // different questions -- that one is which HUD goes where, this one is
        // how the single physical screen is divided.
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Screen Layout",
                .getValue =
                    [] {
                        if (!getSettings().game.dualScreen.getValue()) {
                            return Rml::String{"Off"};
                        }
                        int idx = getSettings().game.dualScreenLayout.getValue();
                        if (idx < 0 || idx >= static_cast<int>(kDualScreenLayoutNames.size())) {
                            idx = dusk::kDualLayoutFlip;
                        }
                        return Rml::String{kDualScreenLayoutNames[idx]};
                    },
                .isModified = [] { return false; },
            }),
            rightPane, [](Pane& pane) {
                for (int i = 0; i < static_cast<int>(kDualScreenLayoutNames.size()); ++i) {
                    pane.add_button({
                            .text = kDualScreenLayoutNames[i],
                            .isSelected =
                                [i] {
                                    return getSettings().game.dualScreen.getValue() &&
                                           getSettings().game.dualScreenLayout.getValue() == i;
                                },
                        })
                        .on_pressed([i] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            getSettings().game.dualScreenLayout.setValue(i);
                            // Flip Grip is built around TP's native 4:3 picture (720x540 on
                            // the portrait canvas); 16:9 would only shrink the game to
                            // 720x405. H33: it forces 4:3 while it is in effect (see
                            // dualscreen::effectiveAspectLock) and never writes the saved
                            // Lock Aspect Ratio preference, so leaving it restores that.
                            dusk::dualscreen::applyAspectPolicy();
                            dusk::dualscreen::requestRedraw();
                            config::save();
                        });
                }
                pane.add_rml(
                    "<br/><b>Flip Grip</b> stacks the game above the companion on a portrait "
                    "canvas, so the console has to be held turned -- a Flip Grip accessory, or "
                    "on its side. The game gets 720x540 (4:3: Flip Grip forces it while it is in "
                    "effect and hands your Lock Aspect Ratio setting back when you leave) "
                    "and the companion the rest, both touchable."
                    "<br/><b>Normal</b> leaves the game full screen and summons the companion "
                    "over it with ZL + R3."
                    "<br/><br/>Flip Grip is handheld only: a docked console cannot be turned, "
                    "so it falls back to Normal on the dock. Docked also has no touchscreen.");
            });

        config_int_select(leftPane, rightPane, getSettings().game.dualScreenRedrawDivisor,
            "Companion Refresh",
            "How often the second screen is redrawn, in game frames. <b>1</b> redraws it "
            "every frame; <b>3</b> redraws it every third."
            "<br/><br/>This is the main frame-rate cost of the second screen. The companion "
            "is a status display, so at 30 fps a value of 3 still updates it ten times a "
            "second while doing a third of the drawing work."
            "<br/><br/>Touching the screen always redraws it at once, whatever this is set "
            "to -- taps, drags and page changes never wait for the cadence.",
            1, 12, 1,
            [] { return !getSettings().game.dualScreen.getValue(); },
            [](int) { dusk::dualscreen::requestRedraw(); },
            " frames");

        config_int_select(leftPane, rightPane, getSettings().game.dualScreenResolution,
            "Companion Resolution",
            "How large the second screen is rendered, in pixels on its longest side. "
            "<b>720</b> matches the panel exactly; <b>640</b> is the default."
            "<br/><br/>The shape never changes -- only how finely it is drawn, and the "
            "final blit smooths the difference. Lower this before lowering the game's own "
            "resolution: the companion is flat 2D and text.",
            320, 720, 40,
            [] { return !getSettings().game.dualScreen.getValue(); },
            [](int) { dusk::dualscreen::requestRedraw(); },
            " px");

        config_bool_select(leftPane, rightPane, getSettings().game.guideEnabled,
            {
                .key = "Guide Reader",
                .helpText = "Shows the guide reader page on the companion.",
            });

        // aleks2 (native mods): the one question standing between compiled-in
        // mods and loaded ones. It lives behind a button rather than running at
        // startup because a page the kernel refuses to execute kills the
        // process, and a boot must never be able to do that.
        // V1-rc5: what a player needs about mods; the diagnostics further down only in developer mode
        // (sdmc:/switch/dusklight/dev.txt, docs/investigaciones/v1-limpieza-release.md).
        if (dusk::dev_mode())  // V1-rc8: developer mode only
        config_int_select(leftPane, rightPane, getSettings().game.minimapRedrawDivisor, "Minimap Refresh",
            "How often the field minimap is redrawn, in game frames. <b>1</b> is every frame, as the game does."
            "<br/><br/>With the full HUD the minimap redraws its whole picture every frame, about 800 draws in "
            "Hyrule Field and Castle Town: the main cost there. <b>2</b> halves it. Experimental: if the map "
            "flickers or goes blank, set it back to 1. Minimal HUD hides the map and has no such cost.",
            1, 3, 1, [] { return false; }, [](int) {}, " frames");

        leftPane.add_section("Mods (Aleks)");
#if ALEKS2_NATIVE_MOD_LOADER
        // aleks2 (H37): native mods from the card (switch_native_loader.hpp). The Loader Mode is
        // persisted and takes effect for mods loaded after the change -- at the next start, or on
        // a mod's reload.
        static constexpr std::array<dusk::switchelf::LoaderMode, 3> kLoaderModes{
            dusk::switchelf::LoaderMode::Off, dusk::switchelf::LoaderMode::DryRun,
            dusk::switchelf::LoaderMode::Execute};
        leftPane.register_control(
            leftPane.add_select_button({
                // V1-rc5: for the player there is no mode (always Execute); the selector is a developer-mode
                // diagnostic (dev.txt). The status below (crashes, quarantine, hook room) is for everyone.
                .key = dusk::dev_mode() ? "Native Mods: Loader Mode" : "Native Mods",
                .getValue =
                    [] {
                        return dusk::dev_mode()
                                   ? Rml::String{dusk::switchelf::loader_mode_name(dusk::switchelf::loader_mode())}
                                   : Rml::String{"Status"};
                    },
                .isModified =
                    [] {
                        return dusk::dev_mode() &&
                               dusk::switchelf::loader_mode() != dusk::switchelf::LoaderMode::Execute;
                    },
            }),
            rightPane, [](Pane& pane) {
                if (!dusk::dev_mode()) {
                    pane.add_rml(Rml::String{
                        "Mods whose code comes from the card run as soon as they are installed and turned on. "
                        "A mod that crashes the game while starting is turned off at the next start and says "
                        "why; turn it off and on again to give it another try."});
                } else {
                for (const auto mode : kLoaderModes) {
                    pane.add_button({
                                  .text = Rml::String{dusk::switchelf::loader_mode_name(mode)},
                                  .isSelected = [mode] { return dusk::switchelf::loader_mode() == mode; },
                              })
                        .on_pressed([mode] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            dusk::switchelf::set_loader_mode(mode);
                        });
                }
                // V1-rc4: the mode applies to mods loaded after the change; restart from here to apply it now.
                pane.add_button({.text = "Restart now"}).on_pressed([] {
                    mDoAud_seStartMenu(kSoundClick);
                    if (aleks2_relaunch_on_exit()) {
                        IsRunning = false;
                    } else {
                        dusk::ui::push_toast({.title = "Restart by hand",
                            .content = "Quit Dusklight and open it again to apply the change.",
                            .duration = std::chrono::seconds(6)});
                    }
                });
                pane.add_rml(Rml::String{
                    "How mods whose code comes from the card (not built into this NRO) are loaded."
                    "<br/><br/><b>Off</b>: not at all. <b>Dry Run</b>: each library is checked "
                    "against this build -- format, relocations, every symbol it needs -- and a report is "
                    "written to sdmc:/switch/dusklight/mods/.reports/; no mod code runs. A diagnostic: native "
                    "mods do not run in it. <b>Execute</b> (the default): the same checks, then the mod is "
                    "loaded and runs. A Dry Run saved by an earlier test build is moved to Execute once."
                    "<br/><br/>Takes effect at the next start: <b>Restart now</b> applies it. A mod that "
                    "crashes the game while starting is disabled at the next boot and says why."});
                }
                const auto& start = dusk::switchelf::startup();
                std::string status;
                if (start.withoutMods) {
                    status += "<br/><br/><b>This boot started without mods:</b> " + dusk::ui::escape(start.withoutModsReason);
                }
                if (start.crashed) {
                    status += "<br/><br/><b>The previous boot crashed in " + dusk::ui::escape(start.crashed->modId) +
                              "</b> (" + dusk::ui::escape(start.crashed->stage) + "); it stays disabled until you "
                              "turn it off and on again.";
                }
                if (auto* bootGuard = dusk::switchelf::boot_guard()) {
                    const auto quarantined = bootGuard->quarantine();
                    if (!quarantined.empty()) {
                        status += "<br/><br/>Disabled after a crash or a failure while playing:";
                        for (const auto& id : quarantined) {
                            status += " " + dusk::ui::escape(id);
                        }
                    }
                }
#if DUSK_HAS_FUNCHOOK
                if (dusk::dev_mode()) {  // 1.0.0: loader internals, developer mode only
                    char arenas[512];
                    funchook_switch_describe(arenas, sizeof arenas);
                    uint32_t hookUsed = 0, hookCapacity = 0;
                    funchook_switch_usage(&hookUsed, nullptr, &hookCapacity);
                    status += "<br/><br/><b>Hook room: " + std::to_string(hookUsed) + " of " +
                              std::to_string(hookCapacity) + " used</b> (one per hooked function)";
                    status += "<br/>Hooks: " + dusk::ui::escape(arenas);
                }
#endif
                pane.add_rml(Rml::String{status});
            });
        leftPane.register_control(
            leftPane.add_group_button({.text = "Next Start Without Mods"}).on_pressed([] {
                mDoAud_seStartMenu(kSoundItemChange);
                if (auto* bootGuard = dusk::switchelf::boot_guard()) {
                    bootGuard->request_boot_without_mods();
                }
            }),
            rightPane, [](Pane& pane) {
                pane.clear();
                pane.add_rml(Rml::String{
                    "The next start loads no mods at all, once. Nothing is deleted: your mods, their "
                    "settings and your saves stay as they are, and the start after that loads mods again. "
                    "Use it when a mod keeps the game from starting.<br/><br/>Without this menu: create an "
                    "empty file named <b>start_without_mods</b> in sdmc:/switch/dusklight/mods/.guard/."});
            });
#ifdef __SWITCH__
        // aleks2 (DM3): a mod's local server (Item Tracker's page, an OBS overlay) listens on 127.0.0.1, which on
        // a console nothing else can reach. Opt-in, off by default: such a listener is opened on the local
        // network instead (src/dusk/switch_net.cpp, svc/net.cpp, loader/glibc_net.cpp).
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Mods: Listen on the Local Network",
                .getValue = [] { return Rml::String{dusk::switchnet::lan_listen_allowed() ? "On" : "Off"}; },
                .isModified = [] { return dusk::switchnet::lan_listen_allowed(); },
            }),
            rightPane, [](Pane& pane) {
                for (const bool on : {false, true}) {
                    pane.add_button({
                                  .text = on ? "On" : "Off",
                                  .isSelected = [on] { return dusk::switchnet::lan_listen_allowed() == on; },
                              })
                        .on_pressed([on] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            dusk::switchnet::set_lan_listen_allowed(on);
                        });
                }
                dusk::switchnet::IpConfig ip;
                std::string where = "this console's address (Settings > Internet on the console)";
                if (dusk::switchnet::ip_config(ip)) {
                    const auto* b = reinterpret_cast<const uint8_t*>(&ip.address);
                    where = "<b>" + std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) +
                            "." + std::to_string(b[3]) + "</b>";
                }
                pane.add_rml(Rml::String{
                    "Some mods run a small server for a browser or OBS on the same computer (Item Tracker's page, "
                    "overlays). On a console there is no browser next to the game, so by default those servers are "
                    "useless here."
                    "<br/><br/><b>Off</b> (default): mods can only listen on the console itself."
                    "<br/><br/><b>On</b>: such a server is opened on your local network instead, so a PC or phone on "
                    "the same Wi-Fi can open it at " + where + " and the port the mod shows. Anyone on that network "
                    "can reach it too: turn it on at home, not on public Wi-Fi."
                    "<br/><br/>Applies to servers a mod opens after the change (restart the mod or the game)."});
            });
#endif
#endif
        if (dusk::dev_mode()) {
        leftPane.add_section("Diagnostics (Aleks, developer mode)");
        // The right pane is normally filled when the row is focused, so after
        // the test runs it would still be showing the answer from before it.
        // Same lambda, called again by hand.
        auto describeJit = [](Pane& pane) {
            pane.clear();
            pane.add_text(Rml::String{"Status: "} + dusk::switchjit::summary());
            pane.add_rml(
                "<br/>Horizon has no mmap(PROT_EXEC) and no mprotect, so code that is not in the "
                "NRO when it is built has nowhere legal to run. What the console does have is the "
                "JIT mechanism, and whether a homebrew launcher hands it to us is the console's "
                "decision, not ours."
                "<br/><br/>Pressing this writes four instructions into a JIT page, asks the "
                "kernel to make it executable and calls it. Passing is what lets a mod be "
                "<i>loaded</i> from the card instead of compiled in."
                "<br/><br/><b>It can crash the game.</b> Save first.");
        };
        // The other half, and a different question: not whether we can make new
        // code, but whether we can change the code we already have. That is what
        // a hook needs, and JIT memory does not provide it.
        auto describePatch = [](Pane& pane) {
            pane.clear();
            pane.add_text(Rml::String{"Own code: "} + dusk::switchjit::patch_summary());
            pane.add_rml(
                "<br/>A hook is installed by writing a branch over the first instruction of the "
                "function being hooked, and that function is in the NRO's own code, which the "
                "kernel maps read-only. Making it writable needs a privileged syscall the "
                "launcher may not have given us."
                "<br/><br/>This only reports, and cannot crash: it asks whether the syscall is "
                "permitted and what the kernel says our code pages are. It does <b>not</b> try "
                "the write, because a wrong guess here takes the process with it and the crash "
                "would not say whether the kernel refused us or whether we shot ourselves."
                "<br/><br/>The size shown is the whole code segment, which is what the kernel "
                "reports for the page it was asked about -- not how much would change at once. "
                "The permission call takes any page-aligned range."
                "<br/><br/>If this says no, hooks need gateways built in at compile time "
                "instead, which is a different piece of work.");
        };
        leftPane.register_control(
            leftPane.add_group_button({.text = "Executable Memory"})
                .on_pressed([&rightPane, describeJit] {
                    mDoAud_seStartMenu(kSoundItemChange);
                    dusk::switchjit::run_self_test();
                    describeJit(rightPane);
                }),
            rightPane, describeJit);
        leftPane.register_control(
            leftPane.add_group_button({.text = "Patch Own Code"}), rightPane, describePatch);

        // aleks2 (H31): a diagnostic switch, here and not under graphics because it
        // changes nothing on screen -- it only decides whether the draw path
        // remembers pipeline lookups. Turn it off, run the benchmark, turn it on.
        config_bool_select(leftPane, rightPane, getSettings().game.pipelineMemo,
            {
                .key = "Pipeline Memo (H31)",
                .helpText = "Diagnostic. Replays the pipeline a draw resolved before instead of "
                            "looking it up again. <b>Off is exactly H30.</b> Output is identical; "
                            "only CPU time changes. Safe to flip while playing; the benchmark "
                            "report records which one it ran with.",
            });

        // aleks2 (H35): uniform block reuse, the same kind of A/B switch as the memo above, but
        // runtime only: it is never written to the configuration and every launch starts Off.
        static constexpr std::array<const char*, 3> kUniformReuseNames{"Off", "Verify", "On"};
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Uniform Reuse (H35)",
                .getValue =
                    [] {
                        const auto mode = static_cast<std::size_t>(aurora_gx_get_uniform_reuse_mode());
                        return Rml::String{mode < kUniformReuseNames.size() ? kUniformReuseNames[mode] : "?"};
                    },
                .isModified = [] { return aurora_gx_get_uniform_reuse_mode() != AURORA_UNIFORM_REUSE_OFF; },
            }),
            rightPane, [](Pane& pane) {
                for (std::size_t i = 0; i < kUniformReuseNames.size(); ++i) {
                    pane.add_button({
                                  .text = kUniformReuseNames[i],
                                  .isSelected =
                                      [i] { return static_cast<std::size_t>(aurora_gx_get_uniform_reuse_mode()) == i; },
                              })
                        .on_pressed([i] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            aurora_gx_set_uniform_reuse_mode(static_cast<AuroraUniformReuseMode>(i));
                        });
                }
                AuroraUniformStats stats{};
                aurora_gx_uniform_stats(&stats);
                pane.add_rml(Rml::String{
                    "Diagnostic. When only the pipeline changes between two draws, keeps the uniform block "
                    "of the previous draw instead of filling it again, if it is provably identical."
                    "<br/><br/><b>Off</b> is exactly H34. <b>Verify</b> decides as On would, but still fills "
                    "every block and compares the two byte for byte; the full block is always the one drawn. "
                    "A difference is logged and disables reuse until the game is restarted. <b>On</b> applies "
                    "the reuse. Safe to change while playing; not saved, every launch starts Off. The "
                    "benchmark and the Performance Tour record the mode they ran with."} +
                    fmt::format("<br/><br/>Since launch: {} full blocks, {} repeats, {} candidates, {} reused, "
                                "{} verified, <b>{} mismatches</b>.{}",
                        stats.full_builds, stats.repeat_hits, stats.reuse_candidates, stats.reuses,
                        stats.verification_checks, stats.verification_mismatches,
                        stats.disabled_after_mismatch != 0 ? " <b>Reuse is disabled after a mismatch.</b>" : ""));
            });


        // aleks2 (H28): the short benchmark. The right pane is also where the
        // reason it cannot start right now is shown, so a greyed-out button is
        // never a mystery.
        auto describeBenchmark = [](Pane& pane) {
            pane.clear();
            std::string why;
            const bool ready = dusk::benchmark::can_start(why);
            pane.add_text(ready ? "Ready." : Rml::String{why});
            pane.add_rml(Rml::String{
                "<br/>Moves you to a heavy, known scene (Hyrule Field outside Castle Town, west), lets it "
                "settle for " + std::to_string(dusk::benchmark::kWarmupSeconds) +
                " s, waits for shader compilation to finish, then measures for " +
                std::to_string(dusk::benchmark::kMeasureSeconds) +
                " s and writes a JSON report."
                "<br/><br/>Controls are locked while it runs; <b>press Start</b> to cancel. Nothing is written "
                "to your save and no setting is changed. Afterwards you are returned to where you were."
                "<br/><br/>It measures the settings you have right now, on the clocks the console has right "
                "now. Run it twice: the second run, with shaders already compiled, is the one to compare."
                "<br/><br/>Reports: <b>" + dusk::benchmark::report_directory() + "</b>"});
            const auto& last = dusk::benchmark::last_summary();
            if (!last.empty()) {
                pane.add_rml("<br/><br/><b>Last result</b><br/>" + dusk::ui::escape(last));
            }
        };
        leftPane.register_control(
            leftPane.add_group_button({
                         .text = "Run Benchmark",
                         .isDisabled =
                             [] {
                                 std::string why;
                                 return !dusk::benchmark::can_start(why);
                             },
                     })
                .on_pressed([] {
                    std::string error;
                    if (dusk::benchmark::start(error)) {
                        mDoAud_seStartMenu(kSoundPlay);
                    } else {
                        mDoAud_seStartMenu(kSoundWarning);
                        push_toast({.title = "Benchmark not started",
                            .content = Rml::String{error},
                            .duration = std::chrono::seconds(4)});
                    }
                }),
            rightPane, describeBenchmark);

        // aleks2 (H33): the three profiles in one go, for the console mode you are in. Separate
        // from the benchmark above, which changes no setting; this one applies each profile as a
        // temporary override that is never written to your configuration.
        auto describeMatrix = [](Pane& pane) {
            pane.clear();
            std::string why;
            const bool ready = dusk::benchmark::can_start_matrix(why);
            pane.add_text(ready ? "Ready." : Rml::String{why});
            pane.add_rml(Rml::String{
                "<br/>Runs the benchmark three times, once with each performance profile (Quality, "
                "Balanced, Performance), and writes one report per profile. About 5 minutes."
                "<br/><br/>The profiles are applied as <b>temporary overrides</b>: nothing is saved to your "
                "configuration, your own settings are given back when it finishes, is cancelled (press "
                "<b>Start</b>), or if you dock or undock the console, and they would not survive a crash "
                "anyway. Controls are locked and nothing is written to your save."
                "<br/><br/>It needs your settings to have been saved once (choose a profile under Optimized "
                "Settings first) so that they can be given back exactly. Run it docked and again handheld to "
                "compare the two."
                "<br/><br/>Reports: <b>" + dusk::benchmark::report_directory() + "</b>"});
        };
        leftPane.register_control(
            leftPane.add_group_button({
                         .text = "Run Profile Matrix",
                         .isDisabled =
                             [] {
                                 std::string why;
                                 return !dusk::benchmark::can_start_matrix(why);
                             },
                     })
                .on_pressed([] {
                    std::string error;
                    if (dusk::benchmark::start_matrix(error)) {
                        mDoAud_seStartMenu(kSoundPlay);
                    } else {
                        mDoAud_seStartMenu(kSoundWarning);
                        push_toast({.title = "Profile matrix not started",
                            .content = Rml::String{error},
                            .duration = std::chrono::seconds(6)});
                    }
                }),
            rightPane, describeMatrix);

        // aleks2 (H34): shader preparation. Not a benchmark: it visits a few places so their
        // shaders are compiled now and kept in the console's own cache, instead of stuttering
        // the first time they appear in play.
        auto describePrep = [](Pane& pane) {
            pane.clear();
            std::string why;
            const bool ready = dusk::benchmark::can_start_prep(why);
            pane.add_text(ready ? "Ready." : Rml::String{why});
            pane.add_rml(Rml::String{
                "<br/>Visits six places (Hyrule Field, Castle Town, Ordon Village, Link's House, Faron "
                "Woods and the Forest Temple) and waits in each until its shaders have been compiled. "
                "They are kept in this console's shader cache, so those places do not stutter the first "
                "time they appear in play. Up to about 15 minutes the first time; much less once the "
                "cache is warm, because a place with nothing new to compile is left after a few seconds."
                "<br/><br/>This is <b>not a benchmark</b>: it measures no frame rate. Controls are locked; "
                "<b>press Start</b> to cancel at any time. Nothing is written to your save, no setting is "
                "changed, and you are returned to where you were."
                "<br/><br/>Report: <b>" + dusk::benchmark::report_directory() + "</b>"});
        };
        leftPane.register_control(
            leftPane.add_group_button({
                         .text = "Prepare Shaders",
                         .isDisabled =
                             [] {
                                 std::string why;
                                 return !dusk::benchmark::can_start_prep(why);
                             },
                     })
                .on_pressed([] {
                    std::string error;
                    if (dusk::benchmark::start_prep(error)) {
                        mDoAud_seStartMenu(kSoundPlay);
                    } else {
                        mDoAud_seStartMenu(kSoundWarning);
                        push_toast({.title = "Shader preparation not started",
                            .content = Rml::String{error},
                            .duration = std::chrono::seconds(4)});
                    }
                }),
            rightPane, describePrep);

        // aleks2 (H35): the performance tour. Three places, heavy to light, each measured on its
        // own with the settings you have now; one report.
        auto describeTour = [](Pane& pane) {
            pane.clear();
            std::string why;
            const bool ready = dusk::benchmark::can_start_tour(why);
            pane.add_text(ready ? "Ready." : Rml::String{why});
            pane.add_rml(Rml::String{
                "<br/>Measures three places one after the other, from heavy to light: Outside Castle Town "
                "(west), Ordon Village and Link's House. In each one it waits for the place to load and for "
                "its shaders, warms up for a few seconds and measures for about half a minute. About 3 to 5 "
                "minutes in all."
                "<br/><br/>It uses your current settings (profile, resolution, second screen, Flip Grip, "
                "interpolation and the Uniform Reuse mode above) and changes none of them. The report records "
                "each place separately, including whether interpolation gave up in the heavy place and came "
                "back in the lighter ones."
                "<br/><br/>Controls are locked; <b>press Start</b> to cancel. Nothing is written to your save, "
                "achievements are paused, and you are returned to where you were."
                "<br/><br/>Report: <b>" + dusk::benchmark::report_directory() + "</b>"});
            const auto& last = dusk::benchmark::last_summary();
            if (!last.empty()) {
                pane.add_rml("<br/><br/><b>Last result</b><br/>" + dusk::ui::escape(last));
            }
        };
        leftPane.register_control(
            leftPane.add_group_button({
                         .text = "Run Performance Tour",
                         .isDisabled =
                             [] {
                                 std::string why;
                                 return !dusk::benchmark::can_start_tour(why);
                             },
                     })
                .on_pressed([] {
                    std::string error;
                    if (dusk::benchmark::start_tour(error)) {
                        mDoAud_seStartMenu(kSoundPlay);
                    } else {
                        mDoAud_seStartMenu(kSoundWarning);
                        push_toast({.title = "Performance tour not started",
                            .content = Rml::String{error},
                            .duration = std::chrono::seconds(4)});
                    }
                }),
            rightPane, describeTour);

        // aleks2 (V1-rc12): Check Tour -- fixed spots, a screenshot and a heartbeat at each (director.hpp).
        auto describeCheck = [](Pane& pane) {
            pane.clear();
            std::string why;
            const bool ready = dusk::director::can_start_check(why);
            pane.add_text(ready ? "Ready." : Rml::String{why});
            pane.add_rml("<br/>" + dusk::ui::escape(dusk::director::check_description()) +
                         "<br/><br/>Controls are locked; <b>press Start</b> to cancel. Nothing is written to your "
                         "save, achievements are paused, and you are returned to where you were.");
        };
        leftPane.register_control(
            leftPane.add_group_button({
                         .text = "Run Check Tour",
                         .isDisabled =
                             [] {
                                 std::string why;
                                 return !dusk::director::can_start_check(why);
                             },
                     })
                .on_pressed([] {
                    std::string error;
                    if (dusk::director::start_check(error)) {
                        mDoAud_seStartMenu(kSoundPlay);
                    } else {
                        mDoAud_seStartMenu(kSoundWarning);
                        push_toast({.title = "Check tour not started",
                            .content = Rml::String{error},
                            .duration = std::chrono::seconds(4)});
                    }
                }),
            rightPane, describeCheck);

        // aleks2 (trailer): every take of director.txt, one after the other (director.hpp). A dev tool: the entry
        // exists only with developer mode and a director.txt on the card.
        if (dusk::director::available()) {
            auto describeReel = [](Pane& pane) {
                pane.clear();
                std::string why;
                const bool ready = dusk::director::can_start_reel(why);
                pane.add_text(ready ? "Ready." : Rml::String{why});
                pane.add_rml("<br/>" + dusk::ui::escape(dusk::director::reel_description()) +
                             "<br/><br/><b>Press Start</b> to stop. Nothing is written to your save, achievements are "
                             "paused, and you are returned to where you were.");
            };
            leftPane.register_control(
                leftPane.add_group_button({
                             .text = "Play trailer takes",
                             .isDisabled =
                                 [] {
                                     std::string why;
                                     return !dusk::director::can_start_reel(why);
                                 },
                         })
                    .on_pressed([] {
                        std::string error;
                        if (dusk::director::start_reel(error)) {
                            mDoAud_seStartMenu(kSoundPlay);
                        } else {
                            mDoAud_seStartMenu(kSoundWarning);
                            push_toast({.title = "Trailer takes not started",
                                .content = Rml::String{error},
                                .duration = std::chrono::seconds(4)});
                        }
                    }),
                rightPane, describeReel);
        }

        // aleks2 (V1-c1): many resolution changes in a row, memory and render targets logged after
        // each one (drs_stress.hpp). The same entry stops it.
        auto describeDrsStress = [](Pane& pane) {
            pane.clear();
            std::string why;
            const bool running = dusk::drs_stress::active();
            const bool ready = running || dusk::drs_stress::can_start(why);
            pane.add_text(running ? "Running. Choose it again to stop." : ready ? "Ready." : Rml::String{why});
            const auto& lab = dusk::frame_telemetry::lab();
            pane.add_rml(Rml::String{
                "<br/>Changes the internal resolution " + std::to_string(lab.stressTransitions) + " times, one "
                "change every " + std::to_string(lab.stressIntervalMs) + " ms, between your resolution and the "
                "Dynamic Resolution steps, the way Auto would, and writes a line after every change: memory, "
                "render targets created or reused, the worst frame. A crash still leaves every line up to it."
                "<br/><br/>Play normally or stand still; do not dock or undock (that stops it). Your settings "
                "are not changed and your resolution comes back at the end."
                "<br/><br/>Files: <b>" + dusk::benchmark::report_directory() + "/drs-stress-*.csv</b> and "
                ".json. Count and pace: perf-lab.ini (drs_stress_transitions, drs_stress_interval_ms, "
                "drs_stress_pattern=ladder for 900/720/540)."});
        };
        leftPane.register_control(
            leftPane.add_group_button({
                         .text = "Run DRS Stress",
                         .isDisabled =
                             [] {
                                 std::string why;
                                 return !dusk::drs_stress::active() && !dusk::drs_stress::can_start(why);
                             },
                     })
                .on_pressed([] {
                    if (dusk::drs_stress::active()) {
                        dusk::drs_stress::request_cancel("stopped from Diagnostics");
                        mDoAud_seStartMenu(kSoundPlay);
                        return;
                    }
                    std::string error;
                    if (dusk::drs_stress::start(error)) {
                        mDoAud_seStartMenu(kSoundPlay);
                    } else {
                        mDoAud_seStartMenu(kSoundWarning);
                        push_toast({.title = "DRS stress not started",
                            .content = Rml::String{error},
                            .duration = std::chrono::seconds(4)});
                    }
                }),
            rightPane, describeDrsStress);
        // V1-rc5: normal starts warm only the current scene's shaders; this queues the whole list.
        leftPane.register_control(leftPane.add_group_button({.text = "Warm All Shaders"}).on_pressed([] {
            aurora_warm_all_shaders();
            mDoAud_seStartMenu(kSoundPlay);
            push_toast({.title = "Warming every shader",
                .content = "The whole list is queued in the background.",
                .duration = std::chrono::seconds(3)});
        }),
            rightPane, [](Pane& pane) {
                pane.add_text("Queues every known shader in the background, as starts did before rc5. For "
                              "comparing hitches against the per-scene warmup.");
            });
        }  // dusk::dev_mode()
    });
#endif

    add_tab("Input", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        auto addOption = [&](const Rml::String& key, ConfigVar<bool>& value,
                             const Rml::String& helpText, std::function<bool()> isDisabled = {}) {
            config_bool_select(leftPane, rightPane, value,
                {
                    .key = key,
                    .helpText = helpText,
                    .isDisabled = std::move(isDisabled),
                });
        };

        leftPane.add_section("Inputs");
        leftPane.register_control(
            leftPane.add_group_button({.text = "Configure Inputs"}).on_pressed([this] {
                push(std::make_unique<ControllerConfigWindow>());
            }),
            rightPane, [](Pane& pane) {
                pane.clear();
                pane.add_text("Open input binding configuration.");
            });
        #ifndef __SWITCH__
        config_bool_select(leftPane, rightPane, getSettings().game.allowBackgroundInput,
            {
                .key = "Allow Background Inputs",
                .helpText = "Allow inputs even when the game window is not focused.",
                .onChange = [](bool value) { aurora_set_background_input(value); },
            });
        #endif

#if TOUCH_CONTROLS_AVAILABLE
        leftPane.add_section("Touch");
        addOption("Touch Controls", getSettings().game.enableTouchControls,
            "Enables controls overlay for touch screens.<br/><br/>Press and drag on the left side "
            "of the screen to move, and on the right side of the screen to control the camera.");
        auto& customizeTouchLayout = leftPane.add_group_button(GroupButton::Props{
            .text = "Customize Layout",
            .isDisabled = [] { return !getSettings().game.enableTouchControls; },
        });
        leftPane.register_control(customizeTouchLayout.on_pressed(
                                      [this] { push(std::make_unique<TouchControlsEditor>()); }),
            rightPane, [](Pane& pane) {
                pane.clear();
                pane.add_text("Open the touch controls layout editor.");
            });
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Touch Targeting",
                .getValue =
                    [] {
                        return touch_targeting_label(getSettings().game.touchTargeting.getValue());
                    },
                .isDisabled = [] { return !getSettings().game.enableTouchControls; },
                .isModified =
                    [] {
                        const auto& targeting = getSettings().game.touchTargeting;
                        return targeting.getValue() != targeting.getDefaultValue();
                    },
            }),
            rightPane, [](Pane& pane) {
                pane.clear();
                for (int i = 0; i < static_cast<int>(kTouchTargetingLabels.size()); ++i) {
                    pane.add_button({
                            .text = kTouchTargetingLabels[i],
                            .isSelected =
                                [i] {
                                    return getSettings().game.touchTargeting.getValue() ==
                                           static_cast<TouchTargeting>(i);
                                },
                        })
                        .on_pressed([i] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            getSettings().game.touchTargeting.setValue(
                                static_cast<TouchTargeting>(i));
                            config::save();
                        });
                }
                pane.add_rml(fmt::format("<br/>Hybrid: {}<br/>Hold: {}<br/>Switch: {}",
                    kTouchTargetingDescriptions[0], kTouchTargetingDescriptions[1],
                    kTouchTargetingDescriptions[2]));
            });
        config_percent_select(leftPane, rightPane, getSettings().game.touchCameraXSensitivity,
            "Touch Camera X Sensitivity",
            "Adjusts touch camera horizontal sensitivity.<br/><br/>Applies to touch input only.",
            25, 400, 5, [] { return !getSettings().game.enableTouchControls; });
        config_percent_select(leftPane, rightPane, getSettings().game.touchCameraYSensitivity,
            "Touch Camera Y Sensitivity",
            "Adjusts touch camera vertical sensitivity.<br/><br/>Applies to touch input only.", 25,
            400, 5, [] { return !getSettings().game.enableTouchControls; });
#endif

        leftPane.add_section("Camera");
        addOption("Free Camera", getSettings().game.freeCamera,
            "Enables free camera control, letting you control the camera fully with the C-Stick.");
        config_percent_select(leftPane, rightPane, getSettings().game.freeCameraXSensitivity,
            "Free Camera X Sensitivity",
            "Adjusts horizontal free camera sensitivity.<br/><br/>Applies to the control stick only.",
            50, 200, 5, [] { return !getSettings().game.freeCamera; });
        config_percent_select(leftPane, rightPane, getSettings().game.freeCameraYSensitivity,
            "Free Camera Y Sensitivity",
            "Adjusts vertical free camera sensitivity.<br/><br/>Applies to the control stick only.",
            50, 200, 5, [] { return !getSettings().game.freeCamera; });
        addOption("Invert Camera X Axis", getSettings().game.invertCameraXAxis,
            "Invert horizontal camera movement.<br/><br/>Applies to the control stick only.");
        addOption("Invert Camera Y Axis", getSettings().game.invertCameraYAxis,
            "Invert vertical camera movement.<br/><br/>Applies to the control stick only.",
            [] { return !getSettings().game.freeCamera; });
        addOption("Invert First Person X Axis", getSettings().game.invertFirstPersonXAxis,
            "Invert horizontal movement while aiming with items or first person camera.<br/><br/>Applies to the control stick only.");
        addOption("Invert First Person Y Axis", getSettings().game.invertFirstPersonYAxis,
            "Invert vertical movement while aiming with items or first person camera.<br/><br/>Applies to the control stick only.");

#ifdef __SWITCH__
        // aleks2 (V1-rc16): src/dusk/hd_rumble.cpp.
        leftPane.add_section("Rumble");
        addOption("HD Rumble", getSettings().game.hdRumble,
            "Uses the Switch controllers' HD Rumble: sharp hits, heavy thuds, rolling quakes and Epona's gallop "
            "from side to side, instead of the original on/off motor.<br/><br/>Applies to handheld mode, Joy-Con "
            "and the Pro Controller. A GameCube controller adapter keeps its classic rumble.");
        config_int_select(leftPane, rightPane, getSettings().game.rumbleIntensity, "Rumble Intensity",
            "How strong every vibration is, HD Rumble and the classic one alike. 0 % turns it off.", 0, 100, 10,
            {}, {}, "%");
#endif

        leftPane.add_section("Gyro");
        addOption("Gyro Aim", getSettings().game.enableGyroAim,
            "Enables gyro controls while in look mode, aiming a hawk, and aiming "
            "supported items.<br/><br/>Supported items include the Slingshot, Gale Boomerang, "
            "Hero's Bow, Clawshot(s), Ball and Chain, and Dominion Rod.");
        addOption("Gyro Rollgoal", getSettings().game.enableGyroRollgoal,
            "Enables gyro controls for Rollgoal in Hena's Cabin.");
        config_percent_select(leftPane, rightPane, getSettings().game.gyroSensitivityY,
            "Gyro Pitch Sensitivity", "Controls vertical gyro aiming sensitivity.", 25, 400, 5,
            [] { return !gyro_enabled(); });
        config_percent_select(leftPane, rightPane, getSettings().game.gyroSensitivityX,
            "Gyro Yaw Sensitivity", "Controls horizontal gyro aiming sensitivity.", 25, 400, 5,
            [] { return !gyro_enabled(); });
        config_percent_select(leftPane, rightPane, getSettings().game.gyroSensitivityRollgoal,
            "Rollgoal Sensitivity", "Controls how strongly gyro input tilts the Rollgoal table.",
            25, 400, 5,
            [] { return !getSettings().game.enableGyroRollgoal; });
        config_percent_select(leftPane, rightPane, getSettings().game.gyroDeadband, "Gyro Deadband",
            "Ignores small gyro movement to reduce drift and jitter.", 0, 50, 1,
            [] { return !gyro_enabled(); });
        config_percent_select(leftPane, rightPane, getSettings().game.gyroSmoothing,
            "Gyro Smoothing", "Higher values smooth gyro input over time.", 0, 100, 1,
            [] { return !gyro_enabled(); });
        addOption("Invert Gyro Pitch", getSettings().game.gyroInvertPitch,
            "Invert vertical gyro aiming.", [] { return !gyro_enabled(); });
        addOption("Invert Gyro Yaw", getSettings().game.gyroInvertYaw,
            "Invert horizontal gyro aiming.", [] { return !gyro_enabled(); });

#ifdef __SWITCH__
        leftPane.add_section("Wii-style Motion");
        addOption("Wii-style Motion Controls (Experimental)", getSettings().game.enableWiiMotion,
            "Plays like the Wii version with Joy-Con: swing the right Joy-Con (the Wii Remote) to swing "
            "the sword, move the left Joy-Con (the Nunchuk) sideways for the spin attack and push it "
            "forward to raise the shield for a shield bash. With Swing Direction Animations on, a clear sideways or "
            "downward swing picks the matching cut.<br/><br/>Still experimental: light or diagonal swings are often "
            "not told apart and play the game's own cut, and the thrust is rarely recognised. The "
            "Nunchuk gestures need two detached Joy-Con; with one controller only the sword swing works. The buttons always work "
            "too. Aiming by motion is Gyro Aim above; there is no Wii pointer cursor in menus.");
        config_percent_select(leftPane, rightPane, getSettings().game.wiiMotionSensitivity,
            "Gesture Sensitivity", "Higher values make smaller movements count as a gesture.", 25,
            400, 5, [] { return !getSettings().game.enableWiiMotion; });
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Motion Sword Hand",
                .getValue =
                    [] {
                        return kWiiSwordHands[static_cast<u8>(getSettings().game.wiiMotionSwordHand.getValue())];
                    },
                .isDisabled = [] { return !getSettings().game.enableWiiMotion; },
                .isModified =
                    [] {
                        return getSettings().game.wiiMotionSwordHand.getValue() !=
                               getSettings().game.wiiMotionSwordHand.getDefaultValue();
                    },
            }),
            rightPane, [](Pane& pane) {
                for (int i = 0; i < static_cast<int>(kWiiSwordHands.size()); i++) {
                    pane.add_button({
                            .text = kWiiSwordHands[i],
                            .isSelected =
                                [i] {
                                    return getSettings().game.wiiMotionSwordHand.getValue() ==
                                           static_cast<WiiSwordHand>(i);
                                },
                        })
                        .on_pressed([i] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            getSettings().game.wiiMotionSwordHand.setValue(static_cast<WiiSwordHand>(i));
                            config::save();
                        });
                }
                pane.add_rml(
                    "<br/>Which detached Joy-Con swings the sword; the other one is the Nunchuk (spin attack and "
                    "shield bash). It only chooses the sensor: Link and the world are not mirrored. With a single "
                    "controller this does nothing.");
            });
        addOption("Swing Direction Animations", getSettings().game.wiiMotionSwingCuts,
            "The sword follows the way you swing: a downward chop is the vertical cut and a sideways swing is "
            "the left or right slash. Ordinary cuts only: finishers and special attacks keep their own "
            "animation. Off plays whichever cut the game picks.",
            [] { return !getSettings().game.enableWiiMotion; });
        addOption("Swap Swing Sides", getSettings().game.wiiMotionSwapSides,
            "Swaps which slash a left-to-right swing plays, in case it looks reversed. Works together with "
            "Mirror Mode (turning on both cancels out).",
            [] { return !getSettings().game.enableWiiMotion || !getSettings().game.wiiMotionSwingCuts; });
        leftPane.register_control(
            leftPane.add_group_button({.text = "Recenter Motion Sensors"}).on_pressed([] {
                mDoAud_seStartMenu(kSoundItemChange);
                dusk::wii::recalibrate();
            }),
            rightPane, [](Pane& pane) {
                pane.clear();
                Rml::String status = dusk::ui::escape(dusk::wii::status_text());
                for (size_t at; (at = status.find('\n')) != Rml::String::npos;) {
                    status.replace(at, 1, "<br/>");
                }
                pane.add_rml(status);
                pane.add_rml(
                    "<br/><br/>Rest the controllers still, as you hold them to play, then press this: it "
                    "re-estimates the resting pose and holds gestures for a moment.");
            });

#endif  // __SWITCH__

        leftPane.add_section("Mouse");
        addOption("Mouse Aim", getSettings().game.enableMouseAim,
            "Enables mouse input while in look mode, aiming a hawk, and aiming "
            "supported items.<br/><br/>Supported items include the Slingshot, Gale Boomerang, "
            "Hero's Bow, Clawshot(s), Ball and Chain, and Dominion Rod.");
        addOption("Mouse Camera", getSettings().game.enableMouseCamera,
            "Enables mouse input for controlling the third-person camera.");
        config_percent_select(leftPane, rightPane, getSettings().game.mouseAimSensitivity,
            "Mouse Aim Sensitivity", "Controls mouse aim sensitivity.", 25, 400, 5,
            [] { return !getSettings().game.enableMouseAim; });
        config_percent_select(leftPane, rightPane, getSettings().game.mouseCameraSensitivity,
            "Mouse Camera Sensitivity", "Controls mouse camera sensitivity.", 25, 400, 5,
            [] { return !getSettings().game.enableMouseCamera; });
        addOption("Invert Mouse Y", getSettings().game.invertMouseY,
            "Invert vertical mouse control for both aiming and camera.",
            [] { return !getSettings().game.enableMouseAim || !getSettings().game.enableMouseCamera; });

        leftPane.add_section("Gameplay");
        addOption("Mouse/Touch in Menus", getSettings().game.enableMenuPointer,
            "Enables mouse and touch input for supported in-game menus.");
        addOption("Invert Air/Swim X Axis", getSettings().game.invertAirSwimX,
            "Invert horizontal movement while flying or swimming.");
        addOption("Invert Air/Swim Y Axis", getSettings().game.invertAirSwimY,
            "Invert vertical movement while flying or swimming.");
        addOption("Swap Direct Select Input", getSettings().game.swapDirectSelect,
            "Swap the controls for using Direct Select on the item wheel, making Direct Select the default and holding L to scroll the wheel.");

        leftPane.add_section("Tools");
        addOption("Turbo Key", getSettings().game.enableTurboKeybind,
            "Hold Tab to increase game speed by up to 4x.",
            [] { return speedrun::isActive(); });
        addOption("Reset Key (" + Rml::String{hotkeys::DO_RESET} + ")",
            getSettings().game.enableResetKeybind,
            "Press " + Rml::String{hotkeys::DO_RESET} + " to reset the game.");
    });

    add_tab("Audio", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);
        #ifndef __SWITCH__
        leftPane.add_section("Output");
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Output Mode",
                .getValue = [] {
                    const auto idx = static_cast<int>(getSettings().audio.outputMode.getValue());
                    return Rml::String{kAudioOutputModeNames[idx]};
                },
                .isModified = [] {
                    const auto& setting = getSettings().audio.outputMode;
                    return setting.getValue() != setting.getDefaultValue();
                },
            }), rightPane, [](Pane& pane) {
                for (int i = 0; i < static_cast<int>(kAudioOutputModeNames.size()); ++i) {
                    pane.add_button({
                        .text = kAudioOutputModeNames[i],
                        .isSelected = [i] {
                            const auto& setting = getSettings().audio.outputMode;
                            return setting.getValue() == static_cast<AudioOutputMode>(i);
                        },
                    }).on_pressed([i] {
                        mDoAud_seStartMenu(kSoundItemChange);
                        getSettings().audio.outputMode.setValue(static_cast<AudioOutputMode>(i));
                        config::save();
                        audio::Reinitialize();
                    });
                }
            });
        #endif
        // TODO: Individual sliders for Sub Music, Sound Effects, and Fanfare.
        leftPane.add_section("Volume");
        leftPane.register_control(
            leftPane.add_child<NumberButton>(NumberButton::Props{
                .key = "Master Volume",
                .getValue = [] { return getSettings().audio.masterVolume.getValue(); },
                .setValue =
                    [](int value) {
                        getSettings().audio.masterVolume.setValue(value);
                        config::save();
                        audio::SetMasterVolume(audio::MasterVolumeToLinear(value / 100.0f));
                    },
                .isModified =
                    [] {
                        return getSettings().audio.masterVolume.getValue() !=
                               getSettings().audio.masterVolume.getDefaultValue();
                    },
                .max = 100,
                .suffix = "%",
            }),
            rightPane, [](Pane& pane) {
                pane.clear();
                pane.add_text("Adjusts the volume of all sounds in the game.");
            });
        leftPane.register_control(
            leftPane.add_child<NumberButton>(NumberButton::Props{
                .key = "Main Music Volume",
                .getValue = [] { return getSettings().audio.mainMusicVolume.getValue(); },
                .setValue =
                    [](int value) {
                        getSettings().audio.mainMusicVolume.setValue(value);
                        config::save();
                    },
                .isModified =
                    [] {
                        return getSettings().audio.mainMusicVolume.getValue() !=
                               getSettings().audio.mainMusicVolume.getDefaultValue();
                    },
                .max = 100,
                .suffix = "%",
            }),
            rightPane, [](Pane& pane) {
                pane.clear();
                pane.add_text("Adjusts the volume of all music in the game.");
            });

        leftPane.add_section("Effects");
        config_bool_select(leftPane, rightPane, getSettings().audio.enableReverb,
            {
                .key = "Enable Reverb",
                .helpText = "Enables the reverb effect in game audio.",
                .onChange = [](bool value) { audio::SetEnableReverb(value); },
            });
        config_bool_select(leftPane, rightPane, getSettings().audio.menuSounds,
            {
                .key = "Dusklight Menu Sounds",
                .helpText = "Play sound effects when navigating the Dusklight menu.",
            });

        leftPane.add_section("Tweaks");
        config_bool_select(leftPane, rightPane, getSettings().game.noLowHpSound,
            {
                .key = "No Low HP Sound",
                .helpText = "Disable the beeping sound when having low health.",
            });
        config_bool_select(leftPane, rightPane, getSettings().game.midnasLamentNonStop,
            {
                .key = "Non-Stop Midna's Lament",
                .helpText = "Prevents enemy music while Midna's Lament is playing.",
            });
    });

    add_tab("Gameplay", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        auto addOption = [&](const Rml::String& key, ConfigVar<bool>& value,
                             const Rml::String& helpText) {
            config_bool_select(leftPane, rightPane, value,
                {
                    .key = key,
                    .helpText = helpText,
                });
        };
        auto addSpeedrunDisabledOption = [&](const Rml::String& key, ConfigVar<bool>& value,
                                             const Rml::String& helpText) {
            add_speedrun_disabled_option(leftPane, rightPane, value, key, helpText);
        };

        leftPane.add_section("General");
        addOption("Mirror Mode", getSettings().game.enableMirrorMode,
            "Mirrors the world horizontally, so Link looks right-handed as in the Wii version of the game. "
            "Off keeps the left-handed Link of the GameCube version.");
        addOption("Minimal HUD", getSettings().game.minimalHUD,
            "Disables the elements of the main HUD of the game.<br/>Useful for a more immersive "
            "experience.");
        addOption("Minimap", getSettings().game.showMinimap,
            "Shows the map in the corner of the screen. Off hides only that map, which is the HUD element that "
            "costs the most in large areas like Hyrule Field. The full map still opens as usual.");
        config_percent_select(leftPane, rightPane, getSettings().game.hudScale,
            "HUD Scale",
            "Scales the size of the gameplay HUD (hearts, buttons, mini-map, etc.). Does not affect dialog boxes or menus.",
            50, 200, 5,
            [] { return getSettings().game.minimalHUD.getValue(); });
        addOption("Restore Wii 1.0 Glitches", getSettings().game.restoreWiiGlitches,
            "Restores patched glitches from Wii USA 1.0, the first released version.");
        addOption("Enable Rotating Link Doll", getSettings().game.enableLinkDollRotation,
            "Enables rotating Link in the collection menu with the C-Stick.");
        addOption("Hide Owl Statue Markers", getSettings().game.removeQuestMapMarkers,
            "Removes completed Owl Statue markers from the map and Minimap.");

        leftPane.add_section("Difficulty");
        leftPane.register_control(
            leftPane.add_child<NumberButton>(NumberButton::Props{
                .key = "Damage Multiplier",
                .getValue = [] { return getSettings().game.damageMultiplier.getValue(); },
                .setValue =
                    [](int value) {
                        getSettings().game.damageMultiplier.setValue(value);
                        config::save();
                    },
                .isDisabled = [] { return speedrun::isActive(); },
                .isModified =
                    [] {
                        return getSettings().game.damageMultiplier.getValue() !=
                               getSettings().game.damageMultiplier.getDefaultValue();
                    },
                .min = 1,
                .max = 8,
                .suffix = "×",
            }),
            rightPane, [](Pane& pane) {
                pane.clear();
                pane.add_text("Multiplies incoming damage.");
            });
        addSpeedrunDisabledOption(
            "Instant Death", getSettings().game.instantDeath, "Any hit will instantly kill you.");
        addSpeedrunDisabledOption("No Heart Drops", getSettings().game.noHeartDrops,
            "Hearts will never drop from enemies, pots, and various other places.");

        leftPane.add_section("Quality of Life");
        addOption("Bigger Wallets", getSettings().game.biggerWallets,
            "Wallet sizes are like in the HD version. (500, 1000, 2000)");
        addOption("Disable Rupee Cutscenes", getSettings().game.disableRupeeCutscenes,
            "Rupees will not play cutscenes after you have collected them the first time.");
        addSpeedrunDisabledOption("Faster Scene Transitions", getSettings().game.fastTransitions,
            "Reduces how long the transitions take when changing maps.");
        addOption("Faster Climbing", getSettings().game.fastClimbing,
            "Quicker climbing on ladders and vines like the HD version.");
        addOption("Faster Tears of Light", getSettings().game.fastTears,
            "Tears of Light dropped by Shadow Insects pop out faster like the HD version.");
        addSpeedrunDisabledOption("Autosave", getSettings().game.autoSave,
            "Autosaves the game when going to a new area or opening a dungeon door.");
        addOption("Instant Saves", getSettings().game.instantSaves,
            "Skips the delay when writing to the Memory Card.");
        addOption("Hold B for Instant Text", getSettings().game.instantText,
            "Makes text scroll immediately by holding B.");
        addSpeedrunDisabledOption("Hold Button to Mash", getSettings().game.holdToMash,
            "Hold the indicated button to mash automatically.");
        addOption("No Climbing Miss Animation", getSettings().game.noMissClimbing,
            "Prevents Link from playing a struggle animation when grabbing ledges or "
            "climbing on vines.");
        addOption("No Rupee Returns", getSettings().game.noReturnRupees,
            "Always collect Rupees even if your Wallet is too full.");
        addOption("No Sword Recoil", getSettings().game.noSwordRecoil,
            "Link will not recoil when his sword hits walls.");
        addOption("No 2nd Fish for Cat", getSettings().game.no2ndFishForCat,
            "Skip needing to catch a second fish for Sera's cat.");
        addOption("Button Fishing", getSettings().game.buttonFishing,
            "Allow fishing with the Fishing Rod using the button the item is assigned to.");
        addOption("Show Poe Count on Map", getSettings().game.enhancedMapMenus,
            "Displays collected/total number of Poe Souls for a region on the map.");
        addSpeedrunDisabledOption("Sun's Song (R+X)", getSettings().game.sunsSong,
            "Allows Wolf Link to howl and change the time of day.");
        addOption("Quick Transform (R+Y)", getSettings().game.enableQuickTransform,
            "Transform instantly by pressing R and Y simultaneously.");
        addOption("Aiming Reticle", getSettings().game.aimingReticle,
            "Shows the aiming reticle for bow and slingshot.");


        leftPane.add_section("Speedrunning");
        config_bool_select(leftPane, rightPane, getSettings().game.speedrunMode,
            {
                .key = "Speedrun Mode",
                .helpText =
                    "Enables Speedrun game mode option in the Dusklight launch menu.",
                .onChange =
                    [this](bool enabled) {
                        if (enabled) {
                            speedrun::registerSpeedrunGameMode();
                        } else {
                            if (speedrun::isActive()) {
                                pop();
                            }
                            speedrun::unregisterSpeedrunGameMode();
                        }
                        MenuBar::refresh_tabs();
                    },
            });
        config_bool_select(leftPane, rightPane, getSettings().game.liveSplitEnabled,
            {
                .key = "LiveSplit Connection",
                .helpText = "Connect to LiveSplit server on localhost:16834. For this to work you must right click LiveSplit, and turn on Control -> Start TCP Server."
                " To see IGT in LiveSplit you must change your comparison to Game Time.",
                .onChange =
                    [](bool enabled) {
                        if (enabled) {
                            speedrun::connectLiveSplit();
                        } else {
                            speedrun::disconnectLiveSplit();
                        }
                    },
                .isDisabled = [] { return IsMobile || !speedrun::isActive(); },
            });
        config_bool_select(leftPane, rightPane, getSettings().game.showSpeedrunRTATimer,
            {
                .key = "Show RTA",
                .helpText = "Display the RTA timer. IGT is always visible.",
                .isDisabled = [] { return !speedrun::isActive(); },
            });
    });

    add_tab("Cheats", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        auto addCheat = [&](const Rml::String& key, ConfigVar<bool>& value,
                            const Rml::String& helpText) {
            add_speedrun_disabled_option(leftPane, rightPane, value, key, helpText);
        };

        leftPane.add_section("Resources");
        addCheat("Infinite Hearts", getSettings().game.infiniteHearts, "Keeps your health full.");
        addCheat(
            "Infinite Arrows", getSettings().game.infiniteArrows, "Keeps your arrow count full.");
        addCheat("Infinite Seeds", getSettings().game.infiniteSeeds, "Keeps your slingshot pellets (seeds) full.");
        addCheat("Infinite Bombs", getSettings().game.infiniteBombs, "Keeps all bomb bags full.");
        addCheat("Infinite Oil", getSettings().game.infiniteOil, "Keeps your lantern oil full.");
        addCheat("Infinite Oxygen", getSettings().game.infiniteOxygen,
            "Keeps your underwater oxygen meter full.");
        addCheat(
            "Infinite Rupees", getSettings().game.infiniteRupees, "Keeps your rupee count full.");
        addCheat("No Item Timer", getSettings().game.enableIndefiniteItemDrops,
            "Item drops such as rupees and hearts will never disappear after they drop.");

        leftPane.add_section("Abilities");

        addCheat(
            "Moon Jump (R+A)", getSettings().game.moonJump, "Hold R and A to rise into the air.");
        addCheat(
            "Easy Quick Spin (R+B)", getSettings().game.easyQuickSpin, "Hold R to always do a Quick Spin when attacking with B.");

        addCheat("Super Clawshot", getSettings().game.superClawshot,
            "Extends Clawshot behavior beyond the normal game rules.");
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Always Greatspin",
                .getValue =
                    [] {
                        return kAlwaysGreatspinModes[static_cast<u8>(
                            getSettings().game.alwaysGreatspin.getValue())];
                    },
                .isDisabled = [] { return dusk::speedrun::isActive(); },
                .isModified =
                    [] {
                        return getSettings().game.alwaysGreatspin.getValue() !=
                               getSettings().game.alwaysGreatspin.getDefaultValue();
                    },
            }),
            rightPane, [](Pane& pane) {
                for (int i = 0; i < static_cast<int>(kAlwaysGreatspinModes.size()); i++) {
                    pane.add_button({
                            .text = kAlwaysGreatspinModes[i],
                            .isSelected =
                                [i] {
                                    return getSettings().game.alwaysGreatspin.getValue() ==
                                           static_cast<AlwaysGreatspinMode>(i);
                                },
                        })
                        .on_pressed([i] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            getSettings().game.alwaysGreatspin.setValue(
                                static_cast<AlwaysGreatspinMode>(i));
                            config::save();
                        });
                }
                pane.add_rml("<br/>Allows the Great Spin attack without requiring full health.");
            });
        addCheat("Fast Iron Boots", getSettings().game.enableFastIronBoots,
            "Speeds up movement while heavy, including wearing the Iron Boots, holding the Ball and Chain, wearing Magic Armor without rupees, etc.");
        addCheat("Can Transform Anywhere", getSettings().game.canTransformAnywhere,
            "Allows transforming even if NPCs are looking.");
        addCheat("Fast Roll", getSettings().game.fastRoll,
            "Makes Link's roll animation and movement twice as fast.");
        addCheat("Fast Spinner", getSettings().game.fastSpinner,
            "Speeds up Spinner movement while holding R.");
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Magic Armor Behavior",
                .getValue =
                    [] {
                        return kMagicArmorModes[static_cast<u8>(
                            getSettings().game.armorRupeeDrain.getValue())];
                    },
                .isDisabled = [] { return speedrun::isActive(); },
                .isModified =
                    [] {
                        return getSettings().game.armorRupeeDrain.getValue() !=
                               getSettings().game.armorRupeeDrain.getDefaultValue();
                    },
            }),
            rightPane, [](Pane& pane) {
                for (int i = 0; i < kMagicArmorModes.size(); i++) {
                    pane.add_button({
                            .text = kMagicArmorModes[i],
                            .isSelected =
                                [i] {
                                    return getSettings().game.armorRupeeDrain.getValue() == static_cast<MagicArmorMode>(i);
                                },
                        })
                        .on_pressed([i] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            getSettings().game.armorRupeeDrain.setValue(static_cast<MagicArmorMode>(i));
                            config::save();
                        });
                }
                pane.add_rml(
                    "<br/>Control the behavior of the Magic Armor.");
            });
        addCheat("Invincible Enemies", getSettings().game.invincibleEnemies,
            "Prevents enemies from taking damage.");
    });

    add_tab("Interface", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Dusklight");
#if DUSK_CAN_OPEN_DATA_FOLDER
        leftPane.register_control(
            leftPane.add_button("Open Data Folder").on_pressed([] {
                mDoAud_seStartMenu(kSoundClick);
                data::open_data_path();
            }),
            rightPane, [](Pane& pane) {
                pane.add_text(
                    "Open the folder where Dusklight stores settings, saves, logs, texture "
                    "replacements, and other app data.");
            });
#endif
        leftPane.register_control(leftPane.add_button("Restart to Main Menu").on_pressed([this] {
            mDoAud_seStartMenu(kSoundClick);
            pop();
            prelaunch_state().returnToPrelaunchOnReset = true;
            JUTGamePad::C3ButtonReset::sResetSwitchPushing = true;
        }),
            rightPane, [](Pane& pane) {
                pane.add_text("Restart Dusklight to the pre-launch menu to change settings, game "
                              "modes, or mods.");
            });
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Notifications",
                .getValue = [] {
                    const bool ach = getSettings().game.enableAchievementToasts.getValue();
                    const bool ctl = getSettings().game.enableControllerToasts.getValue();
                    if (!ach && !ctl) {
                        return Rml::String{"Off"};
                    }
                    if (ach && ctl) {
                        return Rml::String{"All"};
                    }
                    return Rml::String{"Some"};
                },
                .isModified = [] {
                    const auto& ach = getSettings().game.enableAchievementToasts;
                    const auto& ctl = getSettings().game.enableControllerToasts;
                    return ach.getValue() != ach.getDefaultValue() || ctl.getValue() != ctl.getDefaultValue();
                },
            }),
            rightPane, [](Pane& pane) {
                pane.clear();
                pane.add_button("Select All").on_pressed([] {
                    mDoAud_seStartMenu(kSoundItemChange);
                    getSettings().game.enableAchievementToasts.setValue(true);
                    getSettings().game.enableControllerToasts.setValue(true);
                    config::save();
                });
                pane.add_button("Select None").on_pressed([] {
                    mDoAud_seStartMenu(kSoundItemChange);
                    getSettings().game.enableAchievementToasts.setValue(false);
                    getSettings().game.enableControllerToasts.setValue(false);
                    config::save();
                });

                pane.add_section("Types");
                pane.add_button(
                    {
                        .text = "Achievements",
                        .isSelected =
                        [] {
                            return getSettings().game.enableAchievementToasts.getValue();
                        },
                    })
                    .on_pressed([] {
                        mDoAud_seStartMenu(kSoundItemChange);
                        auto& v = getSettings().game.enableAchievementToasts;
                        v.setValue(!v.getValue());
                        config::save();
                    });
                pane.add_button(
                    {
                        .text = "Missing Device",
                        .isSelected =
                            [] { return getSettings().game.enableControllerToasts.getValue(); },
                    })
                    .on_pressed([] {
                        mDoAud_seStartMenu(kSoundItemChange);
                        auto& v = getSettings().game.enableControllerToasts;
                        v.setValue(!v.getValue());
                        config::save();
                    });
                pane.add_rml("<br/>Choose which notifications can be displayed.");
            });
#if BOREALIS_HAS_SENTRY
        auto& crashReporting = leftPane.add_child<BoolButton>(BoolButton::Props{
            .key = "Crash Reporting",
            .getValue =
                [] { return borealis::sentry::get_consent() == borealis::sentry::Consent::Given; },
            .setValue = [](bool enabled) { borealis::sentry::set_consent(enabled); },
            .isDisabled =
                [] {
                    return borealis::sentry::get_consent() ==
                           borealis::sentry::Consent::Unavailable;
                },
            .isModified = [] { return false; },
        });
        leftPane.register_control(crashReporting, rightPane, [](Pane& pane) {
            pane.clear();
            pane.add_rml("Dusklight can automatically send crash reports to the developers. Crash "
                         "reports contain the following:<br/>• Operating system version<br/>• CPU "
                         "architecture<br/>• GPU model & driver version<br/>• File paths (may "
                         "include account username)<br/>• Stack trace");
        });
#endif
        config_bool_select(leftPane, rightPane, getSettings().backend.skipPreLaunchUI,
            {
                .key = "Skip Dusklight Main Menu",
#ifdef __SWITCH__
                .helpText =
                    "When starting Dusklight, skip the main menu and boot straight into the "
                    "game if a disc image is available, in the game mode you chose last (Play or "
                    "Randomizer).<br/><br/>The menu still shows when the disc image cannot be used "
                    "or that game mode is no longer available.",
#else
                .helpText =
                    "When starting Dusklight, skip the main menu and boot straight into the "
                    "game if a disc image is available.<br/><br/>Note: If any mods register game "
                    "modes, this option will be ignored.",
#endif
            });
        config_bool_select(leftPane, rightPane, getSettings().backend.showPipelineCompilation,
            {
                .key = "Show Shader Compilation",
                .helpText = "Show an overlay when shaders are being compiled for your hardware.",
            });
        config_bool_select(leftPane, rightPane, getSettings().backend.checkForUpdates,
            {
                .key = "Check for Dusklight Updates",
#ifdef __SWITCH__
                .helpText = "Checks this port's GitHub releases for a new version when Dusklight starts, at most "
                            "once a day. Nothing is downloaded until you choose to.<br/><br/>"
                            "No personal information is transmitted or collected.",
#else
                .helpText = "Checks GitHub releases for a new Dusklight version on startup.<br/><br/>"
                            "No personal information is transmitted or collected.",
#endif
            });
#if BOREALIS_HAS_DISCORD
        config_bool_select(leftPane, rightPane, getSettings().game.enableDiscordPresence,
            {
                .key = "Enable Discord Rich Presence",
                .helpText = "Enable Dusklight to integrate with Discord Rich Presence. This allows Discord to show your status in-game.",
                .onChange = [](bool enabled) {
                    if (enabled) {
                        discord::initialize();
                    } else {
                        discord::shutdown();
                    }
                },
            });
#endif
        config_bool_select(leftPane, rightPane, getSettings().backend.enableAdvancedSettings,
            {
                .key = "Enable Advanced Settings",
                .icon = "warning",
                .helpText = "Show advanced settings and debugging tools with "
                            "Shift+F1.<br/><br/><icon class=\"warning\"/> WARNING: Debugging tools "
                            "can easily break your game. Do not use on a regular save!",
                .onChange = [](bool) { MenuBar::refresh_tabs(); },
                .isDisabled = [] { return speedrun::isActive(); },
            });
#if DUSK_PROFILER_ENABLED
        config_bool_select(leftPane, rightPane, getSettings().backend.showProfilerOverlay,
            {
                .key = "Show Profiler Overlay",
                .helpText = "Shows a CPU timing overlay. A diagnostic for development."
            });
#endif
        config_bool_select(leftPane, rightPane, getSettings().game.showInputViewer,
            {
                .key = "Show Input Viewer",
                .helpText = "Display a controller input overlay while playing.",
            });
        config_bool_select(leftPane, rightPane, getSettings().game.showInputViewerGyro,
            {
                .key = "Show Gyro Input Viewer",
                .helpText = "Show gyro sensor values in the input viewer.",
                .isDisabled = [] { return !getSettings().game.showInputViewer; },
            });

        leftPane.add_section("Game");
        leftPane.register_control(
            leftPane.add_select_button({
                .key = "Menu Scaling Mode",
                .getValue =
                    [] {
                        return kMenuScalingModeLabels[static_cast<u8>(
                            getSettings().game.menuScalingMode.getValue())];
                    },
                .isModified =
                    [] {
                        const auto& mode = getSettings().game.menuScalingMode;
                        return mode.getValue() != mode.getDefaultValue();
                    },
            }),
            rightPane, [](Pane& pane) {
                for (int i = 0; i < static_cast<int>(kMenuScalingModeLabels.size()); ++i) {
                    pane
                        .add_button({
                            .text = kMenuScalingModeLabels[i],
                            .isSelected =
                                [i] {
                                    return getSettings().game.menuScalingMode.getValue() ==
                                           static_cast<MenuScaling>(i);
                                },
                        })
                        .on_pressed([i] {
                            mDoAud_seStartMenu(kSoundItemChange);
                            getSettings().game.menuScalingMode.setValue(
                                static_cast<MenuScaling>(i));
                            config::save();
                        });
                }
                pane.add_rml("<br/>Changes how the Collection and File Select menus scale to your "
                             "aspect ratio.");
            });
        config_bool_select(leftPane, rightPane, getSettings().game.hideTvSettingsScreen,
            {
                .key = "Skip TV Settings Screen",
                .helpText = "Skips the TV calibration screen shown when loading a save.",
            });
        add_speedrun_disabled_option(leftPane, rightPane, getSettings().game.recordingMode,
            "Recording Mode",
            "Disables the game HUD, all background music and the FPS counter.<br/><br/>Useful for recording footage.");
    });

    add_tab("Tools", [this](Rml::Element* content) {
        auto& leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto& rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Link");
        add_speedrun_disabled_option(leftPane, rightPane, getSettings().game.enableMoveLinkCombo,
            "Move Link (L+R+Y)",
            "Enables the L+R+Y button combo to toggle freely repositioning Link.");
        add_speedrun_disabled_option(leftPane, rightPane, getSettings().game.enableTeleportCombo,
            "Teleport (R+D-pad Up/Down)",
            "R+D-pad Up stores Link's current position.<br/>"
            "R+D-pad Down teleports Link back to it.");
    });
}

void SettingsWindow::update() {
    if (mPrelaunch && top_document() == this) {
        try_push_verification_modal(*this);
        try_push_language_unavailable_modal(*this);
    }

    Window::update();
}

void SettingsWindow::hide(bool close) {
    config::save();
    Window::hide(close);
}

}  // namespace dusk::ui
