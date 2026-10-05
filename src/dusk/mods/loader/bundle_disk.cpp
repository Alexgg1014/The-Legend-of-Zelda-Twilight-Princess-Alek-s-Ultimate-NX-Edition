#include <utility>

#include "dusk/io.hpp"
#include <borealis/io.hpp>
#include "loader.hpp"

namespace fs = std::filesystem;

namespace dusk::mods {
ModBundleDisk::ModBundleDisk(fs::path root) : root_path(std::move(root)) {}

std::vector<u8> ModBundleDisk::readFile(const std::string& fileName) {
    return io::FileStream::ReadAllBytes(toRealPath(fileName));
}

std::vector<std::string> ModBundleDisk::getFileNames() {
    std::vector<std::string> files;

    std::error_code ec;
    for (fs::recursive_directory_iterator it(root_path,
             fs::directory_options::skip_permission_denied |
                 fs::directory_options::follow_directory_symlink,
             ec);
        it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec) {
            break;
        }

        if (!it->is_regular_file()) {
            continue;
        }

        const auto& path = it->path();
        // aleks2: lexically, not fs::relative.
        //
        // fs::relative resolves both sides through weakly_canonical, and
        // canonical throws on Switch: it does not recognise "sdmc:/..." as
        // absolute, so it tries to resolve it against the current directory and
        // gives up. Nothing catches that here, so a single mod in a folder --
        // not a .dusk, a plain folder -- took the whole process down during
        // startup, before the first frame.
        //
        // No canonicalising is needed anyway: these paths come straight out of
        // an iterator over root_path, so the tail is already exactly the
        // relative part, and doing it as text neither touches the card nor
        // throws.
        const auto relPath = path.lexically_relative(root_path);
        auto string = borealis::io::fs_path_to_string(relPath);
        if constexpr (fs::path::preferred_separator != '/') {
            // Convert \ to / on Windows
            for (auto& chr : string) {
                if (chr == fs::path::preferred_separator) {
                    chr = '/';
                }
            }
        }

        files.emplace_back(std::move(string));
    }

    return files;
}

size_t ModBundleDisk::getFileSize(const std::string& fileName) {
    return std::filesystem::file_size(toRealPath(fileName));
}

std::filesystem::path ModBundleDisk::toRealPath(const std::string& fileName) const {
    const fs::path filePath = reinterpret_cast<const char8_t*>(fileName.c_str());
    return root_path / filePath;
}

bool ModBundleDisk::file_exists(const std::string& fileName) {
    return std::filesystem::is_regular_file(toRealPath(fileName));
}

bool ModBundleDisk::directory_exists(const std::string& fileName) {
    return std::filesystem::is_directory(toRealPath(fileName));
}

}  // namespace dusk::mods
