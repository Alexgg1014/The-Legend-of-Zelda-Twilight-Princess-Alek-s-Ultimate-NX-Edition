// H34: the list behind the console's disc selector. See switch/disc_scan.hpp.
#include "switch/disc_scan.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <system_error>

namespace dusk::sw::disc {
namespace {

namespace fs = std::filesystem;

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(
        out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// stdio under a byte budget, counting what the container reader asked for. A read
// that would cross the budget fails, which the reader reports as a damaged image.
struct BudgetFile {
    std::FILE* handle = nullptr;
    std::uint64_t size = 0;
    std::uint64_t budget = 0;
    std::uint64_t bytesRead = 0;
    bool overBudget = false;
};

int seek64(std::FILE* file, std::uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(file, static_cast<long long>(offset), SEEK_SET);
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET);
#endif
}

std::int64_t budget_read_at(void* user, std::uint64_t offset, void* out, std::size_t length) {
    auto* file = static_cast<BudgetFile*>(user);
    if (length > file->budget - std::min(file->budget, file->bytesRead)) {
        file->overBudget = true;
        return -1;
    }
    if (seek64(file->handle, offset) != 0) {
        return -1;
    }
    const std::size_t got = std::fread(out, 1, length, file->handle);
    file->bytesRead += got;
    return static_cast<std::int64_t>(got);
}

std::uint64_t budget_length(void* user) { return static_cast<BudgetFile*>(user)->size; }

std::string normal(const std::string& path) {
    std::string out = fs::path(path).lexically_normal().generic_string();
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

}  // namespace

bool is_disc_image_name(std::string_view name) {
    if (name.starts_with("._")) {
        return false;
    }
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return false;
    }
    const std::string extension = lower(name.substr(dot));
    return std::ranges::find(kImageExtensions, std::string_view{extension}) !=
           std::end(kImageExtensions);
}

const char* status_label(HeaderStatus status) {
    switch (status) {
    case HeaderStatus::Ok: return "disc";
    case HeaderStatus::Unreadable: return "unreadable";
    case HeaderStatus::NotADisc: return "not a disc";
    case HeaderStatus::Unsupported: return "unsupported";
    case HeaderStatus::Damaged: return "damaged";
    }
    return "unknown";
}

Candidate inspect_candidate(const std::string& path, std::uint64_t maxHeaderBytes) {
    Candidate candidate;
    candidate.path = path;
    const fs::path fsPath{path};
    candidate.name = fsPath.filename().string();
    candidate.directory = fsPath.parent_path().string();

    std::error_code error;
    const auto size = fs::file_size(fsPath, error);
    if (error || !fs::is_regular_file(fsPath, error) || error) {
        candidate.header.status = HeaderStatus::Unreadable;
        candidate.header.message = "The file is not there or cannot be read.";
        return candidate;
    }
    candidate.fileSize = size;

    BudgetFile file;
    file.handle = std::fopen(path.c_str(), "rb");
    if (file.handle == nullptr) {
        candidate.header.status = HeaderStatus::Unreadable;
        candidate.header.message = "The file could not be opened.";
        return candidate;
    }
    file.size = size;
    file.budget = maxHeaderBytes;

    RawStream stream;
    stream.user = &file;
    stream.read_at = &budget_read_at;
    stream.length = &budget_length;
    try {
        candidate.header = read_logical_header(stream);
    } catch (const std::exception& exception) {
        candidate.header = {};
        candidate.header.status = HeaderStatus::Damaged;
        candidate.header.message = exception.what();
    } catch (...) {
        candidate.header = {};
        candidate.header.status = HeaderStatus::Damaged;
        candidate.header.message = "The image could not be read.";
    }
    std::fclose(file.handle);
    candidate.bytesRead = file.bytesRead;
    if (file.overBudget && candidate.header.status != HeaderStatus::Ok) {
        candidate.header.status = HeaderStatus::Damaged;
        candidate.header.message =
            "Identifying this image would need more reading than the selector allows.";
    }
    return candidate;
}

ScanResult scan(const ScanOptions& options) {
    ScanResult result;
    std::vector<std::string> seen;
    const std::string configured = options.configuredPath.empty() ?
                                       std::string{} :
                                       normal(options.configuredPath);

    for (const auto& directory : options.directories) {
        std::error_code error;
        fs::directory_iterator entries{fs::path{directory}, error};
        if (error) {
            ++result.unreadableDirectories;
            continue;
        }
        std::vector<fs::path> names;
        std::size_t examined = 0;
        for (auto it = entries; it != fs::directory_iterator{}; it.increment(error)) {
            if (error) {
                break;
            }
            if (examined == options.maxEntriesPerDirectory) {
                result.entryLimitReached = true;
                break;
            }
            ++examined;
            const auto& entry = *it;
            std::error_code typeError;
            if (!entry.is_regular_file(typeError) || typeError) {
                continue;
            }
            if (!is_disc_image_name(entry.path().filename().string())) {
                ++result.skippedByName;
                continue;
            }
            names.push_back(entry.path());
        }
        result.entriesExamined += examined;
        std::ranges::sort(names, [](const fs::path& a, const fs::path& b) {
            return lower(a.filename().string()) < lower(b.filename().string());
        });
        for (const auto& name : names) {
            const std::string path = name.string();
            const std::string key = normal(path);
            if (std::ranges::find(seen, key) != seen.end()) {
                continue;
            }
            if (result.candidates.size() == options.maxCandidates) {
                result.candidateLimitReached = true;
                break;
            }
            seen.push_back(key);
            Candidate candidate = inspect_candidate(path, options.maxHeaderBytes);
            candidate.directory = directory;
            candidate.configured = !configured.empty() && key == configured;
            result.candidates.push_back(std::move(candidate));
        }
    }

    if (!configured.empty() && std::ranges::find(seen, configured) == seen.end()) {
        Candidate candidate = inspect_candidate(options.configuredPath, options.maxHeaderBytes);
        candidate.configured = true;
        const std::string parent = normal(fs::path{options.configuredPath}.parent_path().string());
        candidate.outsideScan = std::ranges::none_of(
            options.directories, [&](const std::string& dir) { return normal(dir) == parent; });
        candidate.missing = candidate.header.status == HeaderStatus::Unreadable;
        // The configured disc goes first: it is the one the person is using.
        result.candidates.insert(result.candidates.begin(), std::move(candidate));
    }
    return result;
}

}  // namespace dusk::sw::disc
