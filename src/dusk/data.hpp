#pragma once

#include <borealis/data.hpp>

#include <filesystem>
#include <string>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(_WIN32) ||                                                                             \
    (defined(__APPLE__) && !TARGET_OS_IOS && !TARGET_OS_TV && !TARGET_OS_MACCATALYST) ||           \
    (defined(__linux__) && !defined(__ANDROID__))
#define DUSK_CAN_OPEN_DATA_FOLDER 1
#else
#define DUSK_CAN_OPEN_DATA_FOLDER 0
#endif

namespace dusk::data {

using Paths = borealis::data::Paths;

borealis::data::Manager& manager();
Paths initialize_data(const std::filesystem::path& userDirectoryOverride = {});
std::filesystem::path base_path_relative(const std::filesystem::path& path);

// std::filesystem::absolute() / weakly_canonical() for paths that may be
// libnx mount-qualified. libnx addresses files as "<device>:/dir/file"
// ("sdmc:/switch/dusklight/mods/x.dusk"), which std::filesystem does not see
// as absolute -- there is no leading '/' -- so absolute() prepends the
// process's current directory and yields
// "sdmc:/switch/dusklight-naga/sdmc:/switch/dusklight/mods/x.dusk", a path
// that cannot be opened. A mount-qualified path is already absolute, so this
// returns it unchanged (normalized) and defers to the standard library
// everywhere else, including for genuinely relative paths on Switch.
std::filesystem::path absolute_path(const std::filesystem::path& path);
std::filesystem::path canonical_path(const std::filesystem::path& path, std::error_code& ec);
std::filesystem::path configured_data_path();
std::filesystem::path cache_path();
bool open_data_path();
bool set_custom_data_path(const char* path, std::string* errorOut);
bool set_custom_data_path(const std::filesystem::path& path, std::string* errorOut);
bool set_portable_data_path();
bool reset_data_path();
bool is_default_data_path();
bool is_data_path_restart_pending();
std::filesystem::path user_home_path();
std::filesystem::path normalized_display_path(const std::filesystem::path& path);
std::string abbreviated_path_string(const std::filesystem::path& path);

}  // namespace dusk::data
