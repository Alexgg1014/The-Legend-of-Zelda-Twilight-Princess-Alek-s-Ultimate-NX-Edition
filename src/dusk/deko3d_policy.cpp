#include "dusk/deko3d_policy.hpp"

#include "dusk/mod_loader.hpp"
#include "dusk/settings.h"
#include "dusk/ui/ui.hpp"

#include <borealis/log.hpp>
#include <fmt/format.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace dusk::deko3d_policy {
namespace detail {
bool g_running = false;
}

namespace {

constexpr borealis::Log Log{"dusk::deko3d"};
// One line per mod: "<id>\t<name>". Written with deko3d running, read at every start.
constexpr const char* kNeedsDawnPath = "sdmc:/switch/dusklight/cache/needs_dawn.txt";
constexpr const char* kRestartHint = "Settings > Graphics Backend > Restart now.";
// rc12: Auto = deko3d whenever nothing on the list below needs Vulkan (Dawn/NVK). One switch for the whole
// choice: the build's ALEKS2_DEKO3D_DEFAULT (cmake/SwitchOptions.cmake, ON in rc12). Off, Auto is Vulkan again
// and deko3d is "Experimental", with no code change.
#if defined(AURORA_GFX_DEKO3D) && ALEKS2_DEKO3D_DEFAULT
constexpr bool kAutoPrefersDeko3d = true;
#else
constexpr bool kAutoPrefersDeko3d = false;
#endif
// deko3d rc12: its present composites Flip Grip and the companion screen (aux_window_switch.cpp's work).
constexpr bool kDeko3dDrawsCompanion = true;

struct NeedsDawnMod {
    std::string id;
    std::string name;
};

// Why this start did not use deko3d although it was picked (empty: it was not, or nothing stopped it).
std::string s_bootReason;
bool s_bootReasonMods = false;
std::vector<std::string> s_noted;

std::vector<NeedsDawnMod> read_needs_dawn() {
    std::vector<NeedsDawnMod> mods;
    std::ifstream in{kNeedsDawnPath};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const auto tab = line.find('\t');
        NeedsDawnMod mod{.id = line.substr(0, tab)};
        mod.name = tab == std::string::npos ? mod.id : line.substr(tab + 1);
        mods.push_back(std::move(mod));
    }
    return mods;
}

std::string names_of(const std::vector<NeedsDawnMod>& mods) {
    std::string out;
    for (const auto& mod : mods) {
        out += out.empty() ? "" : ", ";
        out += mod.name;
    }
    return out;
}

bool deko3d_picked() {
    const auto& id = static_cast<const std::string&>(getSettings().backend.graphicsBackend);
    return id == "deko3d" || (kAutoPrefersDeko3d && id == "auto");
}

// What stops deko3d right now, in words for the player (empty: nothing).
std::string blocker(bool& fromMods) {
    fromMods = false;
    // deko3d rc12: Dual Screen HUD and Flip Grip are drawn by deko3d's present too, so they no longer need Vulkan.
    if (!kDeko3dDrawsCompanion && getSettings().game.dualScreen.getValue()) {
        return "Dual Screen HUD is on (deko3d draws one screen with the normal layout)";
    }
    if (const auto mods = read_needs_dawn(); !mods.empty()) {
        fromMods = true;
        return fmt::format("{} draw{} with the gfx service, which deko3d does not run yet", names_of(mods),
            mods.size() == 1 ? "s" : "");
    }
    return {};
}

void toast(std::string title, std::string content) {
    ui::push_toast({.title = std::move(title), .content = std::move(content), .duration = std::chrono::seconds(8)});
}

}  // namespace

bool deko3d_default() noexcept {
    return kAutoPrefersDeko3d;
}

AuroraBackend boot_backend(AuroraBackend desired) {
    const bool wanted = desired == BACKEND_DEKO3D || (kAutoPrefersDeko3d && desired == BACKEND_AUTO);
    if (!wanted) {
        return desired;
    }
    bool fromMods = false;
    std::string reason = blocker(fromMods);
    if (reason.empty()) {
        return BACKEND_DEKO3D;
    }
    Log.warn("deko3d {}, but this start uses Vulkan: {}", desired == BACKEND_DEKO3D ? "picked" : "is the default",
        reason);
    // Auto with Dual Screen HUD on is the normal case: no message. The rest says why.
    if (desired == BACKEND_DEKO3D || fromMods) {
        s_bootReason = std::move(reason);
        s_bootReasonMods = fromMods;
    }
    return BACKEND_AUTO;  // Aurora's Auto on Switch: Vulkan (Dawn/NVK)
}

void after_boot(AuroraBackend started) {
    detail::g_running = started == BACKEND_DEKO3D;
    if (!s_bootReason.empty()) {
        toast("Running on Vulkan",
            fmt::format("{}. {}", s_bootReason,
                s_bootReasonMods ? "turn those mods off and restart to use deko3d."
                                 : "turn it off and restart to use deko3d."));
    }
}

void after_mods_loaded() {
    const auto mods = read_needs_dawn();
    if (mods.empty()) {
        return;
    }
    std::vector<NeedsDawnMod> kept;
    for (const auto& mod : mods) {
        const auto* loaded = mods::ModLoader::instance().find_mod(mod.id);
        if (loaded != nullptr && loaded->is_enabled()) {
            kept.push_back(mod);
        }
    }
    if (kept.size() == mods.size()) {
        return;
    }
    std::error_code ec;
    if (kept.empty()) {
        std::filesystem::remove(kNeedsDawnPath, ec);
        Log.info("needs_dawn.txt removed: none of its mods is enabled any more ({})", names_of(mods));
        if (deko3d_picked() && s_bootReasonMods) {
            toast("deko3d on the next start", fmt::format("The mods that needed Vulkan are off. {}", kRestartHint));
        }
        return;
    }
    std::ofstream out{kNeedsDawnPath, std::ios::trunc};
    for (const auto& mod : kept) {
        out << mod.id << '\t' << mod.name << '\n';
    }
    Log.info("needs_dawn.txt: {} still enabled", names_of(kept));
}

void on_dual_screen_enabled() {
    if (detail::g_running && !kDeko3dDrawsCompanion) {
        toast("Restart for Dual Screen HUD",
            fmt::format("deko3d draws one screen for now: the next start uses Vulkan for it. {}", kRestartHint));
    }
}

void on_backend_chosen(AuroraBackend backend) {
    if (backend != BACKEND_DEKO3D) {
        return;
    }
    bool fromMods = false;
    if (const std::string reason = blocker(fromMods); !reason.empty()) {
        toast("deko3d will not start yet", fmt::format("{}: Vulkan is used until that changes.", reason));
    }
}

void note_gfx_mod(std::string_view modId, std::string_view modName, const char* what) {
    if (!detail::g_running) {
        return;
    }
    for (const auto& id : s_noted) {
        if (id == modId) {
            return;
        }
    }
    s_noted.emplace_back(modId);
    for (const auto& mod : read_needs_dawn()) {
        if (mod.id == modId) {
            return;  // already on the list (an earlier start)
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path{kNeedsDawnPath}.parent_path(), ec);
    std::ofstream out{kNeedsDawnPath, std::ios::app};
    out << modId << '\t' << modName << '\n';
    Log.warn("[{}] {} through the gfx service: not run with deko3d; the next start uses Vulkan", modId, what);
    toast(fmt::format("{} needs Vulkan: restart?", modName),
        fmt::format("It draws with the gfx service, which deko3d does not run yet. From the next start on, Vulkan is used. {}",
            kRestartHint));
}

}  // namespace dusk::deko3d_policy
