#include "packages.hpp"

#include "loader.hpp"
#include "manifest.hpp"
#include "same_path.hpp"  // V1: equivalent() is wrong on the Switch (st_ino 0)

#include "dusk/data.hpp"
#include "dusk/hash.hpp"

#include <borealis/io.hpp>
#include <borealis/log.hpp>
#include <borealis/update.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <cstdio>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace dusk::mods {
namespace {

constexpr borealis::Log Log{"dusk::mods::loader"};

#if ALEKS2_NATIVE_MOD_LOADER
// V1-rc5: our switch-arm64 packages from before rc4 (1.0.0-beta.1 and beta.2), built against headers whose game
// data sat 0x20 bytes off upstream's (abi_parity.py). They load and then crash this build: a tester's rc4 kept
// beta.2's twilit_essentials.dusk on the card and died four times in dMsgObject_c::getStatus on a NULL the mod
// thought was not. Known by size first, so only a file of exactly that size is hashed.
struct StalePackage {
    uintmax_t size;
    std::string_view sha256;
    std::string_view what;
};
constexpr StalePackage kStalePackages[] = {
    {11116308, "6e160c870f56e6b23aa1678ed3d8da80a0cf0f953a83ef29d89eaa18099c6437",
        "Twilit Essentials 2.5.0 from 1.0.0-beta.1/beta.2"},
    {6483388, "d3ae42115b62c1e240754091a433d74c3756ef03f4a7de06d748bbe4bfce39aa",
        "Randomizer 1.0.6 from 1.0.0-beta.1/beta.2"},
};

std::string file_sha256(const fs::path& path) {
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (f == nullptr) {
        return {};
    }
    hash::Sha256 sha;
    std::vector<uint8_t> buffer(1u << 20);
    for (size_t n; (n = std::fread(buffer.data(), 1, buffer.size(), f)) > 0;) {
        sha.update(std::span<const uint8_t>{buffer.data(), n});
    }
    std::fclose(f);
    return sha.finish();
}

// The stale package's description, or empty.
std::string_view stale_package(const fs::directory_entry& entry) {
    std::error_code error;
    const auto size = entry.file_size(error);
    if (error) {
        return {};
    }
    for (const auto& stale : kStalePackages) {
        if (stale.size == size && file_sha256(entry.path()) == stale.sha256) {
            return stale.what;
        }
    }
    return {};
}
#endif

}  // namespace

std::unique_ptr<ModBundle> load_bundle(const fs::path& modPath, bool fromDir) {
    if (fromDir) {
        return std::make_unique<ModBundleDisk>(modPath);
    } else {
        return std::make_unique<ModBundleZip>(modPath);
    }
}

int compare_package_versions(std::string_view lhs, std::string_view rhs) {
    const auto left = borealis::update::parse_version(lhs);
    const auto right = borealis::update::parse_version(rhs);
    if (left && right) {
        return borealis::update::compare_version(*left, *right);
    }
    return static_cast<int>(left.has_value()) - static_cast<int>(right.has_value());
}

