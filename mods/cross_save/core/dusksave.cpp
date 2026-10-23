#include "dusksave.hpp"

#include "json.hpp"
#include "sha256.hpp"

#include "../third_party/miniz/miniz.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <set>

namespace crosssave::dusksave {
namespace {

constexpr size_t kGciHeaderSize = 0x40;
constexpr size_t kCardBlockSize = 0x2000;
constexpr size_t kMaxRawSize = 16 * 1024 * 1024;
constexpr size_t kMaxArchiveEntryCount = 130;
constexpr size_t kMaxMetadataSize = 1024 * 1024;
constexpr size_t kMaxModFileSize = 1024 * 1024;

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t be32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}

std::string fixed_string(const uint8_t* value, size_t size) {
    size_t length = 0;
    while (length < size && value[length] != 0) {
        ++length;
    }
    return std::string{reinterpret_cast<const char*>(value), length};
}

bool valid_name(const std::string& name, size_t maxBytes) {
    return !name.empty() && name.size() <= maxBytes && name.find('\0') == std::string::npos;
}

bool safe_path_component(const std::string& name) {
    return valid_name(name, std::string::npos) && name != "." && name != ".." &&
           name.find_first_of("/\\:") == std::string::npos;
}

std::optional<Bytes> extract(mz_zip_archive& zip, mz_uint index, size_t maxSize) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&zip, index, &stat) || stat.m_uncomp_size > maxSize) {
        return std::nullopt;
    }
    Bytes bytes(static_cast<size_t>(stat.m_uncomp_size));
    if (!bytes.empty() && !mz_zip_reader_extract_to_mem(&zip, index, bytes.data(), bytes.size(), 0)) {
        return std::nullopt;
    }
    return bytes;
}

}  // namespace

std::optional<GciHeader> parse_gci(const Bytes& bytes) {
    if (bytes.size() < kGciHeaderSize || (bytes.size() - kGciHeaderSize) % kCardBlockSize != 0) {
        return std::nullopt;
    }
    const uint16_t blockCount = be16(bytes.data() + 0x38);
    if (blockCount == 0 || blockCount != (bytes.size() - kGciHeaderSize) / kCardBlockSize) {
        return std::nullopt;
    }
    GciHeader header;
    header.saveName = fixed_string(bytes.data() + 8, 32);
    header.game = fixed_string(bytes.data(), 4);
    header.maker = fixed_string(bytes.data() + 4, 2);
    header.modifiedTime = be32(bytes.data() + 0x28);
    header.blockCount = blockCount;
    if (header.saveName.empty() || header.game.size() != 4 || header.maker.size() != 2) {
        return std::nullopt;
    }
    return header;
}

Compat compatibility(const GciHeader& header, const std::string& game, const std::string& maker) {
    if (header.game.size() != 4 || game.size() != 4 || header.maker.size() != 2 || header.maker != maker) {
        return Compat::Incompatible;
    }
    if (header.game == game) {
        return Compat::Exact;
    }
    const auto a = header.game.substr(0, 3);
    const auto b = game.substr(0, 3);
    if (a == b) {
        return Compat::RegionChange;
    }
    if ((a == "RZD" && b == "GZ2") || (a == "GZ2" && b == "RZD")) {
        return Compat::PlatformChange;
    }
    return Compat::Incompatible;
}

std::string digest(const Bytes& gci, const ModFiles& modFiles) {
    Sha256 hasher;
    const auto put_size = [&hasher](uint64_t value) {
        uint8_t bytes[8];
        for (size_t i = 0; i < 8; ++i) {
            bytes[i] = static_cast<uint8_t>(value >> (8 * i));
        }
        hasher.update(std::span<const uint8_t>{bytes, 8});
    };
    hasher.update("dusklight-save-snapshot-1");
    put_size(gci.size());
    hasher.update(gci);
    put_size(modFiles.size());
    for (const auto& [id, data] : modFiles) {
        put_size(id.size());
        hasher.update(id);
        put_size(data.size());
        hasher.update(data);
    }
    return hasher.finish_hex();
}

std::string sync_key(const std::string& game, const std::string& maker, const std::string& saveName) {
    std::string family = game.substr(0, 3);
    if (family == "RZD") {
        family = "GZ2";
    }
    return family + "-" + maker + "-" + saveName;
}

bool valid_mod_id(const std::string& id) {
    if (!valid_name(id, std::string::npos) || id.front() == '.' || id.back() == '.' ||
        id.find("..") != std::string::npos)
    {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
    });
}

bool valid_save_name(const std::string& name) {
    return valid_name(name, 31) && name != "." && name != ".." &&
           std::all_of(name.begin(), name.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-';
           });
}

