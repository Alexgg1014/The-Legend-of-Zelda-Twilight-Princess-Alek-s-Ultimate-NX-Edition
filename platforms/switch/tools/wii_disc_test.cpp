// aleks2 (rc17): host test for Wii discs -- platforms/switch/source/disc_container.cpp (open_wii_data_partition),
// wii_crypto.cpp (key files) and iso_nod_shim.cpp (the nod API over a Wii disc).
//
// Every disc here is synthetic, built by this test: an encrypted .iso (AES-CBC per cluster, the title key encrypted
// with a made-up "common key" -- not Nintendo's, which this program never contains), and RVZ/WIA files that store
// the same partition decrypted and hash-free. A real Wii rip is the other half of the check, on the console.
#include <nod.h>

#include "switch/disc_container.hpp"
#include "switch/wii_crypto.hpp"

#include <openssl/evp.h>
#include <zstd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace sw = dusk::sw::disc;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

using Bytes = std::vector<std::uint8_t>;

static void put32(Bytes& b, std::size_t at, std::uint32_t v) {
    b[at] = v >> 24; b[at + 1] = v >> 16; b[at + 2] = v >> 8; b[at + 3] = v;
}
static void put64(Bytes& b, std::size_t at, std::uint64_t v) {
    put32(b, at, static_cast<std::uint32_t>(v >> 32)); put32(b, at + 4, static_cast<std::uint32_t>(v));
}
static void put16(Bytes& b, std::size_t at, std::uint16_t v) { b[at] = v >> 8; b[at + 1] = v; }

static void aes_encrypt(const std::uint8_t key[16], const std::uint8_t iv[16], const std::uint8_t* in,
    std::uint8_t* out, std::size_t size) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int len = 0;
    EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, key, iv);
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    EVP_EncryptUpdate(ctx, out, &len, in, static_cast<int>(size));
    EVP_CIPHER_CTX_free(ctx);
}

// The same lagged Fibonacci generator the reader regenerates padding with (k = 521, j = 32, the third byte of each
// word shifted by 18), written again here so packed RVZ groups can be built.
struct RefJunk {
    std::uint32_t s[521];
    std::size_t pos = 0;
    explicit RefJunk(const std::uint8_t* seed) {
        for (int i = 0; i < 17; ++i) s[i] = (seed[4 * i] << 24) | (seed[4 * i + 1] << 16) | (seed[4 * i + 2] << 8) | seed[4 * i + 3];
        for (int i = 17; i < 521; ++i) s[i] = (s[i - 17] << 23) ^ (s[i - 16] >> 9) ^ s[i - 1];
        for (auto& w : s) w = (w & 0xFF00FFFFu) | ((w >> 2) & 0x00FF0000u);
        for (int i = 0; i < 4; ++i) advance();
    }
    void advance() {
        for (int i = 0; i < 32; ++i) s[i] ^= s[i + 521 - 32];
        for (int i = 32; i < 521; ++i) s[i] ^= s[i - 32];
    }
    std::uint8_t next() {
        const std::uint8_t b = static_cast<std::uint8_t>(s[pos / 4] >> (24 - 8 * (pos % 4)));
        if (++pos == 521 * 4) { advance(); pos = 0; }
        return b;
    }
};

// ------------------------------------------------------------ the game --

constexpr std::uint64_t kCluster = 0x8000, kClusterData = 0x7C00;
constexpr std::uint64_t kPartition = 0x50000;    // the game partition on the disc
constexpr std::uint64_t kDataOffset = 0x20000;   // its encrypted data, from the partition's start
constexpr std::uint64_t kClusters = 13;          // 13 * 0x7C00 of game data
static const std::uint8_t kFileContents[] = "inside the Wii partition, through the nod API";
constexpr std::uint64_t kFileOffset = 0x9100;    // multiple of 4; stored >> 2
constexpr std::uint64_t kDolOffset = 0x4000, kFstOffset = 0x5000;

