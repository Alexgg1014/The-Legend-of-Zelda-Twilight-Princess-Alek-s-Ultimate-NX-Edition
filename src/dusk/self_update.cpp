#include "dusk/self_update.hpp"

#include "dusk/config.hpp"
#include "dusk/config_var.hpp"
#include "dusk/hash.hpp"
#include "dusk/main.h"
#include "dusk/self_update_core.hpp"
#include "dusk/settings.h"
#include "dusk/ui/ui.hpp"

#include <borealis/http.hpp>
#include <borealis/log.hpp>
#include <borealis/task.hpp>
#include <borealis/update.hpp>
#include <fmt/format.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <system_error>

#include <switch.h>

extern "C" int __system_argc;
extern "C" char** __system_argv;
extern "C" bool aleks2_relaunch_on_exit(void);  // switch_startup.cpp

namespace dusk::self_update {
namespace {

constexpr borealis::Log UpdLog{"dusk::update"};

// The port's own releases (not upstream's: AppInfo still names those for everything else).
constexpr borealis::AppInfo kPortRelease{
    .orgName = "Aleks",
    .appName = "Dusklight",
    .githubOwner = "Alexgg1014",
    .githubRepo = "The-Legend-of-Zelda-Twilight-Princess-Alek-s-Ultimate-NX-Edition",
};
constexpr const char* kResultFile = "sdmc:/switch/dusklight/update-result.txt";
constexpr std::uint64_t kSpaceMargin = 32ull << 20;
constexpr unsigned kGoodRunFrames = 3600;  // about a minute of game frames with the new NRO

State s_state = State::Idle;
std::string s_detail;
borealis::update::Release s_latest;
std::optional<NroAsset> s_asset;
std::filesystem::path s_target;  // the running NRO
borealis::Task<borealis::update::Result> s_check;
borealis::Task<borealis::http::Result> s_download;
borealis::Task<std::string> s_verify;
bool s_autoCheck = false;
bool s_booted = false;

// Kept out of UserSettings (its layout stays as it was); registered on first use, which back-fills a saved value.
struct Vars {
    config::ConfigVar<bool> includeBetas{"backend.updateIncludeBetas", false};
    config::ConfigVar<int> lastCheckMinutes{"backend.lastUpdateCheckMinutes", 0};
    Vars() {
        config::Register(includeBetas);
        config::Register(lastCheckMinutes);
    }
};
Vars& vars() {
    static Vars v;
    return v;
}

std::filesystem::path running_nro() {
    if (__system_argc < 1 || __system_argv == nullptr || __system_argv[0] == nullptr ||
        !replaceable_nro_path(__system_argv[0]))
    {
        return {};
    }
    return std::filesystem::path{__system_argv[0]};
}

void drop_download_files(const std::filesystem::path& part) {
    std::error_code ec;
    std::filesystem::remove(part, ec);
    std::filesystem::path meta = part;
    meta += ".borealis-resume.json";  // borealis' resume metadata next to a download
    std::filesystem::remove(meta, ec);
}

void fail(std::string why) {
    UpdLog.warn("Update: {}", why);
    s_detail = std::move(why);
    s_state = State::Failed;
}

long long now_minutes() {
    const std::time_t now = std::time(nullptr);
    return now > 1'600'000'000 ? static_cast<long long>(now) / 60 : 0;  // 0: the clock is not set
}

// Once per run: what the last exit did, and download leftovers.
void on_boot() {
    s_booted = true;
    s_target = running_nro();
    if (std::ifstream in{kResultFile}; in) {
        std::string line;
        std::getline(in, line);
        in.close();
        std::error_code ec;
        std::filesystem::remove(kResultFile, ec);
        if (line.rfind("ok ", 0) == 0) {
            UpdLog.info("Update: now running {} (the previous NRO is kept as .bak for a while)", line.substr(3));
            ui::push_toast({.title = "Dusklight updated",
                .content = fmt::format("Now running {}.", line.substr(3)),
                .duration = std::chrono::seconds(6)});
        } else if (!line.empty()) {
            UpdLog.warn("Update: the last install failed: {}", line);
            ui::push_toast({.type = "warning",
                .title = "The update was not installed",
                .content = Rml::String{line},
                .duration = std::chrono::seconds(10)});
        }
    }
    if (!s_target.empty()) {
        drop_download_files(part_path(s_target));
    }
}

void start_check(bool automatic) {
    if (s_state == State::Checking || s_state == State::Downloading || s_state == State::Verifying ||
        s_state == State::Ready)
    {
        return;
    }
    if (!borealis::http::available()) {
        fail("there is no network support in this build");
        return;
    }
    s_autoCheck = automatic;
    s_state = State::Checking;
    s_detail = "Checking for updates...";
    borealis::update::Options options;
    options.includePrereleases = vars().includeBetas.getValue();
    options.timeout = std::chrono::seconds(15);
    s_check = borealis::update::check_latest_github_release(kPortRelease, options);
}

void take_check() {
    if (!s_check || !s_check.ready()) {
        return;
    }
    std::optional<borealis::update::Result> result;
    try {
        result = s_check.try_take();
    } catch (const std::exception& e) {
        result = borealis::update::Result{.message = e.what()};
    }
    s_check = {};
    if (!result || result->status == borealis::update::Status::Failed ||
        result->status == borealis::update::Status::Disabled)
    {
        // An automatic check that fails says nothing beyond the log and the prelaunch line.
        fail(fmt::format("could not check for updates: {}", result ? result->message : "no result"));
        return;
    }
    s_latest = std::move(result->latest);
    if (result->status != borealis::update::Status::UpdateAvailable ||
        !is_newer(s_latest.tagName, BOREALIS_APP_DESCRIBE))
    {
        s_state = State::UpToDate;
        s_detail = fmt::format("Dusklight {} is the latest version.", BOREALIS_APP_DESCRIBE);
        UpdLog.info("Update: up to date ({} is the latest; this is {})", s_latest.tagName, BOREALIS_APP_DESCRIBE);
        return;
    }
    std::string why;
    s_asset = pick_nro_asset(s_latest, why);
    if (!s_asset || s_target.empty()) {
        s_state = State::ManualOnly;
        s_detail = fmt::format("{} is available. Install it by hand from the release page on GitHub ({}).",
            latest_label(), s_target.empty() ? "this NRO was not started from the SD card" : why);
    } else {
        s_state = State::Available;
        s_detail = fmt::format("{} is available ({}).", latest_label(), s_asset->name);
    }
    UpdLog.info("Update: {} available ({})", s_latest.tagName, s_detail);
    if (s_autoCheck) {
        ui::push_toast({.title = "Dusklight update available",
            .content = fmt::format("{} is out. Settings, Dusklight Update.", latest_label()),
            .duration = std::chrono::seconds(6)});
    }
}

std::string verify_file(const std::filesystem::path& path, const std::string& expected,
    borealis::TaskContext& context) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return "could not open the download";
    }
    hash::Sha256 hash;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    std::uint64_t done = 0;
    while (input) {
        if (context.cancel_requested()) {
            return "canceled";
        }
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto count = input.gcount();
        if (count > 0) {
            hash.update(std::span{buffer.data(), static_cast<std::size_t>(count)});
            done += static_cast<std::uint64_t>(count);
            context.report_progress(done);
        }
    }
    if (!input.eof()) {
        return "could not read the download";
    }
    const std::string actual = hash.finish();
    if (actual != expected) {
        return fmt::format("the download does not match the published SHA-256 (got {}, expected {})", actual,
            expected);
    }
    return {};
}

