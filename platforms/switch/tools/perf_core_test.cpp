// aleks2 (V1-c1): host test for src/dusk/perf_core.cpp -- lab options, per-frame attribution,
// window statistics, the "only when the GPU is the limit" DRS policy and the stress pattern.
#include "dusk/perf_core.hpp"
#include "dusk/perf_report.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace dusk::perf;

static int g_failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

constexpr float kBudget = 1000.f / 30.f;

static FrameSample frame(float interval, float main, float fifo, float gpu) {
    FrameSample f;
    f.intervalMs = interval;
    f.mainWorkMs = main;
    f.fifoBusyMs = fifo;
    f.renderWorkMs = interval;
    f.gpuMs = gpu;
    return f;
}

static std::vector<FrameSample> window(int n, int missed, float missInterval, float main, float fifo, float gpu,
                                       float metGpu = -2.f) {
    std::vector<FrameSample> v;
    for (int i = 0; i < n; ++i) {
        if (i < missed) {
            v.push_back(frame(missInterval, main, fifo, gpu));
        } else {
            v.push_back(frame(33.3f, std::min(main, 20.f), std::min(fifo, 20.f), metGpu < -1.f ? std::min(gpu, 25.f) : metGpu));
        }
    }
    return v;
}

static void test_options() {
    auto o = parse_lab_options("");
    CHECK(o.gpuTiming == GpuTiming::Auto && o.rtPool == 4 && o.drsSteps.size() == 3 && o.drsTargetFps == 30);
    CHECK(o.unknown.empty());
    o = parse_lab_options(
        "# comment\n[perf]\ngpu_timing = off\r\nrt_pool=0\ndrs_steps= 100, 90 ,80,70\ndrs_target_fps=20\n"
        "drs_log=on\ndrs_stress_transitions=200\ndrs_stress_interval_ms=500\ndrs_stress_pattern=ladder\nbogus=1\n");
    CHECK(o.gpuTiming == GpuTiming::Off);
    CHECK(o.rtPool == 0);
    CHECK((o.drsSteps == std::vector<int>{100, 90, 80, 70}));
    CHECK(o.drsTargetFps == 20);
    CHECK(o.drsVerbose);
    CHECK(o.stressTransitions == 200 && o.stressIntervalMs == 500 && o.stressLadder);
    CHECK(o.unknown == "bogus=1");
    // Bad values are refused and named, defaults kept.
    o = parse_lab_options("rt_pool=9\ndrs_steps=100,100\ndrs_steps=100,40\ndrs_steps=85,100\ndrs_target_fps=5\n"
                          "drs_stress_interval_ms=10\ngpu_timing=maybe\n");
    CHECK(o.rtPool == 4);
    CHECK((o.drsSteps == std::vector<int>{100, 85, 75}));
    CHECK(o.drsTargetFps == 30);
    CHECK(o.stressIntervalMs == 2000);
    CHECK(o.gpuTiming == GpuTiming::Auto);
    CHECK(o.unknown.find("rt_pool=9") != std::string::npos);
    CHECK(o.unknown.find("gpu_timing=maybe") != std::string::npos);
    CHECK(!describe(parse_lab_options("")).empty());
    // hud_fast
    CHECK(parse_lab_options("").hudFast);  // rc9: on by default
    CHECK(!parse_lab_options("hud_fast=0\n").hudFast);
    CHECK(parse_lab_options("").minimapPan);  // rc10: on by default
    CHECK(!parse_lab_options("minimap_pan=0\n").minimapPan);
    CHECK(parse_lab_options("minimap_pan=1\n").minimapPan);
    // rc10: comments only at the start of a line or after a blank; tour_matrix keeps its ';'
    o = parse_lab_options("tour_matrix = hud_fast:1,0; swap_interval:2,1; minimap_pan:0,1\n");
    CHECK(o.tourMatrix == "hud_fast:1,0; swap_interval:2,1; minimap_pan:0,1");
    CHECK(o.unknown.empty());
    CHECK(!parse_lab_options("hud_fast = 0 ; was 1\n").hudFast);
    CHECK(!parse_lab_options("hud_fast = 0 # was 1\n").hudFast);
    CHECK(parse_lab_options("; hud_fast = 0\n").hudFast);
    CHECK(parse_lab_options("hud_fast=0;x\n").unknown == "hud_fast=0;x");
    CHECK(!parse_lab_options("").hudCullAlpha0);
    CHECK(parse_lab_options("hud_cull_alpha0=1\n").hudCullAlpha0);
    CHECK(parse_lab_options("").exitMode == 0);
    CHECK(parse_lab_options("exit_mode=process\n").exitMode == 2);
    CHECK(parse_lab_options("exit_mode=loader\n").exitMode == 1);
    CHECK(parse_lab_options("exit_mode=maybe\n").exitMode == 0);
    CHECK(parse_lab_options("swap_interval=adaptive\n").swapInterval == 3);
    CHECK(parse_lab_options("swap_interval=3\n").swapInterval == 3);
    CHECK(describe(parse_lab_options("exit_mode=process\n")).find("exit_mode=process") != std::string::npos);
    // rc12: interpolation floor and the black-ground debug switches
    CHECK(parse_lab_options("").interpFloorFps == 0);  // V1-rc16: off by default (Capped 60 = the Unlimited rule)
    CHECK(parse_lab_options("interp_floor_fps=0\n").interpFloorFps == 0);
    CHECK(parse_lab_options("").hookFast && !parse_lab_options("hook_fast=0\n").hookFast);  // V1-rc16
    CHECK(describe(parse_lab_options("hook_fast=0\n")).find("hook_fast=0") != std::string::npos);
    CHECK(parse_lab_options("hook_fast=verify\n").hookVerify && parse_lab_options("hook_fast=verify\n").hookFast);
    CHECK(describe(parse_lab_options("hook_fast=verify\n")).find("hook_fast=2") != std::string::npos);
    CHECK(!parse_lab_options("").vsyncRelaxed && parse_lab_options("vsync_relaxed=1\n").vsyncRelaxed);  // V1-rc16
    CHECK(parse_lab_options("interp_floor_fps=50\n").interpFloorFps == 50);
    CHECK(parse_lab_options("interp_floor_fps=20\n").interpFloorFps == 0);  // refused, default kept
    CHECK(parse_lab_options("").debugFlags == 0 && !parse_lab_options("").dbgNoRealShadow);
    CHECK(parse_lab_options("dbg_fog_off=1\ndbg_no_aniso=on\n").debugFlags == 5);
    CHECK(parse_lab_options("dbg_base_mip=1\n").debugFlags == 2);
    CHECK(parse_lab_options("dbg_no_real_shadow=1\n").dbgNoRealShadow);
    // V1-rc19: mod_lib_cache, on unless it says 0.
    CHECK(parse_lab_options("").modLibCache);
    CHECK(!parse_lab_options("mod_lib_cache = 0\n").modLibCache);
    CHECK(describe(parse_lab_options("mod_lib_cache=0\n")).find("mod_lib_cache=0") != std::string::npos);
    CHECK(parse_lab_options("dbg_copy_shape_match=1\n").debugFlags == 8);
    CHECK(parse_lab_options("wii_record=1\n").wiiRecord && !parse_lab_options("").wiiRecord);
    CHECK(!parse_lab_options("").warmupKeepAll && parse_lab_options("warmup_keep_all=1\n").warmupKeepAll);
    CHECK(!parse_lab_options("").profileMain && parse_lab_options("profile_main=1\n").profileMain);
    CHECK(parse_lab_options("").profileSeconds == 30 && parse_lab_options("profile_seconds=60\n").profileSeconds == 60);
    CHECK(parse_lab_options("profile_seconds=1\n").profileSeconds == 30);  // refused, default kept
    CHECK(parse_lab_options("").profileDelayS == 90 && parse_lab_options("profile_delay_s=0\n").profileDelayS == 0);
    CHECK(describe(parse_lab_options("minimap_pan=on\n")).find("minimap_pan=1") != std::string::npos);
    CHECK(parse_lab_options("hud_fast=1\n").hudFast);
    CHECK(describe(parse_lab_options("hud_fast=on\n")).find("hud_fast=1") != std::string::npos);
    // FIFO-opt fast paths
    CHECK(parse_lab_options("").fifoFast == 0);
    CHECK(parse_lab_options("fifo_fast=all\n").fifoFast == 31);
    CHECK(parse_lab_options("fifo_fast=pipe,pop\n").fifoFast == 24);
    CHECK(parse_lab_options("fifo_fast=31\n").fifoFast == 31);
    CHECK(parse_lab_options("fifo_fast=32\n").fifoFast == 0);
    CHECK(parse_lab_options("fifo_fast = bind, urep\n").fifoFast == 5);
    CHECK(parse_lab_options("fifo_fast=idx\n").fifoFast == 2);
    CHECK(parse_lab_options("fifo_fast=3\n").fifoFast == 3);
    CHECK(parse_lab_options("fifo_fast=all\nfifo_fast=off\n").fifoFast == 0);
    o = parse_lab_options("fifo_fast=bind,turbo\nfifo_fast=99\n");
    CHECK(o.fifoFast == 0 && !o.unknown.empty());
    CHECK(describe(parse_lab_options("fifo_fast=idx,bind\n")).find("fifo_fast=bind,idx") != std::string::npos);
    // V1-rc16: renderer A/B switches go to the environment Aurora reads at start-up; none set without the file
    unsetenv("COS_DK_ZCULL");
    unsetenv("COS_DK_COMPRESSION");
    parse_lab_options("");
    CHECK(std::getenv("COS_DK_ZCULL") == nullptr && std::getenv("COS_DK_COMPRESSION") == nullptr);
    o = parse_lab_options("dk_zcull = 1\ndk_compression = off\ndk_cached_upload = on\n");
    CHECK(o.unknown.empty());
    CHECK(std::string(std::getenv("COS_DK_ZCULL")) == "1" && std::string(std::getenv("COS_DK_COMPRESSION")) == "0");
    CHECK(std::string(std::getenv("COS_DK_CACHED_UPLOAD")) == "1");
    CHECK(!parse_lab_options("dk_zcull = maybe\n").unknown.empty());
}

