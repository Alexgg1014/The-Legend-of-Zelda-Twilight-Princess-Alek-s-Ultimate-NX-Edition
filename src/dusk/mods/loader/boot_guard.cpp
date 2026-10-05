// aleks2 (H37): see boot_guard.hpp.
#include "boot_guard.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace dusk::mods::guard {
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr std::string_view kCodes[] = {"validation", "missing_import", "unsupported_relocation",
    "missing_dependency", "constructor_failure", "initialize_failure", "crash_during_init",
    "hook_conflict", "runtime_failure"};
constexpr std::string_view kTexts[] = {
    "The mod's file is not a module this build can load.",
    "The mod needs a symbol this build does not provide.",
    "The mod uses a relocation type the loader does not support.",
    "The mod depends on a library this build does not provide.",
    "The mod's startup code failed.",
    "The mod reported an error while starting.",
    "The game crashed while this mod was loading or starting. It was disabled.",
    "The mod needs a game function another enabled mod already replaces.",
    "The mod reported a failure while the game was running. It was disabled and stays disabled.",
};
constexpr std::string_view kStages[] = {"load", "constructors", "initialize"};

constexpr const char* kReport = "report.json";
constexpr const char* kPending = "pending.json";
constexpr const char* kBootMarker = "boot.marker";
constexpr const char* kWithoutMods = "start_without_mods";

// Written next to the target and renamed over it, so a crash mid-write leaves the old file or
// the new one, never half of one.
bool write_atomically(const fs::path& path, const std::string& text) {
    const fs::path temp = path.string() + ".tmp";
    {
        std::FILE* f = std::fopen(temp.string().c_str(), "wb");
        if (f == nullptr) {
            return false;
        }
        const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        std::fflush(f);
        std::fclose(f);
        if (!ok) {
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) {
        // Some file systems refuse to rename over an existing file.
        fs::remove(path, ec);
        fs::rename(temp, path, ec);
    }
    return !ec;
}

std::optional<json> read_json(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    // A damaged file is treated as absent: the guard must never be the thing that fails a boot.
    json value = json::parse(buffer.str(), nullptr, false);
    if (value.is_discarded() || !value.is_object()) {
        return std::nullopt;
    }
    return value;
}

json to_json(const Incident& i) {
    return {{"mod", i.modId}, {"category", category_code(i.category)}, {"stage", i.stage},
        {"message", i.message}, {"boot", i.boot}};
}

std::optional<Incident> from_json(const json& j) {
    if (!j.is_object() || !j.contains("mod") || !j["mod"].is_string()) {
        return std::nullopt;
    }
    Incident i;
    i.modId = j["mod"].get<std::string>();
    const auto category = category_from_code(j.value("category", ""));
    if (!category) {
        return std::nullopt;
    }
    i.category = *category;
    i.stage = j.value("stage", "");
    i.message = j.value("message", "");
    i.boot = j.value("boot", uint64_t{0});
    return i;
}

}  // namespace

std::string_view category_code(Category c) {
    return kCodes[static_cast<size_t>(c)];
}

std::optional<Category> category_from_code(std::string_view code) {
    for (size_t i = 0; i < std::size(kCodes); ++i) {
        if (kCodes[i] == code) {
            return static_cast<Category>(i);
        }
    }
    return std::nullopt;
}

std::string_view category_text(Category c) {
    return kTexts[static_cast<size_t>(c)];
}

Category category_for_load_error(std::string_view code) {
    if (code == "missing_symbol" || code == "denied_symbol") {
        return Category::MissingImport;
    }
    if (code == "unsupported_relocation" || code == "relr_unsupported" || code == "ifunc_unsupported" ||
        code == "text_relocations" || code == "relocation_out_of_image")
    {
        return Category::UnsupportedRelocation;
    }
    if (code == "needed_rejected") {
        return Category::MissingDependency;
    }
    return Category::Validation;
}

std::string_view stage_code(Stage s) {
    return kStages[static_cast<size_t>(s)];
}

struct BootGuard::State {
    uint64_t boots = 0;
    uint32_t unattributedCrashes = 0;
    std::map<std::string, Incident> quarantine;
    std::vector<Incident> incidents;
};

BootGuard::BootGuard(fs::path directory) : m_dir(std::move(directory)) {}

BootGuard::State BootGuard::load() const {
    State s;
    const auto j = read_json(m_dir / kReport);
    if (!j) {
        return s;
    }
    s.boots = j->value("boots", uint64_t{0});
    s.unattributedCrashes = j->value("unattributed_crashes", uint32_t{0});
    if (const auto it = j->find("quarantine"); it != j->end() && it->is_array()) {
        for (const auto& q : *it) {
            if (auto i = from_json(q)) {
                s.quarantine[i->modId] = std::move(*i);
            }
        }
    }
    if (const auto it = j->find("incidents"); it != j->end() && it->is_array()) {
        for (const auto& q : *it) {
            if (auto i = from_json(q)) {
                s.incidents.push_back(std::move(*i));
            }
        }
    }
    return s;
}

