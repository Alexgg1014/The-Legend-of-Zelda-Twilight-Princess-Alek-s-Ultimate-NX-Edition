#include "update_journal.hpp"
#include "dusk/file_copy.hpp"  // aleks2 (rc18)

#include <nlohmann/json.hpp>

#include <fstream>
#include <system_error>

namespace dusk::mods::journal {
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string path_string(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

fs::path entry_path(const fs::path& userDir, std::string_view id) {
    return directory(userDir) / (std::string{id} + ".json");
}

bool safe_id(std::string_view id) {
    if (id.empty() || id.size() > 128) {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return id.find("..") == std::string_view::npos;
}

std::string data_note(const Entry& entry) {
    return "the mod's own data was not rolled back; if " + entry.toVersion +
           " migrated it, restore that data from your backup";
}

}  // namespace

fs::path directory(const fs::path& userDir) {
    return userDir / ".pending";
}

bool write(const fs::path& userDir, const Entry& entry, std::string& error) {
    if (!safe_id(entry.id)) {
        error = "unsafe mod id for the update journal";
        return false;
    }
    std::error_code ec;
    fs::create_directories(directory(userDir), ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    const json value{
        {"id", entry.id},
        {"from", entry.fromVersion},
        {"to", entry.toVersion},
        {"destination", path_string(entry.destination)},
        {"backup", path_string(entry.backup)},
    };
    const auto target = entry_path(userDir, entry.id);
    const auto temp = fs::path{target}.concat(".tmp");
    {
        std::ofstream out{temp, std::ios::binary | std::ios::trunc};
        out << value.dump(1);
        out.flush();
        if (!out) {
            error = "could not write the update journal";
            return false;
        }
    }
    fs::remove(target, ec);  // rename over an existing file fails on the console's filesystem
    ec.clear();
    fs::rename(temp, target, ec);
    if (ec) {
        error = ec.message();
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

std::vector<Entry> read_all(const fs::path& userDir, std::vector<std::string>* problems) {
    std::vector<Entry> entries;
    std::error_code ec;
    const auto dir = directory(userDir);
    if (!fs::is_directory(dir, ec)) {
        return entries;
    }
    for (const auto& file : fs::directory_iterator(dir, ec)) {
        if (file.path().extension() != ".json") {
            continue;
        }
        try {
            std::ifstream in{file.path(), std::ios::binary};
            const auto value = json::parse(in);
            Entry entry{
                .id = value.at("id").get<std::string>(),
                .fromVersion = value.at("from").get<std::string>(),
                .toVersion = value.at("to").get<std::string>(),
                .destination = fs::path{value.at("destination").get<std::string>()},
                .backup = fs::path{value.at("backup").get<std::string>()},
            };
            if (!safe_id(entry.id) || file.path().stem() != entry.id) {
                throw std::runtime_error("id does not match the file name");
            }
            entries.push_back(std::move(entry));
        } catch (const std::exception& e) {
            if (problems != nullptr) {
                problems->push_back(path_string(file.path().filename()) + ": " + e.what());
            }
        }
    }
    return entries;
}

void remove(const fs::path& userDir, std::string_view id) {
    if (!safe_id(id)) {
        return;
    }
    std::error_code ec;
    fs::remove(entry_path(userDir, id), ec);
}

bool restore(const fs::path& userDir, const Entry& entry, const ReplaceFn& replace, std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(entry.backup, ec)) {
        error = "the backup of " + entry.fromVersion + " is missing";
        return false;
    }
    const auto restoring = userDir / ".staging" / (entry.id + "-restore.dusk.part");
    fs::create_directories(restoring.parent_path(), ec);
    ec.clear();
    if (!file_copy::copy_file(entry.backup, restoring, ec)) {  // aleks2 (rc18): file_copy.hpp
        error = ec ? ec.message() : "could not copy the backup";
        return false;
    }
    if (!replace(restoring, entry.destination, error)) {
        fs::remove(restoring, ec);
        return false;
    }
    return true;
}

std::vector<Action> recover_at_start(const fs::path& userDir, std::string_view crashedModId,
    const ReadableFn& readable, const ReplaceFn& replace) {
    std::vector<Action> actions;
    std::error_code ec;

    // Downloads and restores of a session that ended: nothing will ever pick them up.
    const auto staging = userDir / ".staging";
    if (fs::is_directory(staging, ec)) {
        std::vector<fs::path> stale;
        for (const auto& file : fs::directory_iterator(staging, ec)) {
            if (file.path().extension() == ".part") {
                stale.push_back(file.path());
            }
        }
        for (const auto& path : stale) {
            fs::remove(path, ec);
            actions.push_back({.id = {}, .message = "removed an unfinished download: " +
                                                       path_string(path.filename())});
        }
    }

    std::vector<std::string> problems;
    for (const auto& entry : read_all(userDir, &problems)) {
        const bool crashed = !crashedModId.empty() && crashedModId == entry.id;
        const bool unreadable = !readable(entry.destination);
        if (!crashed && !unreadable) {
            continue;  // the new version loads this boot; settle_after_boot decides
        }
        std::string error;
        const std::string why = crashed ? "the last start crashed in it" : "the new package cannot be read";
        if (restore(userDir, entry, replace, error)) {
            remove(userDir, entry.id);
            actions.push_back({.id = entry.id,
                .message = "update " + entry.fromVersion + " -> " + entry.toVersion + " undone (" + why +
                           "); " + entry.fromVersion + " is installed again; " + data_note(entry),
                .restored = true});
        } else {
            actions.push_back({.id = entry.id,
                .message = "update " + entry.fromVersion + " -> " + entry.toVersion + " failed (" + why +
                           ") and could not be undone: " + error + "; the backup is kept at " +
                           path_string(entry.backup)});
        }
    }
    for (const auto& problem : problems) {
        actions.push_back({.id = {}, .message = "unreadable update journal entry left alone: " + problem});
    }
    return actions;
}

std::vector<Action> settle_after_boot(const fs::path& userDir,
    const std::function<Outcome(std::string_view id)>& outcome, const ReplaceFn& replace) {
    std::vector<Action> actions;
    for (const auto& entry : read_all(userDir)) {
        const auto state = outcome(entry.id);
        if (state == Outcome::NotStarted) {
            continue;
        }
        if (state == Outcome::Running) {
            remove(userDir, entry.id);
            actions.push_back({.id = entry.id,
                .message = "update " + entry.fromVersion + " -> " + entry.toVersion + " is running; committed"});
            continue;
        }
        std::string error;
        if (restore(userDir, entry, replace, error)) {
            remove(userDir, entry.id);
            actions.push_back({.id = entry.id,
                .message = "update " + entry.fromVersion + " -> " + entry.toVersion +
                           " did not start; " + entry.fromVersion + " is put back and runs after a restart; " +
                           data_note(entry),
                .restored = true});
        } else {
            actions.push_back({.id = entry.id,
                .message = "update " + entry.fromVersion + " -> " + entry.toVersion +
                           " did not start and could not be undone: " + error});
        }
    }
    return actions;
}

}  // namespace dusk::mods::journal
