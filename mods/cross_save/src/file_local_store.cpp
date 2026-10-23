#include "file_local_store.hpp"

#include "../core/json.hpp"
#include "platform_io.hpp"

#include <algorithm>

namespace crosssave::compat {
namespace {

constexpr const char* kRegions[] = {"USA", "EUR", "JAP"};
constexpr size_t kMaxSave = 16 * 1024 * 1024 + 0x40;
constexpr size_t kMaxSidecar = 1024 * 1024;
constexpr int kBackupRetention = 10;  // save_manager::kDefaultBackupRetention

char region_letter(const std::string& region) {
    return region == "EUR" ? 'P' : region == "JAP" ? 'J' : 'E';
}

std::string stem(const std::string& maker, const std::string& game, const std::string& saveName) {
    return maker + "-" + game + "-" + saveName;  // aurora::card::gci_filename without ".gci"
}

LocalStatus compat_status(dusksave::Compat compat) {
    return compat == dusksave::Compat::Exact ? LocalStatus::Ok :
           compat == dusksave::Compat::Incompatible ? LocalStatus::Incompatible :
                                                      LocalStatus::NeedsConversion;
}

Compat to_neutral(dusksave::Compat compat) {
    switch (compat) {
    case dusksave::Compat::Exact:
        return Compat::Exact;
    case dusksave::Compat::RegionChange:
        return Compat::RegionChange;
    case dusksave::Compat::PlatformChange:
        return Compat::PlatformChange;
    case dusksave::Compat::Incompatible:
        break;
    }
    return Compat::Incompatible;
}

dusksave::ModFiles read_sidecars(const std::string& directory) {
    dusksave::ModFiles files;
    for (const auto& name : io::list_dir(directory)) {
        if (name.size() <= 5 || name.substr(name.size() - 5) != ".json") {
            continue;
        }
        const auto id = name.substr(0, name.size() - 5);
        if (!dusksave::valid_mod_id(id)) {
            continue;
        }
        if (auto data = io::read_file(io::join(directory, name), kMaxSidecar)) {
            files.emplace(id, std::move(*data));
        }
    }
    return files;
}

// Replaces a sidecar directory the way save_manager::replace_sidecars does: stage, move the old one
// aside, move the staging in, put the old one back if that fails.
bool replace_sidecars(const std::string& destination, const dusksave::ModFiles& files) {
    const std::string staging = destination + ".replace";
    const std::string previous = destination + ".replace.old";
    for (const auto& dir : {staging, previous}) {
        for (const auto& name : io::list_dir(dir)) {
            io::remove_file(io::join(dir, name));
        }
        io::remove_dir(dir);
    }
    if (!files.empty()) {
        if (!io::make_dir(staging)) {
            return false;
        }
        for (const auto& [id, data] : files) {
            if (!io::write_file_atomic(io::join(staging, id + ".json"), data.data(), data.size())) {
                return false;
            }
        }
    }
    const bool hadOld = io::is_dir(destination);
    if (hadOld && !io::rename_dir(destination, previous)) {
        return false;
    }
    if (!files.empty() && !io::rename_dir(staging, destination)) {
        if (hadOld) {
            io::rename_dir(previous, destination);
        }
        return false;
    }
    for (const auto& name : io::list_dir(previous)) {
        io::remove_file(io::join(previous, name));
    }
    io::remove_dir(previous);
    return true;
}

}  // namespace

FileLocalStore::FileLocalStore(std::string dataDir, std::string saveName)
    : m_dataDir{std::move(dataDir)}, m_userDir{io::parent(io::parent(m_dataDir))}, m_saveName{std::move(saveName)} {}

FileLocalStore::Location FileLocalStore::locate() const {
    Location best;
    uint32_t bestTime = 0;
    std::string firstCard;
    std::string firstRegion;
    for (const char* region : kRegions) {
        const auto cardDir = io::join(io::join(m_userDir, region), "Card A");
        if (!io::is_dir(cardDir)) {
            continue;
        }
        if (firstCard.empty()) {
            firstCard = cardDir;
            firstRegion = region;
        }
        for (const auto& name : io::list_dir(cardDir)) {
            // <maker>-<game>-<save>.gci with a Twilight Princess game ID.
            const std::string suffix = "-" + m_saveName + ".gci";
            if (name.size() != 2 + 1 + 4 + suffix.size() || name.substr(7) != suffix || name[2] != '-') {
                continue;
            }
            const std::string maker = name.substr(0, 2);
            const std::string game = name.substr(3, 4);
            if (game.substr(0, 3) != "GZ2" && game.substr(0, 3) != "RZD") {
                continue;
            }
            const auto bytes = io::read_file(io::join(cardDir, name), kMaxSave);
            const auto header = bytes ? dusksave::parse_gci(*bytes) : std::nullopt;
            if (!header || header->game != game || header->maker != maker || header->saveName != m_saveName) {
                continue;
            }
            // Several regions with a save: the most recently written one is the one in play.
            if (!best.hasSave || header->modifiedTime >= bestTime) {
                best = {cardDir, game, maker, true, {}};
                bestTime = header->modifiedTime;
            }
        }
    }
    if (best.hasSave) {
        return best;
    }
    if (!firstCard.empty()) {
        // No save yet: the card the game created. GameCube Twilight Princess of that region.
        return {firstCard, std::string{"GZ2"} + region_letter(firstRegion), "01", false, {}};
    }
    Location none;
    for (const char* region : kRegions) {
        if (io::is_file(io::join(m_userDir, std::string{"MemoryCardA."} + region + ".raw"))) {
            none.message = "Cloud Save needs the GCI-folder memory card; this game uses a raw card image.";
            return none;
        }
    }
    none.message = "No memory card yet. Start the game once, then sync.";
    return none;
}

FileLocalStore::Local FileLocalStore::readLocal() const {
    Local local;
    local.where = locate();
    if (local.where.cardDir.empty()) {
        local.status = LocalStatus::NoCard;
        local.message = local.where.message;
        return local;
    }
    if (!local.where.hasSave) {
        local.status = LocalStatus::NoSave;
        return local;
    }
    const auto base = io::join(local.where.cardDir, stem(local.where.maker, local.where.game, m_saveName));
    auto gci = io::read_file(base + ".gci", kMaxSave);
    if (!gci || !dusksave::parse_gci(*gci)) {
        local.status = LocalStatus::IoError;
        local.message = "The save file could not be read.";
        return local;
    }
    local.gci = std::move(*gci);
    local.modFiles = read_sidecars(base + ".mods");
    local.digest = dusksave::digest(local.gci, local.modFiles);
    return local;
}

LocalIdentity FileLocalStore::identity() {
    const auto local = readLocal();
    LocalIdentity id;
    if (local.status == LocalStatus::NoCard || local.status == LocalStatus::IoError) {
        id.status = local.status;
        id.message = local.message;
        return id;
    }
    id.key = dusksave::sync_key(local.where.game, local.where.maker, m_saveName);
    id.game = local.where.game;
    id.saveName = m_saveName;
    id.changeCount = changeCount;
    if (local.status == LocalStatus::Ok) {
        id.hasSave = true;
        id.digest = local.digest;
        id.modifiedTime = dusksave::parse_gci(local.gci)->modifiedTime;
    }
    return id;
}

LocalSnapshot FileLocalStore::capture() {
    auto local = readLocal();
    LocalSnapshot snapshot;
    snapshot.status = local.status;
    snapshot.message = local.message;
    if (local.status != LocalStatus::Ok) {
        return snapshot;
    }
    auto packed = dusksave::pack(local.gci, local.modFiles, io::utc_stamp(false));
    if (!packed) {
        snapshot.status = LocalStatus::IoError;
        snapshot.message = "The save could not be packed.";
        return snapshot;
    }
    const auto header = dusksave::parse_gci(local.gci);
    snapshot.bytes = std::move(*packed);
    snapshot.digest = local.digest;
    snapshot.game = header->game;
    snapshot.modifiedTime = header->modifiedTime;
    return snapshot;
}

LocalCheck FileLocalStore::inspect(const Bytes& bytes) {
    LocalCheck check;
    std::string error;
    const auto snapshot = dusksave::read(bytes, error);
    if (!snapshot) {
        check.status = LocalStatus::Invalid;
        check.message = error;
        return check;
    }
    check.digest = dusksave::digest(snapshot->gci, snapshot->modFiles);
    const auto where = locate();
    if (where.cardDir.empty()) {
        check.status = LocalStatus::NoCard;
        check.message = where.message;
        return check;
    }
    const auto compat = dusksave::compatibility(snapshot->header, where.game, where.maker);
    check.compat = to_neutral(compat);
    check.status = compat_status(compat);
    if (snapshot->header.saveName != m_saveName) {
        check.status = LocalStatus::Incompatible;
    }
    if (check.status == LocalStatus::Incompatible) {
        check.message = "The cloud save is " + snapshot->header.maker + "-" + snapshot->header.game + " '" +
                        snapshot->header.saveName + "', this game uses " + where.maker + "-" + where.game + ".";
    }
    return check;
}

LocalCheck FileLocalStore::apply(
    const Bytes& bytes, const std::optional<std::string>& expectedDigest, bool allowConversion) {
    LocalCheck check = inspect(bytes);
    if (check.status == LocalStatus::NeedsConversion && allowConversion) {
        check.status = LocalStatus::Ok;
    }
    if (check.status != LocalStatus::Ok) {
        return check;
    }
    // Stage it; mod_initialize applies it before the game mounts the card.
    json::Value meta{json::Object{}};
    meta["v"] = json::Value{1};
    meta["expected"] = expectedDigest ? *expectedDigest : std::string{};
    meta["checkExpected"] = json::Value{expectedDigest.has_value()};
    meta["allowConversion"] = json::Value{allowConversion};
    const auto metaText = json::dump(meta);
    if (!io::write_file_atomic(io::join(m_dataDir, "staged.dusksave"), bytes.data(), bytes.size()) ||
        !io::write_file_atomic(io::join(m_dataDir, "staged.json"), metaText.data(), metaText.size()))
    {
        check.status = LocalStatus::IoError;
        check.message = "The cloud save could not be staged.";
        return check;
    }
    io::remove_file(io::join(m_dataDir, "applied.json"));
    check.status = LocalStatus::Deferred;
    check.message = "Restart the game to apply the cloud save.";
    return check;
}

bool FileLocalStore::hasStagedApply() const {
    return io::is_file(io::join(m_dataDir, "staged.json"));
}

std::optional<std::string> FileLocalStore::takeStartupApply() {
    const auto path = io::join(m_dataDir, "applied.json");
    const auto bytes = io::read_file(path, 4096);
    if (!bytes) {
        return std::nullopt;
    }
    io::remove_file(path);
    const auto value = json::parse(std::string{bytes->begin(), bytes->end()});
    const auto digest = value ? value->string_or("digest") : std::string{};
    if (digest.empty()) {
        return std::nullopt;
    }
    return digest;
}

LocalCheck FileLocalStore::writeSnapshot(const Location& where, const dusksave::Snapshot& snapshot,
    bool allowConversion, const std::optional<std::string>& expectedDigest) {
    LocalCheck result;
    const auto compat = dusksave::compatibility(snapshot.header, where.game, where.maker);
    if (compat == dusksave::Compat::Incompatible || snapshot.header.saveName != m_saveName ||
        (compat != dusksave::Compat::Exact && !allowConversion))
    {
        result.status = compat == dusksave::Compat::Incompatible ? LocalStatus::Incompatible : LocalStatus::NeedsConversion;
        result.message = "The cloud save does not fit this game.";
        return result;
    }
    const auto base = io::join(where.cardDir, stem(where.maker, where.game, m_saveName));
    // Current save, for the CAS check, the backup and the rollback.
    const auto previous = where.hasSave ? io::read_file(base + ".gci", kMaxSave) : std::nullopt;
    const auto previousMods = read_sidecars(base + ".mods");
    const std::string currentDigest = previous ? dusksave::digest(*previous, previousMods) : std::string{};
    if (expectedDigest && *expectedDigest != currentDigest) {
        result.status = LocalStatus::LocalChanged;
        result.message = "This save changed after the download; nothing was replaced.";
        return result;
    }
    if (previous) {
        const auto backups = io::join(where.cardDir, "backups");
        const auto packed = dusksave::pack(*previous, previousMods, io::utc_stamp(false));
        const auto name = stem(where.maker, where.game, m_saveName) + "-" + io::utc_stamp(true) + ".dusksave";
        if (!packed || !io::make_dir(backups) || !io::write_file_atomic(io::join(backups, name), packed->data(), packed->size())) {
            result.status = LocalStatus::BackupFailed;
            result.message = "The backup could not be written; nothing was replaced.";
            return result;
        }
        result.backupName = name;
        // Retention as in the Saves window: the newest kBackupRetention for this save.
        std::vector<std::string> mine;
        const auto prefix = stem(where.maker, where.game, m_saveName) + "-";
        for (const auto& file : io::list_dir(backups)) {
            if (file.rfind(prefix, 0) == 0 && file.size() > 9 && file.substr(file.size() - 9) == ".dusksave") {
                mine.push_back(file);
            }
        }
        std::sort(mine.begin(), mine.end());  // UTC stamps sort by time
        for (size_t i = 0; i + kBackupRetention < mine.size(); ++i) {
            if (mine[i] != name) {
                io::remove_file(io::join(backups, mine[i]));
            }
        }
    }
    auto gci = snapshot.gci;
    std::copy(where.game.begin(), where.game.end(), gci.begin());  // the Saves window's conversion
    if (!io::write_file_atomic(base + ".gci", gci.data(), gci.size())) {
        result.status = LocalStatus::WriteFailed;
        result.message = "The save could not be written; the previous one is still in place.";
        return result;
    }
    if (!replace_sidecars(base + ".mods", snapshot.modFiles)) {
        if (previous) {
            io::write_file_atomic(base + ".gci", previous->data(), previous->size());
        } else {
            io::remove_file(base + ".gci");
        }
        result.status = LocalStatus::ModDataFailed;
        result.message = "The mod data could not be replaced; the previous save was put back.";
        return result;
    }
    result.status = LocalStatus::Ok;
    result.digest = dusksave::digest(gci, snapshot.modFiles);
    return result;
}

std::string FileLocalStore::applyStaged() {
    const auto metaPath = io::join(m_dataDir, "staged.json");
    const auto dataPath = io::join(m_dataDir, "staged.dusksave");
    const auto metaBytes = io::read_file(metaPath, 4096);
    if (!metaBytes) {
        return {};
    }
    const auto finish = [&](const std::string& message) {
        io::remove_file(dataPath);
        io::remove_file(metaPath);
        return message;
    };
    const auto meta = json::parse(std::string{metaBytes->begin(), metaBytes->end()});
    const auto bytes = io::read_file(dataPath, kMaxSave * 2);
    std::string error;
    const auto snapshot = bytes ? dusksave::read(*bytes, error) : std::nullopt;
    if (!meta || !snapshot) {
        return finish("The staged cloud save was unreadable and was discarded.");
    }
    const auto where = locate();
    if (where.cardDir.empty()) {
        return finish("The staged cloud save could not be applied: " + where.message);
    }
    std::optional<std::string> expected;
    if ((*meta)["checkExpected"].as_bool()) {
        expected = meta->string_or("expected");
    }
    const auto result = writeSnapshot(where, *snapshot, (*meta)["allowConversion"].as_bool(), expected);
    if (result.status != LocalStatus::Ok) {
        return finish("The cloud save was not applied: " + result.message);
    }
    json::Value applied{json::Object{}};
    applied["digest"] = result.digest;
    const auto text = json::dump(applied);
    io::write_file_atomic(io::join(m_dataDir, "applied.json"), text.data(), text.size());
    return finish("The cloud save was applied" +
                  (result.backupName.empty() ? std::string{"."} : " (backup: " + result.backupName + ")."));
}

std::optional<std::string> FileLocalStore::readRecord() {
    const auto local = locate();
    const auto key = local.game.empty() ? std::string{"none"} : dusksave::sync_key(local.game, local.maker, m_saveName);
    const auto bytes = io::read_file(io::join(m_dataDir, "record-" + key + ".json"), 64 * 1024);
    if (!bytes) {
        return std::nullopt;
    }
    return std::string{bytes->begin(), bytes->end()};
}

bool FileLocalStore::writeRecord(const std::string& text) {
    const auto local = locate();
    const auto key = local.game.empty() ? std::string{"none"} : dusksave::sync_key(local.game, local.maker, m_saveName);
    return io::write_file_atomic(io::join(m_dataDir, "record-" + key + ".json"), text.data(), text.size());
}

}  // namespace crosssave::compat