std::vector<PackageCandidate> scan_packages(std::span<const ModSearchDir> searchDirs) {
    std::vector<PackageCandidate> packages;
    for (size_t dirIndex = 0; dirIndex < searchDirs.size(); ++dirIndex) {
        const auto& searchDir = searchDirs[dirIndex];
        std::error_code error;
        bool alreadyScanned = false;
        for (size_t earlier = 0; earlier < dirIndex && !alreadyScanned; ++earlier) {
            alreadyScanned = dusk::mods::same_path(searchDirs[earlier].path, searchDir.path, error);
        }
        if (alreadyScanned || !fs::is_directory(searchDir.path)) {
            continue;
        }
        std::vector<fs::directory_entry> entries;
        for (const auto& entry : fs::directory_iterator{searchDir.path}) {
            if ((entry.is_directory() && fs::exists(entry.path() / "mod.json")) ||
                (entry.is_regular_file() && entry.path().extension() == ".dusk"))
            {
                entries.push_back(entry);
            }
        }
        std::ranges::sort(entries, {}, &fs::directory_entry::path);
        for (const auto& entry : entries) {
#if ALEKS2_NATIVE_MOD_LOADER
            if (entry.is_regular_file()) {
                if (const auto what = stale_package(entry); !what.empty()) {
                    // Moved aside, like a replaced package: a bundled or newer copy is chosen instead.
                    const auto aside = searchDir.path / ".replaced" / (entry.path().filename().string() + ".pre-rc4");
                    std::error_code moveError;
                    fs::create_directories(aside.parent_path(), moveError);
                    fs::rename(entry.path(), aside, moveError);
                    Log.warn("{} is {}, built for an older Dusklight: it would crash this build, so it is not "
                             "loaded{}. Copy the one from this release into sdmc:/switch/dusklight/mods/.",
                        data::abbreviated_path_string(entry.path()), what,
                        moveError ? "" : fmt::format(" (moved to {})", data::abbreviated_path_string(aside)));
                    continue;
                }
            }
#endif
            try {
                const bool fromDirectory = entry.is_directory();
                auto bundle = load_bundle(entry.path(), fromDirectory);
                packages.push_back({
                    .path = data::absolute_path(entry.path()),
                    .metadata = load_manifest(entry.path(), *bundle).metadata,
                    .searchDirIndex = static_cast<uint32_t>(dirIndex),
                    .fromDirectory = fromDirectory,
                    .symlink = entry.is_symlink(),
                    .libraries = classify_package_libraries(*bundle),
                });
            } catch (const std::exception& exception) {
                Log.error("bad mod package {}: {}",
                    data::abbreviated_path_string(entry.path()), exception.what());
            }
        }
    }
    return packages;
}

PackageLibraries classify_package_libraries(ModBundle& bundle) {
    const auto names = bundle.getFileNames();
    return classify_package_libraries(std::span<const std::string>{names});
}

bool prefers_switch_packages() {
#if ALEKS2_NATIVE_MOD_LOADER
    return true;
#else
    return false;
#endif
}

const PackageCandidate* select_package(std::span<const PackageCandidate> packages,
    std::string_view modId) {
    return select_package(packages, modId, prefers_switch_packages());
}

const PackageCandidate* select_package(std::span<const PackageCandidate> packages,
    std::string_view modId, const bool preferSwitch) {
    std::vector<const PackageCandidate*> same;
    std::vector<PackageRank> ranks;
    for (const auto& package : packages) {
        if (package.metadata.id != modId) {
            continue;
        }
        same.push_back(&package);
        ranks.push_back({
            .version = package.metadata.version,
            .searchDirIndex = package.searchDirIndex,
            .path = &package.path,
            .libraries = package.libraries,
        });
    }
    const size_t index = choose_package(ranks, preferSwitch, &compare_package_versions);
    return index == k_noPackage ? nullptr : same[index];
}

void record_package_sources(LoadedMod& mod, std::span<const PackageCandidate> packages) {
    mod.hasUserPackage = false;
    mod.hasBundledCopy = false;
    for (const auto& package : packages) {
        if (package.metadata.id != mod.metadata.id) {
            continue;
        }
        if (package.searchDirIndex != 0) {
            mod.hasBundledCopy = true;
        } else if (!package.fromDirectory && !package.symlink) {
            mod.hasUserPackage = true;
        }
    }
}