static void test_classify() {
    CHECK(classify(frame(33.3f, 30, 25, 30), kBudget) == Bound::Met);
    CHECK(classify(frame(36.0f, 30, 25, 30), kBudget) == Bound::Met);  // within the 10 % slack
    CHECK(classify(frame(50.0f, 15, 12, 31), kBudget) == Bound::Gpu);
    CHECK(classify(frame(50.0f, 32, 12, 20), kBudget) == Bound::CpuMain);
    CHECK(classify(frame(50.0f, 15, 31, 20), kBudget) == Bound::CpuRender);
    // GPU high but the game thread higher: the CPU explains it.
    CHECK(classify(frame(50.0f, 34, 12, 31), kBudget) == Bound::CpuMain);
    // No GPU figure, CPUs idle: unknown, never "GPU".
    CHECK(classify(frame(50.0f, 10, 10, -1), kBudget) == Bound::Unknown);
    CHECK(classify(frame(120.0f, 10, 10, 10), kBudget) == Bound::Unknown);
    CHECK(std::string(bound_name(Bound::CpuRender)) == "cpu_render");
}

static void test_summarize() {
    auto w = summarize(window(30, 6, 50.f, 15, 12, 31), kBudget);
    CHECK(w.frames == 30 && w.missed == 6);
    CHECK(w.byBound[static_cast<int>(Bound::Gpu)] == 6);
    CHECK(w.gpuFrames == 30 && w.hasGpu());
    CHECK(w.missedPct() > 19.9 && w.missedPct() < 20.1);
    CHECK(w.gpuMissShare() > 0.99);
    CHECK(w.gpuUtil > 0.5 && w.gpuUtil < 1.0);
    auto none = summarize(window(30, 6, 50.f, 15, 12, -1, -1), kBudget);
    CHECK(!none.hasGpu() && none.gpuUtil == 0.0);
    CHECK(percentile({}, 50) == 0.f);
    CHECK(percentile({3, 1, 2}, 50) == 2.f);
    CHECK(percentile({3, 1, 2, 4}, 100) == 4.f);
}

