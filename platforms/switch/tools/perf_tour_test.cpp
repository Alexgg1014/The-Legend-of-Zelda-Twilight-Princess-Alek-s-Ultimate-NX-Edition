// H35 host test for src/dusk/perf_tour.cpp: the three-zone plan and its order, the
// per-zone rules, progress, the interpolation recovery tracking and the report.
//   platforms/switch/tools/test-perf-tour.sh
//
// Not covered here (needs the game and the console): the warps, the input lock, the
// measurement itself. The start/end order, the saves guard and the achievement suspension
// of a tour are in diagnostic_run_test.cpp.
#include "dusk/perf_tour.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace pt = dusk::perf_tour;
namespace fr = dusk::fifo_report;

static int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

namespace {

bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// Every zone is a stage/room/point the warp table lists, under the name it uses.
void test_plan_against_warp_table(const std::string& mapDefinitionsPath) {
    std::ifstream in(mapDefinitionsPath);
    CHECK(in.good());
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string table = buffer.str();
    for (const auto& zone : pt::zones()) {
        const std::string head = std::string("MapEntry(\"") + zone.name + "\", \"" + zone.stage + "\"";
        const auto at = table.find(head);
        CHECK(at != std::string::npos);
        if (at == std::string::npos) {
            continue;
        }
        const auto end = table.find("MapEntry(", at + head.size());
        const std::string entry = table.substr(at, end == std::string::npos ? std::string::npos : end - at);
        const std::regex roomLine("\\{\\s*" + std::to_string(zone.room) + "\\s*,\\s*\\{([^}]*)\\}");
        std::smatch match;
        const bool roomFound = std::regex_search(entry, match, roomLine);
        CHECK(roomFound);
        if (!roomFound) {
            continue;
        }
        bool pointFound = false;
        std::stringstream points(match[1].str());
        std::string token;
        while (std::getline(points, token, ',')) {
            pointFound = pointFound || std::stoi(token) == zone.point;
        }
        CHECK(pointFound);
    }
}

void test_plan_and_order() {
    const auto zones = pt::zones();
    CHECK(zones.size() == 3);
    if (zones.size() != 3) {
        return;
    }
    // Heavy, medium, light, exactly in this order: the order is what shows a recovery.
    CHECK(std::strcmp(zones[0].stage, "F_SP122") == 0 && zones[0].room == 8 && zones[0].point == 0);
    CHECK(std::strcmp(zones[0].name, "Outside Castle Town - West") == 0);
    CHECK(std::strcmp(zones[0].category, "heavy") == 0);
    // Ordon Village from Outside Link's House: point 1, never point 0 (the Ordon Ranch
    // pathway, which plays the goat scene on a new file and leaves for F_SP00).
    CHECK(std::strcmp(zones[1].stage, "F_SP103") == 0 && zones[1].room == 0 && zones[1].point == 1);
    CHECK(std::strcmp(zones[1].name, "Ordon Village") == 0);
    CHECK(std::strcmp(zones[1].category, "medium") == 0);
    CHECK(std::strcmp(zones[2].stage, "R_SP01") == 0 && zones[2].room == 4 && zones[2].point == 0);
    CHECK(std::strcmp(zones[2].name, "Link's House") == 0);
    CHECK(std::strcmp(zones[2].category, "light") == 0);
    for (const auto& zone : zones) {
        CHECK(zone.layer == -1);
        CHECK(std::strcmp(zone.stage, "F_SP102") != 0 && std::strcmp(zone.stage, "F_SP112") != 0);
    }
}

void test_timing_and_duration() {
    const pt::Timing t{};
    // Roughly 7-10 s of warm-up and 25-30 s of measurement per zone.
    CHECK(t.warmupSeconds >= 7.0 && t.warmupSeconds <= 10.0);
    CHECK(t.measureSeconds >= 25.0 && t.measureSeconds <= 30.0);
    // A typical tour: loads of ~15 s, a short pipeline wait, the walk back: 3-5 minutes.
    const double typical = 3 * (15.0 + 2.0 + t.warmupSeconds + t.measureSeconds) + 15.0;
    CHECK(typical >= 180.0 && typical <= 300.0);
    // The worst case is bounded: every load at its limit and every queue timing out.
    const double worst = 3 * (t.loadTimeoutSeconds + pt::max_seconds_per_zone_after_load(t)) + t.loadTimeoutSeconds;
    CHECK(worst < 15.0 * 60.0);
}

void test_pipeline_wait() {
    const pt::Timing t{};
    CHECK(pt::decide_pipeline_wait({.waited = 0.5, .queueEmptyFor = 0.0}, t) == pt::PipelineWaitEnd::None);
    CHECK(pt::decide_pipeline_wait({.waited = 3.0, .queueEmptyFor = t.pipelineQuietSeconds}, t) ==
          pt::PipelineWaitEnd::Drained);
    CHECK(pt::decide_pipeline_wait({.waited = t.pipelineTimeoutSeconds, .queueEmptyFor = 0.2}, t) ==
          pt::PipelineWaitEnd::TimedOut);
    // Drained wins when both hold.
    CHECK(pt::decide_pipeline_wait({.waited = t.pipelineTimeoutSeconds, .queueEmptyFor = 5.0}, t) ==
          pt::PipelineWaitEnd::Drained);
}

void test_progress() {
    const pt::Timing t{};
    double last = -1.0;
    for (std::size_t zone = 0; zone < 3; ++zone) {
        for (const auto step : {pt::Step::Loading, pt::Step::PipelineWait, pt::Step::Warmup, pt::Step::Measuring}) {
            for (const double s : {0.0, 5.0, 30.0}) {
                const double p = pt::overall_progress(zone, 3, step, s, t);
                CHECK(p >= 0.0 && p <= 1.0);
                CHECK(p + 1e-12 >= last);  // never goes back
                last = p;
            }
        }
    }
    CHECK(near(pt::overall_progress(0, 3, pt::Step::Loading, 100.0, t), 0.0));
    CHECK(pt::overall_progress(2, 3, pt::Step::Measuring, t.measureSeconds, t) > 0.999);
    CHECK(near(pt::overall_progress(3, 3, pt::Step::Loading, 0.0, t), 1.0));
    CHECK(near(pt::overall_progress(0, 0, pt::Step::Loading, 0.0, t), 1.0));
}

void test_recovery_tracker() {
    // Heavy zone: arrives fine, stalls, stays stalled.
    pt::RecoveryTracker heavy;
    heavy.sample(0.0, true);  // before arrival: ignored
    CHECK(!heavy.arrived() && !heavy.stalledInZone());
    heavy.arrive(10.0, false);
    heavy.sample(11.0, false);
    heavy.sample(20.0, true);
    heavy.sample(40.0, true);
    CHECK(!heavy.stalledAtArrival() && heavy.stalledInZone() && heavy.stalledNow() && !heavy.recovered());
    CHECK(heavy.recoverySeconds() < 0.0);

    // Light zone after it: arrives stalled (nothing reset), recovers 6.5 s later.
    pt::RecoveryTracker light;
    light.arrive(100.0, true);
    light.sample(101.0, true);
    light.sample(106.5, false);
    light.sample(107.0, true);   // a later stall is the governor's business, not a new recovery
    light.sample(110.0, false);
    CHECK(light.stalledAtArrival() && light.recovered());
    CHECK(near(light.recoverySeconds(), 6.5));

    // Never stalled: nothing to recover from.
    pt::RecoveryTracker calm;
    calm.arrive(0.0, false);
    calm.sample(1.0, false);
    CHECK(!calm.stalledInZone() && !calm.recovered());

    // Stalls inside the zone and comes back: counted from arrival.
    pt::RecoveryTracker mid;
    mid.arrive(50.0, false);
    mid.sample(55.0, true);
    mid.sample(58.0, false);
    CHECK(mid.stalledInZone() && mid.recovered() && near(mid.recoverySeconds(), 8.0));

    // A new arrival starts over.
    mid.arrive(200.0, false);
    CHECK(!mid.stalledInZone() && !mid.recovered());
}

void test_interp_frame_counting() {
    dusk::interp_frames::Counters c;
    c.count_frame(true, true, false, 0);   // interpolated only
    c.count_frame(true, true, false, 1);   // interpolated with a tick
    c.count_frame(true, false, true, 1);   // stalled: normal
    c.count_frame(false, false, false, 1);  // interpolation off: normal
    c.count_frame(true, true, false, 2);   // two ticks
    c.count_predictive_reject();
    CHECK(c.frames == 5 && c.normal == 2 && c.stalled == 1);
    CHECK(c.interpolatedOnly == 1 && c.interpolatedWithTick == 2 && c.predictiveRejects == 1);
    CHECK(c.simTicks == 5);
    CHECK(c.normal + c.interpolatedOnly + c.interpolatedWithTick == c.frames);
    const auto d = dusk::interp_frames::delta(dusk::interp_frames::Counters{}, c);
    CHECK(d.frames == c.frames && d.simTicks == c.simTicks);
}

void test_cache_state() {
    pt::Loading l;
    CHECK(std::string(pt::cache_state(l)) == "warm");
    l.createdInZone = 12;
    CHECK(std::string(pt::cache_state(l)) == "cold");
    l.queuedMaxInWindow = 3;
    CHECK(std::string(pt::cache_state(l)) == "compiling");
}

void test_file_name() {
    CHECK(pt::report_file_name("20261002-120000", "H35-c1-abcdef0123") == "tour-20261002-120000-H35-c1-abcdef0123.json");
}

// A zone result with distinct numbers, so a mix-up between zones shows in the report.
pt::ZoneResult measured_zone(int k, bool stalledArrival, bool recovers) {
    pt::ZoneResult z;
    z.status = pt::ZoneStatus::Completed;
    z.measured = true;
    z.measurementComplete = true;
    z.measureSecondsWall = 30.0;
    std::vector<float> frames(900, 33.0f + static_cast<float>(k));
    z.frames = dusk::benchmark::summarize_frames(frames);
    z.cpuZones = {{"Execute", 30.0 + k, 40.0, 1.0}, {"Painter", 18.0 + k, 25.0, 1.0}, {"TickWait", 3.0, 9.0, 1.0},
                  {"DrawRec", 10.0 + k, 14.0, 1.0}, {"Actors", 1.5, 3.0, 1.0}};
    z.zoneTimes = {.painterMs = 18.0 + k, .tickWaitMs = 3.0, .drawRecMs = 10.0 + k, .executeMs = 30.0 + k,
                   .actorsMs = 1.5, .havePainter = true, .haveTickWait = true, .haveDrawRec = true};
    fr::Snapshot a;
    fr::Snapshot b;
    b.fifo.busy_ns = static_cast<std::uint64_t>(900) * (6'000'000 + 1'000'000 * k);
    b.fifo.processed_bytes = 900u * 70000u;
    b.fifo.publishes = 900u * 100u;
    b.fifo.wake_requests = 900u * 95u;
    b.fifo.drain_sites[AURORA_FIFO_DRAIN_DRAW_DONE] = {900, 900, static_cast<std::uint64_t>(900) * (4'000'000 + k * 1'000'000)};
    b.fifo.drain_sites[AURORA_FIFO_DRAIN_END_FRAME] = {900, 0, 0};
    b.fifo.drains = {1800, 900, b.fifo.drain_sites[AURORA_FIFO_DRAIN_DRAW_DONE].wait_ns};
    b.uniforms.full_builds = 900u * 1300u;
    z.fifo = fr::window(a, b, 900);
    z.uniformMismatchSection = "viewport";
    z.interp.requestedMode = "unlimited";
    z.interp.quality = "fast";
    z.interp.predictive = true;
    z.interp.windowSeconds = 30.0;
    for (int i = 0; i < 900; ++i) {
        z.interp.frames.count_frame(true, k != 0, k == 0, 1);
    }
    z.interp.window.stalledSeconds = k == 0 ? 30.0 : 0.0;
    z.interp.recovery.arrive(100.0 * k, stalledArrival);
    if (k == 0) {
        z.interp.recovery.sample(100.0 * k + 5.0, true);
    }
    if (recovers) {
        z.interp.recovery.sample(100.0 * k + 4.25, false);
    }
    z.loading.loadSeconds = 12.0 + k;
    z.loading.queuedAtArrival = 5;
    z.loading.queuedMax = 9;
    z.loading.pipelineWaitSeconds = 2.0;
    z.env.mode = "handheld";
    z.env.profile = "Quality";
    z.env.renderWidth = 1280;
    z.env.renderHeight = 720;
    z.env.companionLayout = "normal";
    return z;
}

pt::TourReport base_report() {
    pt::TourReport r;
    r.buildId = "H35-c1-0123456789";
    r.buildTag = "H35-c1";
    r.buildCommit = "0123456789";
    r.startedUtc = "2026-10-02T12:00:00Z";
    r.startedUnix = 1790000000;
    r.fileName = "tour-20261002-120000-H35-c1-0123456789.json";
    r.originalStage = "F_SP103";
    r.originalRoom = 1;
    r.envAtStart.mode = "handheld";
    r.envAtStart.profile = "Quality";
    r.envAtStart.renderWidth = 1280;
    r.envAtStart.renderHeight = 720;
    r.envAtStart.companionLayout = "normal";
    r.interpRequestedMode = "unlimited";
    r.interpQuality = "fast";
    r.uniformReuseModeAtStart = "off";
    r.mods = {{"rollandsprint", "1.0.0"}};
    r.zones.assign(pt::zones().size(), pt::ZoneResult{});
    r.returned = r.stateRestored = r.savesReenabled = true;
    r.achievementsSuspendedDuringRun = true;
    return r;
}

void test_reports() {
    // Completed: the heavy zone stalls, the light zone arrives stalled and recovers.
    auto completed = base_report();
    completed.zones[0] = measured_zone(0, false, false);
    completed.zones[1] = measured_zone(1, true, false);
    completed.zones[2] = measured_zone(2, true, true);
    completed.totalSeconds = 205.0;
    const std::string json = pt::build_report(completed, pt::zones());
    std::ofstream("perf_tour_test.completed.json", std::ios::binary) << json;
    CHECK(contains(json, "\"schema\": \"dusklight-aleks-performance-tour\""));
    CHECK(contains(json, "\"heavy_zone_stalled\": true"));
    CHECK(contains(json, "\"light_zone_recovered\": true"));
    CHECK(contains(json, "\"config_json_written\": false"));
    const std::string summary = pt::summary_text(completed, pt::zones());
    CHECK(contains(summary, "Outside Castle Town - West (heavy)"));
    CHECK(contains(summary, "stalled then recovered"));

    // Cancelled with Start while measuring the medium zone: partial result, light not reached.
    auto cancelled = base_report();
    cancelled.outcome = pt::Outcome::Cancelled;
    cancelled.detail = "cancelled with Start";
    cancelled.zones[0] = measured_zone(0, false, false);
    cancelled.zones[1] = measured_zone(1, true, false);
    cancelled.zones[1].status = pt::ZoneStatus::Cancelled;
    cancelled.zones[1].measurementComplete = false;
    cancelled.zones[1].detail = "cancelled with Start";
    std::ofstream("perf_tour_test.cancelled.json", std::ios::binary) << pt::build_report(cancelled, pt::zones());
    CHECK(contains(pt::summary_text(cancelled, pt::zones()), "Link's House (light): not reached"));

    // The first zone does not load: failed, the others still run.
    auto failedLoad = base_report();
    failedLoad.zones[0].status = pt::ZoneStatus::Failed;
    failedLoad.zones[0].detail = "the zone did not finish loading";
    failedLoad.zones[1] = measured_zone(1, false, false);
    failedLoad.zones[2] = measured_zone(2, false, false);
    std::ofstream("perf_tour_test.failed_zone.json", std::ios::binary) << pt::build_report(failedLoad, pt::zones());

    // Different environments across zones are flagged, not mixed silently.
    auto mixed = completed;
    mixed.zones[2].env.mode = "dock";
    CHECK(contains(pt::build_report(mixed, pt::zones()), "\"consistent_environment\": false"));
    CHECK(contains(json, "\"consistent_environment\": true"));
    auto modeChange = completed;
    modeChange.zones[1].fifo.modeChangedDuringWindow = true;
    CHECK(contains(pt::build_report(modeChange, pt::zones()), "\"consistent_environment\": false"));

    // Restore failures and a real config write are reported as such.
    auto badRestore = completed;
    badRestore.returned = false;
    badRestore.configSavesDuringRun = 1;
    const std::string bad = pt::build_report(badRestore, pt::zones());
    CHECK(contains(bad, "\"returned_to_original_place\": false"));
    CHECK(contains(bad, "\"config_json_written\": true"));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: perf_tour_test <path to src/dusk/map_loader_definitions.h>\n";
        return 2;
    }
    test_plan_against_warp_table(argv[1]);
    test_plan_and_order();
    test_timing_and_duration();
    test_pipeline_wait();
    test_progress();
    test_recovery_tracker();
    test_interp_frame_counting();
    test_cache_state();
    test_file_name();
    test_reports();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "perf_tour: all checks passed\n";
    return 0;
}