/// The game partition's data as the game sees it: boot.bin (offsets >> 2), bi2, apploader, a DOL, an FST with one
/// file, and noise elsewhere.
static Bytes make_partition_plain() {
    Bytes p(kClusters * kClusterData);
    std::mt19937 rng(7);
    for (auto& b : p) b = static_cast<std::uint8_t>(rng());
    std::memset(p.data(), 0, 0x440);
    std::memcpy(p.data(), "RZDE01", 6);
    put32(p, 0x18, 0x5D1C9EA3);
    std::memcpy(p.data() + 0x20, "THE LEGEND OF ZELDA Twilight Princess (synthetic)", 50);
    put32(p, 0x420, kDolOffset >> 2);
    put32(p, 0x424, kFstOffset >> 2);
    // FST: root + one file, then the string table.
    Bytes fst(2 * 12 + 8, 0);
    fst[0] = 1; put32(fst, 8, 2);                         // root: directory, 2 entries
    fst[12] = 0; fst[13] = 0; fst[14] = 0; fst[15] = 0;   // file, name at 0
    put32(fst, 16, kFileOffset >> 2);
    put32(fst, 20, sizeof kFileContents - 1);
    std::memcpy(fst.data() + 24, "a.bin", 6);
    put32(p, 0x428, static_cast<std::uint32_t>((fst.size() + 3) / 4));
    std::memset(p.data() + kFstOffset, 0, (fst.size() + 3) / 4 * 4);
    std::memcpy(p.data() + kFstOffset, fst.data(), fst.size());
    // A DOL: one text section of 0x200 bytes at 0x100.
    std::memset(p.data() + kDolOffset, 0, 0x100);
    put32(p, kDolOffset + 0x00, 0x100);
    put32(p, kDolOffset + 0x90, 0x200);
    std::memcpy(p.data() + kFileOffset, kFileContents, sizeof kFileContents - 1);
    return p;
}

static const std::uint8_t kTestCommonKey[16] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,
                                                0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