// GPU-bound windows step down after the streak, never without GPU figures, and a drop that
// buys nothing (the CPU is the limit) is undone.
static void test_policy_gpu_bound() {
    DrsPolicy p({100, 85, 75});
    const auto gpuBound = summarize(window(30, 6, 50.f, 15, 12, 31), kBudget);
    double t = 10.0;
    CHECK(p.evaluate(gpuBound, false, t).action == Action::Hold);
    CHECK(p.evaluate(gpuBound, false, t + 1).action == Action::Hold);
    auto d = p.evaluate(gpuBound, false, t + 2);
    CHECK(d.action == Action::Down && d.level == 1 && p.percent() == 85);
    // Still GPU-bound but within the dwell: hold, and the verify windows keep the drop.
    CHECK(p.evaluate(gpuBound, false, t + 3).action == Action::Hold);
    CHECK(p.evaluate(gpuBound, false, t + 4).action == Action::Hold);
    d = p.evaluate(gpuBound, false, t + 6);
    CHECK(d.action == Action::Down && p.percent() == 75);
    // At the floor: holds.
    for (int i = 0; i < 10; ++i) {
        CHECK(p.evaluate(gpuBound, false, t + 7 + i).action == Action::Hold);
    }
    CHECK(p.level() == 2);
}

static void test_policy_cpu_bound() {
    DrsPolicy p;
    const auto cpuBound = summarize(window(30, 9, 50.f, 32, 12, 20), kBudget);
    for (int i = 0; i < 20; ++i) {
        CHECK(p.evaluate(cpuBound, false, i).action == Action::Hold);
    }
    CHECK(p.level() == 0);
    CHECK(p.state().find("not the GPU") != std::string::npos);
    // No GPU timing at all: never moves, says why.
    const auto noGpu = summarize(window(30, 9, 50.f, 15, 12, -1, -1), kBudget);
    for (int i = 0; i < 20; ++i) {
        CHECK(p.evaluate(noGpu, false, 100 + i).action == Action::Hold);
    }
    CHECK(p.level() == 0);
    CHECK(p.state().find("no GPU timing") != std::string::npos);
}

