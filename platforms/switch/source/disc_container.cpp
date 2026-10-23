// aleks2: RVZ, WIA and GCZ. See switch/disc_container.hpp for why.
//
// Three formats, one shape: the disc is cut into fixed-size blocks, each
// block is stored somewhere in the file, and reading disc offset N means
// finding its block, decoding it and handing back a slice. The differences
// are in how the block table is spelled and what "decoding" means.
//
// The one genuinely strange part is junk. A GameCube disc is not zero-padded:
// its unused space is filled by a lagged Fibonacci generator, which is why an
// honest dump of a 1.36 GB game is 1.46 GB of incompressible noise. RVZ
// stores a 68-byte seed in place of each 32 KiB of it, and expects the reader
// to produce the same bytes back. The generator is at the bottom of this file
// and it has two details that are easy to get wrong and impossible to notice:
// see JunkGenerator.
#include "switch/disc_container.hpp"
#include "switch/wii_crypto.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <vector>

#include <zlib.h>
#include <zstd.h>
#include <bzlib.h>
#include <lzma.h>

namespace dusk::sw::disc {
namespace {

constexpr std::uint32_t kGcnMagic = 0xC2339F3Du;
constexpr std::uint32_t kWiiMagic = 0x5D1C9EA3u;
constexpr std::uint32_t kGczMagic = 0xB10BC001u;
constexpr std::uint64_t kSectorSize = 0x8000;  // what the junk seed covers

// How many decoded blocks to keep. The game reads the disc in small pieces --
// a few hundred bytes of an archive at a time -- so without this every read
// would decompress its whole 128 KiB block again. Eight blocks is a megabyte,
// which is nothing here and turns a directory walk from hundreds of
// decompressions into a handful.
constexpr std::size_t kCachedBlocks = 8;

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) |
           std::uint32_t{p[3]};
}

std::uint64_t be64(const std::uint8_t* p) {
    return (std::uint64_t{be32(p)} << 32) | be32(p + 4);
}

std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) |
           (std::uint32_t{p[3]} << 24);
}

std::uint64_t le64(const std::uint8_t* p) {
    return std::uint64_t{le32(p)} | (std::uint64_t{le32(p + 4)} << 32);
}