void BootGuard::save(const State& s) const {
    json j = {{"schema", "dusklight-aleks-mod-guard"}, {"version", 1}, {"boots", s.boots},
        {"unattributed_crashes", s.unattributedCrashes}, {"quarantine", json::array()},
        {"incidents", json::array()}};
    for (const auto& [id, i] : s.quarantine) {
        j["quarantine"].push_back(to_json(i));
    }
    for (const auto& i : s.incidents) {
        j["incidents"].push_back(to_json(i));
    }
    write_atomically(m_dir / kReport, j.dump(1) + "\n");
}

BootGuard::Startup BootGuard::begin_boot() {
    std::error_code ec;
    fs::create_directories(m_dir, ec);
    State s = load();
    Startup out;
    out.boot = m_boot = ++s.boots;

    const bool previousBootCrashed = fs::exists(m_dir / kBootMarker, ec);
    if (const auto pending = read_json(m_dir / kPending)) {
        // A mod's step never returned: that mod crashed the previous boot.
        Incident i;
        i.modId = pending->value("mod", "");
        i.stage = pending->value("stage", "");
        i.boot = pending->value("boot", uint64_t{0});
        i.category = i.stage == stage_code(Stage::Constructors) ? Category::ConstructorFailure :
                                                                   Category::CrashDuringInit;
        i.message = "the game stopped during this mod's " + i.stage + " step";
        if (!i.modId.empty()) {
            s.quarantine[i.modId] = i;
            s.incidents.push_back(i);
            out.crashed = i;
        }
        fs::remove(m_dir / kPending, ec);
        s.unattributedCrashes = 0;
    } else if (previousBootCrashed) {
        out.unattributedCrash = true;
        ++s.unattributedCrashes;
    }

    if (fs::exists(m_dir / kWithoutMods, ec)) {
        out.withoutMods = true;
        out.withoutModsReason = "you asked to start without mods";
        fs::remove(m_dir / kWithoutMods, ec);  // one boot only
    } else if (s.unattributedCrashes >= kCrashesBeforeSafeMode) {
        out.withoutMods = true;
        out.withoutModsReason = "the game crashed " + std::to_string(s.unattributedCrashes) +
                                " times in a row while starting, with no mod to blame";
        s.unattributedCrashes = 0;  // try with mods again after this boot
    }
    m_withoutMods = out.withoutMods;

    while (s.incidents.size() > kMaxIncidents) {
        s.incidents.erase(s.incidents.begin());
    }
    save(s);
    write_atomically(m_dir / kBootMarker, std::to_string(m_boot) + "\n");
    return out;
}

void BootGuard::end_boot() {
    std::error_code ec;
    fs::remove(m_dir / kBootMarker, ec);
    State s = load();
    if (s.unattributedCrashes != 0) {
        s.unattributedCrashes = 0;
        save(s);
    }
}

void BootGuard::enter(std::string_view modId, Stage stage) {
    const json j = {{"mod", modId}, {"stage", stage_code(stage)}, {"boot", m_boot}};
    write_atomically(m_dir / kPending, j.dump() + "\n");
}

void BootGuard::leave() {
    std::error_code ec;
    fs::remove(m_dir / kPending, ec);
}

void BootGuard::report(Incident incident) {
    State s = load();
    incident.boot = m_boot;
    s.incidents.push_back(std::move(incident));
    while (s.incidents.size() > kMaxIncidents) {
        s.incidents.erase(s.incidents.begin());
    }
    save(s);
}

bool BootGuard::record_runtime_failure(Incident incident) {
    std::error_code ec;
    fs::create_directories(m_dir, ec);
    State s = load();
    if (s.quarantine.contains(incident.modId)) {
        return false;
    }
    incident.category = Category::RuntimeFailure;
    incident.boot = m_boot;
    s.quarantine[incident.modId] = incident;
    s.incidents.push_back(std::move(incident));
    while (s.incidents.size() > kMaxIncidents) {
        s.incidents.erase(s.incidents.begin());
    }
    save(s);
    return true;
}

bool BootGuard::quarantined(std::string_view modId, std::string* why) const {
    const State s = load();
    const auto it = s.quarantine.find(std::string(modId));
    if (it == s.quarantine.end()) {
        return false;
    }
    if (why != nullptr) {
        *why = std::string(category_text(it->second.category)) + " (" + it->second.message + ")";
    }
    return true;
}

void BootGuard::release(std::string_view modId) {
    State s = load();
    if (s.quarantine.erase(std::string(modId)) != 0) {
        save(s);
    }
}

std::vector<std::string> BootGuard::quarantine() const {
    std::vector<std::string> out;
    for (const auto& [id, i] : load().quarantine) {
        out.push_back(id);
    }
    return out;
}

void BootGuard::request_boot_without_mods() {
    std::error_code ec;
    fs::create_directories(m_dir, ec);
    write_atomically(m_dir / kWithoutMods, "requested from the mods menu\n");
}

std::vector<Incident> BootGuard::incidents() const {
    return load().incidents;
}

}  // namespace dusk::mods::guard