static void test_policy_drop_undone() {
    DrsPolicy p;
    const auto gpuBound = summarize(window(30, 6, 50.f, 15, 12, 31), kBudget);
    const auto cpuAfter = summarize(window(30, 6, 50.f, 32, 12, 22), kBudget);
    p.evaluate(gpuBound, false, 0);
    p.evaluate(gpuBound, false, 1);
    CHECK(p.evaluate(gpuBound, false, 2).action == Action::Down);
    auto d = p.evaluate(cpuAfter, false, 3);
    CHECK(d.action == Action::Up && p.level() == 0);
    // Backed off: GPU-bound again soon after does not drop until the back-off ends.
    for (int i = 0; i < 10; ++i) {
        CHECK(p.evaluate(gpuBound, false, 4 + i).action == Action::Hold);
    }
    bool dropped = false;
    for (int i = 0; i < 30 && !dropped; ++i) {
        dropped = p.evaluate(gpuBound, false, 34 + i).action == Action::Down;
    }
    CHECK(dropped);
}

static void test_policy_climb() {
    DrsPolicy p;
    const auto gpuBound = summarize(window(30, 6, 50.f, 15, 12, 31), kBudget);
    const auto light = summarize(window(30, 0, 50.f, 10, 8, 12, 12.f), kBudget);
    p.evaluate(gpuBound, false, 0);
    p.evaluate(gpuBound, false, 1);
    CHECK(p.evaluate(gpuBound, false, 2).action == Action::Down);
    int climbs = 0;
    double t = 3;
    for (; t < 40 && p.level() > 0; t += 1) {
        if (p.evaluate(light, false, t).action == Action::Up) {
            ++climbs;
        }
    }
    CHECK(climbs == 1 && p.level() == 0);
    // A failed climb goes back down and waits longer before the next one.
    p.reset();
    p.evaluate(gpuBound, false, 100);
    p.evaluate(gpuBound, false, 101);
    CHECK(p.evaluate(gpuBound, false, 102).action == Action::Down);
    t = 103;
    while (p.level() > 0 && t < 200) {
        p.evaluate(light, false, t);
        t += 1;
    }
    CHECK(p.level() == 0);
    const double climbedAt = t - 1;
    auto d = p.evaluate(gpuBound, false, t);
    CHECK(d.action == Action::Down && p.level() == 1);
    (void)climbedAt;
    // Next climb is held off for the back-off (10 s at least).
    int early = 0;
    for (int i = 1; i <= 9; ++i) {
        if (p.evaluate(light, false, t + i).action == Action::Up) {
            ++early;
        }
    }
    CHECK(early == 0);
}