static const std::uint8_t kTitleKey[16] = {0xA5, 0x5A, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                                           0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
static const std::uint8_t kTitleId[8] = {0x00, 0x01, 0x00, 0x00, 'R', 'Z', 'D', 'E'};

/// The unencrypted part of the disc: header, partition table (an update partition first, then the game), and the
/// game partition's header with its ticket.
static Bytes make_disc_head(std::uint64_t discSize, std::uint8_t keyIndex = 0) {
    Bytes d(kPartition + kDataOffset, 0);
    d.resize(discSize, 0);
    std::memcpy(d.data(), "RZDE01", 6);
    put32(d, 0x18, 0x5D1C9EA3);
    put32(d, 0x40000, 2);                    // group 0: two partitions
    put32(d, 0x40004, 0x40020 >> 2);
    put32(d, 0x40020, 0x48000 >> 2);         // an update partition, type 1
    put32(d, 0x40024, 1);
    put32(d, 0x40028, kPartition >> 2);      // the game, type 0
    put32(d, 0x4002C, 0);
    std::uint8_t iv[16] = {};
    std::memcpy(iv, kTitleId, 8);
    aes_encrypt(kTestCommonKey, iv, kTitleKey, d.data() + kPartition + 0x1BF, 16);
    std::memcpy(d.data() + kPartition + 0x1DC, kTitleId, 8);
    d[kPartition + 0x1F1] = keyIndex;
    put32(d, kPartition + 0x2B8, kDataOffset >> 2);
    put32(d, kPartition + 0x2BC, static_cast<std::uint32_t>(kClusters * kCluster >> 2));
    return d;
}

/// An encrypted .iso: every cluster's hash block (made up here) encrypted with IV 0, its data with the IV at 0x3D0
/// of the encrypted hash block.
static Bytes make_iso(const Bytes& plain, std::uint8_t keyIndex = 0) {
    Bytes d = make_disc_head(kPartition + kDataOffset + kClusters * kCluster, keyIndex);
    std::mt19937 rng(11);
    for (std::uint64_t c = 0; c < kClusters; ++c) {
        std::uint8_t hashes[0x400];
        for (auto& b : hashes) b = static_cast<std::uint8_t>(rng());
        std::uint8_t* at = d.data() + kPartition + kDataOffset + c * kCluster;
        const std::uint8_t zero[16] = {};
        aes_encrypt(kTitleKey, zero, hashes, at, 0x400);
        aes_encrypt(kTitleKey, at + 0x3D0, plain.data() + c * kClusterData, at + 0x400, kClusterData);
    }
    return d;
}

// -------------------------------------------------------------- streams --

struct Mem {
    const Bytes* bytes;
};

static sw::RawStream stream_of(const Bytes& bytes, Mem& mem) {
    mem.bytes = &bytes;
    sw::RawStream s;
    s.user = &mem;
    s.read_at = [](void* u, std::uint64_t off, void* out, std::size_t len) -> std::int64_t {
        const Bytes& b = *static_cast<Mem*>(u)->bytes;
        if (off >= b.size()) return 0;
        const std::size_t n = std::min<std::size_t>(len, b.size() - off);
        std::memcpy(out, b.data() + off, n);
        return static_cast<std::int64_t>(n);
    };
    s.length = [](void* u) -> std::uint64_t { return static_cast<Mem*>(u)->bytes->size(); };
    return s;
}

static bool same_everywhere(sw::Container& c, const Bytes& plain) {
    if (c.size() != plain.size()) return false;
    Bytes all(plain.size());
    if (!c.read(0, all.data(), all.size()) || all != plain) return false;
    // Reads that start and end inside clusters, and cross their boundaries.
    std::mt19937 rng(3);
    for (int i = 0; i < 200; ++i) {
        const std::uint64_t off = rng() % plain.size();
        const std::size_t len = 1 + rng() % std::min<std::uint64_t>(0x11000, plain.size() - off);
        Bytes got(len);
        if (!c.read(off, got.data(), len) || std::memcmp(got.data(), plain.data() + off, len) != 0) return false;
    }
    return true;
}

// ------------------------------------------------------------------ .iso --

static void test_iso(const Bytes& plain) {
    const Bytes iso = make_iso(plain);
    Mem mem{};
    auto opened = sw::open(stream_of(iso, mem));
    CHECK(opened.container != nullptr && opened.format == sw::Format::Raw);
    CHECK(sw::is_wii_disc(*opened.container));

    sw::WiiKeys none;
    std::string error;
    CHECK(sw::open_wii_data_partition(*opened.container, none, error) == nullptr);
    CHECK(error.find("keys.bin") != std::string::npos && error.find("RVZ") != std::string::npos);

    sw::WiiKeys wrong;
    wrong.hasCommonKey = true;
    std::memset(wrong.commonKey, 0x42, 16);
    error.clear();
    CHECK(sw::open_wii_data_partition(*opened.container, wrong, error) == nullptr);
    CHECK(error.find("does not decrypt") != std::string::npos);

    sw::WiiKeys right;
    right.hasCommonKey = true;
    std::memcpy(right.commonKey, kTestCommonKey, 16);
    error.clear();
    auto part = sw::open_wii_data_partition(*opened.container, right, error);
    CHECK(part != nullptr && error.empty());
    CHECK(part != nullptr && same_everywhere(*part, plain));

    // A Korean disc names a different common key.
    const Bytes korean = make_iso(plain, 1);
    Mem mem2{};
    auto k = sw::open(stream_of(korean, mem2));
    error.clear();
    CHECK(sw::open_wii_data_partition(*k.container, right, error) == nullptr);
    CHECK(error.find("Korean") != std::string::npos);
}

// ------------------------------------------------------------- key files --

static void write_file(const fs::path& p, const void* data, std::size_t size) {
    std::FILE* f = std::fopen(p.string().c_str(), "wb");
    std::fwrite(data, 1, size, f);
    std::fclose(f);
}

static void test_key_files(const fs::path& dir) {
    std::uint8_t key[16];
    std::string source, problem;
    fs::create_directories(dir / "none");
    CHECK(!sw::load_wii_common_key((dir / "none").string(), key, source, problem) && problem.empty());

    fs::create_directories(dir / "bin");
    write_file(dir / "bin" / "wii_common_key.bin", kTestCommonKey, 16);
    CHECK(sw::load_wii_common_key((dir / "bin").string(), key, source, problem) && std::memcmp(key, kTestCommonKey, 16) == 0);

    fs::create_directories(dir / "txt");
    const std::string hex = "0x10 32:54-76 98BADCFE0123456789ABCDEF\n";
    write_file(dir / "txt" / "wii_common_key.txt", hex.data(), hex.size());
    CHECK(sw::load_wii_common_key((dir / "txt").string(), key, source, problem) && std::memcmp(key, kTestCommonKey, 16) == 0);

    fs::create_directories(dir / "bootmii");
    Bytes otp(0x400, 0xEE);
    std::memcpy(otp.data() + 0x114, kTestCommonKey, 16);
    write_file(dir / "bootmii" / "keys.bin", otp.data(), otp.size());
    CHECK(sw::load_wii_common_key((dir / "bootmii").string(), key, source, problem) && std::memcmp(key, kTestCommonKey, 16) == 0);
    CHECK(source.find("keys.bin") != std::string::npos);

    fs::create_directories(dir / "bad");
    write_file(dir / "bad" / "wii_common_key.bin", kTestCommonKey, 15);
    CHECK(!sw::load_wii_common_key((dir / "bad").string(), key, source, problem) && !problem.empty());
}

// --------------------------------------------------------- the nod API --

static void test_nod_api(const Bytes& plain, const fs::path& keyDir) {
    const Bytes iso = make_iso(plain);
#ifdef _WIN32
    _putenv_s("DUSK_WII_KEY_DIR", (keyDir / "bootmii").string().c_str());
#else
    setenv("DUSK_WII_KEY_DIR", (keyDir / "bootmii").string().c_str(), 1);
#endif
    NodDiscStream stream{};
    stream.user_data = const_cast<Bytes*>(&iso);
    stream.read_at = [](void* u, uint64_t off, void* out, size_t len) -> int64_t {
        const Bytes& b = *static_cast<const Bytes*>(u);
        if (off >= b.size()) return 0;
        const size_t n = std::min<size_t>(len, b.size() - off);
        std::memcpy(out, b.data() + off, n);
        return static_cast<int64_t>(n);
    };
    stream.stream_len = [](void* u) -> int64_t { return static_cast<int64_t>(static_cast<const Bytes*>(u)->size()); };
    NodHandle* disc = nullptr;
    CHECK(nod_disc_open_stream(&stream, nullptr, &disc) == NOD_RESULT_OK);
    if (disc == nullptr) {
        std::printf("  (%s)\n", nod_error_message() ? nod_error_message() : "");
        return;
    }
    NodDiscHeader header{};
    CHECK(nod_disc_header(disc, &header) == NOD_RESULT_OK);
    CHECK(std::memcmp(&header, "RZDE01", 6) == 0);

    NodHandle* part = nullptr;
    CHECK(nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_DATA, nullptr, &part) == NOD_RESULT_OK);
    NodPartitionMeta meta{};
    CHECK(nod_partition_meta(part, &meta) == NOD_RESULT_OK);
    CHECK(meta.raw_dol.size == 0x300);  // the DOL found at its offset * 4
    CHECK(meta.raw_dol.data != nullptr && std::memcmp(meta.raw_dol.data, plain.data() + kDolOffset, 0x300) == 0);

    struct Found { uint32_t index = 0; bool seen = false; } found;
    nod_partition_iterate_fst(part, [](uint32_t index, NodNodeKind kind, const char* name, uint32_t size, void* u) -> uint32_t {
        auto* f = static_cast<Found*>(u);
        if (kind == NOD_NODE_KIND_FILE && std::strcmp(name, "a.bin") == 0 && size == sizeof kFileContents - 1) {
            f->index = index;
            f->seen = true;
        }
        return index + 1;
    }, &found);
    CHECK(found.seen);

    NodHandle* file = nullptr;
    CHECK(nod_partition_open_file(part, found.index, &file) == NOD_RESULT_OK);
    char buf[64] = {};
    CHECK(file != nullptr && nod_read(file, reinterpret_cast<uint8_t*>(buf), sizeof buf) == sizeof kFileContents - 1);
    CHECK(std::memcmp(buf, kFileContents, sizeof kFileContents - 1) == 0);

    // The disc handle reads the image itself: the (unencrypted) outer header and partition table.
    uint8_t outer[8];
    CHECK(nod_seek(disc, 0x40000, SEEK_SET) == 0x40000 && nod_read(disc, outer, sizeof outer) == sizeof outer);
    CHECK(outer[3] == 2);

    nod_free(file);
    nod_free(part);
    nod_free(disc);
}

