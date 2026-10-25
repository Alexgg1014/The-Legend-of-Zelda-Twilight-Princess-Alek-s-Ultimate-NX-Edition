/**
 * m_Do_main.cpp
 * Main Initialization
 * PC Port Version - based on Aurora integration from Vorversion
 */

#include "m_Do/m_Do_main.h"
#include "DynamicLink.h"
#include "JSystem/JAudio2/JASAudioThread.h"
#include "JSystem/JAudio2/JAUSoundTable.h"
#include "JSystem/JFramework/JFWSystem.h"
#include "JSystem/JKernel/JKRAram.h"
#include "JSystem/JKernel/JKRSolidHeap.h"
#include "JSystem/JUtility/JUTConsole.h"
#include "JSystem/JUtility/JUTReport.h"
#include "JSystem/JUtility/JUTException.h"
#include "JSystem/JUtility/JUTProcBar.h"
#include "JSystem/JHostIO/JORServer.h"
#include "Z2AudioLib/Z2WolfHowlMgr.h"
#include "c/c_dylink.h"
#include "d/d_com_inf_game.h"
#include "d/d_s_logo.h"
#include "d/d_s_menu.h"
#include "d/d_s_play.h"
#include "d/d_debug_pad.h"
#include "d/d_menu_window.h"
#include "f_ap/f_ap_game.h"
#include "f_op/f_op_msg.h"
#include "m_Do/m_Do_MemCard.h"
#include "m_Do/m_Do_Reset.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_dvd_thread.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_machine.h"
#include "m_Do/m_Do_printf.h"
#include "m_Do/m_Do_ext2.h"
#include <cstring>

#include "dusk/app_info.hpp"
#include "dusk/audio/DuskAudioSystem.h"
#include "dusk/audio/DuskDsp.hpp"
#include "dusk/commands.hpp"
#include "dusk/config.hpp"
#include "dusk/deko3d_policy.hpp"
#include "dusk/data.hpp"
#include "dusk/discord_presence.hpp"
#include "dusk/dusk.h"
#include "dusk/dualscreen.h"
#include "dusk/drs_stress.hpp"
#include "dusk/dynres.hpp"
#include "dusk/frame_telemetry.hpp"
#include "dusk/hd_rumble.hpp"
#include "dusk/gpu_boost.hpp"
#include "dusk/suspend_clock.hpp"
#include "dusk/pgo_dump.hpp"
#include "dusk/guide/fetch.hpp"
#include "dusk/guide/store.hpp"
#include "dusk/session_log.hpp"
#include "dusk/ui/boot_splash.hpp"
#include "dusk/self_update.hpp"
#include "dusk/dev_mode.hpp"
#include "dusk/shader_warmup_runtime.hpp"
#ifdef __SWITCH__
#include "dusk/benchmark.hpp"
#include "dusk/director.hpp"
#endif
#ifdef __SWITCH__
extern "C" void aleks2_exit_trace(const char* line);  // platforms/switch/source/switch_startup.cpp
#endif
#include "dusk/game_clock.h"
#include "dusk/game_combos.h"
#include "dusk/gyro.h"
#include "dusk/wii_controls.hpp"
#include "dusk/hq_minimap.hpp"
#include "dusk/imgui/ImGuiConsole.hpp"
#include "dusk/imgui/ImGuiEngine.hpp"
#include "dusk/interp/frame_interpolation.h"
#include "dusk/iso_validate.hpp"
#include "dusk/logging.h"
#include "dusk/main.h"
#include "dusk/mod_loader.hpp"
#include "dusk/mods/svc/window.hpp"
#include "dusk/mouse.h"
#include "dusk/os.h"
#include "dusk/presentation.hpp"
#include "dusk/profiler.hpp"
#include "dusk/settings.h"
#ifdef __SWITCH__
#include "dusk/switch_display.hpp"
#include "switch/pad_port.hpp"
#include "switch/startup.hpp"
#include "switch/touch.hpp"
#if ALEKS2_NATIVE_MOD_LOADER
#include "dusk/mods/svc/hook.hpp"
#include "dusk/switch_native_loader.hpp"
#endif
#endif
#include "dusk/speedrun.h"
#include "dusk/texture_replacements.hpp"
#include "dusk/time.h"
#include "dusk/ui/command_console.hpp"
#include "dusk/ui/menu_bar.hpp"
#include "dusk/ui/overlay.hpp"
#include "dusk/ui/prelaunch.hpp"
#include "dusk/ui/preset.hpp"
#if BOREALIS_HAS_SENTRY
#include "dusk/ui/reporting.hpp"
#endif
#include "dusk/ui/touch_controls.hpp"
#include "dusk/ui/ui.hpp"
#include "dusk/version.hpp"

#include "d/actor/d_a_movie_player.h"

#include "SSystem/SComponent/c_API_graphic.h"

#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/event.h>
#include <borealis/aurora_log.h>
#include <borealis/cli.hpp>
#include <borealis/crash.hpp>
#include <borealis/io.hpp>
#include <borealis/sentry.hpp>
#include <borealis/task.hpp>
#include <borealis/version.h>
#include <cxxopts.hpp>
#include <dolphin/dvd.h>
#include <chrono>
#include <SDL3/SDL_init.h>
#include <tracy/Tracy.hpp>

#include <filesystem>
#include <system_error>
#include <thread>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

// --- GLOBALS ---
DUSK_GAME_DATA s8 mDoMain::developmentMode = -1;
DUSK_GAME_DATA OSTime mDoMain::sPowerOnTime;
DUSK_GAME_DATA OSTime mDoMain::sHungUpTime;
DUSK_GAME_DATA u32 mDoMain::memMargin = 0xFFFFFFFF;
DUSK_GAME_DATA char mDoMain::COPYDATE_STRING[18] = "??/??/?? ??:??:??";
#if TARGET_PC
const int audioHeapSize = 0x14D800 * 2;
#else
const int audioHeapSize = 0x14D800;
#endif

// =========================================================================
// LOAD_COPYDATE - PC Version
// =========================================================================
#define COPYDATE_PATH "/str/Final/Release/COPYDATE"

s32 LOAD_COPYDATE(void*) {
    char buffer[32];
    memset(buffer, 0, sizeof(buffer));

    DVDFileInfo fi;
    if (DVDOpen(COPYDATE_PATH, &fi)) {
        u32 readLen = (fi.length < sizeof(buffer) - 1) ? fi.length : sizeof(buffer) - 1;
        // DVDReadPrio requires 32-byte aligned buffer and length rounded up to 32
        u32 alignedLen = (readLen + 31) & ~31;
        alignas(32) char readBuf[64];
        DVDReadPrio(&fi, readBuf, alignedLen, 0, 2);
        DVDClose(&fi);

        memcpy(buffer, readBuf, readLen);
        buffer[readLen] = '\0';
    } else {
        SAFE_STRCPY(buffer, "PC PORT BUILD");
        DuskLog.warn("COPYDATE file not found at {}", COPYDATE_PATH);
    }

    memcpy(mDoMain::COPYDATE_STRING, buffer, sizeof(mDoMain::COPYDATE_STRING) - 1);
    mDoMain::COPYDATE_STRING[sizeof(mDoMain::COPYDATE_STRING) - 1] = '\0';

    DuskLog.info("COPYDATE=[{}]", mDoMain::COPYDATE_STRING);
    return 1;
}

AuroraInfo auroraInfo;