static void test_policy_tainted() {
    DrsPolicy p;
    const auto gpuBound = summarize(window(30, 6, 50.f, 15, 12, 31), kBudget);
    p.evaluate(gpuBound, false, 0);
    p.evaluate(gpuBound, false, 1);
    CHECK(p.evaluate(gpuBound, true, 2).action == Action::Hold);  // a load resets the streak
    CHECK(p.evaluate(gpuBound, false, 3).action == Action::Hold);
    CHECK(p.evaluate(gpuBound, false, 4).action == Action::Hold);
    CHECK(p.evaluate(gpuBound, false, 5).action == Action::Down);
}

static void test_rows_and_stress() {
    CHECK(rows_for_percent(900, 100) == 900);
    CHECK(rows_for_percent(900, 85) == 764 || rows_for_percent(900, 85) == 766);
    CHECK(rows_for_percent(900, 75) == 674 || rows_for_percent(900, 75) == 676);
    CHECK(rows_for_percent(720, 75) == 540);
    CHECK(rows_for_percent(540, 50) == kMinRows);
    for (int c : {540, 720, 900, 1080}) {
        for (int pct : {50, 75, 85, 100}) {
            CHECK(rows_for_percent(c, pct) % 2 == 0);
        }
    }
    LabOptions o;
    auto rows = stress_rows(900, o, 8);
    CHECK(rows.size() == 8);
    CHECK((rows == std::vector<int>{rows_for_percent(900, 85), rows_for_percent(900, 75), rows_for_percent(900, 85),
                                    900, rows_for_percent(900, 85), rows_for_percent(900, 75),
                                    rows_for_percent(900, 85), 900}));
    o.stressLadder = true;
    rows = stress_rows(900, o, 6);
    CHECK((rows == std::vector<int>{720, 540, 720, 900, 720, 540}));
    rows = stress_rows(720, o, 4);
    CHECK((rows == std::vector<int>{540, 720, 540, 720}));
    rows = stress_rows(1080, o, 4);
    CHECK((rows == std::vector<int>{900, 720, 540, 720}));
    o.drsSteps = {100, 99};
    o.stressLadder = false;
    CHECK(stress_rows(360, o, 4).empty() || stress_rows(360, o, 4).size() == 4);
}

