#include "dusk/shader_prep.hpp"

#include "dusk/benchmark_report.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace dusk::shader_prep {
namespace {

// Six zones, each adding something the others do not. One heavy exterior, one town,
// one village (a medium zone), one interior, one forest and one dungeon. Several
// near-identical zones would only make the tour longer.
constexpr std::array kZones{
    Zone{"F_SP122", 8, 0, -1, "Outside Castle Town - West", "heavy exterior"},
    Zone{"F_SP116", 0, 0, -1, "Castle Town", "town"},
    // Point 1 (from Outside Link's House), never point 0: point 0 is the Ordon Ranch
    // pathway entrance, which on a new file plays the goat-herding scene and leaves for
    // F_SP00 (H34-c2 hardware, cross-checked against the Randomizer's entrance table).
    Zone{"F_SP103", 0, 1, -1, "Ordon Village", "village (medium zone)"},
    Zone{"R_SP01", 4, 0, -1, "Link's House", "interior"},
    Zone{"F_SP108", 0, 0, -1, "South Faron Woods", "forest"},
    Zone{"D_MN05", 0, 0, -1, "Forest Temple", "dungeon"},
};

}  // namespace

std::span<const Zone> zones() {
    return kZones;
}

ZoneEnd decide(const ZoneProgress& p, const Timing& t) {
    if (p.sinceSettled < t.minRenderSeconds) {
        return ZoneEnd::None;
    }
    const bool quiet = p.queued == 0 && p.queueEmptyFor >= t.quietSeconds;
    if (quiet && p.createdInZone == 0) {
        return ZoneEnd::Warm;
    }
    if (quiet && p.sinceLastCreated >= t.idleSeconds) {
        return ZoneEnd::Drained;
    }
    if (p.sinceSettled >= t.maxZoneSeconds) {
        return ZoneEnd::TimedOut;
    }
    return ZoneEnd::None;
}

const char* zone_status_name(ZoneStatus status) {
    switch (status) {
    case ZoneStatus::Pending: return "pending";
    case ZoneStatus::Completed: return "completed";
    case ZoneStatus::Skipped: return "skipped";
    case ZoneStatus::Failed: return "failed";
    case ZoneStatus::Cancelled: return "cancelled";
    }
    return "unknown";
}

const char* zone_end_name(ZoneEnd end) {
    switch (end) {
    case ZoneEnd::None: return "none";
    case ZoneEnd::Warm: return "warm";
    case ZoneEnd::Drained: return "drained";
    case ZoneEnd::TimedOut: return "timed_out";
    }
    return "unknown";
}

const char* outcome_name(Outcome outcome) {
    switch (outcome) {
    case Outcome::Completed: return "completed";
    case Outcome::Cancelled: return "cancelled";
    case Outcome::Failed: return "failed";
    }
    return "unknown";
}

double overall_progress(std::size_t zoneIndex, std::size_t zoneCount, double zoneFraction) {
    if (zoneCount == 0) {
        return 1.0;
    }
    const double done = static_cast<double>(std::min(zoneIndex, zoneCount));
    const double part = zoneIndex >= zoneCount ? 0.0 : std::clamp(zoneFraction, 0.0, 1.0);
    return std::clamp((done + part) / static_cast<double>(zoneCount), 0.0, 1.0);
}

std::string report_file_name(std::string_view stamp, std::string_view buildId) {
    std::string name = benchmark::report_file_name(stamp, buildId);
    constexpr std::string_view benchPrefix = "bench-";
    if (name.starts_with(benchPrefix)) {
        name.replace(0, benchPrefix.size(), "shaders-");
    }
    return name;
}

