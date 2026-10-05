#pragma once

// aleks2 (H37-c5): path handling for libnx mount-qualified paths ("sdmc:/switch/dusklight/..."), with no
// dependency on the rest of the game so a host test can compile it alone.
//
// std::filesystem on libnx does not see "sdmc:/x" as absolute (no leading '/'), so absolute() puts the
// process's current directory in front of it: "sdmc:/switch/dusklight-h37-c4/sdmc:/switch/dusklight/...".
// Everything here treats a "<device>:/..." path as already absolute and falls back to the standard
// library for any other path.

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

namespace dusk::mount_path {

// "sdmc:", "romfs:", "save:" ... -- a device name followed by ":/" before any other separator. Checked on the
// string because path::root_name() is empty for these on a POSIX-flavoured implementation.
inline bool is_mount_qualified(const std::filesystem::path& path) {
    const auto& text = path.native();
    const auto colon = text.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size() || text[colon + 1] != '/') {
        return false;
    }
    return text.find('/') > colon;
}

inline bool is_absolute(const std::filesystem::path& path) {
    return path.is_absolute() || is_mount_qualified(path);
}

// A mount-qualified path is returned normalized; any other path is made absolute against `cwd`
// (std::filesystem::absolute's rule, with the directory given instead of read from the process).
inline std::filesystem::path absolute_from(const std::filesystem::path& path, const std::filesystem::path& cwd) {
    if (is_mount_qualified(path)) {
        return path.lexically_normal();
    }
    if (path.is_absolute()) {
        return path;
    }
    return is_mount_qualified(cwd) ? (cwd / path).lexically_normal() : cwd / path;
}

inline std::filesystem::path absolute(const std::filesystem::path& path) {
    if (is_mount_qualified(path)) {
        return path.lexically_normal();
    }
    return std::filesystem::absolute(path);
}

// absolute() + lexically_normal(), reporting failure through `ec` instead of throwing.
inline std::filesystem::path normalized_absolute(const std::filesystem::path& path, std::error_code& ec) {
    ec.clear();
    if (is_mount_qualified(path)) {
        return path.lexically_normal();
    }
    auto result = std::filesystem::absolute(path, ec);
    return ec ? std::filesystem::path{} : result.lexically_normal();
}

inline std::filesystem::path canonical(const std::filesystem::path& path, std::error_code& ec) {
    if (is_mount_qualified(path)) {
        ec.clear();
        return path.lexically_normal();
    }
    return std::filesystem::weakly_canonical(path, ec);
}

// The persistent directory of a mod: <config>/mod_data/<id>, as an absolute path whose mount prefix
// appears exactly once.
inline std::filesystem::path persistent_data_dir(
    const std::filesystem::path& configPath, std::string_view modId, std::error_code& ec) {
    return normalized_absolute(configPath / "mod_data" / std::filesystem::path(modId), ec);
}

inline bool is_below(const std::filesystem::path& path, const std::filesystem::path& directory) {
    const auto [directoryEnd, pathPosition] =
        std::mismatch(directory.begin(), directory.end(), path.begin(), path.end());
    return directoryEnd == directory.end() && pathPosition != path.end();
}

inline bool is_at_or_below(const std::filesystem::path& path, const std::filesystem::path& directory) {
    const auto [directoryEnd, pathPosition] =
        std::mismatch(directory.begin(), directory.end(), path.begin(), path.end());
    (void)pathPosition;
    return directoryEnd == directory.end();
}

// Where an HTTP download may be written: strictly inside the mod's scratch directory or its persistent
// data directory, never inside <scratch>/downloads (the staging area), whatever ".." the path carries.
// Returns the normalized path, or nullopt. An empty-path request (no file) is the caller's business.
inline std::optional<std::filesystem::path> validate_download_path(
    const std::filesystem::path& supplied, const std::filesystem::path& modDir,
    const std::filesystem::path& dataRoot) {
    if (!is_absolute(supplied)) {
        return std::nullopt;
    }
    std::error_code ec;
    const auto path = normalized_absolute(supplied, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto modRoot = normalized_absolute(modDir, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto data = normalized_absolute(dataRoot, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto stagingRoot = (modRoot / "downloads").lexically_normal();
    if ((!is_below(path, modRoot) && !is_below(path, data)) || is_at_or_below(path, stagingRoot)) {
        return std::nullopt;
    }
    return path;
}

}  // namespace dusk::mount_path
