// Host test for src/dusk/benchmark_report.cpp and (H35) src/dusk/fifo_report.cpp. Plain
// asserts; no framework.
//   platforms/switch/tools/test-benchmark-report.sh
#include "dusk/benchmark_report.hpp"
#include "dusk/fifo_report.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace dusk::benchmark;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond << "\n";             \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

static std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void test_percentile() {
    std::vector<float> v;
    for (int i = 1; i <= 100; ++i) {
        v.push_back(static_cast<float>(i));
    }
    CHECK(near(percentile_sorted(v, 50), 50));
    CHECK(near(percentile_sorted(v, 95), 95));
    CHECK(near(percentile_sorted(v, 99), 99));
    CHECK(near(percentile_sorted(v, 100), 100));
    CHECK(near(percentile_sorted(v, 0), 1));  // rank is clamped to the first sample
    CHECK(near(percentile_sorted({}, 99), 0));
    CHECK(near(percentile_sorted({7.0f}, 99), 7));
}

static void test_summary_steady() {
    // 1800 frames at exactly 33.333 ms: a clean 30 fps run.
    std::vector<float> v(1800, 1000.0f / 30.0f);
    const auto s = summarize_frames(v);
    CHECK(s.frames == 1800);
    CHECK(near(s.avgFps, 30.0, 1e-3));
    CHECK(near(s.low1PctFps, 30.0, 1e-3));
    CHECK(near(s.medianMs, 1000.0 / 30.0, 1e-3));
    CHECK(s.over33 == 1800);  // 33.333 > 33.3: a 30 fps tick is "over" by that definition
    CHECK(s.over50 == 0);
    CHECK(near(s.coveredSeconds, 60.0, 1e-2));
}

static void test_summary_with_stutter() {
    // 98 frames of 30 ms, one 60 ms, one 120 ms.
    std::vector<float> v(98, 30.0f);
    v.push_back(60.0f);
    v.push_back(120.0f);
    const auto s = summarize_frames(v);
    CHECK(s.frames == 100);
    CHECK(s.over33 == 2);
    CHECK(s.over50 == 2);
    CHECK(near(s.pctOver33, 2.0));
    CHECK(near(s.maxMs, 120.0));
    CHECK(near(s.p99Ms, 60.0));   // nearest rank: the 99th of 100
    CHECK(near(s.p95Ms, 30.0));
    // Slowest 1 % of 100 frames is one frame: the 120 ms one.
    CHECK(near(s.low1PctFps, 1000.0 / 120.0, 1e-3));
    // Average rate is frames over time, not the mean of rates.
    const double total = 98 * 30.0 + 60.0 + 120.0;
    CHECK(near(s.avgFps, 100.0 * 1000.0 / total, 1e-6));
}

static void test_summary_empty() {
    const auto s = summarize_frames({});
    CHECK(s.frames == 0);
    CHECK(near(s.avgFps, 0));
    CHECK(near(s.low1PctFps, 0));
}

static void test_json() {
    JsonWriter w;
    w.begin_object();
    w.str("name", "quote\" back\\slash\nnewline\ttab \x01");
    w.num("fps", 29.97312, 2);
    w.num("nan", std::nan(""));
    w.integer("count", -5);
    w.boolean("flag", true);
    w.null("nothing");
    w.begin_object("nested").num("a", 1.5, 1).end_object();
    w.begin_array("list").str_item("x").str_item("y").end_array();
    w.begin_array("empty").end_array();
    w.begin_object("emptyobj").end_object();
    w.end_object();
    CHECK(w.balanced());
    const std::string out = w.take();
    CHECK(out.find("\"fps\": 29.97") != std::string::npos);
    CHECK(out.find("\"nan\": null") != std::string::npos);
    CHECK(out.find("\\\"") != std::string::npos);
    CHECK(out.find("\\u0001") != std::string::npos);
    CHECK(out.find("\"empty\": []") != std::string::npos);
    CHECK(out.find("\"emptyobj\": {}") != std::string::npos);
    // The script also feeds this file to a real JSON parser.
    std::ofstream("benchmark_report_test.sample.json", std::ios::binary) << out;
}

static void test_write_ok_and_replace() {
    const fs::path dir = fs::temp_directory_path() / "dusk_bench_test" / "nested";
    fs::remove_all(fs::temp_directory_path() / "dusk_bench_test");
    auto r = write_report_atomic(dir, "a.json", "{\"v\":1}\n");
    CHECK(r.ok);
    CHECK(slurp(dir / "a.json") == "{\"v\":1}\n");
    CHECK(!fs::exists(dir / "a.json.tmp"));
    r = write_report_atomic(dir, "a.json", "{\"v\":2}\n");  // replaces
    CHECK(r.ok);
    CHECK(slurp(dir / "a.json") == "{\"v\":2}\n");
    CHECK(!fs::exists(dir / "a.json.tmp"));
    fs::remove_all(fs::temp_directory_path() / "dusk_bench_test");
}

