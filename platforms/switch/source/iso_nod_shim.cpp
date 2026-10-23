// A `nod`-compatible reader for uncompressed GameCube disc images.
//
// Aurora reads discs through nod's C API (`extern/aurora/lib/dolphin/dvd`).
// nod itself is Rust, and cross-building it for Horizon drags cargo and a
// custom sysroot into a build that otherwise needs nothing but devkitA64 --
// which is most of what makes this port hard to reproduce.
//
// Nothing in Aurora's DVD layer needs nod specifically: it needs the ten
// entry points below. A GameCube disc image is a flat file with a documented
// layout, so those ten are implementable directly, and this file does that.
// Aurora's own sources are untouched.
//
// Scope: GameCube images in any container disc_container.hpp reads, and (aleks2, rc17) Wii images: their game
// (DATA) partition, decrypted and without hashes, through open_wii_data_partition. The disc handle reads the image
// as it is; the partition and file handles read the partition. A Wii partition stores its DOL/FST offsets and the
// FST's file offsets divided by 4, which is undone here.
//
// Layout (Yet Another GameCube Documentation, and Dolphin's DiscIO):
//   0x000  disc header ("boot.bin", 0x440 bytes)
//     0x000 game_id[6], disc_num, disc_version, audio_streaming,
//           audio_stream_buf_size
//     0x01C magic 0xC2339F3D
//     0x020 game title (0x400-0x20 bytes, NUL padded)
//     0x420 debug monitor offset / load address
//     0x420+ main.dol offset, FST offset, FST size, FST max size
//   0x440  bi2.bin (0x2000 bytes)
//   0x2440 apploader
//   FST: entries of 12 bytes, then a string table
//     u8 kind (0 = file, 1 = directory)
//     u24 name offset into the string table
//     u32 file: data offset      directory: parent entry index
//     u32 file: size             directory: index one past its last child
//   Entry 0 is the root; its length field is the total entry count.

#include <nod.h>

#include "switch/disc_container.hpp"
#include "switch/wii_crypto.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kGamecubeMagic = 0xC2339F3D;
constexpr uint32_t kWiiMagic = 0x5D1C9EA3;
constexpr int64_t kBootSize = 0x440;
constexpr int64_t kBi2Offset = 0x440;
constexpr int64_t kBi2Size = 0x2000;
constexpr int64_t kApploaderOffset = 0x2440;
constexpr size_t kFstEntrySize = 12;

uint32_t read_be32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}

uint32_t read_be24(const uint8_t* p) {
    return (uint32_t{p[0]} << 16) | (uint32_t{p[1]} << 8) | uint32_t{p[2]};
}

// One open image. The stream callbacks come from Aurora, which hands us an
// SDL_IOStream; read_at is absolute, so no shared seek position to protect.
struct Disc {
    NodDiscStream stream{};
    // Between the file and the disc. For an .iso the two are the same thing
    // and this is a passthrough; for .rvz, .wia and .gcz it is where the
    // decompression happens. Everything below -- the metadata, the FST, the
    // file reads -- is written against disc offsets and never learns which.
    std::unique_ptr<dusk::sw::disc::Container> container;
    int64_t length = 0;
    // aleks2 (rc17): what the partition and file handles read. For a GameCube disc it is `container`; for a Wii
    // disc it is the game partition, decrypted and without hashes (owned by `partition`, which reads through
    // `container` and so is declared after it, to go first).
    std::unique_ptr<dusk::sw::disc::Container> partition;
    dusk::sw::disc::Container* data = nullptr;
    int64_t dataLength = 0;
    bool wii = false;
    std::vector<uint8_t> discBoot;    // the image's own header (nod_disc_header); a Wii partition has its own

    std::vector<uint8_t> boot;        // 0x440
    std::vector<uint8_t> bi2;         // 0x2000
    std::vector<uint8_t> apploader;   // to the start of main.dol
    std::vector<uint8_t> dol;         // main.dol, sized from its own header
    std::vector<uint8_t> fst;         // entries + string table

    uint32_t fstEntryCount = 0;
    const uint8_t* fstStrings = nullptr;
    size_t fstStringsSize = 0;

    // `whole`: the image itself (the disc handle) rather than the game partition.
    int64_t read_at(void* buf, size_t len, int64_t offset, bool whole = false) const {
        dusk::sw::disc::Container* from = whole ? container.get() : data;
        if (from == nullptr || offset < 0) {
            return -1;
        }
        if (!from->read(static_cast<uint64_t>(offset), buf, len)) {
            return -1;
        }
        return static_cast<int64_t>(len);
    }

