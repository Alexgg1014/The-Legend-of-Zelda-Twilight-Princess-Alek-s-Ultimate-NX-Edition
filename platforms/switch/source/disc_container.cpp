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
        if (m_cache.size() >= kCachedBlocks) {
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

private:
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

    const std::uint32_t discType = be32(disc.data() + 0x00);
    if (discType != 1) {
        // Honest refusal: a Wii image needs partition decryption and hash
        // reinsertion, and this game is not on one.
        error = "this is a Wii image; Twilight Princess on Switch runs the GameCube version";
        return nullptr;
    }
    if (be32(disc.data() + 0x90) != 0) {
        error = "the image has Wii partitions, which this reader does not decrypt";
        return nullptr;
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
    const std::size_t expected = static_cast<std::size_t>(end - start);

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

    const std::uint8_t* e = m_groups.data() + static_cast<std::size_t>(group) * m_groupEntrySize;
    const std::uint64_t dataOffset = std::uint64_t{be32(e)} * 4;
    const std::uint32_t sizeWord = be32(e + 4);
    const std::uint32_t packedSize = m_rvz ? be32(e + 8) : 0;

    const bool compressed = m_rvz ? (sizeWord & 0x80000000u) != 0 : m_codec != Codec::None;
    const std::size_t storedSize = m_rvz ? (sizeWord & 0x7FFFFFFFu) : sizeWord;

    // Nothing stored means the whole group is zeroes; it is how a blank
    // stretch of disc costs twelve bytes.
    if (storedSize == 0) {
        out.assign(expected, 0);
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
    return true;
}

}  // namespace

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
        return std::string(format_name(format)) +
               " is a Wii format, and this is the GameCube version of the game. Convert your "
               "GameCube copy to ISO or RVZ with Dolphin (right-click the game, Convert File).";
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
        return Format::Raw;  // a Wii disc; refused later, by name
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