static void test_write_errors() {
    const fs::path root = fs::temp_directory_path() / "dusk_bench_test_err";
    fs::remove_all(root);
    fs::create_directories(root);
    // The "directory" is a file.
    std::ofstream(root / "blocker") << "x";
    auto r = write_report_atomic(root / "blocker", "a.json", "{}");
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    r = write_report_atomic(root / "blocker" / "sub", "a.json", "{}");
    CHECK(!r.ok);
    CHECK(r.error.find("cannot create directory") != std::string::npos);
    // A file name that cannot be opened (a directory is in the way of the tmp).
    fs::create_directories(root / "d" / "a.json.tmp");
    r = write_report_atomic(root / "d", "a.json", "{}");
    CHECK(!r.ok);
    CHECK(r.error.find("cannot open") != std::string::npos);
    CHECK(!fs::exists(root / "d" / "a.json"));
    fs::remove_all(root);
}

static void test_names_and_stamps() {
    CHECK(utc_stamp_for_file(0) == "19700101-000000");
    CHECK(utc_stamp_iso(0) == "1970-01-01T00:00:00Z");
    // 2026-09-30 14:25:01 UTC
    CHECK(utc_stamp_iso(1790778301) == "2026-09-30T14:25:01Z");
    CHECK(utc_stamp_for_file(1790778301) == "20260930-142501");
    // Leap day.
    CHECK(utc_stamp_iso(951782400) == "2000-02-29T00:00:00Z");
    CHECK(report_file_name("20260930-142501", "H28-c1-1e2574f7b7") ==
          "bench-20260930-142501-H28-c1-1e2574f7b7.json");
    CHECK(report_file_name("s", "a b/c:d") == "bench-s-a_b_c_d.json");
    CHECK(report_file_name("s", "") == "bench-s-unknown.json");
}

static void test_judder_index() {
    // H33: cadence irregularity, on frames in time order.
    // A clean 30 and a clean 60 are not judder.
    CHECK(summarize_frames(std::vector<float>(600, 33.333f)).judderPairs == 0);
    CHECK(near(summarize_frames(std::vector<float>(600, 16.667f)).judderPct, 0.0));
    // 45 fps on a 60 Hz output: 16.7 / 16.7 / 33.3 repeating. Two of every three consecutive
    // pairs change by 16.7 ms.
    std::vector<float> cadence45;
    for (int i = 0; i < 300; ++i) {
        cadence45.insert(cadence45.end(), {16.667f, 16.667f, 33.333f});
    }
    const auto s45 = summarize_frames(cadence45);
    CHECK(near(s45.judderPct, 100.0 * 2.0 / 3.0, 0.5));
    // The 33/50 stagger of an interpolated frame that overran its tick.
    std::vector<float> stagger;
    for (int i = 0; i < 100; ++i) {
        stagger.insert(stagger.end(), {33.333f, 50.0f});
    }
    CHECK(near(summarize_frames(stagger).judderPct, 100.0));
    // Order matters: the same intervals sorted show almost none.
    std::sort(stagger.begin(), stagger.end());
    CHECK(summarize_frames(stagger).judderPairs <= 1);
    // A step of exactly 8 ms is not judder; over it is.
    CHECK(summarize_frames({20.0f, 28.0f}).judderPairs == 0);
    CHECK(summarize_frames({20.0f, 28.5f}).judderPairs == 1);
    // Degenerate inputs.
    CHECK(summarize_frames({10.0f}).judderPairs == 0);
    CHECK(near(summarize_frames({10.0f}).judderPct, 0.0));
    CHECK(near(summarize_frames({}).judderPct, 0.0));
}

static void test_schema_version() {
    CHECK(kReportSchemaVersion == 4);
}

// ---- H35: FIFO, uniform and frame-wait windows -------------------------------------

namespace fr = dusk::fifo_report;