bool sd_nearly_full() {
    std::error_code ec;
    const auto space = std::filesystem::space(s_target.parent_path(), ec);
    std::error_code ec2;
    const auto size = std::filesystem::file_size(s_target, ec2);
    return !ec && !ec2 && space.available < size + kSpaceMargin;
}

void take_download() {
    if (!s_download || !s_download.ready()) {
        return;
    }
    std::optional<borealis::http::Result> result;
    try {
        result = s_download.try_take();
    } catch (const std::exception& e) {
        result = borealis::http::Result{.error = borealis::http::Error::Network, .message = e.what()};
    }
    s_download = {};
    const auto part = part_path(s_target);
    if (!result || result->error != borealis::http::Error::None || result->response.statusCode != 200) {
        drop_download_files(part);
        const std::string why = !result ? "no result"
                                : result->error != borealis::http::Error::None
                                    ? result->message
                                    : fmt::format("HTTP {}", result->response.statusCode);
        fail(fmt::format("the download failed: {}{}", why,
            sd_nearly_full() ? " (the SD card is nearly full)" : ""));
        return;
    }
    s_state = State::Verifying;
    s_detail = "Checking the download...";
    s_verify = borealis::spawn([part, expected = s_asset->sha256](borealis::TaskContext& context) {
        return verify_file(part, expected, context);
    });
}

void take_verify() {
    if (!s_verify || !s_verify.ready()) {
        return;
    }
    std::string error;
    try {
        if (auto r = s_verify.try_take()) {
            error = std::move(*r);
        }
    } catch (const std::exception& e) {
        error = e.what();
    }
    s_verify = {};
    if (!error.empty()) {
        drop_download_files(part_path(s_target));
        fail(std::move(error));
        return;
    }
    std::error_code ec;
    std::filesystem::path meta = part_path(s_target);
    meta += ".borealis-resume.json";
    std::filesystem::remove(meta, ec);
    s_state = State::Ready;
    s_detail = fmt::format("{} is downloaded and checked. It is installed when Dusklight closes: save, then "
                           "Restart now.",
        latest_label());
    UpdLog.info("Update: {} downloaded and its SHA-256 matches; installed at exit", s_latest.tagName);
    ui::push_toast({.title = "Update ready",
        .content = "Installed when Dusklight closes. Save, then Restart now (Settings, Dusklight Update).",
        .duration = std::chrono::seconds(8)});
}

}  // namespace

