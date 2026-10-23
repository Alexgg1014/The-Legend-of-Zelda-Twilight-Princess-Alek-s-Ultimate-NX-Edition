#pragma once

// aleks2 (V1-rc8): the parts of the in-app updater (self_update.hpp) that need no console: which asset of a release
// is the NRO and its published hash, whether a tag is newer, and putting a downloaded NRO in place of the running one.
// Host-tested (platforms/switch/tools/test-self-update.sh).

#include <borealis/update.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace dusk::self_update {

struct NroAsset {
    std::string name;
    std::string url;
    std::string sha256;  // lowercase hex
};

// The release's .nro and its SHA-256 as GitHub publishes it (the asset's "sha256:<64 hex>" digest). nullopt, with
// `why`, when there is no .nro (a release that needs more than the NRO ships only its .zip: installed by hand), more
// than one, or no digest to check it against.
std::optional<NroAsset> pick_nro_asset(const borealis::update::Release& release, std::string& why);

// True when `tag` is a newer version than `current` (semver precedence; false when either does not parse).
bool is_newer(std::string_view tag, std::string_view current);

// True for the path of an NRO on the SD card ("sdmc:/....nro"): the only thing the updater replaces.
bool replaceable_nro_path(std::string_view argv0);

std::filesystem::path part_path(const std::filesystem::path& target);    // the download, next to the NRO
std::filesystem::path backup_path(const std::filesystem::path& target);  // the previous NRO, kept until a good run

// Puts `part` in place of `target`, the previous one kept as backup_path(target) (an older backup is dropped).
// Horizon's rename does not replace an existing file, hence two renames; if the second fails the first is undone.
// Returns "" or what went wrong (and where the previous NRO is, if it could not be put back).
std::string replace_with_backup(const std::filesystem::path& target, const std::filesystem::path& part);

// The automatic check runs at most once a day: true when `lastMinutes` (minutes since the epoch, 0 = never) is 24 h
// or more before `nowMinutes`, or after it (the clock went back).
bool auto_check_due(long long lastMinutes, long long nowMinutes);

}  // namespace dusk::self_update
