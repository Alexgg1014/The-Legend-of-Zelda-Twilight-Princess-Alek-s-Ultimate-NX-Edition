// borealis::io for Horizon, written against borealis/io.hpp.
//
// borealis' own implementation (extern/borealis/src/io.cpp) is for desktops and
// phones. It cannot be linked here as it stands, for two reasons that are about
// this console and nothing else:
//
//   * it reads by offset with pread(), which devkitA64's libc does not have;
//   * atomic_replace() is a plain rename(), and Horizon's rename() refuses to
//     replace an existing file where POSIX says it must.
//
// Everything else follows the interface's documented behaviour: files are SDL
// IO streams, locations are plain paths (a "scheme://" location is Unsupported),
// and join/create_child take only safe relative names.
#include <borealis/io.hpp>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_iostream.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <system_error>
#include <utility>

namespace borealis::io {
namespace {

bool has_scheme(std::string_view location) {
  const size_t at = location.find("://");
  return at != std::string_view::npos && at != 0;
}

Status status_from_errno(int code) {
  switch (code) {
  case ENOENT:
  case ENOTDIR:
    return Status::NotFound;
  case EEXIST:
    return Status::AlreadyExists;
  default:
    return Status::Failed;
  }
}

std::string describe(std::string_view what, int code) {
  return std::string{what} + ": " + std::strerror(code);
}

// Segments of a relative path: none may be empty, "." or "..", and the path must
// not start at a root or a drive.
bool is_safe_relative(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.front() == '\\') {
    return false;
  }
  size_t begin = 0;
  while (true) {
    const size_t end = path.find_first_of("/\\", begin);
    const std::string_view segment = path.substr(begin, end == std::string_view::npos ? end : end - begin);
    if (segment.empty() || segment == "." || segment == "..") {
      return false;
    }
    if (end == std::string_view::npos) {
      return true;
    }
    begin = end + 1;
  }
}

bool is_safe_name(std::string_view name) {
  return is_safe_relative(name) && name.find_first_of("/\\") == std::string_view::npos;
}

OpenResult open_failure(Status status, std::string message) {
  OpenResult out;
  out.status = status;
  out.message = std::move(message);
  return out;
}

// There is no pread(), so a read by offset is a seek and a read, and two
// threads doing that to one descriptor would interleave them. Every
// RandomAccessFile goes through this one lock.
std::mutex g_offset_read_lock;

}  // namespace

// --- File ---------------------------------------------------------------------

File::~File() { close(); }

File::File(File&& other) noexcept
    : m_handle{std::exchange(other.m_handle, nullptr)},
      m_access{std::exchange(other.m_access, nullptr)},
      m_writable{std::exchange(other.m_writable, false)},
      m_error{std::move(other.m_error)} {}

File& File::operator=(File&& other) noexcept {
  if (this != &other) {
    close();
    m_handle = std::exchange(other.m_handle, nullptr);
    m_access = std::exchange(other.m_access, nullptr);
    m_writable = std::exchange(other.m_writable, false);
    m_error = std::move(other.m_error);
  }
  return *this;
}

void File::set_sdl_error(std::string_view action) noexcept {
  try {
    const char* why = SDL_GetError();
    m_error = std::string{action} + ((why != nullptr && why[0] != '\0') ? std::string{": "} + why : " failed");
  } catch (...) {
    m_error = "I/O error";
  }
}

uint64_t File::size() const noexcept {
  if (m_handle == nullptr) {
    return 0;
  }
  const Sint64 bytes = SDL_GetIOSize(m_handle);
  return bytes > 0 ? static_cast<uint64_t>(bytes) : 0;
}

