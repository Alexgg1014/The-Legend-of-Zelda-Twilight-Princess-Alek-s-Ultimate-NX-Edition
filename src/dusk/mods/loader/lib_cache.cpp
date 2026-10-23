#include "lib_cache.hpp"

#include "dusk/hash.hpp"
#include "dusk/utilities.hpp"

#include <fmt/format.h>

#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <set>
#include <span>

namespace dusk::mods::lib_cache {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kFormat = "dusk-libcache 1";
std::set<fs::path> s_served;

bool sha256_file(const fs::path& path, std::string& hex, uint64_t& size) {
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    hash::Sha256 sha;
    std::vector<uint8_t> buffer(256 * 1024);
    size = 0;
    bool ok = true;
    for (;;) {
        const size_t n = std::fread(buffer.data(), 1, buffer.size(), f);
        if (n != 0) {
            sha.update({buffer.data(), n});
            size += n;
        }
        if (n < buffer.size()) {
            ok = std::ferror(f) == 0;
            break;
        }
    }
    std::fclose(f);
    if (ok) {
        hex = sha.finish();
    }
    return ok;
}

std::string sha256_text(std::string_view text) {
    hash::Sha256 sha;
    sha.update({reinterpret_cast<const uint8_t*>(text.data()), text.size()});
    return sha.finish();
}

bool write_whole_file(const fs::path& path, std::span<const uint8_t> data) {
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fflush(f) == 0 && ok;
    ok = std::fclose(f) == 0 && ok;
    return ok;
}

// Whether `dir` holds exactly this package's files, each with its recorded size and sha256.
bool matches(const fs::path& dir, const std::string& packageSha, const std::string& prefix, size_t expectedFiles) {
    std::ifstream manifest{dir / "manifest.txt"};
    if (!manifest) {
        return false;
    }
    std::string format, package, prefixLine, line;
    std::getline(manifest, format);
    std::getline(manifest, package);
    std::getline(manifest, prefixLine);
    if (format != kFormat || package != "package " + packageSha || prefixLine != "prefix " + prefix) {
        return false;
    }
    size_t files = 0;
    while (std::getline(manifest, line)) {
        if (line.empty()) {
            continue;
        }
        char sha[65] = {};
        unsigned long long size = 0;
        int consumed = 0;
        if (std::sscanf(line.c_str(), "file %llu %64s %n", &size, sha, &consumed) != 2 || consumed <= 0) {
            return false;
        }
        const std::string name = line.substr(static_cast<size_t>(consumed));
        std::string actual;
        uint64_t actualSize = 0;
        if (!utils::is_safe_resource_path(name) || !sha256_file(dir / fs::path{name}, actual, actualSize) ||
            actualSize != size || actual != sha)
        {
            return false;
        }
        ++files;
    }
    return files == expectedFiles;
}

}  // namespace

Served serve(const fs::path& root, std::string_view modId, const fs::path& package, const std::string& platformPrefix,
    const std::vector<std::string>& runtimeEntries, const ReadEntry& read) {
    std::error_code ec;
    if (!fs::is_regular_file(package, ec)) {
        return {};  // a directory mod: its files are used in place
    }
    std::vector<std::string> entries;
    for (const auto& entry : runtimeEntries) {
        if (!entry.starts_with(platformPrefix)) {
            continue;
        }
        if (!utils::is_safe_resource_path(std::string_view{entry}.substr(platformPrefix.size()))) {
            return {};  // the extraction refuses it, with its own message
        }
        entries.push_back(entry);
    }
    if (entries.empty()) {
        return {};
    }
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto since = [](Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); };
    std::string packageSha;
    uint64_t packageSize = 0;
    if (!sha256_file(package, packageSha, packageSize)) {
        return {{}, "lib cache: cannot read the package to hash it; extracting as before"};
    }
    const std::string key =
        sha256_text(fmt::format("{}\n{}\n{}\n", kFormat, packageSha, platformPrefix)).substr(0, 32);
    const fs::path dir = root / utils::safe_filename(modId) / key;
    const double hashSeconds = since(start);

    std::string stale;
    if (fs::exists(dir, ec)) {
        if (matches(dir, packageSha, platformPrefix, entries.size())) {
            s_served.insert(dir);
            return {dir, fmt::format("lib cache: hit, {} file(s) checked, not extracted (hash {:.2f} s, check {:.2f} s)",
                             entries.size(), hashSeconds, since(start) - hashSeconds)};
        }
        fs::remove_all(dir, ec);
        stale = "; the entry there did not match and was replaced";
    }

    // A miss: into a temporary folder, the manifest last, then renamed into place.
    const fs::path tmp = fs::path{dir.string() + ".tmp"};
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    if (ec) {
        return {{}, fmt::format("lib cache: cannot create its folder ({}); extracting as before", ec.message())};
    }
    std::string manifest = fmt::format("{}\npackage {}\nprefix {}\n", kFormat, packageSha, platformPrefix);
    for (const auto& entry : entries) {
        const std::string name = entry.substr(platformPrefix.size());
        const fs::path out = tmp / fs::path{name};
        fs::create_directories(out.parent_path(), ec);
        std::vector<uint8_t> data;
        try {
            data = read(entry);
        } catch (const std::exception& e) {
            fs::remove_all(tmp, ec);
            return {{}, fmt::format("lib cache: cannot read {} ({}); extracting as before", entry, e.what())};
        }
        if (ec || !write_whole_file(out, data)) {
            fs::remove_all(tmp, ec);
            return {{}, fmt::format("lib cache: cannot write {}; extracting as before", name)};
        }
        hash::Sha256 sha;
        sha.update({data.data(), data.size()});
        manifest += fmt::format("file {} {} {}\n", data.size(), sha.finish(), name);
    }
    if (!write_whole_file(tmp / "manifest.txt", {reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size()})) {
        fs::remove_all(tmp, ec);
        return {{}, "lib cache: cannot write its manifest; extracting as before"};
    }
    fs::rename(tmp, dir, ec);
    if (ec) {
        const std::string why = ec.message();
        fs::remove_all(tmp, ec);
        return {{}, fmt::format("lib cache: cannot put its folder in place ({}); extracting as before", why)};
    }
    s_served.insert(dir);
    return {dir, fmt::format("lib cache: written, {} file(s), {:.2f} s (hash {:.2f} s){}", entries.size(),
                     since(start), hashSeconds, stale)};
}

bool served(const fs::path& dir) {
    return !dir.empty() && s_served.contains(dir);
}

size_t prune(const fs::path& root) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return 0;
    }
    size_t removed = 0;
    std::vector<fs::path> modDirs;
    for (const auto& entry : fs::directory_iterator{root, ec}) {
        modDirs.push_back(entry.path());
    }
    for (const auto& modDir : modDirs) {
        std::error_code inner;
        if (!fs::is_directory(modDir, inner)) {
            fs::remove(modDir, inner);
            continue;
        }
        std::vector<fs::path> keys;
        for (const auto& entry : fs::directory_iterator{modDir, inner}) {
            keys.push_back(entry.path());
        }
        for (const auto& key : keys) {
            if (!s_served.contains(key)) {
                std::error_code rm;
                fs::remove_all(key, rm);
                ++removed;
            }
        }
        if (fs::is_empty(modDir, inner)) {
            fs::remove(modDir, inner);
        }
    }
    return removed;
}

}  // namespace dusk::mods::lib_cache