bool launchUILoop() {
    while (dusk::IsRunning && !dusk::IsGameLaunched) {
#ifdef __SWITCH__
        dusk::display::poll();
        dusk::sw::bind_pad_to_port_one();
#endif
        const AuroraEvent* event = aurora_update();
        while (event != nullptr && event->type != AURORA_NONE) {
            switch (event->type) {
            case AURORA_SDL_EVENT:
                if (dusk::mods::svc::window_dispatch_event(event->sdl)) {
                    break;
                }
                dusk::mouse::handle_event(event->sdl);
                dusk::ui::handle_event(event->sdl);
                dusk::g_imguiConsole.HandleSDLEvent(event->sdl);
                break;
            case AURORA_DISPLAY_SCALE_CHANGED:
                dusk::ImGuiEngine_Initialize(event->windowSize.scale);
                break;
            case AURORA_EXIT:
                return false;
            }

            event++;
        }

        if (!aurora_begin_frame()) {
            DuskLog.debug("aurora_begin_frame returned false, skipping draw this frame");
            continue;
        }

        dusk::ui::update();

        dusk::g_imguiConsole.PreDraw();
        dusk::g_imguiConsole.PostDraw();

        aurora_end_frame();
    }

    return dusk::IsRunning;
}

void main01(void) {
    OS_REPORT("\x1b[m");

    // 1. Setup
    mDoMch_Create();
    mDoGph_Create();
    mDoCPd_c::create();

    // Console Setup
    JUTConsole* console = JFWSystem::getSystemConsole();
    if (console) {
        console->setOutput(mDoMain::developmentMode ? JUTConsole::OUTPUT_OSR_AND_CONSOLE :
                                                      JUTConsole::OUTPUT_NONE);
        console->setPosition(32, 42);
    }

    // Loader Init
    mDoDvdThd_callback_c::create((mDoDvdThd_callback_func)LOAD_COPYDATE, NULL);

    OSReport("Calling fapGm_Create()...\n");
    fapGm_Create();

    OSReport("Calling fopAcM_initManager()...\n");
    fopAcM_initManager();

    OSReport("Calling cDyl_InitAsync()...\n");
    cDyl_InitAsync();

    g_mDoAud_audioHeap = JKRCreateSolidHeap(audioHeapSize, JKRGetCurrentHeap(), false);
    JKRHEAP_NAME(g_mDoAud_audioHeap, "g_mDoAud_audioHeap");

    if (DUSK_AUDIO_DISABLED) {
        // Pretend the audio engine initialized already. This is a lie, but needed to boot.
        mDoAud_zelAudio_c::onInitFlag();
    }

    OSReport("Entering Main Loop (main01)...\n");

    dusk::game_clock::initialize();

    do {
        DUSK_PROF_FRAME_BEGIN();
        // 1. Update Window Events
        { DUSK_PROFSCOPE("Events");
        const AuroraEvent* event = aurora_update();
        while (true) {
            switch (event->type) {
            case AURORA_NONE:
                goto eventsDone;
            case AURORA_PAUSED:
                dusk::audio::SetPaused(true);
                dusk::mouse::on_focus_lost();
                break;
            case AURORA_UNPAUSED:
                dusk::audio::SetPaused(false);
                dusk::game_clock::reset();
                dusk::mouse::on_focus_gained();
                break;
            case AURORA_SDL_EVENT:
                if (dusk::mods::svc::window_dispatch_event(event->sdl)) {
                    break;
                }
                dusk::mouse::handle_event(event->sdl);
                dusk::ui::handle_event(event->sdl);
                dusk::g_imguiConsole.HandleSDLEvent(event->sdl);
                break;
            case AURORA_WINDOW_RESIZED:
                if (dusk::getSettings().video.rememberWindowSize && !dusk::getSettings().video.enableFullscreen) {
                    dusk::getSettings().video.lastWindowWidth.setValue(event->windowSize.width);
                    dusk::getSettings().video.lastWindowHeight.setValue(event->windowSize.height);
                    dusk::config::save();
                }
                break;
            case AURORA_DISPLAY_SCALE_CHANGED:
                dusk::ImGuiEngine_Initialize(event->windowSize.scale);
                break;
            case AURORA_EXIT:
                goto exit;
            }

            event++;
        }

        eventsDone:;
        } // Events

        { DUSK_PROFSCOPE("Begin");
        if (!aurora_begin_frame()) {
            DuskLog.debug("aurora_begin_frame returned false, skipping draw this frame");
            continue;
        }
        } // Begin

        { DUSK_PROFSCOPE("VIStub");
        VIWaitForRetrace();
        } // VIStub (VIWaitForRetrace only bumps a counter here; not a wait)

        { DUSK_PROFSCOPE("UI");
        dusk::lastFrameAuroraStats = *aurora_get_stats();
        mDoGph_gInf_c::updateRenderSize();
#ifdef __SWITCH__
        dusk::display::poll();
        dusk::frame_telemetry::tick();
        dusk::hd_rumble::tick();  // aleks2 (V1-rc16)
        dusk::gpu_boost::tick();  // aleks2 (V1-rc16)
        dusk::suspend_clock::tick();  // aleks2 (1.0.1): sleep does not count as play time
        dusk::pgo_dump::tick();   // aleks2 (V1-PGO): nothing unless ALEKS2_PGO=GENERATE
        dusk::dynres::update();
        dusk::drs_stress::tick();
        dusk::session_log::tick(); dusk::boot_timing::frame();  // rc17: boot timing, first frame
        dusk::self_update::game_frame();  // V1-rc8
        dusk::benchmark::tick();
        dusk::director::tick();
        dusk::shader_warmup::tick();
        dusk::sw::bind_pad_to_port_one();
#endif

        dusk::ui::update();
        } // UI

        const auto timing = dusk::game_clock::advance();
        if (timing.separatePresentation) {
            if (timing.numSimTicks > 0) {
                dusk::interp::begin_frame(0.0f);
                { DUSK_PROFSCOPE("Sim");
                for (int i = 0; i < timing.numSimTicks; ++i) {
                    // When a capture is pending, it has to be painted before the next sim tick.
                    if (dMw_isCapturePending()) {
                        break;
                    }
                    if (timing.interpolating) {
                        dusk::interp::begin_sim_tick();
                    }
                    dusk::game_clock::begin_sim_tick();
                    dusk::wii::sample(dusk::game_clock::kSimPeriod);  // before read(), which consumes its pulses
                    mDoCPd_c::read();
                    dusk::mouse::read();
                    dusk::gyro::read(dusk::game_clock::kSimPeriod);
                    dusk::processGameCombos();
                    { DUSK_PROFSCOPE("Execute"); fapGm_Execute(); }
                    dusk::processCameraCommands();
                    { DUSK_PROFSCOPE("Audio"); mDoAud_Execute(); }
                    dusk::game_clock::commit_sim_tick();
                }
                } // Sim
            }

            const float step = timing.interpolating ? dusk::game_clock::sample_interpolation_step() : 1.0f;
            dusk::interp::begin_presentation(step);
            { DUSK_PROFSCOPE("Draw");
            fpcM_DrawIterater((fpcM_DrawIteraterFunc)fpcM_Draw);
            cAPIGph_Painter();
            } // Draw
            dusk::interp::end_presentation();
        } else {
            dusk::interp::begin_frame(0.0f);
            dusk::game_clock::begin_sim_tick();

            // Game Inputs
            dusk::wii::sample(timing.dt);  // before read(), which consumes its pulses
            mDoCPd_c::read();
            dusk::mouse::read();
            dusk::gyro::read(timing.dt);
            dusk::processGameCombos();

            { DUSK_PROFSCOPE("Sim");
            // EXECUTE GAME LOGIC & RENDER
            // This calls mDoGph_Painter -> JFWDisplay -> GX Functions
            { DUSK_PROFSCOPE("Execute"); fapGm_Execute(); }
            dusk::processCameraCommands();

            { DUSK_PROFSCOPE("Audio"); mDoAud_Execute(); }
            dusk::game_clock::commit_sim_tick();
            } // Sim
        }

        { DUSK_PROFSCOPE("Present");
        aurora_end_frame();
        } // Present
        DUSK_PROF_FRAME_END();

        FrameMark;

#if BOREALIS_HAS_DISCORD
        dusk::discord::run_callbacks();
        dusk::discord::update_presence();
#endif

        static Limiter main_loop_limiter;
        static double last_fps_setting = 0.0;
        static Limiter::duration_t target_ns = 0;

        if (dusk::getSettings().game.enableFrameInterpolation.getValue() ==
                dusk::FrameInterpMode::Capped &&
            !dusk::getTransientSettings().turboMode)
        {
            ZoneScopedN("Frame limiter");
            double current_fps = dusk::getSettings().video.maxFrameRate.getValue();
            if (current_fps != last_fps_setting) {
                last_fps_setting = current_fps;
                target_ns = static_cast<Limiter::duration_t>(1'000'000'000.0 / current_fps);
            }

            // H33: report the sleep that actually elapsed to the game clock (the interval it
            // ends is the one advance() measures next).
            const Uint64 limiterStart = SDL_GetTicksNS();
            Limiter::duration_t sleepTime = main_loop_limiter.Sleep(target_ns);
            dusk::game_clock::note_wait(dusk::game_clock::WaitKind::Limiter,
                                        SDL_GetTicksNS() - limiterStart);
            dusk::frameUsagePct =
                100.0f * (1.0f - static_cast<float>(sleepTime) / static_cast<float>(target_ns));
        } else {
            main_loop_limiter.Reset();
        }
    } while (dusk::IsRunning);

    exit:;
#ifdef __SWITCH__
    aleks2_exit_trace("main loop ended: quitting (mods unload next)");  // V1-rc11: before anything can stop us
#endif
    dusk::mods::ModLoader::instance().shutdown();
    dusk::ui::shutdown();
}

static bool IsBackendAvailable(AuroraBackend backend) {
    if (backend == BACKEND_AUTO) {
        return true;
    }

    size_t availableBackendCount = 0;
    const AuroraBackend* availableBackends = aurora_get_available_backends(&availableBackendCount);
    for (size_t i = 0; i < availableBackendCount; ++i) {
        if (availableBackends[i] == backend) {
            return true;
        }
    }

    return false;
}

static AuroraBackend ResolveDesiredBackend(const cxxopts::ParseResult& parsedArgOptions) {
    AuroraBackend desiredBackend = BACKEND_AUTO;

    if (parsedArgOptions.count("backend") != 0) {
        const std::string backendArg = parsedArgOptions["backend"].as<std::string>();
        if (!dusk::try_parse_backend(backendArg, desiredBackend)) {
            fmt::print(stderr, "Unknown backend: {}\n", backendArg);
            exit(1);
        }
    } else if (!dusk::try_parse_backend(
                   static_cast<const std::string&>(dusk::getSettings().backend.graphicsBackend),
                   desiredBackend))
    {
        DuskLog.warn("Unknown configured backend '{}', falling back to Auto",
                     static_cast<const std::string&>(dusk::getSettings().backend.graphicsBackend));
        desiredBackend = BACKEND_AUTO;
    }

    if (!IsBackendAvailable(desiredBackend)) {
        DuskLog.warn("Requested backend '{}' is unavailable, falling back to Auto",
                     dusk::backend_name(desiredBackend));
        desiredBackend = BACKEND_AUTO;
    }

    return desiredBackend;
}

static void aurora_imgui_init_callback(const AuroraWindowSize* size) {
    dusk::ImGuiEngine_Initialize(size->scale);
    dusk::ImGuiEngine_AddTextures();
}

static void ApplyCVarOverrides(const cxxopts::OptionValue& option) {
    if (option.count() == 0) {
        return;
    }

    const auto& cVars = option.as<std::vector<std::string>>();
    for (const auto& cvarArg : cVars) {
        const auto sep = cvarArg.find('=');
        if (sep == std::string::npos) {
            DuskLog.fatal("--cvar argument has no '=': '{}'", cvarArg);
            continue;
        }

        const auto name = std::string_view(cvarArg).substr(0, sep);
        const auto value = std::string_view(cvarArg).substr(sep + 1);

        dusk::config::load_arg_override(name, value);
    }
}

static constexpr PADDefaultMapping defaultPadMapping = {
    .buttons = {
        {SDL_GAMEPAD_BUTTON_SOUTH, PAD_BUTTON_A},
        {SDL_GAMEPAD_BUTTON_EAST, PAD_BUTTON_B},
        {SDL_GAMEPAD_BUTTON_WEST, PAD_BUTTON_X},
        {SDL_GAMEPAD_BUTTON_NORTH, PAD_BUTTON_Y},
        {SDL_GAMEPAD_BUTTON_START, PAD_BUTTON_START},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_TRIGGER_Z},
        {PAD_NATIVE_BUTTON_INVALID, PAD_TRIGGER_L},
        {PAD_NATIVE_BUTTON_INVALID, PAD_TRIGGER_R},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_BUTTON_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_BUTTON_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_BUTTON_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_BUTTON_RIGHT},
    },
    .axes = {
        {{SDL_GAMEPAD_AXIS_LEFTX, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_X_POS},
        {{SDL_GAMEPAD_AXIS_LEFTX, AXIS_SIGN_NEGATIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_X_NEG},
        // SDL's gamepad y-axis is inverted from GC's
        {{SDL_GAMEPAD_AXIS_LEFTY, AXIS_SIGN_NEGATIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_Y_POS},
        {{SDL_GAMEPAD_AXIS_LEFTY, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_Y_NEG},
        {{SDL_GAMEPAD_AXIS_RIGHTX, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_X_POS},
        {{SDL_GAMEPAD_AXIS_RIGHTX, AXIS_SIGN_NEGATIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_X_NEG},
        // see above
        {{SDL_GAMEPAD_AXIS_RIGHTY, AXIS_SIGN_NEGATIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_Y_POS},
        {{SDL_GAMEPAD_AXIS_RIGHTY, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_Y_NEG},
        {{SDL_GAMEPAD_AXIS_LEFT_TRIGGER, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_TRIGGER_L},
        {{SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, AXIS_SIGN_POSITIVE}, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_TRIGGER_R},
    },
};

static bool mainCalled = false;

static u8 selectedLanguage;

u8 OSGetLanguage() {
    return selectedLanguage;
}

static void LanguageInit() {
    // Keep language at 0 (English) if not on a PAL disc.
    // Doubt this matters, but avoid funky shit.
    if (!dusk::version::isRegionPal()) {
        return;
    }

    // Cache this to avoid funky shenanigans.
    selectedLanguage = static_cast<u8>(dusk::getSettings().game.language.getValue());
}

static void log_build_info() {
#ifndef __SWITCH__
    DuskLog.info("Build: {} (rev {}, built {}, type {})", BOREALIS_APP_DESCRIBE, BOREALIS_APP_REVISION, BOREALIS_APP_DATE, BOREALIS_BUILD_TYPE);
#endif
    DuskLog.info("Platform: {}", BOREALIS_PLATFORM_NAME);
}

static void mods_init(const std::filesystem::path& mods_dir) {
    // Mod search directories, highest priority first: user dir (--mods replaces it), then
    // mods/ next to the app, then install-bundled mods inside the app bundle.
    {
        std::vector<dusk::mods::ModSearchDir> modDirs;
        modDirs.push_back({.path = mods_dir});
#if TARGET_ANDROID
        // APK-bundled mods are extracted to internal storage
        // by DuskActivity before SDL_main runs.
        modDirs.push_back({
            .path = dusk::CachePath / "bundled_mods",
        });
#elif defined(__APPLE__) && (TARGET_OS_IOS || TARGET_OS_TV)
        modDirs.push_back({
            .path = dusk::data::base_path_relative("mods"),
            .inPlaceNative = true,
            .nativeLibDir = dusk::data::base_path_relative("Frameworks"),
        });
#else
#if defined(__APPLE__)
        // Base path is Contents/Resources; search up for dev mods
        // TODO: scope to non-CI builds
        modDirs.push_back({
            .path = dusk::data::base_path_relative("../../../mods").lexically_normal(),
            .inPlaceNative = true,
        });
        // Contents/Resources/mods
        modDirs.push_back({
            .path = dusk::data::base_path_relative("mods"),
            .inPlaceNative = true,
        });
#else
        modDirs.push_back({
            .path = dusk::data::base_path_relative("mods"),
            .inPlaceNative = true,
        });
#endif
#endif
        dusk::mods::ModLoader::instance().set_search_dirs(std::move(modDirs));
    }
#if TARGET_ANDROID
    // A user-relocated data dir can live on external storage, which is mounted noexec.
    // Native mod libraries must be extracted to internal storage.
    dusk::mods::ModLoader::instance().set_cache_dir(dusk::CachePath / "mod_cache");
#endif

    DuskLog.info("Initializing mods...");
    dusk::mods::ModLoader::instance().init();
}

#if defined(__SWITCH__) && ALEKS2_NATIVE_MOD_LOADER
// aleks2 (loader-only lab mode): the native mod loader without the game, to test it where the game
// itself cannot start (Ryujinx: no usable GPU device). On when sdmc:/switch/dusklight/loader-only.txt
// exists (an emulator passes no arguments) or with --loader-only; otherwise nothing here runs.
//
// It does what a boot does for mods and nothing else: boot guard and update journal, the TLS
// self-test if tls-selftest/ is there, discovery in mods/, every enabled mod.so through the real
// loader in Execute (this session only; the saved Loader Mode is untouched), imports, relocations,
// constructors, mod_initialize and the hooks it installs. Then one summary line per mod, to the
// game's log and to svcOutputDebugString (an emulator's log), and the process ends. No Aurora, no
// window, no GPU, no disc: the hooks are installed but the game code they patch never runs, so a
// mod that loads here is statically loadable on this build -- not tested, not "working on console".
static bool loader_only_requested(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr && std::string_view(argv[i]) == "--loader-only") {
            return true;
        }
    }
    std::error_code ec;
    return dusk::dev_mode() && std::filesystem::exists(dusk::ConfigPath / "loader-only.txt", ec);
}

static void loader_only_line(const std::string& line) {
    DuskLog.info("{}", line);
    dusk::sw::debug_print(line);
}

// The "tls" object of the mod's loader report (switch_native_loader.cpp writes one per mod), if any.
static std::string loader_only_tls(const std::filesystem::path& modsDir, const std::string& id) {
    std::string text;
    if (std::FILE* f = std::fopen((modsDir / ".reports" / (id + ".json")).string().c_str(), "rb")) {
        char buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) {
            text.append(buf, n);
        }
        std::fclose(f);
    } else {
        return "no report";
    }
    const auto at = text.find("\"tls\":{");
    if (at == std::string::npos) {
        return "none";
    }
    const auto end = text.find('}', at);
    return text.substr(at + 6, end == std::string::npos ? 64 : end - at - 5);
}

static int run_loader_only(const std::filesystem::path& modsDir) {
    loader_only_line(fmt::format("LOADER-ONLY lab mode, build {}: no graphics, no game code; mods in {}",
        dusk::benchmark::build_id(), modsDir.string()));
    dusk::switchelf::force_execute_this_session();
    mods_init(modsDir);
    int total = 0, loaded = 0, refused = 0, disabled = 0;
    for (const auto& mod : dusk::mods::ModLoader::instance().mods()) {
        ++total;
        const auto hooks = dusk::mods::svc::hook_counts(mod);
        const char* state = "REFUSED";
        if (!mod.is_enabled()) {
            state = "DISABLED (config)";
            ++disabled;
        } else if (mod.active) {
            state = "LOADED";
            ++loaded;
        } else {
            ++refused;
        }
        loader_only_line(fmt::format("LOADER-ONLY mod {} v{}: {} | native {} | hooks {} installed, {} callbacks, "
                                     "{} declared | TLS {}{}{}",
            mod.metadata.id, mod.metadata.version, state, mod.native ? "yes" : "no", hooks.installed,
            hooks.callbacks, hooks.declared, loader_only_tls(modsDir, mod.metadata.id),
            mod.failureReason.empty() ? "" : " | reason: ", mod.failureReason));
    }
    // The TLS self-test's verdict (switch_tls_selftest.cpp), if it ran: its last line.
    if (std::FILE* f = std::fopen((modsDir.parent_path() / "tls-selftest" / "result.txt").string().c_str(), "rb")) {
        char buf[512];
        std::string last;
        while (std::fgets(buf, sizeof buf, f) != nullptr) {
            last = buf;
        }
        std::fclose(f);
        while (!last.empty() && (last.back() == '\n' || last.back() == '\r')) {
            last.pop_back();
        }
        loader_only_line("LOADER-ONLY " + last + " (tls-selftest/result.txt)");
    }
    // DM3: the net self-test (switch_net_selftest.cpp) runs on its own thread; wait for its verdict (90 s at
    // most), then show its failures, its INFO lines and the verdict.
    if (std::filesystem::exists(modsDir.parent_path() / "net-selftest" / "net_lab.so")) {
        const auto result = (modsDir.parent_path() / "net-selftest" / "result.txt").string();
        std::vector<std::string> lines;
        for (int waited = 0; waited < 180; ++waited) {
            lines.clear();
            if (std::FILE* f = std::fopen(result.c_str(), "rb")) {
                char buf[512];
                while (std::fgets(buf, sizeof buf, f) != nullptr) {
                    std::string l = buf;
                    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                    lines.push_back(std::move(l));
                }
                std::fclose(f);
            }
            if (!lines.empty() && lines.back().rfind("NET SELFTEST RESULT", 0) == 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        int passes = 0;
        for (const auto& l : lines) {
            if (l.rfind("PASS ", 0) == 0) {
                ++passes;
            } else if (l.rfind("FAIL", 0) == 0 || l.rfind("INFO", 0) == 0 || l.rfind("layer:", 0) == 0) {
                loader_only_line("LOADER-ONLY net " + l);
            }
        }
        loader_only_line("LOADER-ONLY net " + std::to_string(passes) + " check(s) passed; " +
                         (lines.empty() ? std::string{"no result.txt"} : lines.back()) + " (net-selftest/result.txt)");
    }
    // A clean end of this boot for the boot guard: the next start is not counted as a crash.
    if (auto* guard = dusk::switchelf::boot_guard()) {
        guard->end_boot();
    }
    loader_only_line(fmt::format("LOADER-ONLY RESULT: {} mod(s): {} loaded, {} refused, {} disabled -- static "
                                 "load only: hooks installed, the game never ran; not a console test",
        total, loaded, refused, disabled));
    return 0;
}
#endif

// =========================================================================
// PC ENTRY POINT
// =========================================================================
int game_main(int argc, char* argv[]) {
    // On iOS, when connected to an external monitor, SDLUIKitSceneDelegate scene:willConnectToSession:
    // can call our main function again. Explicitly guard against this reinitialization.
    if (mainCalled) {
        return 0;
    }
    mainCalled = true;

#if defined(__SWITCH__)
    // aleks2 (DM1-c): first, so an emulator's log names the build and what it passed in.
    dusk::sw::debug_print(fmt::format("Dusklight {} starting: argc {}, argv[0] {}", dusk::benchmark::build_id(), argc,
        argv != nullptr && argc > 0 && argv[0] != nullptr ? argv[0] : "(null)"));
    if (!dusk::sw::begin_session()) {
        return 0;
    }
#endif

    cxxopts::ParseResult parsed_arg_options;
    borealis::cli::StandardOptions standardOptions;

    try {
        cxxopts::Options arg_options("Dusklight", "PC Port of a classic adventure game");

        borealis::cli::add_standard_options(arg_options);
        arg_options.add_options()
            ("h,help", "Print usage")
            ("dvd", "Path to DVD image file", cxxopts::value<std::string>())
            ("mods", "Path to mods directory", cxxopts::value<std::string>())
            ("backend", "Graphics API backend to use (auto, d3d12, d3d11, metal, vulkan, null)", cxxopts::value<std::string>())
            ("cvar", "Override configuration variables without modifying config", cxxopts::value<std::vector<std::string>>())
            ("develop", "Enable the game's developer mode and OSReport for debugging", cxxopts::value<bool>()->default_value("false")->implicit_value("true"))
            ("load-save", "Skip the opening and load a save from slot 1-3", cxxopts::value<uint8_t>()->default_value("0"))
            ("stage", "Upon launching, load a stage, room, spawn point, and layer. When using --load-save, it uses the specified save on the loaded stage. Format (STAGE,ROOM,POINT,LAYER). Example: (STAGE) or (STAGE,0,0,-1)", cxxopts::value<std::string>());

        arg_options.parse_positional({"dvd"});
        arg_options.positional_help("<dvd-image>");
        arg_options.allow_unrecognised_options();

#if defined(__SWITCH__)
        // An NRO started without hbloader (Eden/yuzu) can get an argc that counts null argv entries;
        // hbmenu always passes the NRO path, so on the console this changes nothing.
        static char kProgramName[] = "dusklight";
        static char* kFallbackArgv[] = {kProgramName, nullptr};
        int validArgc = 0;
        while (argv != nullptr && validArgc < argc && argv[validArgc] != nullptr) {
            ++validArgc;
        }
        if (validArgc == 0) {
            argc = 1;
            argv = kFallbackArgv;
        } else {
            argc = validArgc;
        }
#endif
        parsed_arg_options = arg_options.parse(argc, argv);
        standardOptions = borealis::cli::parse(parsed_arg_options);

        if (parsed_arg_options.count("help"))
        {
            printf("%s", (arg_options.help() + "\n").c_str());
            exit(0);
        }

        if (parsed_arg_options.count("stage")) {
            std::stringstream ss(parsed_arg_options["stage"].as<std::string>());
            std::string token;

            std::getline(ss,token,',');
            std::string stageName = token;
            s8 room = 0;
            s16 point = 0;
            s8 layer = -1;
            if (std::getline(ss,token,',')) {
                room = std::stoi(token);
                if (std::getline(ss,token,',')) {
                    point = std::stoi(token);
                    if (std::getline(ss,token,',')) {
                        layer = std::stoi(token);
                    }
                }
            }

            dusk::StageRequested = {stageName,true, room,point,layer};
        }
    }
    catch (const cxxopts::exceptions::exception& e) {
        fprintf(stderr, "Argument Error: %s\n", e.what());
        exit(1);
    }
    catch (const std::invalid_argument& e) {
        // Handle parsing std::stoi when loading a stage
        fprintf(stderr, "Fatal: Invalid Argument When Parsing Stage\n");
        exit(1);
    }
    catch (const std::out_of_range& e) {
        // Handle parsing std::stoi when loading a stage
        fprintf(stderr, "Fatal: Argument Out of Range In Parsing Stage\n");
        exit(1);
    }

    if (parsed_arg_options.contains("load-save")){
        uint8_t slot = parsed_arg_options["load-save"].as<uint8_t>();
        if (slot >= 1 && slot <= 3) {
            dusk::SaveRequested = slot;
        }
    }

    dusk::registerSettings();

    const auto dataPaths = dusk::data::initialize_data(standardOptions.userDir);
    dusk::ConfigPath = dataPaths.userPath;
    dusk::CachePath = dataPaths.cachePath;
    dusk::InitializeLogging(dusk::CachePath, standardOptions);
    const auto startupLogLevel = borealis::log::level();
#ifdef __SWITCH__
    dusk::pgo_dump::start();  // aleks2 (V1-PGO): nothing unless ALEKS2_PGO=GENERATE
#endif

    // Development Mode
    if (parsed_arg_options.count("develop")) {
        mDoMain::developmentMode = parsed_arg_options["develop"].as<bool>();  // Enable Dev Mode for Debugging
        dusk::OSReportReallyForceEnable = parsed_arg_options["develop"].as<bool>();  // Print OSReport to console
    }

    log_build_info();
#ifdef __SWITCH__
    dusk::session_log::installTerminateLogger();
    dusk::session_log::stampBoot(dusk::dev_mode() ? "game_main (developer mode: dev.txt)" : "game_main"); dusk::boot_timing::entry_report();
#endif

    dusk::config::load_from_user_preferences();
    // aleks2 (companion): one-shot migrations for configs written by older
    // builds -- before the cvar overrides so an explicit command-line value
    // still wins.
    dusk::runConfigMigrations();
#ifdef __SWITCH__
    dusk::session_log::stampSettings("launch");
    // aleks2: convert anything dropped into guides/import at startup. The
    // companion's own poll only runs while the left-column carousel is being
    // drawn, which is Functional-only, so in Wii U style a guide copied onto
    // the card was never picked up -- and in Functional the box that would
    // trigger it is itself hidden until a guide exists. Kicking the worker
    // here breaks that circle for both layouts; it no-ops when nothing is
    // pending and runs off the game thread either way.
    if (dusk::getSettings().game.guideEnabled.getValue()) {
        // Both of these read files written by earlier builds, so both are
        // guarded: a guide store that cannot be read is a missing companion
        // page, never a game that will not start.
        try {
            if (dusk::guide::import_has_pending()) {
                DuskLog.info("guide: pending import found, converting in the background");
                dusk::guide::begin_import();
            }
        } catch (const std::exception& e) {
            DuskLog.warn("guide: import could not be started: {}", e.what());
        } catch (...) {
            DuskLog.warn("guide: import could not be started");
        }
    }
#endif
    ApplyCVarOverrides(parsed_arg_options["cvar"]);
#if defined(__SWITCH__) && ALEKS2_NATIVE_MOD_LOADER
    // aleks2 (loader-only lab mode): before anything graphical; see run_loader_only.
    if (loader_only_requested(argc, argv)) {
        return run_loader_only(parsed_arg_options.count("mods") ?
                                   std::filesystem::path(parsed_arg_options["mods"].as<std::string>()) :
                                   dusk::ConfigPath / "mods");
    }
#endif
#ifdef __SWITCH__
    // aleks2: touchscreen + psm battery for the companion.
    dusk::switch_touch::initialize();
#endif
    borealis::sentry::Options sentryOptions{
        .release = fmt::format("{}@{}", dusk::AppInfo.appName, BOREALIS_APP_DESCRIBE),
        .databaseDirectory = dusk::CachePath / "sentry",
    };
    if (const char* logPath = borealis::log::file_path()) {
        sentryOptions.attachments.emplace_back(logPath);
    }
    borealis::sentry::initialize(sentryOptions);
#ifndef __SWITCH__
    borealis::crash::install();
#endif
    // TODO: How to handle this?
    // PADSetDefaultMapping(&defaultPadMapping, PAD_TYPE_STANDARD);

    {
        const auto mappingsPath = dusk::ConfigPath / "gamecontrollerdb.txt";
        std::error_code ec;
        if (std::filesystem::exists(mappingsPath, ec)) {
            const auto mappingsPathString = borealis::io::fs_path_to_string(mappingsPath);
            if (SDL_AddGamepadMappingsFromFile(mappingsPathString.c_str()) < 0) {
                DuskLog.warn("Failed to load gamecontrollerdb.txt from '{}': {}",
                    mappingsPathString, SDL_GetError());
            }
        } else if (ec) {
            DuskLog.warn("Failed to inspect gamecontrollerdb.txt in data folder '{}': {}",
                borealis::io::fs_path_to_string(mappingsPath), ec.message());
        }
    }

    // Set SDL metadata for audio mixers and macOS "About" menu
    SDL_SetAppMetadata("Dusklight", BOREALIS_APP_VERSION, "dev.twilitrealm.dusk");

#ifdef __SWITCH__
    // aleks2 (SHADERS-c1): cache options for Aurora must be in the environment before it
    // opens its caches; without shader-lab.ini nothing changes.
    dusk::shader_warmup::load_lab_options();
    // aleks2 (V1-c1): perf-lab.ini (GPU timing, DRS steps, stress mode); gpu_timing=off must be
    // in the environment before Aurora picks the device features.
    dusk::frame_telemetry::load_lab_options();
    dusk::session_log::stampBoot("lab options and SQLite check");
#endif
    {
        const auto userPathString = dusk::ConfigPath.u8string();
        const auto cachePathString = dusk::CachePath.u8string();
        AuroraConfig config{};
        config.appName = dusk::AppName;
        config.userPath = reinterpret_cast<const char*>(userPathString.c_str());
        config.cachePath = reinterpret_cast<const char*>(cachePathString.c_str());
#ifdef DUSK_ASSET_DIR
        config.resourcesPath = DUSK_ASSET_DIR;
#endif
        config.vsync = dusk::getSettings().video.enableVsync;
        config.startFullscreen = dusk::getSettings().video.enableFullscreen;
        config.windowPosX = -1;
        config.windowPosY = -1;

        const int lastWindowWidth = dusk::getSettings().video.lastWindowWidth.getValue();
        const int lastWindowHeight = dusk::getSettings().video.lastWindowHeight.getValue();

        if (dusk::getSettings().video.rememberWindowSize && lastWindowWidth > 0 && lastWindowHeight > 0) {
            config.windowWidth = lastWindowWidth;
            config.windowHeight = lastWindowHeight;
        } else {
            config.windowWidth = defaultWindowWidth * 2;
            config.windowHeight = defaultWindowHeight * 2;
        }

        // aleks2 (deko3d rc12): deko3d only when nothing this start needs Dawn/NVK (dusk/deko3d_policy.hpp)
        config.desiredBackend = dusk::deko3d_policy::boot_backend(ResolveDesiredBackend(parsed_arg_options));
        config.logCallback = borealis::log::aurora_callback();
        config.logLevel = borealis::log::to_aurora_level(startupLogLevel);
        config.mem1Size = 256 * 1024 * 1024;
        config.mem2Size = 24 * 1024 * 1024;
        config.allowJoystickBackgroundEvents = dusk::getSettings().game.allowBackgroundInput;
        config.pauseOnFocusLost = dusk::getSettings().game.pauseOnFocusLost;
        config.imGuiInitCallback = &aurora_imgui_init_callback;
        config.allowTextureDumps = false;
        {
            const int aniso = dusk::getSettings().game.maxTextureAnisotropy.getValue();
            config.maxTextureAnisotropy = static_cast<uint16_t>(aniso < 1 ? 1 : (aniso > 16 ? 16 : aniso));
        }
        auroraInfo = aurora_initialize(argc, argv, &config);
    }
#ifdef __SWITCH__
    // aleks2 (deko3d P1, rc12): two starts in a row with deko3d never showed a frame, so this one runs on
    // Vulkan (Dawn/NVK), and so do the next ones: the setting becomes Vulkan, not Auto (Auto is deko3d when
    // it is the default). With deko3d, one screen and the normal layout (its present has no companion
    // window and no Flip Grip rotation yet).
    if (aurora_deko3d_fell_back()) {
        DuskLog.warn("deko3d did not start twice in a row: Graphics Backend set to Vulkan");
        dusk::getSettings().backend.graphicsBackend.setValue("vulkan");
        dusk::config::save();
    }
    dusk::deko3d_policy::after_boot(auroraInfo.backend);
    if (auroraInfo.backend == BACKEND_DEKO3D) {
        // deko3d rc12: Flip Grip and the companion screen are composited by its present (lib/deko/dk_present.cpp)
        DuskLog.info("deko3d renderer");
    }
    dusk::session_log::stampBoot("aurora_initialize (GPU, caches, recipe list)");
    // The startup shader pass is queued by now: decide whether to show the screen.
    dusk::shader_warmup::begin();
#endif

    dusk::presentation::update_frame_rate_preference();

#if BOREALIS_HAS_DISCORD
    if (dusk::getSettings().game.enableDiscordPresence) {
        dusk::discord::initialize();
    }
#endif

    VISetWindowTitle(
        fmt::format("Dusklight {} [{}]", BOREALIS_APP_DESCRIBE, dusk::backend_name(auroraInfo.backend))
        .c_str());

    // H33: saved preference OR Flip Grip (which forces 4:3 without writing the preference).
    dusk::dualscreen::applyAspectPolicy();
    dusk::applyInternalResolutionScale(dusk::getSettings().game.internalResolutionScale.getValue());
    dusk::applyResampler(dusk::getSettings().game.resampler.getValue());

    dusk::audio::SetMasterVolume(dusk::audio::MasterVolumeToLinear(dusk::getSettings().audio.masterVolume / 100.0f));
    dusk::audio::SetEnableReverb(dusk::getSettings().audio.enableReverb);

    // Run ImGui UI loop if Aurora couldn't initialize a backend
    if (auroraInfo.backend == BACKEND_NULL) {
        launchUILoop();
        borealis::shutdown();
        borealis::sentry::shutdown();
        borealis::log::shutdown();
        fflush(stdout);
        fflush(stderr);
#if BOREALIS_HAS_DISCORD
        dusk::discord::shutdown();
#endif
        dusk::ui::shutdown();
        dusk::config::shutdown();
        aurora_shutdown();
        return 0;
    }

    if (dusk::getSettings().game.enableHighQualityMinimapTextures.getValue()) {
        dusk::hq_minimap::set_active(true);
    }

#ifdef __SWITCH__
    dusk::session_log::stampBoot("audio, presence, settings");
#endif
    { const auto bootT = dusk::boot_timing::now(); dusk::texture_replacements::reload(); dusk::boot_timing::phase("texture directory load", bootT); }
    { const auto bootT = dusk::boot_timing::now(); dusk::ui::initialize(); dusk::boot_timing::phase("RmlUi and fonts (ui::initialize)", bootT); }
    dusk::ui::apply_scale();
    dusk::ui::push_document(std::make_unique<dusk::ui::Overlay>(), true, true);
    dusk::ui::push_document(std::make_unique<dusk::ui::TouchControls>(), false, true);
    dusk::ui::push_document(std::make_unique<dusk::ui::MenuBar>(), false);
    dusk::ui::push_document(std::make_unique<dusk::ui::CommandConsole>(), false);

    // H33: a saved setting the console cannot show was repaired at startup (settings.cpp).
    if (auto notice = dusk::takeSettingsRepairNotice(); !notice.empty()) {
        dusk::ui::push_toast({.title = "Settings repaired", .content = notice, .duration = std::chrono::seconds(8)});
    }

    // Invalidate a bad saved isoPath so that Dusklight can't get blocked from starting up.
    // This is only a metadata check; full hash verification is handled by the prelaunch UI.
    bool forcePreLaunchUI = false;
    bool saveConfigBeforePrelaunch = false;

    borealis::io::PathAccess dvdPathAccess;
    const auto resolveDvdLocation = [&dvdPathAccess](const std::string& location) {
        dvdPathAccess = borealis::io::access_path(location);
        return dvdPathAccess ? borealis::io::fs_path_to_string(dvdPathAccess.path()) : location;
    };

    const std::string savedLocation = dusk::getSettings().backend.isoPath; const auto bootDiscT = dusk::boot_timing::now();
    dusk::iso::DiscInfo discInfo{};
    if (!savedLocation.empty() &&
        dusk::iso::inspect(savedLocation.c_str(), discInfo) != dusk::iso::ValidationError::Success)
    {
        DuskLog.warn("Saved DVD image location failed validation, clearing it: {}",
            borealis::io::display_name(savedLocation));
        dusk::getSettings().backend.isoPath.setValue("");
        dusk::getSettings().backend.isoVerification.setValue(dusk::DiscVerificationState::Unknown);
        forcePreLaunchUI = true;
        saveConfigBeforePrelaunch = true;
    } dusk::boot_timing::phase_if(!savedLocation.empty(), "disc check (saved image header)", bootDiscT);

    bool skipPreLaunchUI = dusk::getSettings().backend.skipPreLaunchUI.getValue();

    std::string dvdLocation = dusk::getSettings().backend.isoPath;
    std::string dvdPath = resolveDvdLocation(dvdLocation);
    bool dvd_opened = false;
    if (parsed_arg_options.count("dvd")) {
        dvdLocation = parsed_arg_options["dvd"].as<std::string>();
        dvdPath = resolveDvdLocation(dvdLocation);
        if (dusk::iso::inspect(dvdLocation.c_str(), discInfo) ==
            dusk::iso::ValidationError::Success)
        {
            DuskLog.info("Loading DVD image from command line: {}", dvdPath);
            dvd_opened = aurora_dvd_open(dvdPath.c_str());
            if (!dvd_opened) {
                DuskLog.warn("Failed to open DVD image from command line: {}, opening prelaunch UI",
                    dvdPath);
                forcePreLaunchUI = true;
            } else {
                dusk::getSettings().backend.isoPath.setValue(dvdLocation);
                dusk::getSettings().backend.isoVerification.setValue(
                    dusk::DiscVerificationState::Unknown);
                dusk::config::save();
                dusk::IsGameLaunched = true;
                skipPreLaunchUI = true;
            }
        } else {
            DuskLog.warn(
                "DVD image from command line failed validation: {}, opening prelaunch UI", dvdPath);
            forcePreLaunchUI = true;
        }
    }

    // If we can't load right into the game, stop requesting to load a stage or save
    if (forcePreLaunchUI || dvdPath.empty()) {
        if (dusk::StageRequested.set) {
            DuskLog.warn("Cannot load stage {} because no iso path is set, opening prelaunch UI",dusk::StageRequested.stage);
            dusk::StageRequested = {};
        }
        if (dusk::SaveRequested) {
            DuskLog.warn("Cannot load save {} because no iso path is set, opening prelaunch UI",dusk::SaveRequested);
            dusk::SaveRequested = 0;
        }
    }else if (dusk::StageRequested.set || dusk::SaveRequested) {
        skipPreLaunchUI = true;
    }

    dusk::iso::log_verification_state(
        dusk::getSettings().backend.isoPath.getValue(),
        dusk::getSettings().backend.isoVerification.getValue());

    bool showPrelaunchAfterInit = true;
    if (!dvd_opened && (dusk::getSettings().backend.isoPath.getValue().empty() || (forcePreLaunchUI && skipPreLaunchUI))) {
        showPrelaunchAfterInit = false;
    }

    if (showPrelaunchAfterInit) {
        // Force launchUILoop to not run, we know that the ISO will be loaded.
        dusk::IsGameLaunched = true;
    }

    if (!dvd_opened) {
        if (dusk::getSettings().backend.isoPath.getValue().empty()) {
            forcePreLaunchUI = true;
        }
        if (forcePreLaunchUI && skipPreLaunchUI) {
#ifdef __SWITCH__
            // aleks2 (1.0.1): show the menu for this boot only. A disc moved or swapped once used to turn the player's
            // "Skip Dusklight Main Menu" off for good, and nothing said so.
            DuskLog.warn("Prelaunch UI was disabled with no usable DVD image, showing it for this boot");
            skipPreLaunchUI = false;
#else
            DuskLog.warn("Prelaunch UI was disabled with no usable DVD image, enabling prelaunch UI");
            dusk::getSettings().backend.skipPreLaunchUI.setValue(false);
            saveConfigBeforePrelaunch = true;
#endif
        }
        if (saveConfigBeforePrelaunch) {
            dusk::config::save();
        }

        if (!skipPreLaunchUI) {
            if (!showPrelaunchAfterInit) {
                dusk::ui::push_document(std::make_unique<dusk::ui::Prelaunch>(), true);
            }

            // pre game launch ui main loop
            const auto bootMenuT = dusk::boot_timing::now(); if (!launchUILoop()) {
                borealis::shutdown();
                borealis::sentry::shutdown();
                borealis::log::shutdown();
                fflush(stdout);
                fflush(stderr);
#if BOREALIS_HAS_DISCORD
                dusk::discord::shutdown();
#endif
                dusk::ui::shutdown();
                dusk::config::shutdown();
                aurora_shutdown();
                return 0;
            } dusk::boot_timing::menu_wait(bootMenuT);
        }

        dvdLocation = dusk::getSettings().backend.isoPath;
        dvdPath = resolveDvdLocation(dvdLocation);
        if (dvdPath.empty()) {
            DuskLog.fatal("No DVD image specified, unable to boot!");
        }
        if (!dusk::IsGameLaunched && dusk::iso::inspect(dvdLocation.c_str(), discInfo) !=
                                         dusk::iso::ValidationError::Success)
        {
            DuskLog.fatal("DVD image failed validation: {}", dvdPath);
        }
        DuskLog.info("Loading DVD image: {}", dvdPath);
        const auto bootDvdT = dusk::boot_timing::now(); if (!aurora_dvd_open(dvdPath.c_str())) {
            DuskLog.fatal("Failed to open DVD image: {}", dvdPath);
        } dusk::boot_timing::phase("disc open", bootDvdT);

        dusk::IsGameLaunched = true;
    }

    dusk::version::init();
    LanguageInit();

    OSInit();

    mDoMain::sPowerOnTime = DUSK_IF_ELSE(OSGetSystemTime(), OSGetTime());

    // Reset Data
    static mDoRstData sResetData = {0};
    mDoRst::setResetData(&sResetData);
    mDoRst::offReset();
    mDoRst::setLogoScnFlag(0);

    // Global Context Init
    { const auto bootT = dusk::boot_timing::now(); dComIfG_ct(); dusk::boot_timing::phase("game globals (dComIfG_ct)", bootT); }

    mDoDvdThd::SyncWidthSound = false;

    // Apply after aurora_initialize: speedrun mode mutates cvars whose change callbacks push
    // values into aurora.
    if (dusk::getSettings().game.speedrunMode) {
        dusk::speedrun::registerSpeedrunGameMode();
    }

    if (parsed_arg_options.contains("mods") &&
            !parsed_arg_options["mods"].as<std::string>().empty())
    {
        mods_init(parsed_arg_options["mods"].as<std::string>());
    } else {
        dusk::ui::boot_splash::show();  // V1-rc19: the boot splash covers the mod loading
        { const auto bootT = dusk::boot_timing::now(); mods_init(dusk::ConfigPath / "mods"); dusk::boot_timing::phase("mods, all (mods_init)", bootT); }
        dusk::ui::boot_splash::hide();
    }
#ifdef __SWITCH__
    dusk::deko3d_policy::after_mods_loaded();
    dusk::session_log::stampBoot("disc check and mods");
#endif

    if (!skipPreLaunchUI && showPrelaunchAfterInit) {
        dusk::ui::push_document(std::make_unique<dusk::ui::Prelaunch>(), true);
    }

    if (skipPreLaunchUI == true) {
#ifdef __SWITCH__
        // aleks2 (1.0.1): Randomizer is built in, so there are always two game modes, and the menu below would show
        // on every boot: "Skip Dusklight Main Menu" never worked (josete2k, GBAtemp). With the option on, start the
        // game mode the player chose last, as Play would; the menu still shows if that mode is gone or refuses.
        bool skipIntoLastMode = false;
        if (dusk::gamemode::getGameModeManager().getRegisteredGameModes().size() > 1 &&
            dusk::getSettings().backend.skipPreLaunchUI.getValue())
        {
            auto& gameModes = dusk::gamemode::getGameModeManager();
            gameModes.setGameModeToPrevious();
            const auto* gameMode = gameModes.getCurrentGameMode();
            skipIntoLastMode = gameMode != nullptr && gameMode->invokeOnPlayFunction();
            DuskLog.info("Skip Dusklight Main Menu: game mode '{}' {}", gameMode != nullptr ? gameMode->getId() : "?",
                skipIntoLastMode ? "started" : "did not start; showing the menu");
            if (!skipIntoLastMode) {
                gameModes.setCurrentGameMode(dusk::gamemode::kVanillaGameModeId);
            }
        }
        if (!skipIntoLastMode && dusk::gamemode::getGameModeManager().getRegisteredGameModes().size() > 1 &&
            dusk::getSettings().backend.skipPreLaunchUI.getValue()) {
#else
        if (dusk::gamemode::getGameModeManager().getRegisteredGameModes().size() > 1 && dusk::getSettings().backend.skipPreLaunchUI.getValue()) {
#endif
            // Force pre-launch if we have registered gamemodes that we need to choose from
            dusk::ui::push_document(std::make_unique<dusk::ui::Prelaunch>(), true);
        } else {
            // If we get back to prelaunch later, tell it that we've already started the game
            dusk::ui::prelaunch_state().firstLaunch = false;
        }
    }

#if BOREALIS_HAS_SENTRY
    if (borealis::sentry::get_consent() == borealis::sentry::Consent::Unknown) {
        dusk::ui::push_document(std::make_unique<dusk::ui::CrashReportWindow>());
    }
#endif

    if (!dusk::getSettings().backend.wasPresetChosen) {
        dusk::ui::push_document(std::make_unique<dusk::ui::PresetWindow>());
    }

    OSReport("Starting main01 (Game Loop)...\n");

    main01();
#ifdef __SWITCH__
    dusk::gpu_boost::restore();  // aleks2 (V1-rc16): the clocks as they were before the boost, if it was on
    dusk::pgo_dump::shutdown();  // aleks2 (V1-PGO): the last profile snapshot
#endif
    borealis::shutdown();

    // We need to cleanly shut down the threads to avoid crashes on shutdown.
    if (daMP_c::m_myObj) {
        daMP_c::m_myObj->daMP_c_Finish();
    }

    borealis::sentry::shutdown();
    borealis::log::shutdown();
    fflush(stdout);
    fflush(stderr);

    mDoMch_Destroy();

    // Notifies all CVs and causes threads to exit
    OSResetSystem(OS_RESET_SHUTDOWN, 0, 0);

#if BOREALIS_HAS_DISCORD
    dusk::discord::shutdown();
#endif
    dusk::audio::Shutdown();
    dusk::ui::shutdown();
    dusk::texture_replacements::shutdown();
    dusk::config::shutdown();
    aurora_shutdown();

    return 0;
}


bool JKRHeap::dump_sort() {
    return true;
}

#ifdef __MWERKS__
template <typename T>
JHIComPortManager<T>* JHIComPortManager<T>::instance = nullptr;

template <>
JHIComPortManager<JHICmnMem>* JHIComPortManager<JHICmnMem>::instance = nullptr;

template<>
Z2WolfHowlMgr* JASGlobalInstance<Z2WolfHowlMgr>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
Z2EnvSeMgr* JASGlobalInstance<Z2EnvSeMgr>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
Z2FxLineMgr* JASGlobalInstance<Z2FxLineMgr>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
Z2Audience* JASGlobalInstance<Z2Audience>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
Z2SoundObjMgr* JASGlobalInstance<Z2SoundObjMgr>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
Z2SoundInfo* JASGlobalInstance<Z2SoundInfo>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
JAUSoundInfo* JASGlobalInstance<JAUSoundInfo>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
JAUSoundNameTable* JASGlobalInstance<JAUSoundNameTable>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
JAUSoundTable* JASGlobalInstance<JAUSoundTable>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
JAISoundInfo* JASGlobalInstance<JAISoundInfo>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
Z2SoundMgr* JASGlobalInstance<Z2SoundMgr>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
JAIStreamMgr* JASGlobalInstance<JAIStreamMgr>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
JAISeqMgr* JASGlobalInstance<JAISeqMgr>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
JAISeMgr* JASGlobalInstance<JAISeMgr>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
Z2SpeechMgr2* JASGlobalInstance<Z2SpeechMgr2>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
Z2SoundStarter* JASGlobalInstance<Z2SoundStarter>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
JAISoundStarter* JASGlobalInstance<JAISoundStarter>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
Z2StatusMgr* JASGlobalInstance<Z2StatusMgr>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
Z2SceneMgr* JASGlobalInstance<Z2SceneMgr>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
Z2SeqMgr* JASGlobalInstance<Z2SeqMgr>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
Z2SeMgr* JASGlobalInstance<Z2SeMgr>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
JASAudioThread* JASGlobalInstance<JASAudioThread>::sInstance JAS_GLOBAL_INSTANCE_INIT;

template<>
JASDefaultBankTable* JASGlobalInstance<JASDefaultBankTable>::sInstance JAS_GLOBAL_INSTANCE_INIT;
#endif // __MWERKS__
