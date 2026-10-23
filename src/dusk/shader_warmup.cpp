#include "dusk/shader_warmup.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <vector>

namespace dusk::shader_warmup {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// Calls f(key, value) for each "key=value" line, skipping blanks and '#' comments.
template <typename F>
void for_each_pair(std::string_view text, F&& f) {
    while (!text.empty()) {
        const auto eol = text.find('\n');
        std::string_view line = trim(text.substr(0, eol));
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            f(line, std::string_view{});
            continue;
        }
        f(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
    }
}

bool parse_u64(std::string_view s, std::uint64_t& out, int base) {
    if (s.empty() || s.size() > 32) {
        return false;
    }
    const std::string copy{s};
    char* end = nullptr;
    out = std::strtoull(copy.c_str(), &end, base);
    return end != nullptr && *end == '\0';
}

bool parse_u32(std::string_view s, std::uint32_t& out) {
    std::uint64_t v = 0;
    if (!parse_u64(s, v, 10) || v > 0xffffffffull) {
        return false;
    }
    out = static_cast<std::uint32_t>(v);
    return true;
}

std::string hex64(std::uint64_t v) {
    char text[24];
    std::snprintf(text, sizeof text, "%016" PRIx64, v);
    return text;
}

}  // namespace

Reason decide(const std::optional<Key>& marker, const Key& now, bool cacheRepaired) {
    if (!marker) {
        return Reason::FirstLaunch;
    }
    // Order matters only for the message: the most fundamental change first.
    if (marker->driver != now.driver) {
        return Reason::DriverChanged;
    }
    if (marker->pipelineSchema != now.pipelineSchema || marker->gxVersion != now.gxVersion ||
        marker->clearVersion != now.clearVersion || marker->rmlVersion != now.rmlVersion ||
        marker->dawnSchema != now.dawnSchema)
    {
        return Reason::SchemaChanged;
    }
    if (marker->runtimeIdentity != now.runtimeIdentity) {
        return Reason::RuntimeChanged;
    }
    if (marker->listSignature != now.listSignature) {
        return Reason::ListChanged;
    }
    if (marker->layoutKey != now.layoutKey) {
        return Reason::RenderChanged;
    }
    // A recreated cache lost its compiled blobs (Dawn) or its own recipes: the marker
    // describes work that is no longer on the card.
    if (cacheRepaired) {
        return Reason::CacheRepaired;
    }
    return Reason::None;
}

const char* reason_name(Reason reason) {
    switch (reason) {
    case Reason::None:
        return "none";
    case Reason::FirstLaunch:
        return "first_launch";
    case Reason::DriverChanged:
        return "driver_changed";
    case Reason::SchemaChanged:
        return "schema_changed";
    case Reason::RuntimeChanged:
        return "runtime_changed";
    case Reason::ListChanged:
        return "list_changed";
    case Reason::RenderChanged:
        return "render_changed";
    case Reason::CacheRepaired:
        return "cache_repaired";
    }
    return "unknown";
}

const char* reason_text(Reason reason) {
    switch (reason) {
    case Reason::None:
        return "";
    case Reason::FirstLaunch:
        return "First launch on this console.";
    case Reason::DriverChanged:
        return "The graphics driver changed since the last preparation.";
    case Reason::SchemaChanged:
        return "This version stores shaders in a new format.";
    case Reason::RuntimeChanged:
        return "This version changes how the game's shaders are built.";
    case Reason::ListChanged:
        return "This version ships an updated shader list.";
    case Reason::RenderChanged:
        return "A graphics option that changes how the game renders was changed.";
    case Reason::CacheRepaired:
        return "The shader cache was damaged (for example by a forced power-off) and has been rebuilt.";
    }
    return "";
}

std::string marker_text(const Key& key, std::int64_t completedUnix, double seconds, std::uint32_t pipelines) {
    std::string out;
    out += "# Dusklight shader preparation marker. Delete this file to see the preparation screen again.\n";
    out += "version=2\n";
    out += "driver=" + key.driver + "\n";
    out += "pipeline_schema=" + std::to_string(key.pipelineSchema) + "\n";
    out += "gx_version=" + std::to_string(key.gxVersion) + "\n";
    out += "clear_version=" + std::to_string(key.clearVersion) + "\n";
    out += "rml_version=" + std::to_string(key.rmlVersion) + "\n";
    out += "dawn_schema=" + std::to_string(key.dawnSchema) + "\n";
    out += "list_signature=" + hex64(key.listSignature) + "\n";
    out += "layout_key=" + hex64(key.layoutKey) + "\n";
    out += "runtime_identity=" + hex64(key.runtimeIdentity) + "\n";
    out += "completed_unix=" + std::to_string(completedUnix) + "\n";
    char secs[32];
    std::snprintf(secs, sizeof secs, "%.1f", seconds);
    out += std::string{"seconds="} + secs + "\n";
    out += "pipelines=" + std::to_string(pipelines) + "\n";
    return out;
}

