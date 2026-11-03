#include "ImGuiMenuTools.hpp"

#include "ImGuiConfig.hpp"
#include "ImGuiConsole.hpp"
#include "ImGuiEngine.hpp"

#include "dusk/data.hpp"
#include "dusk/dusk.h"
#include "dusk/hotkeys.h"
#include "dusk/main.h"
#include "dusk/os.h"
#include "dusk/profiler.hpp"
#include "dusk/settings.h"
#include "dusk/speedrun.h"
#ifdef __SWITCH__
#include "dusk/frame_telemetry.hpp"
#include "dusk/gpu_boost.hpp"

#include <algorithm>
#include <cstdint>
#endif

#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_horse.h"
#include "d/d_com_inf_game.h"
#include "m_Do/m_Do_main.h"

#include <aurora/gfx.h>
// aleks2 (H37-c2): global.h, in the precompiled header, and Aurora's lib/internal.hpp both define
// POINTER_ADD_TYPE and POINTER_ADD (TYPEOF there, decltype here: the same in C++), which GCC
// reported as "POINTER_ADD redefined". Aurora's take over from this point on.
#undef POINTER_ADD
#undef POINTER_ADD_TYPE
#include <aurora/lib/internal.hpp>
#include <fmt/format.h>
#include <imgui.h>
#include <SDL3/SDL_misc.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace dusk {
#ifdef __SWITCH__
    namespace {
    // aleks2 (v1.1-perf-lab): where each presented frame goes, live -- the per-thread profiler panel other ports
    // show (LM-NX, banjo-nx; Stevens' suggestion). frame_telemetry already measures it every frame; this only keeps
    // the last kHistory frames and draws them. Series: present interval, game thread work, GX FIFO worker, render
    // worker, GPU.
    constexpr int kHistory = 180;
    struct Series {
        const char* name;
        float values[kHistory] = {};
    };
    Series g_series[] = {{"Frame (present)"}, {"Game thread"}, {"GX FIFO thread"}, {"Render worker"}, {"GPU"}};
    int g_head = 0;
    int g_filled = 0;
    std::uint64_t g_lastTick = 0;
    unsigned g_bounds[perf::kBoundCount] = {};

    void record_frame() {
        const std::uint64_t tick = frame_telemetry::ticks();
        if (tick == g_lastTick) {
            return;
        }
        g_lastTick = tick;
        const auto& f = frame_telemetry::last();
        const float v[] = {f.intervalMs, f.mainWorkMs, f.fifoBusyMs, f.renderWorkMs, f.gpuMs};
        for (int i = 0; i < 5; ++i) {
            g_series[i].values[g_head] = v[i];
        }
        g_head = (g_head + 1) % kHistory;
        g_filled = std::min(g_filled + 1, kHistory);
        ++g_bounds[static_cast<int>(frame_telemetry::last_bound())];
    }

    void show_frame_breakdown() {
        record_frame();
        ImGuiStringViewText(fmt::format("Budget {:.1f} ms (perf-lab drs_target_fps)\n", frame_telemetry::budget_ms()));
        for (const auto& s : g_series) {
            float sum = 0.f, worst = 0.f;
            int n = 0;
            for (int i = 0; i < g_filled; ++i) {
                const float x = s.values[i];
                if (x < 0.f) {
                    continue;  // no GPU figure for that frame
                }
                sum += x;
                worst = std::max(worst, x);
                ++n;
            }
            if (n == 0) {
                ImGuiStringViewText(fmt::format("{:<16} --\n", s.name));
                continue;
            }
            const int last = (g_head + kHistory - 1) % kHistory;
            ImGuiStringViewText(fmt::format("{:<16} {:6.2f} ms  avg {:6.2f}  max {:6.2f}\n", s.name,
                std::max(s.values[last], 0.f), sum / n, worst));
            ImGui::PlotLines(fmt::format("##{}", s.name).c_str(), s.values, g_filled,
                g_filled < kHistory ? 0 : g_head, nullptr, 0.f, 50.f, ImVec2(300.f, 28.f));
        }
        unsigned total = 0;
        for (const unsigned b : g_bounds) {
            total += b;
        }
        if (total != 0) {
            ImGuiStringViewText(fmt::format("Missed since shown: {} of {} (gpu {}, game {}, fifo {}, other {})\n",
                total - g_bounds[static_cast<int>(perf::Bound::Met)], total,
                g_bounds[static_cast<int>(perf::Bound::Gpu)], g_bounds[static_cast<int>(perf::Bound::CpuMain)],
                g_bounds[static_cast<int>(perf::Bound::CpuRender)], g_bounds[static_cast<int>(perf::Bound::Unknown)]));
        }
        unsigned cpu = 0, gpu = 0, emc = 0;
        gpu_boost::clocks_mhz(cpu, gpu, emc);
        if (cpu != 0) {
            ImGuiStringViewText(fmt::format("Clocks: CPU {} / GPU {} / memory {} MHz\n", cpu, gpu, emc));
        }
    }
    }  // namespace
