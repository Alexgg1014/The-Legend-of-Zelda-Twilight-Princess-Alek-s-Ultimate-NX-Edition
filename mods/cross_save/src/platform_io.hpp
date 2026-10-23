#pragma once

// Small file, clock and randomness helpers with the narrowest libc surface: C stdio, rename/remove
// and time(). The same package runs on PC, Android and, through the linux-aarch64 bridge, on
// Switch, where clock_gettime's clock IDs and std::filesystem are not safe to rely on (glibc and
// newlib disagree; see docs/investigaciones/dm3-diseno-red.md).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace crosssave::io {

std::string join(const std::string& directory, const std::string& name);
std::string parent(const std::string& path);
std::optional<std::vector<uint8_t>> read_file(const std::string& path, size_t maxBytes);
// Writes path.tmp, then renames it over path (MoveFileEx with replace on Windows).
bool write_file_atomic(const std::string& path, const void* data, size_t size);
bool rename_replace(const std::string& from, const std::string& to);
// Renames a directory; the destination must not exist.
bool rename_dir(const std::string& from, const std::string& to);
bool remove_file(const std::string& path);
bool remove_dir(const std::string& path);  // empty directories only
bool make_dir(const std::string& path);    // one level; true if it exists
bool is_dir(const std::string& path);
bool is_file(const std::string& path);
std::vector<std::string> list_dir(const std::string& path);  // names, without "." and ".."
// UTC, "20261016T213000Z" for file names or "2026-10-16T21:30:00Z".
std::string utc_stamp(bool forFilename);

// Seconds from time(), as milliseconds, never going backwards within a process. One-second
// resolution is plenty for debounce and backoff.
int64_t monotonic_ms();
int64_t wall_ms();

// A SHA-256 based generator seeded from the clock, addresses and a seed file kept in the data
// directory (refreshed every start). Good for snapshot IDs and jitter; not a key generator.
class Random {
public:
    explicit Random(const std::string& seedFile);
    uint64_t next();

private:
    uint8_t m_state[32];
    uint64_t m_counter = 0;
};

// "Switch", "Android", "Windows", "macOS" or "Linux". The Switch runs the linux-aarch64 build, so it
// is recognized by its "sdmc:" data directory.
std::string platform_name(const std::string& dataDir);

}  // namespace crosssave::io
