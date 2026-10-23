#pragma once
// aleks2 (V1): whether two paths name the same file, for the mod loader's install and scan checks.
//
// std::filesystem::equivalent compares st_dev and st_ino. On the Switch the SD card's devoptab (libnx fsdev, FAT or
// exFAT) and romfs report st_ino 0 for every file, so any two existing files on the same device came out "the
// same": an update over an installed package (the file already there) found every other installed mod at its
// destination and was refused with "The destination filename belongs to a different mod" (rc14 console, Dawnlight
// 4.1.0 -> 5.0.0; a first install has no destination yet, so it never showed). There the names are compared --
// absolute, normalized, case-insensitively as FAT is. Elsewhere inodes are real and equivalent() is kept.
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>

namespace dusk::mods {

// The name comparison, on its own so it builds and is tested on a PC (platforms/switch/tools/same_path_test.cpp).
inline bool same_path_by_name(const std::filesystem::path& a, const std::filesystem::path& b) {
    auto key = [](const std::filesystem::path& p) {
        std::error_code ec;
        auto absolute = std::filesystem::absolute(p, ec);
        std::string s = (ec ? p : absolute).lexically_normal().generic_string();
        while (s.size() > 1 && s.back() == '/') {
            s.pop_back();
        }
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    return key(a) == key(b);
}

// Both must exist, as for equivalent(); `error` is left clear when either does not.
inline bool same_path(const std::filesystem::path& a, const std::filesystem::path& b, std::error_code& error) {
#ifdef __SWITCH__
    error.clear();
    if (!std::filesystem::exists(a, error) || !std::filesystem::exists(b, error)) {
        error.clear();
        return false;
    }
    return same_path_by_name(a, b);
#else
    return std::filesystem::equivalent(a, b, error);
#endif
}

}  // namespace dusk::mods