uint64_t File::read(void* buf, uint64_t len) noexcept {
  m_error.clear();
  if (m_handle == nullptr || (buf == nullptr && len != 0)) {
    m_error = "File is not open";
    return 0;
  }
  const size_t asked = static_cast<size_t>(std::min<uint64_t>(len, SIZE_MAX));
  const size_t got = SDL_ReadIO(m_handle, buf, asked);
  if (got < asked && SDL_GetIOStatus(m_handle) == SDL_IO_STATUS_ERROR) {
    set_sdl_error("Failed to read file");
  }
  return got;
}

bool File::seek(uint64_t offset) noexcept {
  m_error.clear();
  if (m_handle == nullptr) {
    m_error = "File is not open";
    return false;
  }
  if (offset > static_cast<uint64_t>(INT64_MAX) || SDL_SeekIO(m_handle, static_cast<Sint64>(offset), SDL_IO_SEEK_SET) < 0) {
    set_sdl_error("Failed to seek file");
    return false;
  }
  return true;
}

bool File::write(std::span<const std::byte> bytes) noexcept {
  m_error.clear();
  if (bytes.empty()) {
    return true;
  }
  if (m_handle == nullptr || !m_writable) {
    m_error = m_handle == nullptr ? "File is not open" : "File is not writable";
    return false;
  }
  if (SDL_WriteIO(m_handle, bytes.data(), bytes.size()) != bytes.size()) {
    set_sdl_error("Failed to write file");
    return false;
  }
  return true;
}

bool File::flush() noexcept {
  m_error.clear();
  if (m_handle == nullptr) {
    return true;
  }
  if (!SDL_FlushIO(m_handle)) {
    set_sdl_error("Failed to flush file");
    return false;
  }
  return true;
}

bool File::close() noexcept {
  if (m_handle == nullptr) {
    return true;
  }
  const bool flushed = !m_writable || flush();
  m_writable = false;
  SDL_IOStream* stream = std::exchange(m_handle, nullptr);
  const bool closed = SDL_CloseIO(stream);
  if (!closed && flushed) {
    set_sdl_error("Failed to close file");
  }
  return flushed && closed;
}

// --- RandomAccessFile ---------------------------------------------------------

RandomAccessFile::~RandomAccessFile() { close(); }

RandomAccessFile::RandomAccessFile(RandomAccessFile&& other) noexcept
    : m_handle{std::exchange(other.m_handle, -1)}, m_size{std::exchange(other.m_size, 0)} {}

RandomAccessFile& RandomAccessFile::operator=(RandomAccessFile&& other) noexcept {
  if (this != &other) {
    close();
    m_handle = std::exchange(other.m_handle, -1);
    m_size = std::exchange(other.m_size, 0);
  }
  return *this;
}

RandomAccessFile::OpenResult RandomAccessFile::open(const std::filesystem::path& path) {
  OpenResult out;
  const std::string where = fs_path_to_string(path);
  const int fd = ::open(where.c_str(), O_RDONLY);
  if (fd < 0) {
    out.status = status_from_errno(errno);
    out.message = describe("Failed to open file", errno);
    return out;
  }
  struct stat info{};
  if (::fstat(fd, &info) != 0) {
    const int code = errno;
    ::close(fd);
    out.message = describe("Failed to get file size", code);
    return out;
  }
  // A directory opens without complaint and then reads as empty, which would
  // surface much later as a confusing parse failure.
  if (!S_ISREG(info.st_mode)) {
    ::close(fd);
    out.message = "Not a regular file";
    return out;
  }
  out.status = Status::Ok;
  out.file.m_handle = fd;
  out.file.m_size = static_cast<uint64_t>(info.st_size);
  return out;
}

RandomAccessFile::operator bool() const noexcept { return m_handle >= 0; }

