// H34 host test for platforms/switch/source/disc_scan.cpp: the list behind the
// console's disc selector.
//   platforms/switch/tools/test-disc-scan.sh
//
// Everything is built here in a scratch folder: plain ISOs, a GCZ, the refused
// containers, truncated and invalid files, a folder with too many entries, names with
// spaces and non-ASCII characters, a subfolder that must not be entered, and a large
// sparse ISO that must be listed from its header alone.
#include "switch/disc_scan.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace sw = dusk::sw::disc;
namespace fs = std::filesystem;

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

Bytes make_disc(const char* id, std::uint8_t revision, std::size_t size) {
    Bytes disc(size, 0);
    std::memcpy(disc.data(), id, 6);
    disc[7] = revision;
    put_be32(disc, 0x1C, 0xC2339F3Du);
    return disc;
}

// Same layout as disc_header_test.cpp: 32-byte header, pointer and checksum tables,
// every block stored except block 0, which is deflated.
Bytes make_gcz(const Bytes& disc, std::uint32_t blockSize) {
    const auto blocks = static_cast<std::uint32_t>((disc.size() + blockSize - 1) / blockSize);
    Bytes data;
    std::vector<std::uint64_t> pointers;
    std::vector<std::uint32_t> sums;
    for (std::uint32_t i = 0; i < blocks; ++i) {
        Bytes block(blockSize, 0);
        const std::size_t start = std::size_t{i} * blockSize;
        std::memcpy(block.data(), disc.data() + start,
                    std::min<std::size_t>(blockSize, disc.size() - start));
        sums.push_back(static_cast<std::uint32_t>(adler32(1, block.data(), blockSize)));
        pointers.push_back(data.size());
        if (i == 0) {
            uLongf outSize = compressBound(blockSize);
            Bytes packed(outSize);
            compress(packed.data(), &outSize, block.data(), blockSize);
            data.insert(data.end(), packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(outSize));
        } else {
            pointers.back() |= 1ull << 63;
            data.insert(data.end(), block.begin(), block.end());
        }
    }
    Bytes out(32 + std::size_t{blocks} * 12, 0);
    put_le32(out, 0, 0xB10BC001u);
    put_le64(out, 8, data.size());
    put_le64(out, 16, disc.size());
    put_le32(out, 24, blockSize);
    put_le32(out, 28, blocks);
    for (std::uint32_t i = 0; i < blocks; ++i) {
        put_le64(out, 32 + std::size_t{i} * 8, pointers[i]);
        put_le32(out, 32 + std::size_t{blocks} * 8 + std::size_t{i} * 4, sums[i]);
    }
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::string write_file(const fs::path& path, const Bytes& bytes) {
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    if (f != nullptr) {
        if (!bytes.empty()) {
            std::fwrite(bytes.data(), 1, bytes.size(), f);
        }
        std::fclose(f);
    }
    return path.string();
}

const sw::Candidate* find(const sw::ScanResult& result, const std::string& name) {
    for (const auto& candidate : result.candidates) {
        if (candidate.name == name) {
            return &candidate;
        }
    }
    return nullptr;
}

void test_names() {
    CHECK(sw::is_disc_image_name("game.iso"));
    CHECK(sw::is_disc_image_name("GAME.ISO"));
    CHECK(sw::is_disc_image_name("game.gcm"));
    CHECK(sw::is_disc_image_name("Game.Rvz"));
    CHECK(sw::is_disc_image_name("game.wia"));
    CHECK(sw::is_disc_image_name("game.gcz"));
    CHECK(sw::is_disc_image_name("My Game (EU) [v1.0].iso"));
    CHECK(!sw::is_disc_image_name("game.wbfs"));
    CHECK(!sw::is_disc_image_name("game.ciso"));
    CHECK(!sw::is_disc_image_name("game.nfs"));
    CHECK(!sw::is_disc_image_name("game.tgc"));
    CHECK(!sw::is_disc_image_name("game.iso.part"));
    CHECK(!sw::is_disc_image_name("game"));
    CHECK(!sw::is_disc_image_name(".iso"));
    CHECK(!sw::is_disc_image_name("._game.iso"));
    CHECK(!sw::is_disc_image_name("config.json"));
    CHECK(!sw::is_disc_image_name(""));
}

void test_scan(const fs::path& root) {
    const fs::path main = root / "dusklight";
    const fs::path discs = main / "discs";
    fs::create_directories(discs / "nested");

    write_file(main / "GZ2P01.iso", make_disc("GZ2P01", 0, 0x10000));
    write_file(main / "Zelda TP (USA) – copia ñ.rvz.gcz",
               make_gcz(make_disc("GZ2E01", 0, 0x20000), 0x8000));
    write_file(main / "wii.wbfs", Bytes(0x1000, 0));            // filtered by name
    write_file(main / "config.json", Bytes{'{', '}'});           // filtered by name
    write_file(main / "._GZ2P01.iso", Bytes(4096, 0));           // AppleDouble, filtered
    write_file(main / "junk.iso", Bytes(0x1000, 0x41));          // not a disc
    write_file(main / "empty.gcm", Bytes());                     // empty
    Bytes cut = make_gcz(make_disc("GZ2P01", 0, 0x20000), 0x8000);
    cut.resize(0x48);
    write_file(main / "truncated.gcz", cut);                     // damaged container
    {
        Bytes ciso(0x1000, 0);
        std::memcpy(ciso.data(), "CISO", 4);
        write_file(main / "renamed-ciso.iso", ciso);             // refused by content
    }
    write_file(main / "other game.iso", make_disc("GALE01", 0, 0x10000));
    fs::create_directories(main / "folder.iso");                 // a directory, not a file
    write_file(discs / "second copy.gcm", make_disc("GZ2J01", 0, 0x10000));
    write_file(discs / "nested" / "deep.iso", make_disc("GZ2P01", 0, 0x10000));

    sw::ScanOptions options;
    options.directories = {main.string(), discs.string(), (root / "missing").string()};
    const sw::ScanResult result = sw::scan(options);

    // Only image names are opened; everything else is skipped by name.
    CHECK(find(result, "wii.wbfs") == nullptr);
    CHECK(find(result, "config.json") == nullptr);
    CHECK(find(result, "._GZ2P01.iso") == nullptr);
    CHECK(find(result, "folder.iso") == nullptr);
    CHECK(result.skippedByName >= 3);
    // One level deep: the nested folder is never entered.
    CHECK(find(result, "deep.iso") == nullptr);
    // A missing folder is counted, not an error.
    CHECK(result.unreadableDirectories == 1);
    CHECK(!result.entryLimitReached && !result.candidateLimitReached);

    const auto* iso = find(result, "GZ2P01.iso");
    CHECK(iso != nullptr && iso->header.status == sw::HeaderStatus::Ok);
    CHECK(iso != nullptr && iso->header.format == sw::Format::Raw && iso->header.game_id == "GZ2P01");
    CHECK(iso != nullptr && iso->directory == main.string() && !iso->configured);

    // Spaces, a dash and non-ASCII letters survive, and the content decides the
    // format, not the name.
    const auto* spaced = find(result, "Zelda TP (USA) – copia ñ.rvz.gcz");
    CHECK(spaced != nullptr && spaced->header.status == sw::HeaderStatus::Ok);
    CHECK(spaced != nullptr && spaced->header.format == sw::Format::Gcz);
    CHECK(spaced != nullptr && spaced->header.game_id == "GZ2E01");

    const auto* junk = find(result, "junk.iso");
    CHECK(junk != nullptr && junk->header.status == sw::HeaderStatus::NotADisc);
    const auto* empty = find(result, "empty.gcm");
    CHECK(empty != nullptr && empty->header.status == sw::HeaderStatus::NotADisc);
    const auto* truncated = find(result, "truncated.gcz");
    CHECK(truncated != nullptr && truncated->header.status == sw::HeaderStatus::Damaged);
    const auto* ciso = find(result, "renamed-ciso.iso");
    CHECK(ciso != nullptr && ciso->header.status == sw::HeaderStatus::Unsupported);
    const auto* other = find(result, "other game.iso");
    CHECK(other != nullptr && other->header.status == sw::HeaderStatus::Ok &&
          other->header.game_id == "GALE01");
    const auto* second = find(result, "second copy.gcm");
    CHECK(second != nullptr && second->directory == discs.string());

    // Folders in the order given, names sorted case-insensitively inside each.
    std::vector<std::string> mainNames;
    for (const auto& candidate : result.candidates) {
        if (candidate.directory == main.string()) {
            mainNames.push_back(candidate.name);
        }
    }
    CHECK(std::ranges::is_sorted(mainNames, [](const std::string& a, const std::string& b) {
        std::string la = a, lb = b;
        for (auto& c : la) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (auto& c : lb) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return la < lb;
    }));
    CHECK(!result.candidates.empty() && result.candidates.back().name == "second copy.gcm");

    // The configured path is flagged when it is one of the listed images...
    options.configuredPath = (main / "GZ2P01.iso").string();
    const auto withConfigured = sw::scan(options);
    const auto* configured = find(withConfigured, "GZ2P01.iso");
    CHECK(configured != nullptr && configured->configured && !configured->outsideScan);
    CHECK(withConfigured.candidates.size() == result.candidates.size());

    // ...a trailing slash on a folder does not hide it...
    options.directories = {main.string() + "/"};
    options.configuredPath = (main / "GZ2P01.iso").string();
    const auto slashed = sw::scan(options);
    CHECK(find(slashed, "GZ2P01.iso") != nullptr && find(slashed, "GZ2P01.iso")->configured);
    CHECK(std::ranges::count_if(slashed.candidates,
                                [](const sw::Candidate& c) { return c.configured; }) == 1);

    // ...kept, first, when it lives outside the scanned folders...
    const fs::path elsewhere = root / "my discs";
    fs::create_directories(elsewhere);
    write_file(elsewhere / "manual pick.iso", make_disc("GZ2P01", 0, 0x10000));
    options.directories = {main.string()};
    options.configuredPath = (elsewhere / "manual pick.iso").string();
    const auto outside = sw::scan(options);
    CHECK(!outside.candidates.empty() && outside.candidates.front().name == "manual pick.iso");
    CHECK(outside.candidates.front().configured && outside.candidates.front().outsideScan);
    CHECK(!outside.candidates.front().missing);
    CHECK(outside.candidates.front().header.status == sw::HeaderStatus::Ok);

    // ...and kept, flagged missing, when the file is gone. Nothing is rewritten: the
    // scan never changes the configured value, it only reports it.
    options.configuredPath = (elsewhere / "deleted.rvz").string();
    const auto gone = sw::scan(options);
    CHECK(!gone.candidates.empty() && gone.candidates.front().missing);
    CHECK(gone.candidates.front().configured &&
          gone.candidates.front().header.status == sw::HeaderStatus::Unreadable);
    CHECK(options.configuredPath == (elsewhere / "deleted.rvz").string());

    // A missing configured file inside a scanned folder is not "outside".
    options.configuredPath = (main / "deleted.iso").string();
    const auto goneInside = sw::scan(options);
    CHECK(!goneInside.candidates.empty() && goneInside.candidates.front().missing &&
          !goneInside.candidates.front().outsideScan);
}

void test_empty_and_limits(const fs::path& root) {
    const fs::path empty = root / "empty";
    fs::create_directories(empty);
    sw::ScanOptions options;
    options.directories = {empty.string()};
    const auto none = sw::scan(options);
    CHECK(none.candidates.empty() && none.entriesExamined == 0 && none.unreadableDirectories == 0);

    // Nothing at all to look at.
    options.directories = {};
    CHECK(sw::scan(options).candidates.empty());

    // A folder with more entries than the limit is cut off and says so.
    const fs::path crowded = root / "crowded";
    fs::create_directories(crowded);
    for (int i = 0; i < 300; ++i) {
        write_file(crowded / ("note" + std::to_string(i) + ".txt"), Bytes{'x'});
    }
    options.directories = {crowded.string()};
    const auto cut = sw::scan(options);
    CHECK(cut.entryLimitReached && cut.entriesExamined == options.maxEntriesPerDirectory);

    // And the list is capped.
    const fs::path many = root / "many";
    fs::create_directories(many);
    for (int i = 0; i < 5; ++i) {
        write_file(many / ("disc" + std::to_string(i) + ".iso"), make_disc("GZ2P01", 0, 0x1000));
    }
    options.directories = {many.string()};
    options.maxCandidates = 3;
    const auto capped = sw::scan(options);
    CHECK(capped.candidates.size() == 3 && capped.candidateLimitReached);
}

void test_header_only(const fs::path& root) {
    // A 1.4 GiB ISO, sparse, so the test does not write it: listing it reads a few
    // hundred bytes, never the image.
    const fs::path big = root / "big.iso";
    write_file(big, make_disc("GZ2P01", 0, 0x1000));
    fs::resize_file(big, 1459978240ull);
    const auto bigCandidate = sw::inspect_candidate(big.string(), 16ull * 1024 * 1024);
    CHECK(bigCandidate.header.status == sw::HeaderStatus::Ok);
    CHECK(bigCandidate.fileSize == 1459978240ull);
    CHECK(bigCandidate.bytesRead > 0 && bigCandidate.bytesRead <= 4096);

    // A compressed image: the container's header, its tables and block 0 -- not the
    // other 255 blocks.
    const Bytes disc = make_disc("GZ2P01", 0, 0x800000);
    const fs::path gcz = root / "big.gcz";
    write_file(gcz, make_gcz(disc, 0x8000));
    const auto gczCandidate = sw::inspect_candidate(gcz.string(), 16ull * 1024 * 1024);
    CHECK(gczCandidate.header.status == sw::HeaderStatus::Ok);
    CHECK(gczCandidate.bytesRead < gczCandidate.fileSize / 16);

    // Under a budget too small for its tables it is reported damaged, not read further.
    const auto starved = sw::inspect_candidate(gcz.string(), 256);
    CHECK(starved.header.status == sw::HeaderStatus::Damaged);
    CHECK(starved.bytesRead <= 256);
    CHECK(!starved.header.message.empty());

    // Missing files.
    const auto missing = sw::inspect_candidate((root / "nope.iso").string(), 4096);
    CHECK(missing.header.status == sw::HeaderStatus::Unreadable && missing.bytesRead == 0);
    CHECK(sw::inspect_candidate("", 4096).header.status == sw::HeaderStatus::Unreadable);
}

void test_labels() {
    CHECK(std::string(sw::status_label(sw::HeaderStatus::Ok)) == "disc");
    CHECK(std::string(sw::status_label(sw::HeaderStatus::Damaged)) == "damaged");
    CHECK(std::string(sw::status_label(sw::HeaderStatus::Unsupported)) == "unsupported");
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? fs::path{argv[1]} : fs::temp_directory_path() / "dusk-disc-scan";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root);

    test_names();
    test_scan(root);
    test_empty_and_limits(root);
    test_header_only(root);
    test_labels();

    fs::remove_all(root, error);
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "disc_scan: all checks passed\n";
    return 0;
}
