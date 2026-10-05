// What a disc image says about itself. See switch/disc_header.hpp for why this goes
// through the container reader instead of looking at the file's first bytes.
#include "switch/disc_header.hpp"

#include <array>
#include <cstdio>

namespace dusk::sw::disc {
namespace {

// Disc headers are big-endian. Each console puts its magic word at its own offset,
// which is how the two are told apart.
constexpr std::uint32_t kGameCubeMagic = 0xC2339F3D;  // at 0x1C
constexpr std::uint32_t kWiiMagic = 0x5D1C9EA3;       // at 0x18
constexpr std::size_t kHeaderBytes = 0x20;

std::uint32_t be32_at(const std::array<std::uint8_t, kHeaderBytes>& bytes, std::size_t offset) {
    return (std::uint32_t{bytes[offset]} << 24) | (std::uint32_t{bytes[offset + 1]} << 16) |
           (std::uint32_t{bytes[offset + 2]} << 8) | std::uint32_t{bytes[offset + 3]};
}

// 64-bit file offsets: a compressed image keeps its tables and chunks wherever the
// writer put them, which for a full-size disc is past 2 GiB.
#ifdef _WIN32
int seek64(std::FILE* file, std::uint64_t offset) {
    return _fseeki64(file, static_cast<long long>(offset), SEEK_SET);
}
std::uint64_t file_size(std::FILE* file) {
    _fseeki64(file, 0, SEEK_END);
    const long long size = _ftelli64(file);
    return size < 0 ? 0 : static_cast<std::uint64_t>(size);
}
#else
int seek64(std::FILE* file, std::uint64_t offset) {
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET);
}
std::uint64_t file_size(std::FILE* file) {
    fseeko(file, 0, SEEK_END);
    const off_t size = ftello(file);
    return size < 0 ? 0 : static_cast<std::uint64_t>(size);
}
#endif

struct StdioFile {
    std::FILE* handle = nullptr;
    std::uint64_t size = 0;
};

std::int64_t stdio_read_at(void* user, std::uint64_t offset, void* out, std::size_t length) {
    auto* file = static_cast<StdioFile*>(user);
    if (seek64(file->handle, offset) != 0) {
        return -1;
    }
    return static_cast<std::int64_t>(std::fread(out, 1, length, file->handle));
}

std::uint64_t stdio_length(void* user) { return static_cast<StdioFile*>(user)->size; }

}  // namespace

LogicalHeader read_logical_header(const RawStream& stream) {
    LogicalHeader header;

    OpenResult opened = open(stream);
    header.format = opened.format;
    if (opened.format == Format::Unknown) {
        header.status = HeaderStatus::NotADisc;
        header.message = format_advice(Format::Unknown);
        return header;
    }
    if (!format_supported(opened.format)) {
        header.status = HeaderStatus::Unsupported;
        header.message = format_advice(opened.format);
        return header;
    }
    if (opened.container == nullptr) {
        header.status = HeaderStatus::Damaged;
        header.message = std::string(format_name(opened.format)) + ": " + opened.error;
        return header;
    }

    // The disc, not the file: for an .iso this is the file's own first bytes, and for
    // the others it is what the container decodes them to.
    std::array<std::uint8_t, kHeaderBytes> bytes{};
    if (!opened.container->read(0, bytes.data(), bytes.size())) {
        header.status = HeaderStatus::Damaged;
        header.message = std::string(format_name(opened.format)) +
                         ": the disc header could not be read from it.";
        return header;
    }

    if (be32_at(bytes, 0x1C) == kGameCubeMagic) {
        header.console = Console::GameCube;
    } else if (be32_at(bytes, 0x18) == kWiiMagic) {
        header.console = Console::Wii;
    } else {
        header.status = HeaderStatus::NotADisc;
        header.message = std::string(format_name(opened.format)) +
                         ": what is inside it is not a GameCube or Wii disc.";
        return header;
    }

    header.status = HeaderStatus::Ok;
    header.game_id.assign(reinterpret_cast<const char*>(bytes.data()), 6);
    header.disc_number = bytes[6];
    header.revision = bytes[7];
    return header;
}

LogicalHeader read_logical_header(const char* path) {
    LogicalHeader header;
    if (path == nullptr || path[0] == '\0') {
        header.status = HeaderStatus::Unreadable;
        return header;
    }
    StdioFile file;
    file.handle = std::fopen(path, "rb");
    if (file.handle == nullptr) {
        header.status = HeaderStatus::Unreadable;
        return header;
    }
    file.size = file_size(file.handle);

    RawStream stream;
    stream.user = &file;
    stream.read_at = &stdio_read_at;
    stream.length = &stdio_length;
    header = read_logical_header(stream);
    std::fclose(file.handle);
    return header;
}

}  // namespace dusk::sw::disc