// ------------------------------------------------------------ RVZ / WIA --

/// One partition group as RVZ/WIA store it: exception lists, then the data, maybe packed, maybe compressed.
struct GroupSpec {
    bool compressed = true;
    bool packed = false;
    std::uint16_t exceptions = 0;
};

static Bytes zstd_compress(const Bytes& in) {
    Bytes out(ZSTD_compressBound(in.size()));
    out.resize(ZSTD_compress(out.data(), out.size(), in.data(), in.size(), 3));
    return out;
}

/// Packs `data` (which sits at partition-data offset `offset`) as: a literal run, a junk run starting mid-sector,
/// then the rest literal. The junk bytes in `data` are overwritten with what the seed generates.
static Bytes pack_with_junk(Bytes& data, std::uint64_t offset) {
    // Somewhere inside the group that is not on a 32 KiB boundary of the partition data, so the generator has
    // to be advanced to the run's offset.
    std::size_t junkAt = data.size() / 2 + 0x123;
    if ((offset + junkAt) % 0x8000 == 0) junkAt += 0x40;
    const std::size_t junkLen = std::min<std::size_t>(0x300, data.size() - junkAt);
    std::uint8_t seed[68];
    for (int i = 0; i < 68; ++i) seed[i] = static_cast<std::uint8_t>(i * 7 + 1);
    RefJunk junk(seed);
    for (std::uint64_t i = 0; i < (offset + junkAt) % 0x8000; ++i) junk.next();
    for (std::size_t i = 0; i < junkLen; ++i) data[junkAt + i] = junk.next();
    Bytes out;
    auto lit = [&](std::size_t from, std::size_t len) {
        Bytes w(4); put32(w, 0, static_cast<std::uint32_t>(len));
        out.insert(out.end(), w.begin(), w.end());
        out.insert(out.end(), data.begin() + from, data.begin() + from + len);
    };
    lit(0, junkAt);
    Bytes w(4); put32(w, 0, 0x80000000u | static_cast<std::uint32_t>(junkLen));
    out.insert(out.end(), w.begin(), w.end());
    out.insert(out.end(), seed, seed + 68);
    if (junkAt + junkLen < data.size()) lit(junkAt + junkLen, data.size() - junkAt - junkLen);
    return out;
}

