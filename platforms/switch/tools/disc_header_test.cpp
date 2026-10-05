// Host test for platforms/switch/source/disc_header.cpp: what the console's disc
// validator asks of an image, answered through the container reader.
//   platforms/switch/tools/test-disc-header.sh
//
// Two halves. The first builds small images here (an ISO, a GCZ, the containers
// that are refused, and a few that are not discs at all) so the test runs with
// nothing else on the machine. The second, when it is given real images
// (`--reference disc.iso a.rvz b.wia c.gcz ...`), requires every one of them to
// yield exactly the logical header the ISO does: the same game id, disc number,
// revision and console. That is the property the validator depends on, and the one
// that was missing when it looked at the file's own first bytes instead.
#include "switch/disc_header.hpp"

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace sw = dusk::sw::disc;

static int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

namespace {

using Bytes = std::vector<std::uint8_t>;

void put_be32(Bytes& b, std::size_t at, std::uint32_t v) {
    b[at] = static_cast<std::uint8_t>(v >> 24);
    b[at + 1] = static_cast<std::uint8_t>(v >> 16);
    b[at + 2] = static_cast<std::uint8_t>(v >> 8);
    b[at + 3] = static_cast<std::uint8_t>(v);
}
void put_le32(Bytes& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void put_le64(Bytes& b, std::size_t at, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// A GameCube disc as far as the header goes: id, disc number, revision, magic, and
// zeros after it, padded to `size`.
Bytes make_disc(const char* id, std::uint8_t disc, std::uint8_t revision, std::size_t size,
                bool wii = false) {
    Bytes disc_bytes(size, 0);
    std::memcpy(disc_bytes.data(), id, 6);
    disc_bytes[6] = disc;
    disc_bytes[7] = revision;
    put_be32(disc_bytes, wii ? 0x18 : 0x1C, wii ? 0x5D1C9EA3u : 0xC2339F3Du);
    return disc_bytes;
}

// A GCZ holding `disc`: block 0 deflated, the rest stored, which exercises both
// paths of the reader. The layout is the one disc_container.cpp reads: a 32-byte
// header, a u64 pointer and a u32 checksum per block, then the data.
Bytes make_gcz(const Bytes& disc, std::uint32_t block_size) {
    const std::uint32_t blocks = static_cast<std::uint32_t>((disc.size() + block_size - 1) / block_size);
    Bytes data;
    std::vector<std::uint64_t> pointers;
    std::vector<std::uint32_t> sums;
    for (std::uint32_t i = 0; i < blocks; ++i) {
        Bytes block(block_size, 0);
        const std::size_t start = std::size_t{i} * block_size;
        std::memcpy(block.data(), disc.data() + start, std::min<std::size_t>(block_size, disc.size() - start));
        sums.push_back(static_cast<std::uint32_t>(adler32(1, block.data(), block_size)));
        pointers.push_back(data.size());
        if (i == 0) {
            uLongf out_size = compressBound(block_size);
            Bytes packed(out_size);
            compress(packed.data(), &out_size, block.data(), block_size);
            data.insert(data.end(), packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(out_size));
        } else {
            pointers.back() |= 1ull << 63;
            data.insert(data.end(), block.begin(), block.end());
        }
    }
    Bytes out(32 + std::size_t{blocks} * 12, 0);
    put_le32(out, 0, 0xB10BC001u);
    put_le64(out, 8, data.size());
    put_le64(out, 16, disc.size());
    put_le32(out, 24, block_size);
    put_le32(out, 28, blocks);
    for (std::uint32_t i = 0; i < blocks; ++i) {
        put_le64(out, 32 + std::size_t{i} * 8, pointers[i]);
        put_le32(out, 32 + std::size_t{blocks} * 8 + std::size_t{i} * 4, sums[i]);
    }
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

Bytes with_magic(const char* magic, std::size_t magic_len, std::size_t size = 0x1000) {
    Bytes b(size, 0);
    std::memcpy(b.data(), magic, magic_len);
    return b;
}

std::string write_file(const std::string& name, const Bytes& bytes) {
    std::FILE* f = std::fopen(name.c_str(), "wb");
    if (f != nullptr) {
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
    return name;
}

bool contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

void test_synthetic() {
    // An ISO says what it says, from the file's own bytes.
    {
        const Bytes iso = make_disc("GZ2P01", 0, 0, 0x100000);
        const sw::LogicalHeader h = sw::read_logical_header(write_file("t_plain.iso", iso).c_str());
        CHECK(h.status == sw::HeaderStatus::Ok && h.format == sw::Format::Raw);
        CHECK(h.console == sw::Console::GameCube && h.game_id == "GZ2P01");
        CHECK(h.disc_number == 0 && h.revision == 0 && h.message.empty());
    }
    // Disc number and revision are read, not assumed.
    {
        const Bytes iso = make_disc("RZDE01", 1, 2, 0x100000);
        const sw::LogicalHeader h = sw::read_logical_header(write_file("t_rev.iso", iso).c_str());
        CHECK(h.status == sw::HeaderStatus::Ok && h.game_id == "RZDE01");
        CHECK(h.disc_number == 1 && h.revision == 2);
    }
    // A Wii disc is told apart by its own magic, at its own offset.
    {
        const Bytes iso = make_disc("RZDE01", 0, 2, 0x100000, /*wii=*/true);
        const sw::LogicalHeader h = sw::read_logical_header(write_file("t_wii.iso", iso).c_str());
        CHECK(h.status == sw::HeaderStatus::Ok && h.console == sw::Console::Wii);
    }

    // The same disc inside a GCZ gives the same header. The first bytes of the GCZ
    // file are the container's, which is the whole reason this goes through the reader.
    {
        const Bytes iso = make_disc("GZ2P01", 0, 0, 0x20000);
        const std::string gcz = write_file("t_disc.gcz", make_gcz(iso, 0x8000));
        const sw::LogicalHeader a = sw::read_logical_header(write_file("t_same.iso", iso).c_str());
        const sw::LogicalHeader b = sw::read_logical_header(gcz.c_str());
        CHECK(b.status == sw::HeaderStatus::Ok && b.format == sw::Format::Gcz);
        CHECK(a.game_id == b.game_id && a.disc_number == b.disc_number);
        CHECK(a.revision == b.revision && a.console == b.console);
    }
    {
        const Bytes iso = make_disc("RZDE01", 0, 2, 0x20000);
        const sw::LogicalHeader h =
            sw::read_logical_header(write_file("t_rev.gcz", make_gcz(iso, 0x8000)).c_str());
        CHECK(h.status == sw::HeaderStatus::Ok && h.game_id == "RZDE01" && h.revision == 2);
    }
    // A GCZ that holds something that is not a disc is not accepted as one.
    {
        const Bytes not_disc(0x20000, 0x5A);
        const sw::LogicalHeader h =
            sw::read_logical_header(write_file("t_junk.gcz", make_gcz(not_disc, 0x8000)).c_str());
        CHECK(h.status == sw::HeaderStatus::NotADisc && h.format == sw::Format::Gcz);
        CHECK(contains(h.message, "GCZ"));
    }
    // A GCZ cut off in its block table, and an RVZ that is only a magic, are damaged
    // containers of a kind we read -- not "unknown", and not a crash.
    {
        Bytes cut = make_gcz(make_disc("GZ2P01", 0, 0, 0x20000), 0x8000);
        cut.resize(0x48);  // long enough to be recognised, ends inside the block table
        const sw::LogicalHeader h = sw::read_logical_header(write_file("t_cut.gcz", cut).c_str());
        CHECK(h.status == sw::HeaderStatus::Damaged && h.format == sw::Format::Gcz);
        CHECK(!h.message.empty());
    }
    for (const char* ext : {"rvz", "wia"}) {
        std::string magic = std::string(ext == std::string("rvz") ? "RVZ" : "WIA") + "\x01";
        const sw::LogicalHeader h = sw::read_logical_header(
            write_file(std::string("t_stub.") + ext, with_magic(magic.c_str(), 4)).c_str());
        CHECK(h.status == sw::HeaderStatus::Damaged);
        CHECK(h.format == (ext == std::string("rvz") ? sw::Format::Rvz : sw::Format::Wia));
    }

    // The containers that are refused are refused by name, with their own advice.
    {
        const sw::LogicalHeader ciso =
            sw::read_logical_header(write_file("t.ciso", with_magic("CISO", 4)).c_str());
        CHECK(ciso.status == sw::HeaderStatus::Unsupported && ciso.format == sw::Format::Ciso);
        CHECK(contains(ciso.message, "CISO") && contains(ciso.message, "Wii format"));

        const sw::LogicalHeader wbfs =
            sw::read_logical_header(write_file("t.wbfs", with_magic("WBFS", 4)).c_str());
        CHECK(wbfs.status == sw::HeaderStatus::Unsupported && wbfs.format == sw::Format::Wbfs);
        CHECK(contains(wbfs.message, "WBFS") && contains(wbfs.message, "Wii format"));

        const sw::LogicalHeader nfs = sw::read_logical_header(
            write_file("t.nfs", with_magic("\x01\x00\x00\x00\x45\x47\x47\x53", 8)).c_str());
        CHECK(nfs.status == sw::HeaderStatus::Unsupported && nfs.format == sw::Format::Nfs);
        CHECK(contains(nfs.message, "Wii U"));

        Bytes tgc(0x1000, 0);
        put_be32(tgc, 0, 0xAE0F38A2u);
        const sw::LogicalHeader t = sw::read_logical_header(write_file("t.tgc", tgc).c_str());
        CHECK(t.status == sw::HeaderStatus::Unsupported && t.format == sw::Format::Tgc);
        CHECK(contains(t.message, "mini-disc"));
    }

    // Files that are not disc images, and files that are not there.
    {
        const sw::LogicalHeader junk =
            sw::read_logical_header(write_file("t_junk.bin", Bytes(0x1000, 0x41)).c_str());
        CHECK(junk.status == sw::HeaderStatus::NotADisc && !junk.message.empty());
        CHECK(sw::read_logical_header(write_file("t_empty.bin", Bytes()).c_str()).status ==
              sw::HeaderStatus::NotADisc);
        CHECK(sw::read_logical_header(write_file("t_short.bin", Bytes(0x10, 1)).c_str()).status ==
              sw::HeaderStatus::NotADisc);
        CHECK(sw::read_logical_header("t_does_not_exist.iso").status ==
              sw::HeaderStatus::Unreadable);
        CHECK(sw::read_logical_header("").status == sw::HeaderStatus::Unreadable);
        CHECK(sw::read_logical_header(static_cast<const char*>(nullptr)).status ==
              sw::HeaderStatus::Unreadable);
    }
}

void test_reference(const std::vector<std::string>& paths) {
    const sw::LogicalHeader want = sw::read_logical_header(paths[0].c_str());
    std::printf("reference %s: %s rev %u disc %u\n", paths[0].c_str(), want.game_id.c_str(),
                want.revision, want.disc_number);
    CHECK(want.status == sw::HeaderStatus::Ok && want.format == sw::Format::Raw);
    CHECK(want.console == sw::Console::GameCube && want.game_id.size() == 6);
    for (std::size_t i = 1; i < paths.size(); ++i) {
        const sw::LogicalHeader got = sw::read_logical_header(paths[i].c_str());
        const bool same = got.status == sw::HeaderStatus::Ok && got.console == want.console &&
                          got.game_id == want.game_id && got.disc_number == want.disc_number &&
                          got.revision == want.revision;
        std::printf("  %-5s %-40s %s\n", sw::format_name(got.format), paths[i].c_str(),
                    same ? "same header as the ISO" : "DIFFERENT");
        CHECK(same);
        CHECK(got.format != sw::Format::Raw && sw::format_supported(got.format));
    }
}

}  // namespace

int main(int argc, char** argv) {
    test_synthetic();

    std::vector<std::string> real;
    bool collecting = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--reference") {
            collecting = true;
        } else if (collecting) {
            real.emplace_back(argv[i]);
        }
    }
    if (!real.empty()) {
        test_reference(real);
    } else {
        std::printf("no reference images given: synthetic checks only\n");
    }

    if (failures == 0) {
        std::cout << "disc_header: all checks passed\n";
        return 0;
    }
    std::cerr << failures << " check(s) failed\n";
    return 1;
}
