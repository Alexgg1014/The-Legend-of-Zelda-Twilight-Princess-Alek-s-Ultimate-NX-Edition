// H34 host test for src/dusk/shader_prep.cpp: the zone list, the per-zone stopping
// rule, progress and the report of Prepare Shaders.
//   platforms/switch/tools/test-shader-prep.sh
//
// Not covered here (needs the game and the console): the warps themselves, the input
// lock, the saves block and the restore. Those are benchmark.cpp's, shared with the
// benchmark and the profile matrix.
#include "dusk/shader_prep.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <string>

namespace sp = dusk::shader_prep;

static int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

namespace {

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// Every zone must be a stage/room/point the warp table itself lists, so the tour never
// asks for a place the game's own warp menu would not.
void test_zones_against_warp_table(const std::string& mapDefinitionsPath, std::span<const sp::Zone> zones,
                                   size_t minZones, size_t maxZones) {
    std::ifstream in(mapDefinitionsPath);
    CHECK(in.good());
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string table = buffer.str();
    CHECK(!table.empty());

    CHECK(zones.size() >= minZones && zones.size() <= maxZones);
    std::set<std::string> stages;
    std::set<std::string> categories;
    for (const auto& zone : zones) {
        stages.insert(zone.stage);
        categories.insert(zone.category);
        // Never the title or file-select stages, which benchmark::can_start refuses.
        CHECK(std::strcmp(zone.stage, "F_SP102") != 0 && std::strcmp(zone.stage, "F_SP112") != 0);
        CHECK(zone.layer == -1);

        // Find a MapEntry with this name and stage, then its room line.
        const std::string head = std::string("MapEntry(\"") + zone.name + "\", \"" + zone.stage + "\"";
        const auto at = table.find(head);
        CHECK(at != std::string::npos);
        if (at == std::string::npos) {
            std::cerr << "  zone not in warp table: " << zone.name << " " << zone.stage << "\n";
            continue;
        }
        const auto end = table.find("MapEntry(", at + head.size());
        const std::string entry = table.substr(at, end == std::string::npos ? std::string::npos : end - at);
        const std::regex roomLine("\\{\\s*" + std::to_string(zone.room) + "\\s*,\\s*\\{([^}]*)\\}");
        std::smatch match;
        const bool roomFound = std::regex_search(entry, match, roomLine);
        CHECK(roomFound);
        if (!roomFound) {
            std::cerr << "  room " << zone.room << " not listed for " << zone.name << "\n";
            continue;
        }
        bool pointFound = false;
        std::stringstream points(match[1].str());
        std::string token;
        while (std::getline(points, token, ',')) {
            // Long point lists span lines and may end in a comma: skip blank tokens.
            if (token.find_first_of("0123456789") == std::string::npos) {
                continue;
            }
            if (std::stoi(token) == zone.point) {
                pointFound = true;
            }
        }
        CHECK(pointFound);
    }
    // No two zones are the same stage: coverage, not length.
    CHECK(stages.size() == zones.size());
    CHECK(categories.size() == zones.size());
    // The heavy exterior is the benchmark's scene.
    CHECK(std::strcmp(zones[0].stage, "F_SP122") == 0 && zones[0].room == 8 && zones[0].point == 0);

    // Ordon Village must be entered from Outside Link's House (point 1). Point 0 is also
    // in the warp table, which is why the check above could not catch it: it is the Ordon
    // Ranch pathway entrance and on a new file it starts the goat scene and leaves for
    // F_SP00 (H34-c2 on hardware). The exact entry is required, not just a listed one.
    int ordon = 0;
    for (const auto& zone : zones) {
        if (std::strcmp(zone.name, "Ordon Village") == 0) {
            ++ordon;
            CHECK(std::strcmp(zone.stage, "F_SP103") == 0);
            CHECK(zone.room == 0);
            CHECK(zone.point == 1);
            CHECK(zone.layer == -1);
        }
    }
    CHECK(ordon == 1);
}

void test_decide() {
    const sp::Timing t{};
    sp::ZoneProgress p;
    // Always drawn for the minimum, even when everything is quiet.
    p.sinceSettled = t.minRenderSeconds - 0.1;
    p.queueEmptyFor = 100;
    p.sinceLastCreated = 100;
    CHECK(sp::decide(p, t) == sp::ZoneEnd::None);

    // Warm: nothing created in the zone, queue quiet.
    p.sinceSettled = t.minRenderSeconds;
    p.createdInZone = 0;
    CHECK(sp::decide(p, t) == sp::ZoneEnd::Warm);

    // Queue not yet quiet long enough.
    p.queueEmptyFor = t.quietSeconds - 0.5;
    CHECK(sp::decide(p, t) == sp::ZoneEnd::None);

    // New pipelines: wait until nothing new for idleSeconds and the queue is quiet.
    p.queueEmptyFor = t.quietSeconds;
    p.createdInZone = 40;
    p.sinceLastCreated = t.idleSeconds - 1;
    CHECK(sp::decide(p, t) == sp::ZoneEnd::None);
    p.sinceLastCreated = t.idleSeconds;
    CHECK(sp::decide(p, t) == sp::ZoneEnd::Drained);

    // Still compiling at the limit: move on.
    p.queued = 12;
    p.queueEmptyFor = 0;
    p.sinceSettled = t.maxZoneSeconds - 1;
    CHECK(sp::decide(p, t) == sp::ZoneEnd::None);
    p.sinceSettled = t.maxZoneSeconds;
    CHECK(sp::decide(p, t) == sp::ZoneEnd::TimedOut);

    // A non-zero queue is never "quiet", whatever the timer says.
    p.sinceSettled = 20;
    p.queued = 1;
    p.queueEmptyFor = 50;
    p.createdInZone = 0;
    CHECK(sp::decide(p, t) == sp::ZoneEnd::None);

    // Expected duration: warm, every zone ends at the minimum; cold, at most the limit.
    const auto zones = sp::zones().size();
    CHECK(zones * t.minRenderSeconds <= 120.0);
    CHECK(zones * (t.maxZoneSeconds + 20.0) <= 20.0 * 60.0);
}

void test_progress() {
    CHECK(sp::overall_progress(0, 6, 0.0) == 0.0);
    CHECK(sp::overall_progress(3, 6, 0.0) == 0.5);
    CHECK(sp::overall_progress(5, 6, 1.0) == 1.0);
    CHECK(sp::overall_progress(6, 6, 0.7) == 1.0);
    CHECK(sp::overall_progress(2, 6, 5.0) == 0.5);   // fraction clamped
    CHECK(sp::overall_progress(2, 6, -1.0) == 2.0 / 6.0);
    CHECK(sp::overall_progress(0, 0, 0.0) == 1.0);
}

sp::TourReport sample_report(sp::Outcome outcome) {
    sp::TourReport r;
    r.outcome = outcome;
    r.detail = outcome == sp::Outcome::Cancelled ? "cancelled with Start" : "";
    r.buildId = "H34-c1-0123456789";
    r.startedUtc = "2026-10-02T12:00:00Z";
    r.startedUnix = 1790942400;
    r.fileName = sp::report_file_name("20261002-120000", r.buildId);
    r.mode = "dock";
    r.originalStage = "F_SP103";
    r.originalRoom = 0;
    r.totalSeconds = 612.4;
    r.queuedAtStart = 3;
    r.queuedMax = 57;
    r.queuedAtEnd = 0;
    r.createdTotal = 211;
    r.knownAtStart = 649;
    r.zones.assign(sp::zones().size(), sp::ZoneResult{});
    r.zones[0] = {.status = sp::ZoneStatus::Completed, .end = sp::ZoneEnd::Warm, .loadSeconds = 9.5,
                  .renderSeconds = 15.1, .arrivedStage = "F_SP122", .arrivedRoom = 8};
    r.zones[1] = {.status = sp::ZoneStatus::Completed, .end = sp::ZoneEnd::Drained, .loadSeconds = 12,
                  .renderSeconds = 61, .queuedAtStart = 30, .queuedMax = 57, .created = 120,
                  .arrivedStage = "F_SP116", .arrivedRoom = 0};
    r.zones[2] = {.status = sp::ZoneStatus::Failed, .detail = "the zone did not finish loading",
                  .loadSeconds = 120};
    r.zones[3] = {.status = sp::ZoneStatus::Skipped, .detail = "left the zone for \"F_SP103\""};
    if (outcome == sp::Outcome::Cancelled) {
        r.zones[4] = {.status = sp::ZoneStatus::Cancelled, .detail = "cancelled with Start"};
    }
    r.returned = true;
    r.stateRestored = true;
    r.savesReenabled = true;
    r.configSavesDuringRun = 0;
    return r;
}

void write(const std::string& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
}

void test_report() {
    const auto plan = sp::zones();
    CHECK(sp::report_file_name("20261002-120000", "H34-c1-abc") == "shaders-20261002-120000-H34-c1-abc.json");

    const auto completed = sample_report(sp::Outcome::Completed);
    const std::string json = sp::build_report(completed, plan);
    CHECK(contains(json, "\"schema\": \"dusklight-aleks-shader-prep\""));
    CHECK(contains(json, "\"schema_version\": 1"));
    CHECK(contains(json, "\"status\": \"completed\""));
    CHECK(contains(json, "\"planned\": 6"));
    CHECK(contains(json, "\"skipped\": 1"));
    CHECK(contains(json, "\"failed\": 1"));
    CHECK(contains(json, "\"config_json_written\": false"));
    CHECK(contains(json, "\"saves_reenabled\": true"));
    CHECK(contains(json, "\"end\": \"warm\""));
    CHECK(contains(json, "\"end\": \"drained\""));
    CHECK(contains(json, "\\\"F_SP103\\\""));  // quotes in a detail are escaped
    CHECK(contains(json, "not a benchmark"));
    CHECK(!contains(json, "fps"));
    write("shader_prep_test.completed.json", json);

    const auto cancelled = sample_report(sp::Outcome::Cancelled);
    const std::string cancelledJson = sp::build_report(cancelled, plan);
    CHECK(contains(cancelledJson, "\"status\": \"cancelled\""));
    CHECK(contains(cancelledJson, "\"cancelled\": 1"));
    CHECK(contains(cancelledJson, "\"not_reached\": 1"));
    write("shader_prep_test.cancelled.json", cancelledJson);

    auto failed = sample_report(sp::Outcome::Failed);
    failed.configSavesDuringRun = 2;
    failed.returned = false;
    const std::string failedJson = sp::build_report(failed, plan);
    CHECK(contains(failedJson, "\"status\": \"failed\""));
    CHECK(contains(failedJson, "\"config_json_written\": true"));
    CHECK(contains(failedJson, "\"returned_to_original_place\": false"));
    write("shader_prep_test.failed.json", failedJson);

    const std::string text = sp::summary_text(completed, plan);
    CHECK(contains(text, "Finished after 612 s"));
    CHECK(contains(text, "Outside Castle Town - West: already prepared"));
    CHECK(contains(text, "Castle Town: prepared, 120 new"));
    CHECK(contains(text, "Ordon Village: failed - the zone did not finish loading"));
    CHECK(contains(text, "Forest Temple: not reached"));
    CHECK(contains(sp::summary_text(cancelled, plan), "Cancelled after"));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: shader_prep_test <path to src/dusk/map_loader_definitions.h>\n";
        return 2;
    }
    test_zones_against_warp_table(argv[1], sp::player_zones(), 5, 8);
    // aleks2 (SHADERS-c1): the Shader Tour's list, same checks; zones() follows the switch.
    test_zones_against_warp_table(argv[1], sp::master_zones(), 15, 40);
    CHECK(sp::zones().size() == sp::player_zones().size());
    sp::use_master_tour(true);
    CHECK(sp::zones().size() == sp::master_zones().size());
    sp::use_master_tour(false);
    test_decide();
    test_progress();
    test_report();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "shader_prep: all checks passed\n";
    return 0;
}