static fr::Snapshot snapshot_at(std::uint64_t base) {
    fr::Snapshot s;
    s.fifo.busy_ns = base * 1000000;  // base ms
    s.fifo.processed_bytes = base * 4096;
    s.fifo.process_calls = base * 10;
    s.fifo.publishes = base * 100;
    s.fifo.wake_requests = base * 90;
    s.fifo.worker_sleeps = base * 80;
    s.fifo.processing_mode = 2;
    for (int i = 0; i < AURORA_FIFO_DRAIN_SITE_COUNT; ++i) {
        s.fifo.drain_sites[i] = {base * (i + 1), base * i, base * 500000ull * (i + 1)};
        s.fifo.drains.calls += s.fifo.drain_sites[i].calls;
        s.fifo.drains.waits += s.fifo.drain_sites[i].waits;
        s.fifo.drains.wait_ns += s.fifo.drain_sites[i].wait_ns;
    }
    s.uniforms.full_builds = base * 1300;
    s.uniforms.repeat_hits = base * 300;
    s.uniforms.reuse_candidates = base * 200;
    s.uniforms.reuses = base * 150;
    s.uniforms.verification_checks = base * 50;
    s.waits.frame_slot_acquires = base;
    s.waits.frame_slot_waits = base / 2;
    s.waits.frame_slot_wait_ns = base * 250000;
    s.waits.staging_waits = base / 4;
    s.waits.staging_wait_ns = base * 100000;
    s.waits.render_items = base * 12;
    s.waits.render_work_ns = base * 9000000;
    return s;
}

static void test_fifo_window_deltas() {
    // The counters are cumulative from launch; a window is end minus start, never a reset.
    const auto a = snapshot_at(1000);
    const auto b = snapshot_at(1600);
    const auto w = fr::window(a, b, 600);
    CHECK(w.frames == 600);
    CHECK(near(w.busyMs, 600.0));
    CHECK(near(fr::per_frame(w.busyMs, w.frames), 1.0));
    CHECK(w.processedBytes == 600u * 4096u);
    CHECK(w.publishes == 60000u);
    CHECK(w.wakes == 54000u);
    CHECK(w.workerSleeps == 48000u);
    // Sites: calls, waits and wait time, and the total equals the sum of sites.
    std::uint64_t calls = 0;
    double waitMs = 0.0;
    for (int i = 0; i < AURORA_FIFO_DRAIN_SITE_COUNT; ++i) {
        const auto& site = w.sites[static_cast<std::size_t>(i)];
        CHECK(site.calls == 600u * static_cast<std::uint64_t>(i + 1));
        CHECK(site.waits == 600u * static_cast<std::uint64_t>(i));
        CHECK(near(site.waitMs, 0.5 * 600.0 * (i + 1)));
        calls += site.calls;
        waitMs += site.waitMs;
    }
    CHECK(calls == w.drains.calls);
    CHECK(near(waitMs, w.drains.waitMs));
    CHECK(w.fullBuilds == 600u * 1300u);
    CHECK(w.reuses == 600u * 150u);
    CHECK(near(w.frameSlotWaitMs, 150.0));
    CHECK(near(w.renderWorkMs, 5400.0));
    // Same reading twice: an empty window, all zeros.
    const auto empty = fr::window(b, b, 0);
    CHECK(empty.drains.calls == 0 && near(empty.busyMs, 0.0) && empty.frames == 0);
}

static void test_fifo_window_wraps() {
    // A counter that wrapped between the readings still gives the right difference.
    fr::Snapshot a;
    fr::Snapshot b;
    a.fifo.publishes = ~std::uint64_t{0} - 9;
    b.fifo.publishes = 30;
    a.fifo.drain_sites[AURORA_FIFO_DRAIN_DRAW_DONE].wait_ns = ~std::uint64_t{0};
    b.fifo.drain_sites[AURORA_FIFO_DRAIN_DRAW_DONE].wait_ns = 1999999;
    const auto w = fr::window(a, b, 10);
    CHECK(w.publishes == 40u);
    CHECK(near(w.sites[AURORA_FIFO_DRAIN_DRAW_DONE].waitMs, 2.0));
}

static void test_no_division_by_zero() {
    CHECK(near(fr::per_frame(123.0, 0), 0.0));
    CHECK(near(fr::per_frame(10.0, 4), 2.5));
    const auto w = fr::window(snapshot_at(5), snapshot_at(9), 0);
    JsonWriter j;
    j.begin_object();
    fr::write_fifo(j, w);
    fr::write_uniforms(j, w, "kcolors");
    fr::write_frame_waits(j, w);
    fr::write_derived(j, fr::ZoneTimes{}, w);
    fr::write_interp_frames(j, {}, 0.0);
    j.end_object();
    CHECK(j.balanced());
    const auto out = j.take();
    // JsonWriter writes NaN/inf as null; none may appear.
    CHECK(out.find("null") == std::string::npos);
    CHECK(out.find("\"worker_busy_ms_per_frame\": 0") != std::string::npos);
}

