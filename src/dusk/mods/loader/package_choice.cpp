#include "package_choice.hpp"

namespace dusk::mods {
namespace {

constexpr std::string_view k_libDir = "lib/";

// A package whose native code is for another platform only is set behind any Switch copy of the same mod.
bool demoted(const PackageLibraries& libraries, bool preferSwitch, bool switchCopyExists) {
    return preferSwitch && switchCopyExists && libraries.native() && !libraries.switchArm64;
}

}  // namespace

PackageLibraries classify_package_libraries(std::span<const std::string> fileNames) {
    PackageLibraries result;
    for (const auto& name : fileNames) {
        const std::string_view view{name};
        if (!view.starts_with(k_libDir)) {
            continue;
        }
        const auto rest = view.substr(k_libDir.size());
        const auto platformEnd = rest.find('/');
        if (platformEnd == std::string_view::npos || platformEnd == 0) {
            continue;
        }
        const auto platform = rest.substr(0, platformEnd);
        const auto entry = rest.substr(platformEnd + 1);
        if (entry != "mod.so" && entry != "mod.dll") {
            continue;
        }
        if (platform == "switch-arm64") {
            result.switchArm64 = true;
        } else if (platform == "linux-aarch64") {
            result.linuxAarch64 = true;
        } else {
            result.other = true;
        }
    }
    return result;
}

size_t choose_package(
    std::span<const PackageRank> ranks, const bool preferSwitch, const PackageVersionCompare compare) {
    bool switchCopyExists = false;
    for (const auto& rank : ranks) {
        switchCopyExists = switchCopyExists || rank.libraries.switchArm64;
    }
    size_t selected = k_noPackage;
    for (size_t i = 0; i < ranks.size(); ++i) {
        if (selected == k_noPackage) {
            selected = i;
            continue;
        }
        const auto& candidate = ranks[i];
        const auto& current = ranks[selected];
        const bool candidateDemoted = demoted(candidate.libraries, preferSwitch, switchCopyExists);
        const bool currentDemoted = demoted(current.libraries, preferSwitch, switchCopyExists);
        if (candidateDemoted != currentDemoted) {
            if (currentDemoted) {
                selected = i;
            }
            continue;
        }
        const int order = compare(candidate.version, current.version);
        const bool samePathOrder = candidate.path != nullptr && current.path != nullptr &&
                                   candidate.searchDirIndex == current.searchDirIndex &&
                                   *candidate.path < *current.path;
        if (order > 0 || (order == 0 && (candidate.searchDirIndex < current.searchDirIndex || samePathOrder))) {
            selected = i;
        }
    }
    return selected;
}

bool refuses_non_switch_update(const PackageLibraries& incoming, std::span<const PackageLibraries> existing,
    const bool preferSwitch) {
    if (!preferSwitch || !incoming.native() || incoming.switchArm64) {
        return false;
    }
    for (const auto& libraries : existing) {
        if (libraries.switchArm64) {
            return true;
        }
    }
    return false;
}

}  // namespace dusk::mods