std::optional<Key> parse_marker(std::string_view text) {
    Key key;
    unsigned seen = 0;
    bool bad = false;
    bool versionOk = false;
    for_each_pair(text, [&](std::string_view k, std::string_view v) {
        std::uint64_t u64 = 0;
        if (k == "version") {
            versionOk = v == "2";  // a version-1 marker (SHADERS-c1) has no runtime identity
        } else if (k == "driver") {
            key.driver = std::string{v};
            seen |= 1u << 0;
        } else if (k == "pipeline_schema") {
            bad |= !parse_u32(v, key.pipelineSchema);
            seen |= 1u << 1;
        } else if (k == "gx_version") {
            bad |= !parse_u32(v, key.gxVersion);
            seen |= 1u << 2;
        } else if (k == "clear_version") {
            bad |= !parse_u32(v, key.clearVersion);
            seen |= 1u << 3;
        } else if (k == "rml_version") {
            bad |= !parse_u32(v, key.rmlVersion);
            seen |= 1u << 4;
        } else if (k == "dawn_schema") {
            bad |= !parse_u32(v, key.dawnSchema);
            seen |= 1u << 5;
        } else if (k == "list_signature") {
            bad |= !parse_u64(v, u64, 16);
            key.listSignature = u64;
            seen |= 1u << 6;
        } else if (k == "layout_key") {
            bad |= !parse_u64(v, u64, 16);
            key.layoutKey = u64;
            seen |= 1u << 7;
        } else if (k == "runtime_identity") {
            bad |= !parse_u64(v, u64, 16);
            key.runtimeIdentity = u64;
            seen |= 1u << 8;
        }
    });
    if (!versionOk || bad || seen != 0x1ffu || key.driver.empty()) {
        return std::nullopt;
    }
    return key;
}

void Eta::start(double now, std::uint32_t done) {
    startTime = now;
    startDone = done;
}

std::optional<double> Eta::seconds_left(double now, std::uint32_t done, std::uint32_t total) const {
    if (startTime < 0.0 || done < startDone || total <= done) {
        return std::nullopt;
    }
    const double elapsed = now - startTime;
    const std::uint32_t progressed = done - startDone;
    if (elapsed < minSeconds || progressed < minDone) {
        return std::nullopt;
    }
    const double rate = progressed / elapsed;
    return (total - done) / rate;
}

std::string counter_text(std::uint32_t done, std::uint32_t total) {
    return std::to_string(done > total ? total : done) + " / " + std::to_string(total);
}

std::string eta_text(std::optional<double> secondsLeft) {
    if (!secondsLeft) {
        return {};
    }
    const double s = *secondsLeft;
    if (s >= 90.0) {
        return "About " + std::to_string(static_cast<int>(std::lround(s / 60.0))) + " min left";
    }
    // Rounded to 10 s: a seconds counter that jumps around reads as noise.
    const int rounded = std::max(10, static_cast<int>(std::lround(s / 10.0)) * 10);
    return "About " + std::to_string(rounded) + " s left";
}

LabOptions parse_lab_options(std::string_view text) {
    LabOptions o;
    const auto note = [&o](std::string_view k, std::string_view v) {
        if (!o.unknown.empty()) {
            o.unknown += ", ";
        }
        o.unknown += std::string{k};
        if (!v.empty()) {
            o.unknown += "=" + std::string{v};
        }
    };
    const auto one_of = [](std::string_view v, std::initializer_list<std::string_view> choices) {
        for (const auto c : choices) {
            if (v == c) {
                return true;
            }
        }
        return false;
    };
    for_each_pair(text, [&](std::string_view k, std::string_view v) {
        if (k == "cache_journal" && one_of(v, {"wal", "delete", "truncate", "persist", "memory"})) {
            o.journal = std::string{v};
        } else if (k == "cache_sync" && one_of(v, {"off", "normal", "full"})) {
            o.sync = std::string{v};
        } else if (k == "cache_check" && one_of(v, {"dirty", "always", "never"})) {
            o.check = std::string{v};
        } else if (k == "prep" && v == "auto") {
            o.prep = PrepMode::Auto;
        } else if (k == "prep" && v == "force") {
            o.prep = PrepMode::Force;
        } else if (k == "prep" && v == "off") {
            o.prep = PrepMode::Off;
        } else if (k == "seed_limit" && !v.empty() && v.size() <= 6 &&
                   v.find_first_not_of("0123456789") == std::string_view::npos) {
            o.seedLimit = static_cast<unsigned>(std::stoul(std::string{v}));
        } else if (k == "sqlite_selftest" && one_of(v, {"0", "off", "false"})) {
            o.sqliteSelftest = false;
        } else if (k == "sqlite_selftest" && one_of(v, {"1", "on", "true"})) {
            o.sqliteSelftest = true;
        } else if (k == "shader_tour" && v == "all") {
            o.masterTour = true;
            o.harvestTour = true;
        } else if (k == "shader_tour" && one_of(v, {"1", "on", "true"})) {
            o.masterTour = true;
        } else if (k == "shader_tour" && one_of(v, {"0", "off", "false"})) {
            o.masterTour = false;
        } else if (k == "warmup_scope" && v == "all") {
            o.warmAll = true;
        } else if (k == "warmup_scope" && v == "scene") {
            o.warmAll = false;
        } else if (k == "warmup_duty" && !v.empty() && v.size() <= 3 &&
                   v.find_first_not_of("0123456789") == std::string_view::npos &&
                   std::stoi(std::string{v}) >= 10 && std::stoi(std::string{v}) <= 100) {
            o.warmupDuty = static_cast<unsigned>(std::stoi(std::string{v}));
        } else {
            note(k, v);
        }
    });
    return o;
}

}  // namespace dusk::shader_warmup
