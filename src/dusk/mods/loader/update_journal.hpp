#pragma once
// aleks2 (DM1-b): native code changes take effect at the next start, and an update that does not
// survive that start is undone.
//
// Code mods are never unloaded on Switch (the mapping stays, FINI is not run, hooks are not
// guaranteed to come off cleanly -- TP Classic 1.3.2 crashed in convert_texture after a hot
// disable). So installing, updating, enabling or disabling a package with native code changes only
// files and configuration; the code running now keeps running until the game restarts.
//
// An update writes a journal entry before the new package replaces the old one: the mod, both
// versions, and the backup of the version it replaces (<mods>/.backup/<id>.dusk, which install_staged
// already made). At the next start:
//   * a package that cannot be read, or a mod the previous boot crashed in (boot_guard), gets the
//     backup back before anything loads -- the journal says it was put back;
//   * otherwise the new version loads; once the boot is over (boot_guard's end_boot), the entry is
//     committed if the mod is running, and the backup goes back if it failed to load.
// Leftover downloads (.staging/*.part) of a session that ended are removed at start.
//
// The backup restores the PACKAGE only. A mod that migrated its own data (files in its data
// directory, save extensions) is not migrated back; the journal records that the data directory
// was not touched, and the message says so.
//
// Platform-neutral and synchronous; tested on the host (platforms/switch/tools/update_journal_test.cpp).
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace dusk::mods::journal {

struct Entry {
    std::string id;
    std::string fromVersion;
    std::string toVersion;
    std::filesystem::path destination;  // the installed package, <mods>/<id>.dusk
    std::filesystem::path backup;       // the previous version, <mods>/.backup/<id>.dusk
};

// Replaces `to` with `from` atomically where the platform allows (borealis::io::atomic_replace on
// the console). Returns false with `error` set on failure; `from` is consumed on success.
using ReplaceFn = std::function<bool(const std::filesystem::path& from, const std::filesystem::path& to,
    std::string& error)>;
// Whether a package file is a readable mod package (inspect_mod_bundle on the console).
using ReadableFn = std::function<bool(const std::filesystem::path& package)>;

std::filesystem::path directory(const std::filesystem::path& userDir);  // <mods>/.pending

// Writes <mods>/.pending/<id>.json (via a temporary file and a rename).
bool write(const std::filesystem::path& userDir, const Entry& entry, std::string& error);
// Every readable entry; unreadable journal files are reported in `problems` and left alone.
std::vector<Entry> read_all(const std::filesystem::path& userDir, std::vector<std::string>* problems = nullptr);
void remove(const std::filesystem::path& userDir, std::string_view id);

// Copies the backup over the destination (through <mods>/.staging/<id>-restore.dusk.part).
bool restore(const std::filesystem::path& userDir, const Entry& entry, const ReplaceFn& replace,
    std::string& error);

struct Action {
    std::string id;
    std::string message;  // for the log and a toast
    bool restored = false;
};

// Before any mod loads. `crashedModId` is the mod the previous boot crashed in, or empty.
std::vector<Action> recover_at_start(const std::filesystem::path& userDir, std::string_view crashedModId,
    const ReadableFn& readable, const ReplaceFn& replace);

enum class Outcome { Running, Failed, NotStarted };
// After the boot is over. `outcome(id)`: Running (active, no failure) commits the entry; Failed
// (the new version failed to load or initialize) puts the backup back; NotStarted (disabled by the
// player, waiting for a provider...) keeps the entry for a later boot.
std::vector<Action> settle_after_boot(const std::filesystem::path& userDir,
    const std::function<Outcome(std::string_view id)>& outcome, const ReplaceFn& replace);

}  // namespace dusk::mods::journal