    bool read_exact(void* buf, size_t len, int64_t offset) const {
        int64_t done = 0;
        while (static_cast<size_t>(done) < len) {
            const int64_t n =
                read_at(static_cast<uint8_t*>(buf) + done, len - static_cast<size_t>(done), offset + done);
            if (n <= 0) {
                return false;
            }
            done += n;
        }
        return true;
    }

    const uint8_t* entry(uint32_t index) const {
        if (index >= fstEntryCount) {
            return nullptr;
        }
        return fst.data() + size_t{index} * kFstEntrySize;
    }

    const char* name_of(uint32_t index) const {
        const uint8_t* e = entry(index);
        if (e == nullptr) {
            return "";
        }
        const uint32_t off = read_be24(e + 1);
        if (off >= fstStringsSize) {
            return "";
        }
        return reinterpret_cast<const char*>(fstStrings + off);
    }
};

// What a NodHandle points at. One type, three roles: nod hands the same
// opaque handle back for a disc, a partition and an open file, and frees them
// all through nod_free.
enum class HandleKind { Disc, Partition, File };

struct FileCursor {
    int64_t offset = 0;  // absolute, in the image
    int64_t size = 0;
    int64_t pos = 0;
};

}  // namespace

// Opaque to callers; the name has to match nod's forward declaration.
struct NodHandle {
    HandleKind kind = HandleKind::Disc;
    std::shared_ptr<Disc> disc;
    FileCursor file{};
};

namespace {

thread_local std::string t_lastError;

NodResult fail(const char* message) {
    t_lastError = message;
    return NOD_RESULT_ERR_INVALID_HANDLE;
}

// main.dol's size is not stored anywhere: it is the end of the last section
// the DOL header describes. Text and data offsets/sizes are big-endian at the
// top of the file (7 text + 11 data sections), so the size is the largest
// offset + size across all of them.
uint32_t dol_size(const uint8_t* header) {
    uint32_t end = 0x100;  // the header itself
    for (int i = 0; i < 18; ++i) {
        const uint32_t offset = read_be32(header + 0x00 + i * 4);
        const uint32_t size = read_be32(header + 0x90 + i * 4);
        if (offset == 0 || size == 0) {
            continue;
        }
        end = std::max(end, offset + size);
    }
    return end;
}

bool load_metadata(Disc& d) {
    d.boot.resize(kBootSize);
    if (!d.read_exact(d.boot.data(), d.boot.size(), 0)) {
        return false;
    }
    if (d.wii ? read_be32(d.boot.data() + 0x18) != kWiiMagic : read_be32(d.boot.data() + 0x1C) != kGamecubeMagic) {
        return false;
    }

    // A Wii partition stores these divided by 4.
    const int shift = d.wii ? 2 : 0;
    const uint64_t dolOffset = uint64_t{read_be32(d.boot.data() + 0x420)} << shift;
    const uint64_t fstOffset = uint64_t{read_be32(d.boot.data() + 0x424)} << shift;
    const uint64_t fstSize = uint64_t{read_be32(d.boot.data() + 0x428)} << shift;
    if (fstSize > (64u << 20)) {
        return false;
    }
    if (fstOffset == 0 || fstSize < kFstEntrySize) {
        return false;
    }

    d.bi2.resize(kBi2Size);
    if (!d.read_exact(d.bi2.data(), d.bi2.size(), kBi2Offset)) {
        d.bi2.clear();
    }

    if (dolOffset > kApploaderOffset) {
        d.apploader.resize(dolOffset - kApploaderOffset);
        if (!d.read_exact(d.apploader.data(), d.apploader.size(), kApploaderOffset)) {
            d.apploader.clear();
        }
    }

    if (dolOffset != 0) {
        uint8_t dolHeader[0x100];
        if (d.read_exact(dolHeader, sizeof dolHeader, dolOffset)) {
            const uint32_t size = dol_size(dolHeader);
            if (size > 0 && static_cast<int64_t>(dolOffset) + size <= d.dataLength) {
                d.dol.resize(size);
                if (!d.read_exact(d.dol.data(), d.dol.size(), dolOffset)) {
                    d.dol.clear();
                }
            }
        }
    }

    d.fst.resize(fstSize);
    if (!d.read_exact(d.fst.data(), d.fst.size(), fstOffset)) {
        return false;
    }
    const uint32_t count = read_be32(d.fst.data() + 8);  // root entry's length
    if (count == 0 || size_t{count} * kFstEntrySize > d.fst.size()) {
        return false;
    }
    d.fstEntryCount = count;
    d.fstStrings = d.fst.data() + size_t{count} * kFstEntrySize;
    d.fstStringsSize = d.fst.size() - size_t{count} * kFstEntrySize;
    return true;
}

NodBlob blob(const std::vector<uint8_t>& v) {
    return NodBlob{v.empty() ? nullptr : v.data(), v.size()};
}

}  // namespace

