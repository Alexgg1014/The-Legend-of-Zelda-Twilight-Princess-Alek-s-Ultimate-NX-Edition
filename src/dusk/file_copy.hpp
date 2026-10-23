#pragma once
// aleks2 (rc18): a file copy that works on the Switch's card.
//
// libstdc++'s std::filesystem::copy_file copies the bytes, then the permissions with fchmod
// (_GLIBCXX_USE_FCHMOD), and libnx's sdmc device does not implement fchmod: fsdev_fchmod sets errno 88
// (ENOSYS) and fails. So on the console every copy_file reported "Function not implemented" after the copy was
// already on the card -- Saves > Export Save, and the mod updater's backup of the installed version, which
// stopped every update of an installed mod. Here the copy is fopen/fread/fwrite, and nothing else; `ec` carries
// errno. On other platforms copy_file() is std::filesystem's (wide paths on Windows), and copy_file_stdio() is
// what a host test checks.
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace dusk::file_copy {

// Copies `from` over `to` (created or replaced). On failure, `to` is removed and `ec` says why.
inline bool copy_file_stdio(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
    ec.clear();
    std::FILE* in = std::fopen(from.string().c_str(), "rb");
    if (in == nullptr) {
        ec.assign(errno != 0 ? errno : EIO, std::generic_category());
        return false;
    }
    std::FILE* out = std::fopen(to.string().c_str(), "wb");
    if (out == nullptr) {
        ec.assign(errno != 0 ? errno : EIO, std::generic_category());
        std::fclose(in);
        return false;
    }
    static constexpr std::size_t kChunk = 64 * 1024;
    static thread_local char buffer[kChunk];
    int error = 0;
    for (;;) {
        const std::size_t n = std::fread(buffer, 1, kChunk, in);
        if (n != 0 && std::fwrite(buffer, 1, n, out) != n) {
            error = errno != 0 ? errno : EIO;
            break;
        }
        if (n < kChunk) {
            if (std::ferror(in)) error = errno != 0 ? errno : EIO;
            break;
        }
    }
    std::fclose(in);
    if (std::fflush(out) != 0 && error == 0) error = errno != 0 ? errno : EIO;
    if (std::fclose(out) != 0 && error == 0) error = errno != 0 ? errno : EIO;
    if (error != 0) {
        std::error_code ignored;
        std::filesystem::remove(to, ignored);
        ec.assign(error, std::generic_category());
        return false;
    }
    return true;
}

// copy_file(from, to, overwrite_existing) that works on the Switch's card.
inline bool copy_file(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
#ifdef __SWITCH__
    return copy_file_stdio(from, to, ec);
#else
    return std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
#endif
}

}  // namespace dusk::file_copy
