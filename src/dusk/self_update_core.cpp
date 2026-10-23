#include "dusk/self_update_core.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace dusk::self_update {
namespace {

bool ends_with_nocase(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) {
        return false;
    }
    return std::equal(suffix.begin(), suffix.end(), s.end() - static_cast<std::ptrdiff_t>(suffix.size()),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        });
}

std::optional<std::string> sha256_of_digest(std::string_view digest) {
    constexpr std::string_view prefix = "sha256:";
    if (digest.substr(0, prefix.size()) != prefix || digest.size() != prefix.size() + 64) {
        return std::nullopt;
    }
    std::string hex{digest.substr(prefix.size())};
    for (char& c : hex) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
    }
    return hex;
}

std::filesystem::path with_suffix(const std::filesystem::path& p, const char* suffix) {
    std::filesystem::path out = p;
    out += suffix;
    return out;
}

}  // namespace

std::optional<NroAsset> pick_nro_asset(const borealis::update::Release& release, std::string& why) {
    const borealis::update::Asset* found = nullptr;
    for (const auto& asset : release.assets) {
        if (!ends_with_nocase(asset.name, ".nro")) {
            continue;
        }
        if (found != nullptr) {
            why = "the release has more than one .nro";
            return std::nullopt;
        }
        found = &asset;
    }
    if (found == nullptr) {
        why = "the release has no .nro to install from here";
        return std::nullopt;
    }
    if (found->browserDownloadUrl.rfind("https://", 0) != 0) {
        why = "the release's .nro has no HTTPS download link";
        return std::nullopt;
    }
    auto sha = sha256_of_digest(found->digest);
    if (!sha) {
        why = "the release does not publish a SHA-256 for its .nro";
        return std::nullopt;
    }
    return NroAsset{.name = found->name, .url = found->browserDownloadUrl, .sha256 = std::move(*sha)};
}

bool is_newer(std::string_view tag, std::string_view current) {
    const auto latest = borealis::update::parse_version(tag);
    const auto running = borealis::update::parse_version(current);
    return latest && running && borealis::update::compare_version(*latest, *running) > 0;
}

bool replaceable_nro_path(std::string_view argv0) {
    return argv0.substr(0, 6) == "sdmc:/" && argv0.size() > 10 && ends_with_nocase(argv0, ".nro") &&
           argv0.find("..") == std::string_view::npos;
}

std::filesystem::path part_path(const std::filesystem::path& target) { return with_suffix(target, ".part"); }
std::filesystem::path backup_path(const std::filesystem::path& target) { return with_suffix(target, ".bak"); }

std::string replace_with_backup(const std::filesystem::path& target, const std::filesystem::path& part) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(part, ec)) {
        return "the downloaded NRO is missing";
    }
    const std::filesystem::path bak = backup_path(target);
    std::filesystem::remove(bak, ec);  // an older backup; a failure shows in the rename below
    ec.clear();
    std::filesystem::rename(target, bak, ec);
    if (ec) {
        return "could not move the current NRO aside: " + ec.message();
    }
    std::filesystem::rename(part, target, ec);
    if (!ec) {
        return {};
    }
    const std::string why = ec.message();
    std::error_code back;
    std::filesystem::rename(bak, target, back);
    if (back) {
        return "could not put the new NRO in place (" + why + ") nor the previous one back (" + back.message() +
               "): rename " + bak.filename().string() + " to " + target.filename().string() + " by hand";
    }
    return "could not put the new NRO in place (" + why + "); the previous one is back";
}

bool auto_check_due(long long lastMinutes, long long nowMinutes) {
    return lastMinutes <= 0 || nowMinutes < lastMinutes || nowMinutes - lastMinutes >= 24 * 60;
}

}  // namespace dusk::self_update