extern "C" {

const char* nod_error_message(void) {
    return t_lastError.empty() ? nullptr : t_lastError.c_str();
}

NodResult nod_disc_open_stream(const NodDiscStream* stream, const NodDiscOptions* options, NodHandle** out) {
    (void)options;  // preloader threads: this reader is synchronous
    if (stream == nullptr || out == nullptr || stream->read_at == nullptr || stream->stream_len == nullptr) {
        return fail("null disc stream");
    }
    auto disc = std::make_shared<Disc>();
    disc->stream = *stream;

    dusk::sw::disc::RawStream raw;
    raw.user = disc.get();
    raw.read_at = [](void* user, uint64_t offset, void* out, size_t length) -> int64_t {
        const auto& s = static_cast<Disc*>(user)->stream;
        return s.read_at(s.user_data, offset, out, length);
    };
    raw.length = [](void* user) -> uint64_t {
        const auto& s = static_cast<Disc*>(user)->stream;
        return static_cast<uint64_t>(s.stream_len(s.user_data));
    };

    auto opened = dusk::sw::disc::open(raw);
    if (opened.container == nullptr) {
        // The message is the interesting part. Somebody whose copy is a .rvz
        // used to be told "not a GameCube disc image", which is both wrong
        // and useless; this says what the file is and what to do with it.
        t_lastError = opened.error;
        return NOD_RESULT_ERR_FORMAT;
    }
    disc->container = std::move(opened.container);
    disc->length = static_cast<int64_t>(disc->container->size());
    if (disc->length <= kApploaderOffset) {
        return fail("disc image too small");
    }
    disc->data = disc->container.get();
    disc->dataLength = disc->length;
    disc->discBoot.resize(kBootSize);
    if (!disc->read_exact(disc->discBoot.data(), disc->discBoot.size(), 0)) {
        return fail("could not read the disc header");
    }

    // aleks2 (rc17): a Wii disc. The game is in its DATA partition; an encrypted image needs the player's own
    // common key (wii_crypto.hpp), read from the data directory -- never part of this program.
    if (dusk::sw::disc::is_wii_disc(*disc->container)) {
        dusk::sw::disc::WiiKeys keys;
#ifdef __SWITCH__
        const std::string keyDir = "sdmc:/switch/dusklight";
#else
        const char* env = std::getenv("DUSK_WII_KEY_DIR");
        const std::string keyDir = env != nullptr ? env : ".";
#endif
        std::string keySource;
        std::string keyProblem;
        keys.hasCommonKey = dusk::sw::disc::load_wii_common_key(keyDir, keys.commonKey, keySource, keyProblem);
        std::string error;
        disc->partition = dusk::sw::disc::open_wii_data_partition(*disc->container, keys, error);
        if (disc->partition == nullptr) {
            t_lastError = keyProblem.empty() ? error : error + " (" + keyProblem + ")";
            return NOD_RESULT_ERR_FORMAT;
        }
        disc->wii = true;
        disc->data = disc->partition.get();
        disc->dataLength = static_cast<int64_t>(disc->partition->size());
    }

    if (!load_metadata(*disc)) {
        t_lastError = std::string("the ") + dusk::sw::disc::format_name(opened.format) + " image opened, but it is " +
                      (disc->wii ? "not a readable Wii game partition" : "not a GameCube disc");
        return NOD_RESULT_ERR_FORMAT;
    }
    auto* handle = new NodHandle{};
    handle->kind = HandleKind::Disc;
    handle->disc = std::move(disc);
    *out = handle;
    return NOD_RESULT_OK;
}

NodResult nod_disc_header(const NodHandle* disc, NodDiscHeader* out) {
    if (disc == nullptr || out == nullptr || disc->disc == nullptr) {
        return fail("null disc handle");
    }
    // The struct mirrors the on-disc header byte for byte, so this is a copy,
    // not a parse. Only the first 0x400 are described by it; the rest of
    // boot.bin (the offsets Aurora reads through nod_partition_meta) stays in
    // raw_boot.
    const auto& boot = disc->disc->discBoot;
    std::memset(out, 0, sizeof *out);
    const size_t n = std::min(sizeof *out, boot.size());
    std::memcpy(out, boot.data(), n);
    return NOD_RESULT_OK;
}

NodResult nod_disc_open_partition_kind(NodHandle* disc, uint32_t kind, const NodPartitionOptions* options,
                                       NodHandle** out) {
    (void)options;
    if (disc == nullptr || out == nullptr || disc->disc == nullptr) {
        return fail("null disc handle");
    }
    // A GameCube disc has no partition table: the whole image is the data
    // partition, so any kind but DATA is genuinely absent. A Wii disc's other
    // partitions (the system update) are never needed by the game.
    if (kind != NOD_PARTITION_KIND_DATA) {
        t_lastError = "only the game (data) partition is available";
        return NOD_RESULT_ERR_NOT_FOUND;
    }
    auto* handle = new NodHandle{};
    handle->kind = HandleKind::Partition;
    handle->disc = disc->disc;
    *out = handle;
    return NOD_RESULT_OK;
}

NodResult nod_partition_meta(const NodHandle* partition, NodPartitionMeta* out) {
    if (partition == nullptr || out == nullptr || partition->disc == nullptr) {
        return fail("null partition handle");
    }
    const auto& d = *partition->disc;
    std::memset(out, 0, sizeof *out);
    out->raw_boot = blob(d.boot);
    out->raw_bi2 = blob(d.bi2);
    out->raw_apploader = blob(d.apploader);
    out->raw_dol = blob(d.dol);
    out->raw_fst = blob(d.fst);
    // Ticket, TMD, certificate chain and H3 table stay null: Aurora reads only the DOL from here.
    return NOD_RESULT_OK;
}

void nod_partition_iterate_fst(const NodHandle* partition, NodFstCallback callback, void* user_data) {
    if (partition == nullptr || callback == nullptr || partition->disc == nullptr) {
        return;
    }
    const auto& d = *partition->disc;
    // Entry 0 is the root and is never reported: Aurora starts with a root of
    // its own and uses the callback's index to close directories, so handing
    // it the root would nest everything one level too deep.
    uint32_t index = 1;
    while (index < d.fstEntryCount) {
        const uint8_t* e = d.entry(index);
        const bool isDir = e[0] != 0;
        // For a directory the callback wants the index one past its last
        // child, which is exactly what the FST stores; for a file it wants
        // the size. Both live in the same field.
        const uint32_t sizeOrNext = read_be32(e + 8);
        const uint32_t next = callback(index, isDir ? NOD_NODE_KIND_DIRECTORY : NOD_NODE_KIND_FILE,
                                       d.name_of(index), sizeOrNext, user_data);
        if (next == NOD_FST_STOP || next <= index) {
            break;  // the callback asked to stop, or would not advance
        }
        index = next;
    }
}

NodResult nod_partition_open_file(NodHandle* partition, uint32_t fst_index, NodHandle** out) {
    if (partition == nullptr || out == nullptr || partition->disc == nullptr) {
        return fail("null partition handle");
    }
    const auto& d = *partition->disc;
    const uint8_t* e = d.entry(fst_index);
    if (e == nullptr || e[0] != 0) {
        t_lastError = "no such file in the FST";
        return NOD_RESULT_ERR_NOT_FOUND;
    }
    auto* handle = new NodHandle{};
    handle->kind = HandleKind::File;
    handle->disc = partition->disc;
    handle->file.offset = static_cast<int64_t>(uint64_t{read_be32(e + 4)} << (d.wii ? 2 : 0));  // Wii: >> 2 on disc
    handle->file.size = read_be32(e + 8);
    handle->file.pos = 0;
    *out = handle;
    return NOD_RESULT_OK;
}

int64_t nod_read(NodHandle* handle, uint8_t* buf, size_t len) {
    if (handle == nullptr || buf == nullptr || handle->disc == nullptr) {
        return -1;
    }
    const auto& d = *handle->disc;
    // The disc handle reads the image as it is; the partition and its files read the game partition.
    const bool whole = handle->kind == HandleKind::Disc;
    int64_t base = 0;
    int64_t limit = whole ? d.length : d.dataLength;
    if (handle->kind == HandleKind::File) {
        base = handle->file.offset;
        limit = handle->file.size;
    }
    if (handle->file.pos >= limit) {
        return 0;  // EOF, not an error
    }
    const int64_t want = std::min<int64_t>(static_cast<int64_t>(len), limit - handle->file.pos);
    const int64_t got = d.read_at(buf, static_cast<size_t>(want), base + handle->file.pos, whole);
    if (got > 0) {
        handle->file.pos += got;
    }
    return got;
}

int64_t nod_seek(NodHandle* handle, int64_t offset, int32_t whence) {
    if (handle == nullptr || handle->disc == nullptr) {
        return -1;
    }
    const int64_t size = handle->kind == HandleKind::File        ? handle->file.size
                         : handle->kind == HandleKind::Partition ? handle->disc->dataLength
                                                                 : handle->disc->length;
    int64_t target = offset;
    if (whence == SEEK_CUR) {
        target = handle->file.pos + offset;
    } else if (whence == SEEK_END) {
        target = size + offset;
    }
    if (target < 0) {
        return -1;
    }
    handle->file.pos = target;  // seeking past the end is allowed; reads return 0
    return target;
}

void nod_free(NodHandle* handle) {
    delete handle;
}

}  // extern "C"