/// Builds an RVZ (rvz = true, zstd) or WIA (no compression) file holding the disc head as raw data and the game
/// partition decrypted in groups. `plain` is updated where packing replaced bytes with generated junk.
static Bytes make_rvz_or_wia(Bytes& plain, bool rvz, std::uint32_t chunk, const std::vector<GroupSpec>& specs,
    std::uint32_t firstRunSectors) {
    const std::uint64_t dataStart = kPartition + kDataOffset;
    const Bytes head = make_disc_head(dataStart);
    const std::uint64_t discSize = dataStart + kClusters * kCluster;
    const std::uint64_t spg = chunk / kCluster;

    Bytes file(0x48 + 0x100, 0);
    std::memcpy(file.data(), rvz ? "RVZ\x01" : "WIA\x01", 4);
    put32(file, 0x0C, 0x100);  // disc struct size
    put64(file, 0x24, discSize);
    const std::size_t d = 0x48;
    put32(file, d + 0x00, 2);  // Wii
    put32(file, d + 0x04, rvz ? 5 : 0);
    put32(file, d + 0x0C, chunk);
    std::memcpy(file.data() + d + 0x10, head.data(), 0x80);

    std::vector<Bytes> groupEntries;  // 8 or 12 bytes each
    auto add_group = [&](const Bytes& stored, bool compressed, std::uint32_t packedSize) {
        while (file.size() % 4) file.push_back(0);
        Bytes e(rvz ? 12 : 8, 0);
        put32(e, 0, static_cast<std::uint32_t>(file.size() / 4));
        put32(e, 4, static_cast<std::uint32_t>(stored.size()) | (rvz && compressed ? 0x80000000u : 0));
        if (rvz) put32(e, 8, packedSize);
        file.insert(file.end(), stored.begin(), stored.end());
        groupEntries.push_back(e);
    };

    // Raw data: the disc head, from 0 to the partition data, in chunk-sized groups.
    const std::uint32_t rawGroups = static_cast<std::uint32_t>((dataStart + chunk - 1) / chunk);
    for (std::uint32_t g = 0; g < rawGroups; ++g) {
        const std::uint64_t from = std::uint64_t{g} * chunk;
        Bytes piece(head.begin() + from, head.begin() + std::min<std::uint64_t>(from + chunk, dataStart));
        if (rvz) add_group(zstd_compress(piece), true, 0);
        else add_group(piece, false, 0);
    }

    // The partition: pd[0] = firstRunSectors, pd[1] = the rest, each in its own groups.
    std::uint32_t specIndex = 0;
    const std::uint32_t partGroupStart = static_cast<std::uint32_t>(groupEntries.size());
    std::uint32_t runGroups[2] = {0, 0};
    const std::uint64_t runSectors[2] = {firstRunSectors, kClusters - firstRunSectors};
    std::uint64_t sectorBase = 0;
    for (int run = 0; run < 2; ++run) {
        for (std::uint64_t s = 0; s < runSectors[run]; s += spg) {
            const std::uint64_t n = std::min<std::uint64_t>(spg, runSectors[run] - s);
            const std::uint64_t off = (sectorBase + s) * kClusterData;
            Bytes data(plain.begin() + off, plain.begin() + off + n * kClusterData);
            const GroupSpec spec = specIndex < specs.size() ? specs[specIndex] : GroupSpec{};
            ++specIndex;
            const std::uint64_t lists = std::max<std::uint64_t>(1, chunk / 0x200000);
            Bytes body;
            for (std::uint64_t l = 0; l < lists; ++l) {
                Bytes list(2, 0);
                put16(list, 0, spec.exceptions);
                for (std::uint16_t x = 0; x < spec.exceptions; ++x) {
                    for (int k = 0; k < 22; ++k) list.push_back(static_cast<std::uint8_t>(0xC0 + x + k));
                }
                body.insert(body.end(), list.begin(), list.end());
            }
            const bool compressed = rvz && spec.compressed;
            if (!compressed) while (body.size() % 4) body.push_back(0);
            std::uint32_t packedSize = 0;
            Bytes payload = data;
            if (rvz && spec.packed) {
                payload = pack_with_junk(data, off);
                std::memcpy(plain.data() + off, data.data(), data.size());  // the junk is what the disc holds
                packedSize = static_cast<std::uint32_t>(payload.size());
            }
            body.insert(body.end(), payload.begin(), payload.end());
            add_group(compressed ? zstd_compress(body) : body, compressed, packedSize);
            ++runGroups[run];
        }
        sectorBase += runSectors[run];
    }

    // Partition entry (stored as-is), raw-data table and group table (compressed like the data, for RVZ).
    while (file.size() % 4) file.push_back(0);
    const std::uint64_t partOff = file.size();
    Bytes part(48, 0);
    std::memcpy(part.data(), kTitleKey, 16);
    put32(part, 16, static_cast<std::uint32_t>(dataStart / kCluster));
    put32(part, 20, firstRunSectors);
    put32(part, 24, partGroupStart);
    put32(part, 28, runGroups[0]);
    put32(part, 32, static_cast<std::uint32_t>(dataStart / kCluster + firstRunSectors));
    put32(part, 36, static_cast<std::uint32_t>(kClusters - firstRunSectors));
    put32(part, 40, partGroupStart + runGroups[0]);
    put32(part, 44, runGroups[1]);
    file.insert(file.end(), part.begin(), part.end());

    Bytes raw(24, 0);
    put64(raw, 0, 0x80);
    put64(raw, 8, dataStart - 0x80);
    put32(raw, 16, 0);
    put32(raw, 20, rawGroups);
    Bytes groups;
    for (const Bytes& e : groupEntries) groups.insert(groups.end(), e.begin(), e.end());
    const Bytes rawStored = rvz ? zstd_compress(raw) : raw;
    const Bytes groupsStored = rvz ? zstd_compress(groups) : groups;
    const std::uint64_t rawOff = file.size();
    file.insert(file.end(), rawStored.begin(), rawStored.end());
    const std::uint64_t groupOff = file.size();
    file.insert(file.end(), groupsStored.begin(), groupsStored.end());

    put32(file, d + 0x90, 1);
    put32(file, d + 0x94, 48);
    put64(file, d + 0x98, partOff);
    put32(file, d + 0xB4, 1);
    put64(file, d + 0xB8, rawOff);
    put32(file, d + 0xC0, static_cast<std::uint32_t>(rawStored.size()));
    put32(file, d + 0xC4, static_cast<std::uint32_t>(groupEntries.size()));
    put64(file, d + 0xC8, groupOff);
    put32(file, d + 0xD0, static_cast<std::uint32_t>(groupsStored.size()));
    return file;
}