bool read_exact(const RawStream& stream, std::uint64_t offset, void* out, std::size_t length) {
    auto* bytes = static_cast<std::uint8_t*>(out);
    std::size_t done = 0;
    while (done < length) {
        const std::int64_t n = stream.read_at(stream.user, offset + done, bytes + done, length - done);
        if (n <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

// ---------------------------------------------------------------- codecs --

enum class Codec { None, Zlib, Zstd, Bzip2, Lzma, Lzma2, Purge, Unsupported };

/// Decodes one block. `out` is sized by the caller to the exact expected
/// length, because every one of these formats knows it in advance -- which is
/// also the check: a codec that produces a different amount has gone wrong.
bool decode(Codec codec, const std::uint8_t* in, std::size_t inSize, std::uint8_t* out,
    std::size_t outSize, const std::uint8_t* props, std::size_t propsSize) {
    switch (codec) {
    case Codec::None:
        if (inSize != outSize) {
            return false;
        }
        std::memcpy(out, in, outSize);
        return true;

    case Codec::Zlib: {
        uLongf produced = static_cast<uLongf>(outSize);
        return uncompress(out, &produced, in, static_cast<uLong>(inSize)) == Z_OK &&
               produced == outSize;
    }

    case Codec::Zstd: {
        const std::size_t produced = ZSTD_decompress(out, outSize, in, inSize);
        return !ZSTD_isError(produced) && produced == outSize;
    }

    case Codec::Bzip2: {
        unsigned int produced = static_cast<unsigned int>(outSize);
        return BZ2_bzBuffToBuffDecompress(reinterpret_cast<char*>(out), &produced,
                   const_cast<char*>(reinterpret_cast<const char*>(in)),
                   static_cast<unsigned int>(inSize), 0, 0) == BZ_OK &&
               produced == outSize;
    }

    case Codec::Lzma:
    case Codec::Lzma2: {
        // Raw streams: the properties live once in the file header rather than
        // at the front of every block, which is why this cannot just be
        // lzma_stream_buffer_decode.
        lzma_filter filters[2];
        filters[0].id = codec == Codec::Lzma ? LZMA_FILTER_LZMA1 : LZMA_FILTER_LZMA2;
        filters[0].options = nullptr;
        filters[1].id = LZMA_VLI_UNKNOWN;
        filters[1].options = nullptr;
        // Allocates filters[0].options, which lzma_filters_free releases below.
        if (lzma_properties_decode(&filters[0], nullptr, props, propsSize) != LZMA_OK) {
            return false;
        }

        lzma_stream stream = LZMA_STREAM_INIT;
        if (lzma_raw_decoder(&stream, filters) != LZMA_OK) {
            lzma_filters_free(filters, nullptr);
            return false;
        }
        stream.next_in = in;
        stream.avail_in = inSize;
        stream.next_out = out;
        stream.avail_out = outSize;
        const lzma_ret ret = lzma_code(&stream, LZMA_FINISH);
        const bool ok = (ret == LZMA_OK || ret == LZMA_STREAM_END) && stream.avail_out == 0;
        lzma_end(&stream);
        lzma_filters_free(filters, nullptr);
        return ok;
    }

    default:
        return false;
    }
}

const char* codec_name(Codec codec) {
    switch (codec) {
    case Codec::None: return "none";
    case Codec::Zlib: return "zlib";
    case Codec::Zstd: return "zstd";
    case Codec::Bzip2: return "bzip2";
    case Codec::Lzma: return "lzma";
    case Codec::Lzma2: return "lzma2";
    case Codec::Purge: return "purge";
    default: return "unknown";
    }
}

// ------------------------------------------------------------------ junk --

/// Nintendo's disc padding, regenerated rather than stored.
///
/// A lagged Fibonacci generator, k = 521, j = 32, over XOR. Two details are
/// worth stating because both produce output that looks like noise, passes
/// every eyeball test, and is not the disc:
///
///   - the third byte of each word comes out shifted by 18, not 16. Applying
///     that to the whole state once at the start is the same as doing it on
///     the way out, and it MUST happen before the four warm-up advances,
///     because those XOR whole words together and the shift does not commute
///     with that.
///   - the advance is x[i] ^= x[i-j], wrapping at the front. Written the
///     other way round it is still a perfectly good PRNG and never matches.
class JunkGenerator {
public:
    explicit JunkGenerator(const std::uint8_t* seed) {
        for (std::size_t i = 0; i < kSeedWords; ++i) {
            m_state[i] = be32(seed + i * 4);
        }
        for (std::size_t i = kSeedWords; i < kK; ++i) {
            m_state[i] = (m_state[i - 17] << 23) ^ (m_state[i - 16] >> 9) ^ m_state[i - 1];
        }
        for (std::uint32_t& word : m_state) {
            word = (word & 0xFF00FFFFu) | ((word >> 2) & 0x00FF0000u);
        }
        for (int i = 0; i < 4; ++i) {
            advance();
        }
    }

    /// Moves forward without producing anything. Whole states are skipped by
    /// advancing, not by generating and discarding.
    void skip(std::size_t count) {
        m_position += count;
        while (m_position >= kStateBytes) {
            advance();
            m_position -= kStateBytes;
        }
    }

    void generate(std::uint8_t* out, std::size_t count) {
        while (count > 0) {
            const std::size_t take = std::min(count, kStateBytes - m_position);
            for (std::size_t i = 0; i < take; ++i) {
                const std::size_t at = m_position + i;
                out[i] = static_cast<std::uint8_t>(m_state[at / 4] >> (24 - 8 * (at % 4)));
            }
            out += take;
            count -= take;
            m_position += take;
            if (m_position == kStateBytes) {
                advance();
                m_position = 0;
            }
        }
    }

private:
    static constexpr std::size_t kK = 521;
    static constexpr std::size_t kJ = 32;
    static constexpr std::size_t kSeedWords = 17;
    static constexpr std::size_t kStateBytes = kK * 4;

    void advance() {
        for (std::size_t i = 0; i < kJ; ++i) {
            m_state[i] ^= m_state[i + kK - kJ];
        }
        for (std::size_t i = kJ; i < kK; ++i) {
            m_state[i] ^= m_state[i - kJ];
        }
    }

    std::array<std::uint32_t, kK> m_state{};
    std::size_t m_position = 0;
};

// ------------------------------------------------------------ containers --

/// The block cache and the read-splitting logic, which are the same for every
/// format. A subclass says how big a block is, how many there are, and how to
/// produce one.
class BlockContainer : public Container {
public:
    bool read(std::uint64_t offset, void* out, std::size_t length) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto* bytes = static_cast<std::uint8_t*>(out);
        while (length > 0) {
            if (offset >= m_discSize) {
                return false;
            }
            const std::uint64_t index = block_index(offset);
            const std::vector<std::uint8_t>* block = cached(index);
            if (block == nullptr) {
                return false;
            }
            const std::uint64_t start = block_start(index);
            if (offset < start || offset - start >= block->size()) {
                return false;
            }
            const std::size_t within = static_cast<std::size_t>(offset - start);
            const std::size_t take = std::min(length, block->size() - within);
            std::memcpy(bytes, block->data() + within, take);
            bytes += take;
            offset += take;
            length -= take;
        }
        return true;
    }

    std::uint64_t size() const override { return m_discSize; }

protected:
    /// Which block holds this disc offset, and where that block begins.
    virtual std::uint64_t block_index(std::uint64_t offset) const = 0;
    virtual std::uint64_t block_start(std::uint64_t index) const = 0;
    virtual bool produce(std::uint64_t index, std::vector<std::uint8_t>& out) = 0;

    std::uint64_t m_discSize = 0;
    // aleks2 (rc17): how many blocks to keep. Wii clusters are 31 KiB and a cache miss costs an AES pass over one,
    // so those keep more (a few MiB) than the default.
    std::size_t m_cacheBlocks = kCachedBlocks;

private:
    struct Entry {
        std::uint64_t index = ~0ull;
        std::vector<std::uint8_t> data;
    };

    const std::vector<std::uint8_t>* cached(std::uint64_t index) {
        for (std::size_t i = 0; i < m_cache.size(); ++i) {
            if (m_cache[i].index == index) {
                // Most recent at the front; the scan above is over eight
                // entries, so a list would cost more than it saved.
                if (i != 0) {
                    std::rotate(m_cache.begin(), m_cache.begin() + i, m_cache.begin() + i + 1);
                }
                return &m_cache.front().data;
            }
        }
        Entry entry;
        entry.index = index;
        if (!produce(index, entry.data)) {
            return nullptr;
        }
        if (m_cache.size() >= m_cacheBlocks) {
            m_cache.pop_back();
        }
        m_cache.insert(m_cache.begin(), std::move(entry));
        return &m_cache.front().data;
    }

    std::vector<Entry> m_cache;
    std::mutex m_mutex;
};

/// No container at all: the file is the disc. Present so that everything
/// above this layer has exactly one path to go through.
class RawContainer : public Container {
public:
    RawContainer(const RawStream& stream, std::uint64_t length)
        : m_stream(stream), m_length(length) {}

    bool read(std::uint64_t offset, void* out, std::size_t length) override {
        if (offset + length > m_length) {
            return false;
        }
        return read_exact(m_stream, offset, out, length);
    }

    std::uint64_t size() const override { return m_length; }
    Format format() const override { return Format::Raw; }

private:
    RawStream m_stream;
    std::uint64_t m_length;
};

class GczContainer : public BlockContainer {
public:
    static std::unique_ptr<GczContainer> open(const RawStream& stream, std::string& error);

    Format format() const override { return Format::Gcz; }

protected:
    std::uint64_t block_index(std::uint64_t offset) const override { return offset / m_blockSize; }
    std::uint64_t block_start(std::uint64_t index) const override { return index * m_blockSize; }

    bool produce(std::uint64_t index, std::vector<std::uint8_t>& out) override {
        if (index >= m_offsets.size()) {
            return false;
        }
        // The top bit says the block was stored as it is, because compressing
        // it made it bigger -- which is most of a GameCube disc, since the
        // junk does not compress.
        constexpr std::uint64_t kStoredFlag = 1ull << 63;
        const std::uint64_t raw = m_offsets[index];
        const std::uint64_t start = raw & ~kStoredFlag;
        const std::uint64_t end = index + 1 < m_offsets.size()
                                      ? (m_offsets[index + 1] & ~kStoredFlag)
                                      : m_storedSize;
        if (end < start) {
            return false;
        }

        std::vector<std::uint8_t> stored(static_cast<std::size_t>(end - start));
        if (!read_exact(m_stream, m_dataStart + start, stored.data(), stored.size())) {
            return false;
        }

        // Every block is a whole block, including the last one: a disc whose
        // size is not a multiple of the block size is padded, and the padding
        // is stored like anything else. So decode a full block and clip
        // afterwards -- asking the codec for the clipped length instead makes
        // the last block of most discs fail to decode at all.
        out.resize(static_cast<std::size_t>(m_blockSize));
        if (!decode(raw & kStoredFlag ? Codec::None : Codec::Zlib, stored.data(), stored.size(),
                out.data(), out.size(), nullptr, 0)) {
            return false;
        }
        const std::uint64_t remaining = m_discSize - index * m_blockSize;
        out.resize(static_cast<std::size_t>(std::min<std::uint64_t>(m_blockSize, remaining)));
        return true;
    }

private:
    RawStream m_stream{};
    std::uint64_t m_blockSize = 0;
    std::uint64_t m_storedSize = 0;
    std::uint64_t m_dataStart = 0;
    std::vector<std::uint64_t> m_offsets;
};

std::unique_ptr<GczContainer> GczContainer::open(const RawStream& stream, std::string& error) {
    std::uint8_t head[32];
    if (!read_exact(stream, 0, head, sizeof head)) {
        error = "could not read the GCZ header";
        return nullptr;
    }
    auto gcz = std::unique_ptr<GczContainer>(new GczContainer());
    gcz->m_stream = stream;
    gcz->m_storedSize = le64(head + 8);
    gcz->m_discSize = le64(head + 16);
    gcz->m_blockSize = le32(head + 24);
    const std::uint32_t blocks = le32(head + 28);

    if (gcz->m_blockSize == 0 || blocks == 0 || gcz->m_discSize == 0) {
        error = "the GCZ header does not describe a disc";
        return nullptr;
    }
    if (blocks > (1u << 24)) {
        error = "the GCZ block table is implausibly large";
        return nullptr;
    }

    std::vector<std::uint8_t> table(std::size_t{blocks} * 8);
    if (!read_exact(stream, 32, table.data(), table.size())) {
        error = "could not read the GCZ block table";
        return nullptr;
    }
    gcz->m_offsets.resize(blocks);
    for (std::uint32_t i = 0; i < blocks; ++i) {
        gcz->m_offsets[i] = le64(table.data() + std::size_t{i} * 8);
    }
    // Block table, then a CRC per block, then the data.
    gcz->m_dataStart = 32 + std::uint64_t{blocks} * 12;
    return gcz;
}

class WiaContainer : public BlockContainer {
public:
    static std::unique_ptr<WiaContainer> open(const RawStream& stream, bool rvz, std::string& error);

    Format format() const override { return m_rvz ? Format::Rvz : Format::Wia; }

protected:
    std::uint64_t block_index(std::uint64_t offset) const override { return offset / m_chunkSize; }
    std::uint64_t block_start(std::uint64_t index) const override { return index * m_chunkSize; }

    bool produce(std::uint64_t index, std::vector<std::uint8_t>& out) override;

public:
    std::unique_ptr<Container> stored_wii_partition(std::uint64_t dataStart, std::string& error) override;

private:
    friend class WiaPartitionData;

    // aleks2 (rc17): a Wii partition as WIA/RVZ keep it -- decrypted, 0x7C00 bytes per 0x8000-byte sector.
    struct PartData {
        std::uint32_t firstSector = 0;
        std::uint32_t sectors = 0;
        std::uint32_t groupIndex = 0;
        std::uint32_t groupCount = 0;
    };
    struct Partition {
        std::array<std::uint8_t, 16> titleKey{};  // in the clear; not needed to read
        PartData pd[2];
    };

    struct RawData {
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        std::uint32_t groupIndex = 0;
        std::uint32_t groupCount = 0;
    };

    bool read_table(std::uint64_t offset, std::uint32_t storedSize, std::size_t plainSize,
        std::vector<std::uint8_t>& out);
    bool unpack(const std::uint8_t* in, std::size_t inSize, std::uint64_t discOffset,
        std::vector<std::uint8_t>& out);

    RawStream m_stream{};
    bool m_rvz = false;
    Codec m_codec = Codec::None;
    std::uint64_t m_chunkSize = 0;
    std::array<std::uint8_t, 0x80> m_discHead{};
    std::array<std::uint8_t, 7> m_codecProps{};
    std::size_t m_codecPropsSize = 0;
    std::vector<RawData> m_rawData;
    std::vector<std::uint8_t> m_groups;
    std::size_t m_groupEntrySize = 0;
    std::vector<Partition> m_partitions;  // aleks2 (rc17): Wii only
};

bool WiaContainer::read_table(std::uint64_t offset, std::uint32_t storedSize, std::size_t plainSize,
    std::vector<std::uint8_t>& out) {
    std::vector<std::uint8_t> stored(storedSize);
    if (!read_exact(m_stream, offset, stored.data(), stored.size())) {
        return false;
    }
    out.resize(plainSize);
    return decode(m_codec, stored.data(), stored.size(), out.data(), out.size(), m_codecProps.data(),
        m_codecPropsSize);
}

std::unique_ptr<WiaContainer> WiaContainer::open(
    const RawStream& stream, bool rvz, std::string& error) {
    std::uint8_t head[0x48];
    if (!read_exact(stream, 0, head, sizeof head)) {
        error = "could not read the file header";
        return nullptr;
    }
    auto wia = std::unique_ptr<WiaContainer>(new WiaContainer());
    wia->m_stream = stream;
    wia->m_rvz = rvz;
    wia->m_discSize = be64(head + 0x24);

    const std::uint32_t discStructSize = be32(head + 0x0C);
    if (discStructSize < 0xDC) {
        error = "the disc structure is too short to be one";
        return nullptr;
    }
    std::vector<std::uint8_t> disc(discStructSize);
    if (!read_exact(stream, sizeof head, disc.data(), disc.size())) {
        error = "could not read the disc structure";
        return nullptr;
    }

    // aleks2 (rc17): Wii images too. Their partitions are stored decrypted and without hashes, in groups of their
    // own; everything else (the disc header, the partition table, each partition's ticket/TMD area) is raw data
    // like a GameCube disc's, so the disc view below still works and the partitions are read through
    // stored_wii_partition.
    const std::uint32_t discType = be32(disc.data() + 0x00);
    if (discType != 1 && discType != 2) {
        error = "the image says neither GameCube nor Wii";
        return nullptr;
    }
    const std::uint32_t partCount = be32(disc.data() + 0x90);
    if (discType == 1 && partCount != 0) {
        error = "a GameCube image with Wii partitions is not one this reader understands";
        return nullptr;
    }
    if (partCount > 64) {
        error = "the partition table is implausibly large";
        return nullptr;
    }
    if (partCount != 0) {
        // Stored uncompressed. An entry shorter than the struct is padded with zeroes.
        const std::uint32_t entrySize = be32(disc.data() + 0x94);
        if (entrySize == 0 || entrySize > 0x1000) {
            error = "the partition entries have an implausible size";
            return nullptr;
        }
        std::vector<std::uint8_t> table(std::size_t{partCount} * entrySize);
        if (!read_exact(stream, be64(disc.data() + 0x98), table.data(), table.size())) {
            error = "could not read the partition entries";
            return nullptr;
        }
        for (std::uint32_t i = 0; i < partCount; ++i) {
            std::uint8_t e[48] = {};
            std::memcpy(e, table.data() + std::size_t{i} * entrySize, std::min<std::size_t>(entrySize, sizeof e));
            Partition p;
            std::memcpy(p.titleKey.data(), e, 16);
            for (int k = 0; k < 2; ++k) {
                const std::uint8_t* d = e + 16 + k * 16;
                p.pd[k] = {be32(d), be32(d + 4), be32(d + 8), be32(d + 12)};
            }
            wia->m_partitions.push_back(p);
        }
    }

    switch (be32(disc.data() + 0x04)) {
    case 0: wia->m_codec = Codec::None; break;
    case 1: wia->m_codec = Codec::Purge; break;
    case 2: wia->m_codec = Codec::Bzip2; break;
    case 3: wia->m_codec = Codec::Lzma; break;
    case 4: wia->m_codec = Codec::Lzma2; break;
    case 5: wia->m_codec = Codec::Zstd; break;
    default: wia->m_codec = Codec::Unsupported; break;
    }
    if (wia->m_codec == Codec::Purge || wia->m_codec == Codec::Unsupported) {
        error = std::string("the image uses ") + codec_name(wia->m_codec) +
                " compression, which this reader does not implement";
        return nullptr;
    }

    wia->m_chunkSize = be32(disc.data() + 0x0C);
    if (wia->m_chunkSize == 0 || (wia->m_chunkSize & (wia->m_chunkSize - 1)) != 0) {
        error = "the chunk size is not a power of two";
        return nullptr;
    }
    std::memcpy(wia->m_discHead.data(), disc.data() + 0x10, wia->m_discHead.size());

    wia->m_codecPropsSize = std::min<std::size_t>(disc[0xD4], wia->m_codecProps.size());
    std::memcpy(wia->m_codecProps.data(), disc.data() + 0xD5, wia->m_codecPropsSize);

    // RVZ adds one field per group to what WIA stores, and nothing else.
    wia->m_groupEntrySize = rvz ? 12 : 8;

    const std::uint32_t rawCount = be32(disc.data() + 0xB4);
    std::vector<std::uint8_t> rawTable;
    if (rawCount == 0 || rawCount > (1u << 20) ||
        !wia->read_table(be64(disc.data() + 0xB8), be32(disc.data() + 0xC0),
            std::size_t{rawCount} * 24, rawTable)) {
        error = "could not read the raw-data table";
        return nullptr;
    }
    wia->m_rawData.resize(rawCount);
    for (std::uint32_t i = 0; i < rawCount; ++i) {
        const std::uint8_t* e = rawTable.data() + std::size_t{i} * 24;
        wia->m_rawData[i] = {be64(e), be64(e + 8), be32(e + 16), be32(e + 20)};
    }

    const std::uint32_t groupCount = be32(disc.data() + 0xC4);
    if (groupCount == 0 || groupCount > (1u << 24) ||
        !wia->read_table(be64(disc.data() + 0xC8), be32(disc.data() + 0xD0),
            std::size_t{groupCount} * wia->m_groupEntrySize, wia->m_groups)) {
        error = "could not read the group table";
        return nullptr;
    }
    return wia;
}

bool WiaContainer::unpack(const std::uint8_t* in, std::size_t inSize, std::uint64_t discOffset,
    std::vector<std::uint8_t>& out) {
    std::size_t pos = 0;
    while (pos + 4 <= inSize) {
        const std::uint32_t word = be32(in + pos);
        pos += 4;
        const std::size_t runSize = word & 0x7FFFFFFFu;
        if (word & 0x80000000u) {
            // Junk: 68 bytes of seed in place of up to 32 KiB of noise. The
            // seed belongs to the 32 KiB sector the run sits in, so a run
            // starting partway into a sector starts partway into the
            // sequence.
            if (pos + 68 > inSize) {
                return false;
            }
            JunkGenerator generator(in + pos);
            pos += 68;
            generator.skip(static_cast<std::size_t>(discOffset % kSectorSize));
            const std::size_t at = out.size();
            out.resize(at + runSize);
            generator.generate(out.data() + at, runSize);
        } else {
            if (pos + runSize > inSize) {
                return false;
            }
            out.insert(out.end(), in + pos, in + pos + runSize);
            pos += runSize;
        }
        discOffset += runSize;
    }
    return pos == inSize;
}

bool WiaContainer::produce(std::uint64_t index, std::vector<std::uint8_t>& out) {
    const std::uint64_t start = index * m_chunkSize;
    const std::uint64_t end = std::min(start + m_chunkSize, m_discSize);
    if (start >= m_discSize) {
        return false;
    }
    std::size_t expected = static_cast<std::size_t>(end - start);

    // The first 0x80 bytes of the disc are not in any group: a copy lives in
    // the disc structure, so a tool can name the game without decompressing
    // anything. They are inside group 0 as well, so this only matters for a
    // disc whose first raw-data entry starts past them.
    const RawData* entry = nullptr;
    for (const RawData& candidate : m_rawData) {
        const std::uint64_t base = (candidate.offset / m_chunkSize) * m_chunkSize;
        if (start >= base && start < candidate.offset + candidate.size) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) {
        out.assign(expected, 0);
        return true;
    }

    // Groups tile from the chunk boundary BELOW the entry, not from the entry
    // itself. An entry beginning at 0x80 still has its first group covering
    // 0x00..0x20000. Taking the entry's start as the group's start shifts
    // every byte after it, which yields a disc that looks plausible right up
    // to the moment the game tries to use it.
    const std::uint64_t base = (entry->offset / m_chunkSize) * m_chunkSize;
    const std::uint64_t group = entry->groupIndex + (start - base) / m_chunkSize;
    if (group >= m_groups.size() / m_groupEntrySize) {
        return false;
    }

    // aleks2 (rc17): on a Wii image a raw-data entry ends where a partition's (stored separately) data begins,
    // which need not be on a chunk boundary, and its last group holds only the bytes up to the entry's end. The
    // rest of this block reads as zeroes; the partition itself is read through stored_wii_partition. A GameCube
    // image's one entry runs to the end of the disc, so for it nothing changes.
    const std::size_t blockSize = expected;
    expected = static_cast<std::size_t>(std::min(end, entry->offset + entry->size) - start);

    const std::uint8_t* e = m_groups.data() + static_cast<std::size_t>(group) * m_groupEntrySize;
    const std::uint64_t dataOffset = std::uint64_t{be32(e)} * 4;
    const std::uint32_t sizeWord = be32(e + 4);
    const std::uint32_t packedSize = m_rvz ? be32(e + 8) : 0;

    const bool compressed = m_rvz ? (sizeWord & 0x80000000u) != 0 : m_codec != Codec::None;
    const std::size_t storedSize = m_rvz ? (sizeWord & 0x7FFFFFFFu) : sizeWord;

    // Nothing stored means the whole group is zeroes; it is how a blank
    // stretch of disc costs twelve bytes.
    if (storedSize == 0) {
        out.assign(blockSize, 0);
        return true;
    }

    std::vector<std::uint8_t> stored(storedSize);
    if (!read_exact(m_stream, dataOffset, stored.data(), stored.size())) {
        return false;
    }

    // With packing, what comes out of the codec is the packed stream, whose
    // length is the group entry's third field -- not the size of the disc
    // region.
    const std::size_t plainSize = packedSize != 0 ? packedSize : expected;
    std::vector<std::uint8_t> plain;
    if (compressed) {
        plain.resize(plainSize);
        if (!decode(m_codec, stored.data(), stored.size(), plain.data(), plain.size(),
                m_codecProps.data(), m_codecPropsSize)) {
            return false;
        }
    } else {
        if (stored.size() != plainSize) {
            return false;
        }
        plain = std::move(stored);
    }

    if (packedSize != 0) {
        out.clear();
        out.reserve(expected);
        if (!unpack(plain.data(), plain.size(), start, out)) {
            return false;
        }
    } else {
        out = std::move(plain);
    }

    // A group that decodes to the wrong length is a bug somewhere above, and
    // silently padding it would hide it until the game read the wrong bytes.
    if (out.size() < expected) {
        return false;
    }
    out.resize(expected);
    out.resize(blockSize, 0);  // past a Wii raw-data entry's end (see above)
    return true;
}

// ------------------------------------------------------------------ Wii --
//
// aleks2 (rc17). Layout from wiibrew's "Wii disc" page; the WIA/RVZ partition groups from the format's public
// description (Dolphin's docs/WiaAndRvz.md, used as a format reference). Our own code.

constexpr std::uint64_t kWiiPartitionTable = 0x40000;  // 4 x {u32 count, u32 table offset >> 2}
constexpr std::uint32_t kWiiDataPartition = 0;
constexpr std::uint64_t kWiiCluster = 0x8000;          // as stored: 0x400 of hashes, then data
constexpr std::uint64_t kWiiClusterData = 0x7C00;
constexpr std::uint64_t kWiiClusterHashes = 0x400;
constexpr std::size_t kWiiCachedClusters = 128;        // ~4 MiB of decrypted data

/// Reads a codec's output a piece at a time. A Wii partition group starts with exception lists whose length is
/// only known by reading them, and ends with data whose RVZ-packed length is only known by unpacking it; reading
/// exactly what is needed avoids having to know either total up front (or whether a raw LZMA stream carries an end
/// marker).
class PullReader {
public:
    PullReader(Codec codec, const std::uint8_t* in, std::size_t size, const std::uint8_t* props,
        std::size_t propsSize)
        : m_codec(codec), m_in(in), m_size(size) {
        switch (codec) {
        case Codec::None:
            m_ok = true;
            break;
        case Codec::Zstd:
            m_zstd = ZSTD_createDStream();
            m_ok = m_zstd != nullptr && !ZSTD_isError(ZSTD_initDStream(m_zstd));
            m_zin = {in, size, 0};
            break;
        case Codec::Bzip2:
            m_ok = BZ2_bzDecompressInit(&m_bz, 0, 0) == BZ_OK;
            m_bzInit = m_ok;
            m_bz.next_in = const_cast<char*>(reinterpret_cast<const char*>(in));
            m_bz.avail_in = static_cast<unsigned int>(size);
            break;
        case Codec::Lzma:
        case Codec::Lzma2: {
            lzma_filter filters[2];
            filters[0].id = codec == Codec::Lzma ? LZMA_FILTER_LZMA1 : LZMA_FILTER_LZMA2;
            filters[0].options = nullptr;
            filters[1].id = LZMA_VLI_UNKNOWN;
            filters[1].options = nullptr;
            if (lzma_properties_decode(&filters[0], nullptr, props, propsSize) != LZMA_OK) {
                break;
            }
            m_ok = lzma_raw_decoder(&m_lzma, filters) == LZMA_OK;
            m_lzmaInit = m_ok;
            lzma_filters_free(filters, nullptr);
            m_lzma.next_in = in;
            m_lzma.avail_in = size;
            break;
        }
        default:
            break;
        }
    }

    ~PullReader() {
        if (m_zstd != nullptr) ZSTD_freeDStream(m_zstd);
        if (m_bzInit) BZ2_bzDecompressEnd(&m_bz);
        if (m_lzmaInit) lzma_end(&m_lzma);
    }

    PullReader(const PullReader&) = delete;
    PullReader& operator=(const PullReader&) = delete;

    bool ok() const { return m_ok; }

    /// Exactly `n` bytes, or false.
    bool read(std::uint8_t* out, std::size_t n) {
        if (!m_ok) {
            return false;
        }
        bool done = false;
        switch (m_codec) {
        case Codec::None:
            done = m_pos + n <= m_size;
            if (done) {
                std::memcpy(out, m_in + m_pos, n);
                m_pos += n;
            }
            break;
        case Codec::Zstd: {
            ZSTD_outBuffer zout{out, n, 0};
            while (zout.pos < n) {
                const std::size_t before = zout.pos;
                const std::size_t inBefore = m_zin.pos;
                const std::size_t r = ZSTD_decompressStream(m_zstd, &zout, &m_zin);
                if (ZSTD_isError(r) || (zout.pos == before && m_zin.pos == inBefore)) {
                    break;
                }
            }
            done = zout.pos == n;
            break;
        }
        case Codec::Bzip2:
            m_bz.next_out = reinterpret_cast<char*>(out);
            m_bz.avail_out = static_cast<unsigned int>(n);
            while (m_bz.avail_out > 0) {
                const int r = BZ2_bzDecompress(&m_bz);
                if (r == BZ_STREAM_END || r != BZ_OK) {
                    break;
                }
            }
            done = m_bz.avail_out == 0;
            break;
        case Codec::Lzma:
        case Codec::Lzma2:
            m_lzma.next_out = out;
            m_lzma.avail_out = n;
            while (m_lzma.avail_out > 0) {
                const std::size_t before = m_lzma.avail_out;
                const lzma_ret r = lzma_code(&m_lzma, m_lzma.avail_in == 0 ? LZMA_FINISH : LZMA_RUN);
                if (r == LZMA_STREAM_END || (r != LZMA_OK) || m_lzma.avail_out == before) {
                    break;
                }
            }
            done = m_lzma.avail_out == 0;
            break;
        default:
            break;
        }
        if (done) {
            m_produced += n;
        } else {
            m_ok = false;
        }
        return done;
    }

    bool skip(std::size_t n) {
        std::uint8_t scratch[256];
        while (n > 0) {
            const std::size_t take = std::min(n, sizeof scratch);
            if (!read(scratch, take)) {
                return false;
            }
            n -= take;
        }
        return true;
    }

    /// Bytes handed out so far: the position in the decoded stream.
    std::size_t produced() const { return m_produced; }

private:
    Codec m_codec;
    const std::uint8_t* m_in;
    std::size_t m_size;
    std::size_t m_pos = 0;
    std::size_t m_produced = 0;
    bool m_ok = false;
    ZSTD_DStream* m_zstd = nullptr;
    ZSTD_inBuffer m_zin{};
    bz_stream m_bz{};
    bool m_bzInit = false;
    lzma_stream m_lzma = LZMA_STREAM_INIT;
    bool m_lzmaInit = false;
};

/// RVZ packing read from a PullReader until `expected` bytes are out. `offset` is where the first byte goes in the
/// address space the junk seed is relative to (here: the partition's data, without hashes).
bool unpack_pulled(PullReader& in, std::size_t expected, std::uint64_t offset, std::vector<std::uint8_t>& out) {
    out.clear();
    out.reserve(expected);
    while (out.size() < expected) {
        std::uint8_t word[4];
        if (!in.read(word, sizeof word)) {
            return false;
        }
        const std::uint32_t w = be32(word);
        const std::size_t run = w & 0x7FFFFFFFu;
        if (run == 0 || out.size() + run > expected) {
            return false;
        }
        const std::size_t at = out.size();
        out.resize(at + run);
        if (w & 0x80000000u) {
            std::uint8_t seed[68];
            if (!in.read(seed, sizeof seed)) {
                return false;
            }
            JunkGenerator generator(seed);
            generator.skip(static_cast<std::size_t>((offset + at) % kSectorSize));
            generator.generate(out.data() + at, run);
        } else if (!in.read(out.data() + at, run)) {
            return false;
        }
    }
    return true;
}

/// A Wii partition's data as WIA/RVZ store it: decrypted and without hashes, 0x7C00 bytes per disc sector, in the
/// partition's own groups. The two pd[] runs are contiguous on the disc; the second starts a fresh group.
class WiaPartitionData : public BlockContainer {
public:
    WiaPartitionData(WiaContainer& wia, const WiaContainer::Partition& part) : m_wia(wia), m_part(part) {
        m_sectorsPerGroup = std::max<std::uint64_t>(1, wia.m_chunkSize / kWiiCluster);
        m_firstRunGroups = (part.pd[0].sectors + m_sectorsPerGroup - 1) / m_sectorsPerGroup;
        m_discSize = (std::uint64_t{part.pd[0].sectors} + part.pd[1].sectors) * kWiiClusterData;
        m_cacheBlocks = kCachedBlocks;
    }

    Format format() const override { return m_wia.format(); }

protected:
    std::uint64_t first_run_bytes() const { return std::uint64_t{m_part.pd[0].sectors} * kWiiClusterData; }
    std::uint64_t group_bytes() const { return m_sectorsPerGroup * kWiiClusterData; }

    std::uint64_t block_index(std::uint64_t offset) const override {
        if (offset < first_run_bytes()) {
            return offset / group_bytes();
        }
        return m_firstRunGroups + (offset - first_run_bytes()) / group_bytes();
    }

    std::uint64_t block_start(std::uint64_t index) const override {
        if (index < m_firstRunGroups) {
            return index * group_bytes();
        }
        return first_run_bytes() + (index - m_firstRunGroups) * group_bytes();
    }

    bool produce(std::uint64_t index, std::vector<std::uint8_t>& out) override {
        const int run = index < m_firstRunGroups ? 0 : 1;
        const WiaContainer::PartData& pd = m_part.pd[run];
        const std::uint64_t groupInRun = run == 0 ? index : index - m_firstRunGroups;
        if (groupInRun >= pd.groupCount) {
            return false;
        }
        const std::uint64_t firstSector = groupInRun * m_sectorsPerGroup;
        if (firstSector >= pd.sectors) {
            return false;
        }
        const std::uint64_t sectors = std::min<std::uint64_t>(m_sectorsPerGroup, pd.sectors - firstSector);
        const std::size_t expected = static_cast<std::size_t>(sectors * kWiiClusterData);

        const std::uint64_t group = pd.groupIndex + groupInRun;
        if (group >= m_wia.m_groups.size() / m_wia.m_groupEntrySize) {
            return false;
        }
        const std::uint8_t* e = m_wia.m_groups.data() + static_cast<std::size_t>(group) * m_wia.m_groupEntrySize;
        const std::uint64_t dataOffset = std::uint64_t{be32(e)} * 4;
        const std::uint32_t sizeWord = be32(e + 4);
        const std::uint32_t packedSize = m_wia.m_rvz ? be32(e + 8) : 0;
        const bool compressed = m_wia.m_rvz ? (sizeWord & 0x80000000u) != 0 : m_wia.m_codec != Codec::None;
        const std::size_t storedSize = m_wia.m_rvz ? (sizeWord & 0x7FFFFFFFu) : sizeWord;
        if (storedSize == 0) {
            out.assign(expected, 0);  // all zeroes, and no exceptions
            return true;
        }
        std::vector<std::uint8_t> stored(storedSize);
        if (!read_exact(m_wia.m_stream, dataOffset, stored.data(), stored.size())) {
            return false;
        }

        PullReader in(compressed ? m_wia.m_codec : Codec::None, stored.data(), stored.size(),
            m_wia.m_codecProps.data(), m_wia.m_codecPropsSize);
        // The exception lists (hash corrections, which this view has no use for) come first: one per 2 MiB of
        // chunk, or exactly one for RVZ's smaller chunks. Stored as-is they are padded to 4 bytes.
        const std::uint64_t lists = std::max<std::uint64_t>(1, m_wia.m_chunkSize / 0x200000);
        for (std::uint64_t i = 0; i < lists; ++i) {
            std::uint8_t count[2];
            if (!in.read(count, sizeof count) || !in.skip(std::size_t{(std::uint32_t{count[0]} << 8) | count[1]} * 22)) {
                return false;
            }
        }
        if (!compressed && (in.produced() % 4) != 0 && !in.skip(4 - in.produced() % 4)) {
            return false;
        }

        if (packedSize != 0) {
            return unpack_pulled(in, expected, block_start(index), out);
        }
        out.resize(expected);
        return in.read(out.data(), expected);
    }

private:
    WiaContainer& m_wia;
    WiaContainer::Partition m_part;
    std::uint64_t m_sectorsPerGroup = 1;
    std::uint64_t m_firstRunGroups = 0;
};

std::unique_ptr<Container> WiaContainer::stored_wii_partition(std::uint64_t dataStart, std::string& error) {
    for (const Partition& p : m_partitions) {
        if (std::uint64_t{p.pd[0].firstSector} * kWiiCluster != dataStart) {
            continue;
        }
        if (p.pd[1].sectors != 0 && p.pd[1].firstSector != p.pd[0].firstSector + p.pd[0].sectors) {
            error = "the image stores this partition in two pieces that are not contiguous";
            return nullptr;
        }
        return std::make_unique<WiaPartitionData>(*this, p);
    }
    if (!m_partitions.empty()) {
        error = "the image does not store the partition its partition table points at";
    }
    return nullptr;
}

/// A Wii partition read from an encrypted (.iso) image: each 0x8000-byte cluster is decrypted with the
/// partition's title key -- the 0x7C00 data bytes with the IV the cluster carries at 0x3D0 of its (encrypted)
/// hash block -- and the hashes dropped.
class WiiCryptPartition : public BlockContainer {
public:
    WiiCryptPartition(Container& disc, std::uint64_t dataStart, std::uint64_t dataSize, const std::uint8_t key[16])
        : m_disc(disc), m_dataStart(dataStart) {
        std::memcpy(m_key, key, sizeof m_key);
        m_discSize = (dataSize / kWiiCluster) * kWiiClusterData;
        m_cacheBlocks = kWiiCachedClusters;
    }

    Format format() const override { return m_disc.format(); }

protected:
    std::uint64_t block_index(std::uint64_t offset) const override { return offset / kWiiClusterData; }
    std::uint64_t block_start(std::uint64_t index) const override { return index * kWiiClusterData; }

    bool produce(std::uint64_t index, std::vector<std::uint8_t>& out) override {
        std::vector<std::uint8_t> cluster(kWiiCluster);
        if (!m_disc.read(m_dataStart + index * kWiiCluster, cluster.data(), cluster.size())) {
            return false;
        }
        out.resize(kWiiClusterData);
        aes128_cbc_decrypt(m_key, cluster.data() + 0x3D0, cluster.data() + kWiiClusterHashes, out.data(),
            kWiiClusterData);
        return true;
    }

private:
    Container& m_disc;
    std::uint64_t m_dataStart;
    std::uint8_t m_key[16];
};

}  // namespace

std::unique_ptr<Container> Container::stored_wii_partition(std::uint64_t, std::string&) {
    return nullptr;
}

bool is_wii_disc(Container& disc) {
    std::uint8_t magic[4];
    return disc.read(0x18, magic, sizeof magic) && be32(magic) == kWiiMagic;
}

std::unique_ptr<Container> open_wii_data_partition(Container& disc, const WiiKeys& keys, std::string& error) {
    if (!is_wii_disc(disc)) {
        error = "this is not a Wii disc";
        return nullptr;
    }
    std::uint8_t discId[6];
    if (!disc.read(0, discId, sizeof discId)) {
        error = "could not read the disc header";
        return nullptr;
    }

    // The partition table: four groups, each a count and where its {offset >> 2, type} entries are.
    std::uint8_t groups[32];
    if (!disc.read(kWiiPartitionTable, groups, sizeof groups)) {
        error = "could not read the Wii partition table";
        return nullptr;
    }
    std::uint64_t partition = 0;
    for (int g = 0; g < 4 && partition == 0; ++g) {
        const std::uint32_t count = be32(groups + g * 8);
        const std::uint64_t table = std::uint64_t{be32(groups + g * 8 + 4)} << 2;
        if (count == 0 || count > 32) {
            continue;
        }
        std::vector<std::uint8_t> entries(std::size_t{count} * 8);
        if (!disc.read(table, entries.data(), entries.size())) {
            continue;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            if (be32(entries.data() + i * 8 + 4) == kWiiDataPartition) {
                partition = std::uint64_t{be32(entries.data() + i * 8)} << 2;
                break;
            }
        }
    }
    if (partition == 0) {
        error = "the disc has no game (DATA) partition";
        return nullptr;
    }

    // The partition header, unencrypted: the ticket (title key, title id, common key index), then where the
    // encrypted data starts and how long it is.
    std::uint8_t header[0x2C0];
    if (!disc.read(partition, header, sizeof header)) {
        error = "could not read the game partition's header";
        return nullptr;
    }
    const std::uint64_t dataStart = partition + (std::uint64_t{be32(header + 0x2B8)} << 2);
    const std::uint64_t dataSize = std::uint64_t{be32(header + 0x2BC)} << 2;
    if (dataSize < kWiiCluster || dataStart + dataSize > disc.size() + kWiiCluster) {
        error = "the game partition's header does not describe its data";
        return nullptr;
    }

    std::string storedError;
    std::unique_ptr<Container> data = disc.stored_wii_partition(dataStart, storedError);
    const bool decrypting = data == nullptr;
    if (data == nullptr && !storedError.empty()) {
        error = storedError;
        return nullptr;
    }
    if (data == nullptr) {
        // Encrypted: decrypt the title key with the common key (IV = the title id, then zeroes).
        if (header[0x1F1] != 0) {
            error = "this disc's partition uses a different common key (a Korean or vWii disc); only the standard "
                    "one is supported";
            return nullptr;
        }
        if (!keys.hasCommonKey) {
            error = "this Wii disc image is encrypted and needs your Wii's common key. Put BootMii's keys.bin from "
                    "your own console (or the key as wii_common_key.bin / wii_common_key.txt) in "
                    "/switch/dusklight/ on the SD card -- or convert the image to RVZ with Dolphin, which needs "
                    "no key here.";
            return nullptr;
        }
        std::uint8_t iv[16] = {};
        std::memcpy(iv, header + 0x1DC, 8);
        std::uint8_t titleKey[16];
        aes128_cbc_decrypt(keys.commonKey, iv, header + 0x1BF, titleKey, sizeof titleKey);
        data = std::make_unique<WiiCryptPartition>(disc, dataStart, dataSize, titleKey);
    }

    // A wrong key decrypts to noise: the partition's own header must say Wii and name the same game.
    std::uint8_t inner[0x20];
    if (!data->read(0, inner, sizeof inner) || be32(inner + 0x18) != kWiiMagic ||
        std::memcmp(inner, discId, 4) != 0) {
        error = decrypting
                    ? "the Wii common key on the SD card does not decrypt this disc (a different key in the file?)"
                    : "the game partition does not read back as this game";
        return nullptr;
    }
    return data;
}

const char* format_name(Format format) {
    switch (format) {
    case Format::Raw: return "ISO/GCM";
    case Format::Rvz: return "RVZ";
    case Format::Wia: return "WIA";
    case Format::Gcz: return "GCZ";
    case Format::Ciso: return "CISO";
    case Format::Wbfs: return "WBFS";
    case Format::Nfs: return "NFS";
    case Format::Tgc: return "TGC";
    default: return "unrecognised";
    }
}

bool format_supported(Format format) {
    return format == Format::Raw || format == Format::Rvz || format == Format::Wia ||
           format == Format::Gcz;
}

std::string format_advice(Format format) {
    switch (format) {
    case Format::Raw:
    case Format::Rvz:
    case Format::Wia:
    case Format::Gcz:
        return std::string(format_name(format)) + " is supported.";
    case Format::Ciso:
    case Format::Wbfs:
        // aleks2 (rc17): Wii discs are read now, but not from these two containers.
        return std::string(format_name(format)) +
               " is a Wii format this reader does not open. Convert your copy to ISO or RVZ with Dolphin "
               "(right-click the game, Convert File); RVZ also needs no Wii key on this console.";
    case Format::Nfs:
        return "NFS is the Wii U Virtual Console layout, not a GameCube disc.";
    case Format::Tgc:
        return "TGC is a mini-disc embedded in another game, not a disc of its own.";
    default:
        return "This file is not a disc image this can read. Convert your copy to ISO or RVZ with "
               "Dolphin (right-click the game, Convert File).";
    }
}

Format detect(const RawStream& stream) {
    if (stream.read_at == nullptr || stream.length == nullptr) {
        return Format::Unknown;
    }
    std::uint8_t head[0x40];
    if (!read_exact(stream, 0, head, sizeof head)) {
        return Format::Unknown;
    }
    if (std::memcmp(head, "RVZ\x01", 4) == 0) {
        return Format::Rvz;
    }
    if (std::memcmp(head, "WIA\x01", 4) == 0) {
        return Format::Wia;
    }
    if (le32(head) == kGczMagic) {
        return Format::Gcz;
    }
    if (std::memcmp(head, "CISO", 4) == 0) {
        return Format::Ciso;
    }
    if (std::memcmp(head, "WBFS", 4) == 0) {
        return Format::Wbfs;
    }
    if (std::memcmp(head, "\x01\x00\x00\x00\x45\x47\x47\x53", 8) == 0) {
        return Format::Nfs;
    }
    if (be32(head) == 0xAE0F38A2u) {
        return Format::Tgc;
    }
    // No container header: it is a disc if it says so where a disc says so.
    std::uint8_t magic[4];
    if (read_exact(stream, 0x1C, magic, sizeof magic) && be32(magic) == kGcnMagic) {
        return Format::Raw;
    }
    if (read_exact(stream, 0x18, magic, sizeof magic) && be32(magic) == kWiiMagic) {
        return Format::Raw;  // a Wii disc (rc17: its game partition is opened by open_wii_data_partition)
    }
    return Format::Unknown;
}

OpenResult open(const RawStream& stream) {
    OpenResult result;
    result.format = detect(stream);
    switch (result.format) {
    case Format::Raw:
        result.container =
            std::make_unique<RawContainer>(stream, stream.length(stream.user));
        break;
    case Format::Gcz:
        result.container = GczContainer::open(stream, result.error);
        break;
    case Format::Rvz:
    case Format::Wia:
        result.container = WiaContainer::open(stream, result.format == Format::Rvz, result.error);
        break;
    default:
        result.error = format_advice(result.format);
        break;
    }
    if (result.container == nullptr && result.error.empty()) {
        result.error = format_advice(result.format);
    }
    return result;
}

}  // namespace dusk::sw::disc