#endif

    ImGuiMenuTools::ImGuiMenuTools() {}

    void ImGuiMenuTools::draw() {
        if (ImGui::BeginMenu("Tools")) {
            if (!dusk::IsGameLaunched) {
                ImGui::BeginDisabled();
            }

            ImGui::BeginDisabled(dusk::speedrun::isActive());

            ImGui::MenuItem("Save Editor", hotkeys::SHOW_SAVE_EDITOR, &m_showSaveEditor);
            ImGui::MenuItem("State Share", hotkeys::SHOW_STATE_SHARE, &m_showStateShare);

            ImGui::EndDisabled();

            if (!dusk::IsGameLaunched) {
                ImGui::EndDisabled();
            }

#if DUSK_CAN_OPEN_DATA_FOLDER
            ImGui::Separator();
            if (ImGui::MenuItem("Open Data Folder")) {
                data::open_data_path();
            }
#endif

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Debug")) {
            ImGui::BeginDisabled(dusk::speedrun::isActive());

            bool developmentMode = mDoMain::developmentMode == 1;
            if (ImGui::Checkbox("Development Mode", &developmentMode)) {
                mDoMain::developmentMode = developmentMode ? 1 : -1;
            }

            ImGui::Separator();

            auto& collisionView = getTransientSettings().collisionView;
            if (ImGui::BeginMenu("Graphics Settings")) {
                bool disableWaterRefraction = getSettings().game.disableWaterRefraction;
                if (ImGui::Checkbox("Disable Water Refraction", &disableWaterRefraction)) {
                    getSettings().game.disableWaterRefraction.setValue(disableWaterRefraction);
                    config::save();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Collision View")) {
                ImGui::Checkbox("Enable Terrain view", &collisionView.enableTerrainView);
                ImGui::Checkbox("Enable wireframe view", &collisionView.enableWireframe);
                ImGui::SliderFloat("Opacity##terrain", &collisionView.terrainViewOpacity, 0.0f, 100.0f);
                ImGui::SliderFloat("Draw Range", &collisionView.drawRange, 0.0f, 1000.0f);
                ImGui::Separator();
                ImGui::Checkbox("Enable Attack Collider view", &collisionView.enableAtView);
                ImGui::Checkbox("Enable Target Collider view", &collisionView.enableTgView);
                ImGui::Checkbox("Enable Push Collider view", &collisionView.enableCoView);
                ImGui::SliderFloat("Opacity##colliders", &collisionView.colliderViewOpacity, 0.0f, 100.0f);
                ImGui::EndMenu();
            }

            auto& triggerView = getTransientSettings().triggerView;
            if (ImGui::BeginMenu("Trigger View")) {
                ImGui::Checkbox("Load Zones", &triggerView.loadZones);
                ImGui::Checkbox("Event Areas", &triggerView.eventAreas);
                ImGui::Checkbox("Event Tags", &triggerView.eventTags);
                ImGui::Checkbox("Switch Areas", &triggerView.switchAreas);
                ImGui::Checkbox("Midna Stops", &triggerView.midnaStops);
                ImGui::Checkbox("Twilight Gates", &triggerView.twilightGates);
                ImGui::Checkbox("Checkpoints", &triggerView.checkpoints);
                ImGui::Checkbox("Paths", &triggerView.paths);
                ImGui::Separator();
                ImGui::Checkbox("Transform Distances", &triggerView.transformDists);
                ImGui::Checkbox("Attention Distances", &triggerView.attentionDists);
                ImGui::Checkbox("Purple Mist Avoid", &triggerView.purpleMistAvoid);
                ImGui::Checkbox("Leever Ranges", &triggerView.leevers);
                ImGui::Separator();
                ImGui::SliderFloat("Opacity##triggers", &triggerView.opacity, 0.0f, 100.0f);
                ImGui::EndMenu();
            }

            if (!dusk::IsGameLaunched) {
                ImGui::BeginDisabled();
            }

            ImGui::MenuItem("Process Management", hotkeys::SHOW_PROCESS_MANAGEMENT, &m_showProcessManagement);
#if DUSK_PROFILER_ENABLED
            ImGui::MenuItem("Debug Overlay", hotkeys::SHOW_DEBUG_OVERLAY, &m_showDebugOverlay);
#endif
            ImGui::MenuItem("Heap Viewer", hotkeys::SHOW_HEAP_VIEWER, &m_showHeapOverlay);
            ImGui::MenuItem("Player Info", hotkeys::SHOW_PLAYER_INFO, &m_showPlayerInfo);
            ImGui::MenuItem("Debug Camera", hotkeys::SHOW_DEBUG_CAMERA, &m_showCameraOverlay);
            ImGui::MenuItem("Audio Debug", hotkeys::SHOW_AUDIO_DEBUG, &m_showAudioDebug);
            ImGui::MenuItem("Bloom", nullptr, &m_showBloomWindow);
            ImGui::MenuItem("Stub Log", nullptr, &m_showStubLog);
            ImGui::MenuItem("Actor Spawner", nullptr, &m_showActorSpawner);

            if (!dusk::IsGameLaunched) {
                ImGui::EndDisabled();
            }

            ImGui::MenuItem("OSReport Force", nullptr, &OSReportReallyForceEnable);

            ImGui::EndDisabled();

            ImGui::EndMenu();
        }
    }

    // What the draw path rebuilt this frame, and why consecutive draws could not merge.
    static void ShowRebuildCounters(const AuroraStats& stats) {
        const std::pair<const char*, uint32_t> rebuilds[] = {
            {"BindGroup rebuilds", stats.bindGroupRebuilds},
            {"Pipeline rebuilds", stats.pipelineRebuilds},
            {"Uniform rebuilds", stats.uniformRebuilds},
        };
        for (const auto& [label, count] : rebuilds) {
            ImGuiStringViewText(fmt::format("{}: {}\n", label, count));
        }
        ImGuiStringViewText(fmt::format("Merge blocked (fmt/pipe/tex/uni): {}/{}/{}/{}\n",
            stats.mergeBlockedFmt, stats.mergeBlockedPipeline, stats.mergeBlockedTextures,
            stats.mergeBlockedUniformOnly));
    }

    // The CPU frame time and a row per profiler zone (see dusk/profiler.hpp).
    static void ShowProfilerZones() {
        ImGuiStringViewText(fmt::format("CPU frame: avg {:.2f}ms p95 {:.2f}ms\n",
            dusk::profiler::frameAvgMs(), dusk::profiler::frameP95Ms()));
        if (!ImGui::BeginTable("dusk_profiler", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            return;
        }
        for (const char* heading : {"scope", "avg ms", "peak ms", "calls"}) {
            ImGui::TableSetupColumn(heading);
        }
        ImGui::TableHeadersRow();
        for (int zone = 0; zone < dusk::profiler::numZones(); ++zone) {
            ImGui::TableNextRow();
            const std::string cells[] = {
                dusk::profiler::zoneName(zone),
                fmt::format("{:.2f}", dusk::profiler::zoneAvgMs(zone)),
                fmt::format("{:.2f}", dusk::profiler::zonePeakMs(zone)),
                fmt::format("{:.0f}", dusk::profiler::zoneAvgCalls(zone)),
            };
            for (const std::string& cell : cells) {
                ImGui::TableNextColumn();
                ImGuiStringViewText(cell);
            }
        }
        ImGui::EndTable();
    }

    void ImGuiMenuTools::ShowDebugOverlay() {
#if !DUSK_PROFILER_ENABLED
        return;
#endif
        if (getSettings().backend.showProfilerOverlay) {
            m_showDebugOverlay = true;
        } else if (!getSettings().backend.enableAdvancedSettings ||
                   !ImGuiConsole::CheckMenuViewToggle(ImGuiKey_F3, m_showDebugOverlay)) {
#ifdef __SWITCH__
            frame_telemetry::want_gpu_timing(frame_telemetry::kOverlay, false);
#endif
            return;
        }
#ifdef __SWITCH__
        // v1.1-perf-lab: GPU timestamps while the overlay shows its GPU line.
        frame_telemetry::want_gpu_timing(frame_telemetry::kOverlay, true);
#endif

        ImGui::PushFont(ImGuiEngine::fontMono);

        ImGuiIO& io = ImGui::GetIO();
        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav;
        if (m_debugOverlayCorner != -1) {
            SetOverlayWindowLocation(m_debugOverlayCorner);
            windowFlags |= ImGuiWindowFlags_NoMove;
        }

        ImGui::SetNextWindowBgAlpha(0.65f);
        if (ImGui::Begin("Debug Overlay", nullptr, windowFlags)) {
            ImGuiStringViewText(fmt::format(FMT_STRING("FPS: {:.2f}\n"), io.Framerate));
            if (frameUsagePct > 0.f) {
                ImGuiStringViewText(fmt::format(FMT_STRING("Frame usage: {:.1f}%\n"), frameUsagePct));
            }

            ImGui::Separator();
#ifdef __SWITCH__
            show_frame_breakdown();
            ImGui::Separator();
#endif

            ImGuiStringViewText(fmt::format(FMT_STRING("Backend: {}\n"), backend_name(aurora_get_backend())));

            ImGui::Separator();

            const auto& stats = lastFrameAuroraStats;

            ImGuiStringViewText(
                fmt::format(FMT_STRING("Queued pipelines:  {}\n"), stats.queuedPipelines));
            ImGuiStringViewText(
                fmt::format(FMT_STRING("Done pipelines:    {}\n"), stats.createdPipelines));
            ImGuiStringViewText(
                fmt::format(FMT_STRING("Draw call count:   {}\n"), stats.drawCallCount));
            ImGuiStringViewText(fmt::format(FMT_STRING("Merged draw calls: {}\n"),
                stats.mergedDrawCallCount));
            ShowRebuildCounters(stats);

            ImGui::Separator();

            ShowProfilerZones();

            // TODO: persist to config
            ShowCornerContextMenu(m_debugOverlayCorner, m_cameraOverlayCorner);
        }
        ImGui::End();

        ImGui::PopFont();
    }

    void ImGuiMenuTools::ShowPlayerInfo() {
        if (!getSettings().backend.enableAdvancedSettings ||
            !ImGuiConsole::CheckMenuViewToggle(ImGuiKey_F5, m_showPlayerInfo))
        {
            return;
        }

        ImGui::PushFont(ImGuiEngine::fontMono);

        ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav;
        if (m_playerInfoOverlayCorner != -1) {
            SetOverlayWindowLocation(m_playerInfoOverlayCorner);
            windowFlags |= ImGuiWindowFlags_NoMove;
        }

        ImGui::SetNextWindowBgAlpha(0.65f);

        if (ImGui::Begin("Player Info", nullptr, windowFlags)) {
            daAlink_c* player = (daAlink_c*)dComIfGp_getPlayer(0);
            daHorse_c* horse = dComIfGp_getHorseActor();

            double speedXzy = 0.0;
            if (player != nullptr) {
                speedXzy = sqrtf(player->speed.x * player->speed.x
                    + player->speed.z * player->speed.z
                    + player->speed.y * player->speed.y);
            }

            ImGui::Text("Global");
            ImGuiStringViewText(
                player != nullptr
                ? fmt::format("Stage: {}\n", dComIfGp_getStartStageName()) 
                : "Stage: ?\n"
            );

            ImGuiStringViewText(
                player != nullptr
                ? fmt::format("Layer: {0}\n", dComIfG_play_c::getLayerNo(0))
                : "Layer: ?\n"
            );

            ImGui::Separator();
            ImGui::Text("Link");
            ImGuiStringViewText(
                player != nullptr
                ? fmt::format("Position: {: .4f}, {: .4f}, {: .4f}\n", player->current.pos.x, player->current.pos.y, player->current.pos.z)
                : "Position: ?, ?, ?\n"
            );

            ImGuiStringViewText(
                player != nullptr
                ? fmt::format("Velocity (XYZ): {: .4f}, {: .4f}, {: .4f}\n", player->speed.x, player->speed.y, player->speed.z)
                : "Velocity (XYZ): ?, ?, ?\n"
            );

            ImGuiStringViewText(
                player != nullptr
                ? fmt::format("Speed (SpeedF): {: .4f}\n", player->speedF)
                : "Speed (SpeedF): ?\n"
            );

            ImGuiStringViewText(
                player != nullptr
                ? fmt::format("Speed (3D): {: .4f}\n", speedXzy)
                : "Speed (3D): ?\n"
            );

            ImGuiStringViewText(
                 player != nullptr
                 ? fmt::format("Angle: {0}\n", player->shape_angle.y)
                 : "Angle: ?\n"
            );

            ImGuiStringViewText(
                player != nullptr
                ? fmt::format("Room: {0}\n", fopAcM_GetRoomNo(player))
                : "Room: ?\n"
            );

            ImGuiStringViewText(
                player != nullptr
                ? fmt::format("Entry: {0}\n", dComIfGp_getStartStagePoint())
                : "Entry: ?\n"
            );

            ImGui::Separator();
            ImGui::Text("Epona");
            ImGuiStringViewText(
                horse != nullptr
                ? fmt::format("Position: {: .4f}, {: .4f}, {: .4f}\n", horse->current.pos.x, horse->current.pos.y, horse->current.pos.z)
                : "Position: ?, ?, ?\n"
            );

            ImGuiStringViewText(
                 horse != nullptr
                 ? fmt::format("Velocity (XYZ): {: .4f}, {: .4f}, {: .4f}\n", horse->speed.x, horse->speed.y, horse->speed.z)
                 : "Velocity (XYZ): ?, ?, ?\n"
            );

            ImGuiStringViewText(
                horse != nullptr
                ? fmt::format("Speed (SpeedF): {: .4f}\n", horse->speedF)
                : "Speed (SpeedF): ?\n"
            );

            ImGuiStringViewText(
                horse != nullptr
                ? fmt::format("Angle: {0}\n", horse->shape_angle.y)
                : "Angle: ?\n"
            );

            ImGuiStringViewText(
                horse != nullptr
                ? fmt::format("Room: {0}\n", fopAcM_GetRoomNo(horse))
                : "Room: ?\n"
            );

            ImGuiStringViewText(
                player != nullptr
                ? fmt::format("Saved Stage: {}\n", dComIfGs_getHorseRestartStageName())
                : "Saved Stage: ?\n"
            );

            ImGuiStringViewText(
                player != nullptr
                ? fmt::format("Saved Room: {0}\n", dComIfGs_getHorseRestartRoomNo())
                : "Saved Room: ?\n"
            );

            ShowCornerContextMenu(m_playerInfoOverlayCorner, m_debugOverlayCorner);
        }

        ImGui::End();
        ImGui::PopFont();
    }
}
