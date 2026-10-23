#pragma once
// aleks2 (V1-rc19): the extracted native mod libraries, kept on the card between boots (perf-lab.ini mod_lib_cache).
//
// rc18 on the console: of 18.6 s spent loading mods at boot, 14.4 s went to writing each mod's library back to the
// card (mods/.cache is cleared at every boot). Here a library is written once per package version:
//   <root>/<mod id>/<key>/   the files of the package's platform folder, and manifest.txt
//   key = sha256(format, the package file's sha256, the platform folder)
// The manifest is written last, and the folder is renamed into place only after it. It lists every file with its
// size and sha256, and each one is hashed again before the folder is used. Anything that does not match, or cannot
// be read, gives an empty result -- the caller extracts as before -- and a stale entry is removed. A new package
// version (an update, a reinstall of another build) has another key; prune() removes, after the boot's loads,
// every entry no package used (an older version, an uninstalled mod). Deleting the folder is always safe.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dusk::mods::lib_cache {

struct Served {
    std::filesystem::path dir;  ///< the folder to load from; empty: extract as before
    std::string note;           ///< one line for the mod's log (hit, written, or why not)
};

using ReadEntry = std::function<std::vector<uint8_t>(const std::string& entry)>;

/// `package`: the .dusk file (a directory mod gets an empty result). `platformPrefix`: "lib/<platform>/".
/// `entries`: the package's runtime entries, as the extraction filters them. `read` throws on failure.
Served serve(const std::filesystem::path& root, std::string_view modId, const std::filesystem::path& package,
    const std::string& platformPrefix, const std::vector<std::string>& entries, const ReadEntry& read);

/// A folder serve() returned this session: unloading a mod must not delete it.
bool served(const std::filesystem::path& dir);

/// Removes every entry under `root` that serve() did not return this session; how many, for the log.
size_t prune(const std::filesystem::path& root);

}  // namespace dusk::mods::lib_cache
