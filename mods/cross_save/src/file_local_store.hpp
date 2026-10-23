#pragma once

// Compatibility mode: the LocalStore for Dusklight builds without SaveSnapshotService (the official
// PC and Android releases). Works on the GCI-folder memory card directly:
//
//   <user dir>/<USA|EUR|JAP>/Card A/<maker>-<game>-<save>.gci   the save (all three slots)
//   <user dir>/<USA|EUR|JAP>/Card A/<maker>-<game>-<save>.mods/  its mod sidecars
//   <user dir>/<USA|EUR|JAP>/Card A/backups/                     backups, named like the Saves window's
//
// where <user dir> is two levels above the mod's data_dir (<user dir>/mod_data/<mod id>), as in
// aurora's card path and the host's persistent_data_dir.
//
// Those builds cannot remount the card under a running game, so a downloaded save is staged and
// applied at the next start, from mod_initialize: mods initialize before the game mounts the card.
// The apply backs up first and refuses if the local save changed since the download was decided.
// Raw card images are not supported here (the Saves window can convert); GCI folder is the default.

#include "../core/dusksave.hpp"
#include "../core/sync_types.hpp"

#include <optional>
#include <string>

namespace crosssave::compat {

class FileLocalStore : public LocalStore {
public:
    FileLocalStore(std::string dataDir, std::string saveName = "gczelda2");

    // Applies a staged download, if any. Call once from mod_initialize, before the game runs.
    // Returns a message for the player (empty when nothing was staged).
    std::string applyStaged();

    LocalIdentity identity() override;
    LocalSnapshot capture() override;
    LocalCheck inspect(const Bytes& snapshot) override;
    LocalCheck apply(const Bytes& snapshot, const std::optional<std::string>& expectedDigest,
        bool allowConversion) override;
    std::optional<std::string> readRecord() override;
    bool writeRecord(const std::string& text) override;
    bool hasStagedApply() const override;
    std::optional<std::string> takeStartupApply() override;

    // For tests: where the card is and what it is called.
    struct Location {
        std::string cardDir;
        std::string game;  // e.g. "GZ2E"
        std::string maker;
        bool hasSave = false;
        std::string message;
    };
    Location locate() const;

    uint64_t changeCount = 0;  // bumped by the mod on every SaveService on_save_written

private:
    struct Local {
        LocalStatus status = LocalStatus::Ok;
        std::string message;
        Location where;
        Bytes gci;
        dusksave::ModFiles modFiles;
        std::string digest;
    };
    Local readLocal() const;
    LocalCheck writeSnapshot(const Location& where, const dusksave::Snapshot& snapshot, bool allowConversion,
        const std::optional<std::string>& expectedDigest);

    std::string m_dataDir;
    std::string m_userDir;
    std::string m_saveName;
};

}  // namespace crosssave::compat