size_t RandomAccessFile::read_at(uint64_t offset, std::span<std::byte> out, std::error_code& error) const noexcept {
  error.clear();
  if (m_handle < 0 || out.empty()) {
    return 0;
  }
  std::lock_guard lock{g_offset_read_lock};
  if (::lseek(m_handle, static_cast<off_t>(offset), SEEK_SET) < 0) {
    error.assign(errno, std::generic_category());
    return 0;
  }
  // read() may return less than asked, and does for large requests on the card.
  size_t done = 0;
  while (done < out.size()) {
    const ssize_t n = ::read(m_handle, out.data() + done, out.size() - done);
    if (n > 0) {
      done += static_cast<size_t>(n);
    } else if (n == 0) {
      break;  // end of file
    } else if (errno != EINTR) {
      error.assign(errno, std::generic_category());
      break;
    }
  }
  return done;
}

bool RandomAccessFile::close() noexcept {
  if (m_handle < 0) {
    return true;
  }
  m_size = 0;
  return ::close(std::exchange(m_handle, -1)) == 0;
}

// --- Free functions -----------------------------------------------------------

Status check(std::string_view location) {
  if (location.empty()) {
    return Status::NotFound;
  }
  if (has_scheme(location)) {
    return Status::Unsupported;
  }
  struct stat info{};
  if (::stat(std::string{location}.c_str(), &info) == 0) {
    return Status::Ok;
  }
  // A path that exists but may not be read is, to a caller choosing files,
  // the same as one that is not there.
  return (errno == EACCES) ? Status::NotFound : status_from_errno(errno);
}

OpenResult open(std::string_view location, File::Mode mode) {
  if (location.empty()) {
    return open_failure(Status::NotFound, "Location is empty");
  }
  if (has_scheme(location)) {
    return open_failure(Status::Unsupported, "Location scheme is not supported");
  }
  const char* flags = mode == File::Mode::Read ? "rb" : (mode == File::Mode::Append ? "ab" : "wb");
  const std::string where{location};
  SDL_ClearError();
  SDL_IOStream* stream = SDL_IOFromFile(where.c_str(), flags);
  if (stream == nullptr) {
    const char* why = SDL_GetError();
    return open_failure(check(location) == Status::NotFound ? Status::NotFound : Status::Failed,
                        (why != nullptr && why[0] != '\0') ? why : "Failed to open file");
  }
  OpenResult out;
  out.status = Status::Ok;
  out.file = File{stream, nullptr, mode != File::Mode::Read};
  return out;
}

std::string display_name(std::string_view location) {
  while (location.size() > 1 && (location.back() == '/' || location.back() == '\\')) {
    location.remove_suffix(1);
  }
  const size_t cut = location.find_last_of("/\\");
  return std::string{cut == std::string_view::npos ? location : location.substr(cut + 1)};
}

// POSIX rename() replaces its destination. Horizon's fails with EEXIST instead,
// and every atomic write in the game is "write a temporary, rename it over the
// original", so without this the second save of anything fails and leaves its
// temporary behind. The destination is removed and the rename retried; that
// leaves a short window with no file at all, which is the best the filesystem
// offers. If even the retry fails, the contents are copied across instead,
// so a failed replace never leaves the destination missing.
bool atomic_replace(const std::filesystem::path& source, const std::filesystem::path& destination,
                    std::string& error) {
  const std::string from = fs_path_to_string(source);
  const std::string to = fs_path_to_string(destination);

  if (::rename(from.c_str(), to.c_str()) == 0) {
    return true;
  }
  const int first = errno;
  if (first != EEXIST && first != ENOTEMPTY && first != EACCES) {
    error = describe("Failed to replace file", first);
    return false;
  }

  std::error_code ec;
  if (std::filesystem::exists(destination, ec) && ::remove(to.c_str()) != 0) {
    // V1-rc19: the delete's own errno (rc18 reported the rename's EEXIST). An open file is refused by Horizon.
    const int removeErrno = errno;
    error = describe("Failed to remove the file being replaced (open?)", removeErrno != 0 ? removeErrno : first);
    return false;
  }
  if (::rename(from.c_str(), to.c_str()) == 0) {
    return true;
  }
  const int second = errno;
  std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    error = describe("Failed to replace file", second);
    return false;
  }
  std::filesystem::remove(source, ec);
  return true;
}

