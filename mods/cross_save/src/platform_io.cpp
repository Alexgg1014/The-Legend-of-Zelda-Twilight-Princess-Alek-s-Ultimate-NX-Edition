#include "platform_io.hpp"

#include "../core/sha256.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace crosssave::io {
namespace {

#if defined(_WIN32)
// Paths from the host are UTF-8; Windows' narrow APIs would read them in the ANSI code page.
std::wstring wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string narrow(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size > 0 ? size - 1 : 0), '\0');
    if (size > 1) {
        WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    }
    return out;
}
#endif

std::FILE* open_file(const std::string& path, const char* mode) {
#if defined(_WIN32)
    return _wfopen(wide(path).c_str(), mode[0] == 'r' ? L"rb" : L"wb");
#else
    return std::fopen(path.c_str(), mode);
#endif
}

}  // namespace

std::string join(const std::string& directory, const std::string& name) {
    if (directory.empty()) {
        return name;
    }
    const char last = directory.back();
    return last == '/' || last == '\\' ? directory + name : directory + "/" + name;
}

std::string parent(const std::string& path) {
    std::string trimmed = path;
    while (trimmed.size() > 1 && (trimmed.back() == '/' || trimmed.back() == '\\')) {
        trimmed.pop_back();
    }
    const auto slash = trimmed.find_last_of("/\\");
    return slash == std::string::npos ? std::string{} : trimmed.substr(0, slash);
}

std::optional<std::vector<uint8_t>> read_file(const std::string& path, size_t maxBytes) {
    std::FILE* file = open_file(path, "rb");
    if (file == nullptr) {
        return std::nullopt;
    }
    std::vector<uint8_t> bytes;
    uint8_t buffer[16384];
    while (true) {
        const size_t read = std::fread(buffer, 1, sizeof(buffer), file);
        if (read == 0) {
            break;
        }
        if (bytes.size() + read > maxBytes) {
            std::fclose(file);
            return std::nullopt;
        }
        bytes.insert(bytes.end(), buffer, buffer + read);
    }
    // No ferror(): the Switch image does not export it. A short read cannot slip through anyway:
    // every file read here is checked afterwards (JSON parse, snapshot digest, host validation).
    std::fclose(file);
    return bytes;
}

bool rename_replace(const std::string& from, const std::string& to) {
#if defined(_WIN32)
    return MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

bool rename_dir(const std::string& from, const std::string& to) {
#if defined(_WIN32)
    return MoveFileW(wide(from).c_str(), wide(to).c_str()) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

bool write_file_atomic(const std::string& path, const void* data, size_t size) {
    const std::string temporary = path + ".tmp";
    std::FILE* file = open_file(temporary, "wb");
    if (file == nullptr) {
        return false;
    }
    const bool written = size == 0 || std::fwrite(data, 1, size, file) == size;
    const bool flushed = std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!written || !flushed || !closed || !rename_replace(temporary, path)) {
        remove_file(temporary);
        return false;
    }
    return true;
}

bool remove_file(const std::string& path) {
#if defined(_WIN32)
    return DeleteFileW(wide(path).c_str()) != 0;
#else
    return std::remove(path.c_str()) == 0;
#endif
}

bool remove_dir(const std::string& path) {
#if defined(_WIN32)
    return RemoveDirectoryW(wide(path).c_str()) != 0;
#else
    return std::remove(path.c_str()) == 0;
#endif
}

bool make_dir(const std::string& path) {
#if defined(_WIN32)
    return CreateDirectoryW(wide(path).c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(path.c_str(), 0755) == 0 || is_dir(path);
#endif
}

bool is_dir(const std::string& path) {
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(wide(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    // opendir rather than stat: struct stat differs between glibc and newlib, and the Switch
    // runs this library through the glibc bridge.
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return false;
    }
    closedir(dir);
    return true;
#endif
}

bool is_file(const std::string& path) {
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(wide(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    if (is_dir(path)) {
        return false;
    }
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    std::fclose(file);
    return true;
#endif
}

std::vector<std::string> list_dir(const std::string& path) {
    std::vector<std::string> names;
#if defined(_WIN32)
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW(wide(join(path, "*")).c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) {
        return names;
    }
    do {
        const std::string name = narrow(entry.cFileName);
        if (name != "." && name != "..") {
            names.push_back(name);
        }
    } while (FindNextFileW(find, &entry));
    FindClose(find);
#else
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return names;
    }
    while (const dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") {
            names.push_back(name);
        }
    }
    closedir(dir);
#endif
    return names;
}

int64_t wall_ms() {
    return static_cast<int64_t>(std::time(nullptr)) * 1000;
}

int64_t monotonic_ms() {
    static int64_t last = 0;
    const int64_t now = wall_ms();
    if (now > last) {
        last = now;
    }
    return last;
}

std::string utc_stamp(bool forFilename) {
    const std::time_t now = std::time(nullptr);
    std::tm value{};
#if defined(_WIN32)
    gmtime_s(&value, &now);
#else
    gmtime_r(&now, &value);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), forFilename ? "%Y%m%dT%H%M%SZ" : "%Y-%m-%dT%H:%M:%SZ", &value);
    return buffer;
}

Random::Random(const std::string& seedFile) {
    Sha256 hasher;
    const int64_t now = wall_ms();
    hasher.update(std::string_view{reinterpret_cast<const char*>(&now), sizeof(now)});
    const void* addresses[] = {this, &now, reinterpret_cast<const void*>(&std::fopen)};
    hasher.update(std::string_view{reinterpret_cast<const char*>(addresses), sizeof(addresses)});
    if (auto previous = read_file(seedFile, 64)) {
        hasher.update(*previous);
    }
    const auto seed = hasher.finish();
    std::memcpy(m_state, seed.data(), sizeof(m_state));
    // Next start mixes this one in, so two starts in the same second still differ.
    Sha256 next;
    next.update(std::string_view{reinterpret_cast<const char*>(m_state), sizeof(m_state)});
    next.update("next-seed");
    const auto stored = next.finish();
    write_file_atomic(seedFile, stored.data(), stored.size());
}

uint64_t Random::next() {
    Sha256 hasher;
    hasher.update(std::string_view{reinterpret_cast<const char*>(m_state), sizeof(m_state)});
    ++m_counter;
    hasher.update(std::string_view{reinterpret_cast<const char*>(&m_counter), sizeof(m_counter)});
    const auto block = hasher.finish();
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value = (value << 8) | block[i];
    }
    return value;
}

std::string platform_name(const std::string& dataDir) {
#if defined(_WIN32)
    (void)dataDir;
    return "Windows";
#elif defined(__ANDROID__)
    (void)dataDir;
    return "Android";
#elif defined(__APPLE__)
    (void)dataDir;
    return "macOS";
#else
    return dataDir.rfind("sdmc:", 0) == 0 ? "Switch" : "Linux";
#endif
}

}  // namespace crosssave::io
