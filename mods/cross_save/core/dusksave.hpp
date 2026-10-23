#pragma once

// The .dusksave container and save rules, as the host's save_manager defines them, for the
// compatibility mode (official Dusklight builds without SaveSnapshotService). Must stay
// byte-for-byte compatible with src/dusk/save_manager.cpp: a snapshot packed here is read by a
// host's apply and the other way round, and both compute the same content digest.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace crosssave::dusksave {

using Bytes = std::vector<uint8_t>;
using ModFiles = std::map<std::string, Bytes>;

struct GciHeader {
    std::string game;      // 4 characters, e.g. "GZ2E"
    std::string maker;     // 2 characters, e.g. "01"
    std::string saveName;  // e.g. "gczelda2"
    uint32_t modifiedTime = 0;
    uint16_t blockCount = 0;
};

enum class Compat { Exact, RegionChange, PlatformChange, Incompatible };

struct Snapshot {
    GciHeader header;
    Bytes gci;
    ModFiles modFiles;
};

std::optional<GciHeader> parse_gci(const Bytes& gci);
// save_manager::disc_compatibility.
Compat compatibility(const GciHeader& header, const std::string& game, const std::string& maker);
// save_manager::snapshot_digest: SHA-256 hex over the GCI and the sidecars, framed and sorted.
std::string digest(const Bytes& gci, const ModFiles& modFiles);
// "GZ2-01-gczelda2" (RZD counts as GZ2).
std::string sync_key(const std::string& game, const std::string& maker, const std::string& saveName);
// utils::is_valid_mod_id / is_valid_save_name.
bool valid_mod_id(const std::string& id);
bool valid_save_name(const std::string& name);

// Packs a .dusksave version 1 (save.json, save.gci, mods/<id>.json).
std::optional<Bytes> pack(const Bytes& gci, const ModFiles& modFiles, const std::string& createdUtc);
// Reads a .dusksave or a bare .gci with save_manager's checks; nullopt with error set otherwise.
std::optional<Snapshot> read(const Bytes& bytes, std::string& error);

}  // namespace crosssave::dusksave
