#pragma once
// H34: the list behind the console's disc selector.
//
// The console has no file picker (borealis_file_select_switch.cpp), so the disc
// selector looks in a few known folders instead. This is that look, and nothing
// else: no UI, no settings, no game headers, so the host test can drive it.
//
// What it promises, because the selector runs it while the menu is on screen:
//
//   - Only the folders it is given, one level deep. It never walks the card.
//   - Only file names with an extension the reader opens (.iso .gcm .rvz .wia .gcz);
//     anything else is skipped without being opened.
//   - At most `maxEntriesPerDirectory` directory entries looked at per folder and
//     `maxCandidates` images listed, so a folder full of files cannot stall it.
//   - Each image is identified by its *logical* disc header through the same
//     container reader the game uses (disc_header.hpp), under a byte budget: an image
//     is never read or decompressed in full to be listed. `bytesRead` reports what it
//     cost, and a container that would need more than `maxHeaderBytes` is reported as
//     damaged instead of being read further.
//   - The configured path is always in the list, flagged, even when it lives outside
//     the scanned folders or no longer exists, so the selector can show it without
//     anybody changing it.
#include "switch/disc_header.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dusk::sw::disc {

/// File-name extensions the selector lists. They are the formats disc_container.cpp
/// reads; matching is case-insensitive.
inline constexpr std::string_view kImageExtensions[] = {".iso", ".gcm", ".rvz", ".wia", ".gcz"};

/// True when `name`'s extension is one of kImageExtensions. AppleDouble companions
/// ("._game.iso", which macOS leaves on FAT cards) are not images and return false.
bool is_disc_image_name(std::string_view name);

struct ScanOptions {
    std::vector<std::string> directories;  ///< scanned non-recursively, in this order
    std::string configuredPath;            ///< always listed when not empty
    std::size_t maxEntriesPerDirectory = 256;
    std::size_t maxCandidates = 64;
    std::uint64_t maxHeaderBytes = 16ull * 1024 * 1024;
};

struct Candidate {
    std::string path;       ///< as it would be stored in backend.isoPath
    std::string name;       ///< file name only
    std::string directory;  ///< the folder it was found in (or the configured path's)
    std::uint64_t fileSize = 0;
    LogicalHeader header;     ///< status Unreadable when the file is missing
    std::uint64_t bytesRead = 0;  ///< what identifying it read from the file
    bool configured = false;      ///< this is backend.isoPath
    bool outsideScan = false;     ///< configured, and not in any scanned folder
    bool missing = false;         ///< configured, and the file is not there
};

struct ScanResult {
    std::vector<Candidate> candidates;
    std::size_t entriesExamined = 0;
    std::size_t skippedByName = 0;     ///< entries whose extension is not a disc image
    bool entryLimitReached = false;    ///< a folder had more entries than the limit
    bool candidateLimitReached = false;
    std::size_t unreadableDirectories = 0;  ///< missing folders are normal, not errors
};

/// Identifies one file under the byte budget. Never throws.
Candidate inspect_candidate(const std::string& path, std::uint64_t maxHeaderBytes);

/// Lists the images in `options.directories`. Never throws; a missing or unreadable
/// folder is counted and skipped.
ScanResult scan(const ScanOptions& options);

/// The selector's one-word label for a header status.
const char* status_label(HeaderStatus status);

}  // namespace dusk::sw::disc
