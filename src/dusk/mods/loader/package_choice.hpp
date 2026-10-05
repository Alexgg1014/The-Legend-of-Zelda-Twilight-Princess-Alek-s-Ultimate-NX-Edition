#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

// aleks2 (H38): which of several packages with the same mod id the loader runs, and whether a new one may be installed.
// Only std is used here, so the rules are checked on the host (platforms/switch/tools/test-switch-package-preference.sh).
// It decides which package to try; it never says a package is safe -- the ELF policy and the Dry Run still decide that.
namespace dusk::mods {

// The native libraries a package carries: lib/<platform>/mod.so (or mod.dll), one level under lib/, as natives.cpp finds them.
struct PackageLibraries {
    bool switchArm64 = false;
    bool linuxAarch64 = false;
    bool other = false;

    [[nodiscard]] bool native() const { return switchArm64 || linuxAarch64 || other; }
    bool operator==(const PackageLibraries&) const = default;
};

// Zip entries and the lexical paths of a directory bundle (ModBundle::getFileNames) both come out the same way here.
PackageLibraries classify_package_libraries(std::span<const std::string> fileNames);

// What the choice between packages of one mod id looks at.
struct PackageRank {
    std::string_view version;
    uint32_t searchDirIndex = 0;
    const std::filesystem::path* path = nullptr;
    PackageLibraries libraries;
};

using PackageVersionCompare = int (*)(std::string_view lhs, std::string_view rhs);

inline constexpr size_t k_noPackage = static_cast<size_t>(-1);

// The package to load among `ranks` (all one mod id), as an index, or k_noPackage when it is empty.
// With preferSwitch, a package with a switch-arm64 library beats one whose native code is for another platform: the
// highest version among the Switch copies wins, and only when there is none does the plain highest version (and the
// Linux fallback) apply. A package with no native code (assets, script runtimes) is never demoted. Equal versions keep
// the lower search directory, then the lower path.
size_t choose_package(std::span<const PackageRank> ranks, bool preferSwitch, PackageVersionCompare compare);

// True when installing `incoming` over a mod that already has the `existing` packages must be refused: it has native
// code but none for switch-arm64 while a Switch copy exists, so it would only shadow a copy that works.
bool refuses_non_switch_update(
    const PackageLibraries& incoming, std::span<const PackageLibraries> existing, bool preferSwitch);

inline constexpr std::string_view k_nonSwitchUpdateMessage =
    "This update has no switch-arm64 library; the compatible Switch version was kept.";

}  // namespace dusk::mods