std::string build_report(const TourReport& r, std::span<const Zone> plan) {
    benchmark::JsonWriter j;
    j.begin_object();
    j.str("schema", "dusklight-aleks-shader-prep");
    j.integer("schema_version", kReportSchemaVersion);
    j.str("note", "Prepare Shaders is not a benchmark: it records what was compiled, not frame rates");

    j.begin_object("build");
    j.str("id", r.buildId);
    j.end_object();

    j.begin_object("run");
    j.str("status", outcome_name(r.outcome));
    j.str("detail", r.detail);
    j.str("started_utc", r.startedUtc);
    j.integer("started_unix", r.startedUnix);
    j.str("file", r.fileName);
    j.str("mode", r.mode);
    j.str("original_stage", r.originalStage);
    j.integer("original_room", r.originalRoom);
    j.num("total_seconds", r.totalSeconds, 1);
    j.end_object();

    std::size_t completed = 0, skipped = 0, failed = 0, cancelled = 0;
    for (const auto& zone : r.zones) {
        completed += zone.status == ZoneStatus::Completed;
        skipped += zone.status == ZoneStatus::Skipped;
        failed += zone.status == ZoneStatus::Failed;
        cancelled += zone.status == ZoneStatus::Cancelled;
    }
    j.begin_object("zones_summary");
    j.integer("planned", static_cast<std::int64_t>(plan.size()));
    j.integer("completed", static_cast<std::int64_t>(completed));
    j.integer("skipped", static_cast<std::int64_t>(skipped));
    j.integer("failed", static_cast<std::int64_t>(failed));
    j.integer("cancelled", static_cast<std::int64_t>(cancelled));
    j.integer("not_reached",
        static_cast<std::int64_t>(plan.size() - std::min(plan.size(), completed + skipped + failed + cancelled)));
    j.end_object();

    j.begin_object("pipelines");
    j.integer("known_at_start", r.knownAtStart);
    j.integer("queued_at_start", r.queuedAtStart);
    j.integer("queued_max", r.queuedMax);
    j.integer("queued_at_end", r.queuedAtEnd);
    j.integer("created_during_run", r.createdTotal);
    j.str("note", "created counts pipelines read from Dawn's cache and freshly compiled alike; "
                  "Aurora does not tell them apart");
    j.end_object();

    j.begin_array("zones");
    for (std::size_t i = 0; i < plan.size(); ++i) {
        const ZoneResult empty{};
        const ZoneResult& z = i < r.zones.size() ? r.zones[i] : empty;
        j.begin_object();
        j.str("stage", plan[i].stage);
        j.integer("room", plan[i].room);
        j.integer("point", plan[i].point);
        j.str("name", plan[i].name);
        j.str("category", plan[i].category);
        j.str("status", zone_status_name(z.status));
        j.str("end", zone_end_name(z.end));
        j.str("detail", z.detail);
        j.str("arrived_stage", z.arrivedStage);
        j.integer("arrived_room", z.arrivedRoom);
        j.num("load_seconds", z.loadSeconds, 1);
        j.num("render_seconds", z.renderSeconds, 1);
        j.integer("queued_at_start", z.queuedAtStart);
        j.integer("queued_max", z.queuedMax);
        j.integer("queued_at_end", z.queuedAtEnd);
        j.integer("created", z.created);
        j.end_object();
    }
    j.end_array();

    j.begin_object("restore");
    j.boolean("returned_to_original_place", r.returned);
    j.boolean("in_memory_save_state_restored", r.stateRestored);
    j.boolean("saves_reenabled", r.savesReenabled);
    j.integer("config_saves_during_run", static_cast<std::int64_t>(r.configSavesDuringRun));
    j.boolean("config_json_written", r.configSavesDuringRun != 0);
    j.end_object();

    j.end_object();
    return j.take();
}

std::string summary_text(const TourReport& r, std::span<const Zone> plan) {
    std::string out;
    char line[256];
    std::snprintf(line, sizeof line, "Build %s\n", r.buildId.c_str());
    out += line;
    std::snprintf(line, sizeof line, "%s after %.0f s - %u pipelines created, %u still queued\n",
        r.outcome == Outcome::Completed ? "Finished" :
        r.outcome == Outcome::Cancelled ? "Cancelled" :
                                          "Stopped",
        r.totalSeconds, r.createdTotal, r.queuedAtEnd);
    out += line;
    for (std::size_t i = 0; i < plan.size() && i < r.zones.size(); ++i) {
        const auto& z = r.zones[i];
        if (z.status == ZoneStatus::Pending) {
            std::snprintf(line, sizeof line, "%s: not reached\n", plan[i].name);
        } else if (z.status == ZoneStatus::Completed) {
            std::snprintf(line, sizeof line, "%s: %s, %u new, %.0f s\n", plan[i].name,
                z.end == ZoneEnd::Warm ? "already prepared" :
                z.end == ZoneEnd::Drained ? "prepared" :
                                            "time limit reached",
                z.created, z.loadSeconds + z.renderSeconds);
        } else {
            std::snprintf(line, sizeof line, "%s: %s%s%s\n", plan[i].name, zone_status_name(z.status),
                z.detail.empty() ? "" : " - ", z.detail.c_str());
        }
        out += line;
    }
    if (r.queuedAtEnd > 0) {
        out += "The remaining shaders keep compiling in the background.\n";
    }
    return out;
}

}  // namespace dusk::shader_prep