PackageInstallResult install_package(const fs::path& path, const fs::path& destination,
    std::string_view modId) {
    const auto userDir = destination.parent_path();
    std::error_code error;
    std::string validationError;
    const auto destinationStatus = fs::symlink_status(destination, error);
    if (error == std::errc::no_such_file_or_directory) {
        error.clear();
    }
    if (error) {
        return {.replaced = false,
            .error = fmt::format("Could not inspect the destination: {}", error.message())};
    }
    if (fs::exists(destinationStatus)) {
        ModMetadata existingMetadata;
        if (!fs::is_regular_file(destinationStatus) ||
            !inspect_mod_bundle(destination, existingMetadata, validationError) ||
            existingMetadata.id != modId)
        {
            return {
                .replaced = false,
                .error = "The destination filename is already used by another package",
            };
        }
    }
    std::vector<fs::path> duplicates;
    for (fs::directory_iterator entry{userDir, error}, end; !error && entry != end;
        entry.increment(error))
    {
        const auto& candidate = entry->path();
        if (candidate.extension() != ".dusk") {
            continue;
        }
        const bool regularFile = entry->is_regular_file(error);
        if (error) {
            break;
        }
        if (!regularFile) {
            continue;
        }
        ModMetadata candidateMetadata;
        if (inspect_mod_bundle(candidate, candidateMetadata, validationError) &&
            candidateMetadata.id == modId)
        {
            duplicates.push_back(candidate);
        }
    }
    if (error) {
        return {.replaced = false,
            .error = fmt::format("Could not scan the mods directory: {}", error.message())};
    }

    std::string replaceError;
    if (!borealis::io::atomic_replace(path, destination, replaceError)) {
#if ALEKS2_NATIVE_MOD_LOADER
        // aleks2 (V1-rc19): the installed package is open -- its mod is loaded and reads from it -- and Horizon
        // refuses to replace or delete an open file (rc18: "Failed to remove the file being replaced: File exists",
        // Dawnlight 5.0.0 -> 5.1.0). The new package waits next to it and goes in place at the next start, before
        // anything opens it; the running mod keeps its own package until then.
        std::error_code pendingError;
        if (fs::exists(destination, pendingError)) {
            const fs::path pending{destination.string() + ".pending"};
            fs::remove(pending, pendingError);
            pendingError.clear();
            fs::rename(path, pending, pendingError);
            if (!pendingError) {
                Log.info("{} could not be replaced now ({}); the new package waits as {} and is put in place at "
                         "the next start",
                    data::abbreviated_path_string(destination), replaceError,
                    data::abbreviated_path_string(pending));
                return {.replaced = true, .error = {}, .pending = true};
            }
            Log.warn("could not keep the new package as {}: {}", data::abbreviated_path_string(pending),
                pendingError.message());
        }
#endif
        return {
            .replaced = false,
            .error = std::move(replaceError),
        };
    }

    std::string cleanupError;
    for (const auto& duplicate : duplicates) {
        // Replacing a file on a case-insensitive filesystem can preserve its old spelling.
        const bool destinationAlias =
            dusk::mods::same_path(duplicate, destination, error) && !fs::is_symlink(duplicate, error);
        error.clear();
        if (destinationAlias) {
            if (duplicate.filename() == destination.filename()) {
                continue;
            }
            fs::rename(duplicate, destination, error);
        } else {
            fs::remove(duplicate, error);
        }
        if (error) {
            cleanupError = fmt::format("Installed, but could not consolidate '{}': {}",
                data::abbreviated_path_string(duplicate), error.message());
            Log.warn("{}", cleanupError);
        }
    }
    return {.replaced = true, .error = std::move(cleanupError)};
}

#if ALEKS2_NATIVE_MOD_LOADER
void apply_pending_packages(const fs::path& userDir) {
    std::error_code error;
    std::vector<fs::path> pending;
    for (fs::directory_iterator entry{userDir, error}, end; !error && entry != end; entry.increment(error)) {
        const auto& candidate = entry->path();
        if (candidate.extension() == ".pending" && candidate.stem().extension() == ".dusk") {
            pending.push_back(candidate);
        }
    }
    for (const auto& file : pending) {
        const fs::path target = file.parent_path() / file.stem();  // <name>.dusk
        std::string replaceError;
        if (borealis::io::atomic_replace(file, target, replaceError)) {
            Log.info("pending update put in place: {}", data::abbreviated_path_string(target));
        } else {
            Log.warn("pending update {} not put in place ({}); tried again at the next start",
                data::abbreviated_path_string(file), replaceError);
        }
    }
}
#endif

}  // namespace dusk::mods