std::optional<Bytes> pack(const Bytes& gci, const ModFiles& modFiles, const std::string& createdUtc) {
    if (!parse_gci(gci)) {
        return std::nullopt;
    }
    json::Array mods;
    for (const auto& [id, data] : modFiles) {
        json::Value entry{json::Object{}};
        entry["id"] = id;
        entry["version"] = std::string{};  // the host writes the loaded mod's version; unknown here
        mods.push_back(std::move(entry));
    }
    json::Value metadata{json::Object{}};
    metadata["version"] = json::Value{1};
    metadata["dusklight"] = "cloud-save-compat";
    metadata["created"] = createdUtc;
    metadata["mods"] = json::Value{std::move(mods)};
    const std::string metadataText = json::dump(metadata);

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
        return std::nullopt;
    }
    bool ok = mz_zip_writer_add_mem(&zip, "save.json", metadataText.data(), metadataText.size(), MZ_BEST_COMPRESSION) &&
              mz_zip_writer_add_mem(&zip, "save.gci", gci.data(), gci.size(), MZ_BEST_COMPRESSION);
    for (const auto& [id, data] : modFiles) {
        const std::string name = "mods/" + id + ".json";
        ok = ok && mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_BEST_COMPRESSION);
    }
    void* archive = nullptr;
    size_t archiveSize = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &archive, &archiveSize);
    std::optional<Bytes> out;
    if (ok) {
        const auto* begin = static_cast<const uint8_t*>(archive);
        out = Bytes{begin, begin + archiveSize};
    }
    mz_free(archive);
    mz_zip_writer_end(&zip);
    return out;
}

std::optional<Snapshot> read(const Bytes& bytes, std::string& error) {
    const bool zipped = bytes.size() >= 4 && bytes[0] == 'P' && bytes[1] == 'K' && bytes[2] == 3 && bytes[3] == 4;
    if (!zipped) {
        auto header = parse_gci(bytes);
        if (!header) {
            error = "not a GCI or Dusklight save";
            return std::nullopt;
        }
        return Snapshot{std::move(*header), bytes, {}};
    }
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) {
        error = "the ZIP is not a Dusklight save";
        return std::nullopt;
    }
    const std::unique_ptr<mz_zip_archive, decltype(&mz_zip_reader_end)> guard{&zip, &mz_zip_reader_end};
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (count > kMaxArchiveEntryCount) {
        error = "too many entries";
        return std::nullopt;
    }
    int metadataIndex = -1;
    int gciIndex = -1;
    int metadataCount = 0;
    int gciCount = 0;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || mz_zip_reader_is_file_a_directory(&zip, i)) {
            continue;
        }
        const std::string name{stat.m_filename};
        if (name == "save.json") {
            metadataIndex = static_cast<int>(i);
            ++metadataCount;
        } else if (name == "save.gci") {
            gciIndex = static_cast<int>(i);
            ++gciCount;
        }
    }
    if (metadataCount != 1 || gciCount != 1) {
        error = "the ZIP is not a Dusklight save";
        return std::nullopt;
    }
    auto metadataBytes = extract(zip, static_cast<mz_uint>(metadataIndex), kMaxMetadataSize);
    auto gci = extract(zip, static_cast<mz_uint>(gciIndex), kMaxRawSize + kGciHeaderSize);
    if (!metadataBytes || !gci) {
        error = "an entry is invalid or oversized";
        return std::nullopt;
    }
    auto header = parse_gci(*gci);
    if (!header) {
        error = "the save inside is not a valid GCI";
        return std::nullopt;
    }
    const auto metadata = json::parse(std::string{metadataBytes->begin(), metadataBytes->end()});
    if (!metadata || !metadata->is_object() || (*metadata)["version"].as_number() != 1 ||
        !(*metadata)["dusklight"].is_string() || !(*metadata)["created"].is_string() || !(*metadata)["mods"].is_array())
    {
        error = "unsupported save format";
        return std::nullopt;
    }
    std::set<std::string> declared;
    for (const auto& mod : (*metadata)["mods"].as_array()) {
        const auto id = mod.string_or("id");
        if (!mod.is_object() || !valid_mod_id(id) || !declared.insert(id).second || !mod["version"].is_string()) {
            error = "invalid save metadata";
            return std::nullopt;
        }
    }
    Snapshot snapshot{std::move(*header), std::move(*gci), {}};
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || mz_zip_reader_is_file_a_directory(&zip, i)) {
            continue;
        }
        const std::string name{stat.m_filename};
        if (name.rfind("mods/", 0) != 0) {
            continue;
        }
        const std::string child = name.substr(5);
        if (!safe_path_component(child) || child.size() < 6 || child.substr(child.size() - 5) != ".json") {
            error = "unsafe mod data path";
            return std::nullopt;
        }
        const std::string id = child.substr(0, child.size() - 5);
        if (!valid_mod_id(id) || snapshot.modFiles.contains(id)) {
            error = "invalid mod data entry";
            return std::nullopt;
        }
        auto data = extract(zip, i, kMaxModFileSize);
        if (!data) {
            error = "invalid mod data entry";
            return std::nullopt;
        }
        snapshot.modFiles.emplace(id, std::move(*data));
    }
    if (snapshot.modFiles.size() != declared.size() ||
        std::any_of(declared.begin(), declared.end(), [&](const std::string& id) { return !snapshot.modFiles.contains(id); }))
    {
        error = "metadata does not match the mod data";
        return std::nullopt;
    }
    return snapshot;
}

}  // namespace crosssave::dusksave
