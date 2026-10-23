#pragma once

#include "package_choice.hpp"

#include "dusk/mod_loader.hpp"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dusk::mods {

struct PackageCandidate {
    std::filesystem::path path;
    ModMetadata metadata;
    uint32_t searchDirIndex = 0;
    bool fromDirectory = false;
    bool symlink = false;
    // aleks2 (H38): which native libraries the package carries; what the Switch preference looks at.
    PackageLibraries libraries;
};

std::vector<PackageCandidate> scan_packages(std::span<const ModSearchDir> searchDirs);
int compare_package_versions(std::string_view lhs, std::string_view rhs);
// aleks2 (H38): on the Switch build a package with a switch-arm64 library is chosen over a newer one that only has
// code for another platform (see package_choice.hpp); everywhere else the highest version wins as before.
const PackageCandidate* select_package(
    std::span<const PackageCandidate> packages, std::string_view modId);
const PackageCandidate* select_package(
    std::span<const PackageCandidate> packages, std::string_view modId, bool preferSwitch);
bool prefers_switch_packages();
PackageLibraries classify_package_libraries(ModBundle& bundle);
void record_package_sources(LoadedMod& mod, std::span<const PackageCandidate> packages);

std::unique_ptr<ModBundle> load_bundle(const std::filesystem::path& modPath, bool fromDir);

struct PackageInstallResult {
    bool replaced = false;
    std::string error;
    // aleks2 (V1-rc19): the installed package is open (its mod is loaded) and Horizon will not replace an open
    // file: the new one waits as <destination>.pending and is put in place at the next start
    // (apply_pending_packages). Counts as installed; the change applies after a restart.
    bool pending = false;
};

PackageInstallResult install_package(const std::filesystem::path& path,
    const std::filesystem::path& destination, std::string_view modId);

// aleks2 (V1-rc19): at start, before any package is opened (and before the update journal looks at them): every
// <name>.dusk.pending in `userDir` replaces <name>.dusk. A failure is logged and tried again at the next start.
void apply_pending_packages(const std::filesystem::path& userDir);

}  // namespace dusk::mods