JoinResult join(std::string_view folder, std::string_view relativePath) {
  if (!is_safe_relative(relativePath)) {
    return {Status::Failed, {}, "Child path must be a safe relative path"};
  }
  if (has_scheme(folder)) {
    return {Status::Unsupported, {}, "Folder scheme is not supported"};
  }
  const std::string child = fs_path_to_string(fs_path_from_utf8(folder) / fs_path_from_utf8(relativePath));
  const Status found = check(child);
  if (found != Status::Ok) {
    return {found, {}, "Child does not exist"};
  }
  return {Status::Ok, child, {}};
}

JoinResult create_child(std::string_view folder, std::string_view name) {
  if (folder.empty()) {
    return {Status::Failed, {}, "Folder location is empty"};
  }
  if (!is_safe_name(name)) {
    return {Status::Failed, {}, "Child name must be a safe file name"};
  }
  if (has_scheme(folder)) {
    return {Status::Unsupported, {}, "Folder scheme is not supported"};
  }
  const std::string child = fs_path_to_string(fs_path_from_utf8(folder) / fs_path_from_utf8(name));
  // O_EXCL makes an existing child an error instead of a file to open, and the
  // caller gets to tell the two apart.
  const int fd = ::open(child.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
  if (fd < 0) {
    return {status_from_errno(errno), {}, describe("Unable to create child", errno)};
  }
  if (::close(fd) != 0) {
    const int code = errno;
    std::error_code ignored;
    std::filesystem::remove(child, ignored);
    return {Status::Failed, {}, describe("Unable to close child", code)};
  }
  return {Status::Ok, child, {}};
}

ListResult list(std::string_view folder) {
  if (has_scheme(folder)) {
    return {Status::Unsupported, {}, "Folder scheme is not supported"};
  }
  const std::string base{folder};
  DIR* dir = ::opendir(base.c_str());
  if (dir == nullptr) {
    return {status_from_errno(errno), {}, describe("Failed to list folder", errno)};
  }
  ListResult out;
  out.status = Status::Ok;
  while (const dirent* item = ::readdir(dir)) {
    const std::string_view name{item->d_name};
    if (name == "." || name == "..") {
      continue;
    }
    Entry entry;
    entry.name = std::string{name};
    entry.location = base;
    if (entry.location.empty() || entry.location.back() != '/') {
      entry.location += '/';
    }
    entry.location += entry.name;
    // The directory entry normally says whether it is a folder, and asking is
    // free where a stat is a round trip to the card per file. Only a filesystem
    // that declines to say (DT_UNKNOWN) is worth the stat.
    if (item->d_type == DT_UNKNOWN) {
      struct stat info{};
      entry.isDirectory = ::stat(entry.location.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
    } else {
      entry.isDirectory = item->d_type == DT_DIR;
    }
    out.entries.push_back(std::move(entry));
  }
  ::closedir(dir);
  std::ranges::sort(out.entries, {}, &Entry::name);
  return out;
}

// --- PathAccess ---------------------------------------------------------------
// Desktops hand out a scoped permission after a file picker. The SD card needs
// none, so the token carries only the path.

PathAccess::~PathAccess() = default;

PathAccess::PathAccess(PathAccess&& other) noexcept
    : m_path{std::move(other.m_path)}, m_access{std::exchange(other.m_access, nullptr)} {
  other.m_path.clear();
}

PathAccess& PathAccess::operator=(PathAccess&& other) noexcept {
  if (this != &other) {
    m_path = std::move(other.m_path);
    m_access = std::exchange(other.m_access, nullptr);
    other.m_path.clear();
  }
  return *this;
}

PathAccess access_path(std::string_view location) {
  if (location.empty() || has_scheme(location)) {
    return {};
  }
  return PathAccess{fs_path_from_utf8(location), nullptr};
}

}  // namespace borealis::io