void update() {
    if (!s_booted) {
        on_boot();
        maybe_auto_check();  // also when the main menu is skipped
    }
    take_check();
    take_download();
    take_verify();
}

void game_frame() {
    static unsigned frames = 0;
    if (frames > kGoodRunFrames || ++frames <= kGoodRunFrames || s_target.empty()) {
        return;
    }
    std::error_code ec;
    const auto bak = backup_path(s_target);
    if (std::filesystem::exists(bak, ec) && std::filesystem::remove(bak, ec)) {
        UpdLog.info("Update: the new NRO ran fine; the previous one ({}) was removed", bak.filename().string());
    }
}

void maybe_auto_check() {
    if (!s_booted) {
        on_boot();
    }
    if (!getSettings().backend.checkForUpdates.getValue() || s_state != State::Idle) {
        return;
    }
    const long long now = now_minutes();
    if (now != 0 && !auto_check_due(vars().lastCheckMinutes.getValue(), now)) {
        return;
    }
    if (now != 0) {
        vars().lastCheckMinutes.setValue(static_cast<int>(now));
        config::save();
    }
    start_check(true);
}

void check_now() {
    if (!s_booted) {
        on_boot();
    }
    start_check(false);
}

void install() {
    if (s_state != State::Available || !s_asset || s_target.empty()) {
        ui::push_toast({.title = "Nothing to install",
            .content = Rml::String{s_state == State::ManualOnly ? s_detail : "Check for updates first."},
            .duration = std::chrono::seconds(5)});
        return;
    }
    const auto part = part_path(s_target);
    drop_download_files(part);
    if (sd_nearly_full()) {
        fail("not enough free space on the SD card for the new NRO");
        return;
    }
    s_state = State::Downloading;
    s_detail = fmt::format("Downloading {}...", s_asset->name);
    UpdLog.info("Update: downloading {} from {}", s_asset->name, s_asset->url);
    s_download = borealis::http::start({
        .url = s_asset->url,
        .headers = {{.name = "User-Agent", .value = borealis::user_agent(kPortRelease)},
            {.name = "Accept", .value = "application/octet-stream"}},
        .downloadTo = part,
        .connectTimeout = std::chrono::seconds(15),
        .idleTimeout = std::chrono::seconds(30),
    });
}

void restart_now() {
    if (s_state != State::Ready) {
        ui::push_toast({.title = "Nothing to install yet",
            .content = "Download an update first.",
            .duration = std::chrono::seconds(5)});
        return;
    }
    if (aleks2_relaunch_on_exit()) {
        IsRunning = false;
    } else {
        ui::push_toast({.title = "Close Dusklight by hand",
            .content = "The update is installed when Dusklight closes: quit it, then open it again.",
            .duration = std::chrono::seconds(6)});
    }
}

State state() { return s_state; }

config::ConfigVar<bool>& include_betas() { return vars().includeBetas; }

std::string latest_label() {
    std::string_view tag = s_latest.tagName;
    if (!tag.empty() && tag.front() == 'v') {
        tag.remove_prefix(1);
    }
    return std::string{tag};
}

std::string status_value() {
    switch (s_state) {
    case State::Idle: return "Not checked";
    case State::Checking: return "Checking...";
    case State::UpToDate: return "Up to date";
    case State::Available: return latest_label() + " available";
    case State::ManualOnly: return latest_label() + " (by hand)";
    case State::Downloading: {
        const auto p = s_download.progress();
        if (p.total && *p.total > 0) {
            return fmt::format("Downloading {}%", p.completed * 100 / *p.total);
        }
        return fmt::format("Downloading {} MB", p.completed >> 20);
    }
    case State::Verifying: return "Checking...";
    case State::Ready: return "Ready: restart";
    case State::Failed: return "Failed";
    }
    return {};
}

std::string status_detail() {
    if (s_state == State::Downloading) {
        return s_detail + " " + status_value().substr(12);
    }
    return s_detail;
}

}  // namespace dusk::self_update

extern "C" void aleks2_apply_pending_update(void) {
    using namespace dusk::self_update;
    if (s_state != State::Ready || s_target.empty()) {
        return;
    }
    // romfs reads from the running NRO and keeps its file open: close it first. Nothing reads romfs after the game
    // loop ends (userAppExit's romfsExit then finds nothing to unmount).
    romfsExit();
    const std::string error = replace_with_backup(s_target, part_path(s_target));
    if (std::FILE* f = std::fopen(kResultFile, "w")) {
        if (error.empty()) {
            std::fprintf(f, "ok %s\n", s_latest.tagName.c_str());
        } else {
            std::fprintf(f, "%s\n", error.c_str());
        }
        std::fclose(f);
    }
}