static void test_stored(bool rvz, std::uint32_t chunk, const std::vector<GroupSpec>& specs, std::uint32_t firstRun,
    const char* what) {
    Bytes plain = make_partition_plain();
    const Bytes file = make_rvz_or_wia(plain, rvz, chunk, specs, firstRun);
    Mem mem{};
    auto opened = sw::open(stream_of(file, mem));
    CHECK(opened.container != nullptr);
    if (opened.container == nullptr) {
        std::printf("  %s: %s\n", what, opened.error.c_str());
        return;
    }
    CHECK(opened.format == (rvz ? sw::Format::Rvz : sw::Format::Wia));
    CHECK(sw::is_wii_disc(*opened.container));
    sw::WiiKeys none;  // stored decrypted: no key needed
    std::string error;
    auto part = sw::open_wii_data_partition(*opened.container, none, error);
    CHECK(part != nullptr);
    if (part == nullptr) {
        std::printf("  %s: %s\n", what, error.c_str());
        return;
    }
    const bool same = same_everywhere(*part, plain);
    CHECK(same);
    if (!same) std::printf("  %s: partition data differs\n", what);
}

int main(int argc, char** argv) {
    const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "dusk-wii-disc-test";
    fs::create_directories(dir);
    const Bytes plain = make_partition_plain();

    test_iso(plain);
    test_key_files(dir / "keys");
    test_nod_api(plain, dir / "keys");

    // RVZ, 128 KiB chunks (4 sectors a group, one exception list each): compressed, stored, compressed + packed,
    // stored + packed, with exceptions to skip; the first run ends mid-group so the second starts a fresh one.
    test_stored(true, 0x20000,
        {{true, false, 2}, {false, false, 1}, {true, true, 0}, {false, true, 3}, {true, false, 0}}, 6, "rvz");
    // RVZ, 32 KiB chunks: one sector a group.
    test_stored(true, 0x8000, {{true, true, 1}, {false, false, 0}}, 2, "rvz-32k");
    // WIA without compression, 2 MiB chunks (padding after the exception lists).
    test_stored(false, 0x200000, {{false, false, 5}}, 13, "wia");

    if (g_failures == 0) {
        std::printf("wii_disc: all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