static void test_report() {
    auto frames = window(60, 12, 50.f, 15, 12, 31);
    frames[3].gpuMs = -1.f;
    GpuInfo gpu;
    gpu.supported = gpu.enabled = gpu.calibrated = true;
    dusk::benchmark::JsonWriter j;
    j.begin_object();
    write_telemetry(j, frames, kBudget, gpu);
    j.begin_object("no_gpu");
    write_telemetry(j, window(30, 9, 50.f, 32, 12, -1, -1), kBudget, GpuInfo{});
    j.end_object();
    j.end_object();
    CHECK(j.balanced());
    const std::string json = j.take();
    CHECK(json.find("\"verdict\": \"gpu\"") != std::string::npos);
    CHECK(json.find("\"verdict\": \"cpu_main\"") != std::string::npos);
    CHECK(json.find("\"available\": false") != std::string::npos);
    if (FILE* f = std::fopen("perf_core_test.telemetry.json", "wb")) {
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
    }
    std::string csv = csv_header();
    append_csv(csv, frames, kBudget, 2);
    int lines = 0;
    for (char c : csv) {
        lines += c == '\n';
    }
    CHECK(lines == 61);
    CHECK(csv.find("\n2,3,") != std::string::npos);
    CHECK(csv.find(",,0,") != std::string::npos);  // frame 3 has no GPU figure
    CHECK(csv_name_for("bench-20261016-120000-V1-c1.json") == "bench-20261016-120000-V1-c1.csv");
    CHECK(std::string(verdict(summarize(window(60, 1, 50.f, 15, 12, 31), kBudget))) == "met");
}

// V1-rc10: swap_interval adaptive -- 2 once the frame's work stops fitting 60, back to 1 only well under it.
static void test_swap_governor() {
    SwapGovernor g;
    FrameSample light;
    light.mainWorkMs = 9.f;
    light.fifoBusyMs = 8.f;
    light.gpuMs = 11.f;
    FrameSample heavy = light;
    heavy.fifoBusyMs = 19.f;  // Hyrule Field: the FIFO worker
    for (int i = 0; i < 400; ++i) {
        CHECK(g.update(light) == 1);
    }
    CHECK(g.changes() == 0);
    // a few slow frames (under 10 %) do not switch
    for (int i = 0; i < 400; ++i) {
        g.update(i % 20 == 0 ? heavy : light);
    }
    CHECK(g.current() == 1);
    // heavy for more than a tenth of the window: to 2 as soon as a tenth of it is heavy
    unsigned at = 0;
    for (unsigned i = 1; i <= 400 && at == 0; ++i) {
        if (g.update(heavy) == 2) {
            at = i;
        }
    }
    CHECK(at >= 1 && at <= SwapGovernor::kWindow / 10);  // the mixed stretch left a few heavy frames in it
    CHECK(g.changes() == 1);
    // at 2: 14 ms of work is under 16 but not under 13.5 -- stays at 2 (hysteresis)
    FrameSample mid = light;
    mid.mainWorkMs = 14.f;
    for (int i = 0; i < 600; ++i) {
        CHECK(g.update(mid) == 2);
    }
    // light again: back to 1 once nine tenths of the window are light (the dwell has long passed)
    unsigned back = 0;
    for (unsigned i = 1; i <= 400 && back == 0; ++i) {
        if (g.update(light) == 1) {
            back = i;
        }
    }
    CHECK(back >= SwapGovernor::kWindow * 8 / 10 && back <= SwapGovernor::kWindow);
    CHECK(g.changes() == 2);
    // a frame without a GPU figure counts its CPU sides only
    FrameSample noGpu = light;
    noGpu.gpuMs = -1.f;
    SwapGovernor g2;
    for (int i = 0; i < 400; ++i) {
        CHECK(g2.update(noGpu) == 1);
    }
    g2.reset(2);
    CHECK(g2.current() == 2);
}

int main() {
    test_swap_governor();
    test_report();
    test_options();
    test_classify();
    test_summarize();
    test_policy_gpu_bound();
    test_policy_cpu_bound();
    test_policy_drop_undone();
    test_policy_climb();
    test_policy_tainted();
    test_rows_and_stress();
    if (g_failures == 0) {
        std::printf("perf_core_test: all passed\n");
        return 0;
    }
    std::printf("perf_core_test: %d failure(s)\n", g_failures);
    return 1;
}