static void test_painter_net_unavailable_when_interpolating() {
    // Painter did not run (interpolation paints in Draw): no painter_net, whatever TickWait says.
    fr::ZoneTimes zones;
    zones.drawMs = 12.0;
    zones.haveDraw = true;
    zones.tickWaitMs = 4.0;
    zones.haveTickWait = true;
    JsonWriter j;
    j.begin_object();
    fr::write_derived(j, zones, fr::window(snapshot_at(1), snapshot_at(2), 10));
    j.end_object();
    const auto out = j.take();
    CHECK(out.find("\"painter_net_available\": false") != std::string::npos);
    CHECK(out.find("\"painter_net_ms_per_frame\": 0") != std::string::npos);
    CHECK(out.find("\"painter_net_clamped\": false") != std::string::npos);
    CHECK(out.find("\"draw_ms_per_frame\": 12") != std::string::npos);
}

static void test_net_metrics() {
    // Painter - TickWait and DrawRec - GXDrawDone wait; never negative.
    const auto p = fr::net_ms(19.5, 3.3);
    CHECK(near(p.ms, 16.2) && !p.clamped);
    const auto zero = fr::net_ms(5.0, 5.0);
    CHECK(near(zero.ms, 0.0) && !zero.clamped);
    const auto neg = fr::net_ms(4.0, 4.2);
    CHECK(near(neg.ms, 0.0) && neg.clamped);
}

static void test_names() {
    CHECK(std::string(fr::site_key(AURORA_FIFO_DRAIN_DRAW_DONE)) == "gx_draw_done");
    CHECK(std::string(fr::site_key(AURORA_FIFO_DRAIN_COMPANION)) == "companion_capture");
    CHECK(std::string(fr::site_key(99)) == "unknown");
    CHECK(std::string(fr::mode_name(AURORA_UNIFORM_REUSE_OFF)) == "off");
    CHECK(std::string(fr::mode_name(AURORA_UNIFORM_REUSE_VERIFY)) == "verify");
    CHECK(std::string(fr::mode_name(AURORA_UNIFORM_REUSE_ON)) == "on");
    CHECK(std::string(fr::processing_mode_name(2)) == "thread");
}

// A sample with every H35 object; the script parses it with a real JSON parser and
// checks the numbers below.
static void test_fifo_sample() {
    auto start = snapshot_at(100);
    auto end = snapshot_at(400);
    start.mode = AURORA_UNIFORM_REUSE_VERIFY;
    end.mode = AURORA_UNIFORM_REUSE_VERIFY;
    end.uniforms.verification_mismatches = 0;
    auto w = fr::window(start, end, 300);
    fr::ZoneTimes zones;
    zones.painterMs = 19.5;
    zones.tickWaitMs = 3.5;
    zones.drawRecMs = 10.0;
    zones.executeMs = 31.0;
    zones.actorsMs = 1.5;
    zones.havePainter = zones.haveTickWait = zones.haveDrawRec = true;
    dusk::interp_frames::Counters frames;
    for (int i = 0; i < 300; ++i) {
        frames.count_frame(true, i % 3 != 0, false, i % 2);
    }
    frames.count_predictive_reject();
    JsonWriter j;
    j.begin_object();
    j.str("schema", "dusklight-aleks-benchmark");
    j.integer("schema_version", kReportSchemaVersion);
    j.begin_object("interpolation");
    fr::write_interp_frames(j, frames, 10.0);
    j.end_object();
    fr::write_fifo(j, w);
    fr::write_uniforms(j, w, "kcolors");
    fr::write_frame_waits(j, w);
    fr::write_derived(j, zones, w);
    j.end_object();
    CHECK(j.balanced());
    std::ofstream("benchmark_report_test.fifo.json", std::ios::binary) << j.take();
    // drawrec_net = 10.0 - (gx_draw_done wait per frame): 300 * 0.5 ms / 300 frames = 0.5.
    CHECK(near(fr::per_frame(w.sites[AURORA_FIFO_DRAIN_DRAW_DONE].waitMs, w.frames), 0.5));
}

int main() {
    test_percentile();
    test_summary_steady();
    test_summary_with_stutter();
    test_summary_empty();
    test_judder_index();
    test_schema_version();
    test_fifo_window_deltas();
    test_fifo_window_wraps();
    test_no_division_by_zero();
    test_net_metrics();
    test_painter_net_unavailable_when_interpolating();
    test_names();
    test_fifo_sample();
    test_json();
    test_write_ok_and_replace();
    test_write_errors();
    test_names_and_stamps();
    if (failures == 0) {
        std::cout << "benchmark_report: all checks passed\n";
        return 0;
    }
    std::cerr << failures << " check(s) failed\n";
    return 1;
}
