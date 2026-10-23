#pragma once
// aleks2 (H35): the FIFO, uniform-reuse and frame-wait telemetry of a measurement
// window, as the benchmark (schema 4) and the Performance Tour report it. Game-free:
// it takes readings of Aurora's cumulative counters (aurora/tuning.h) at the start and
// the end of a window and turns the differences into totals and per-frame figures, so
// it is tested on a PC (platforms/switch/tools/test-benchmark-report.sh).
//
// Nothing here resets a counter: Aurora's are cumulative since launch and a window is
// always end minus start.
#include "dusk/benchmark_report.hpp"
#include "dusk/interp_frames.hpp"

#include <aurora/tuning.h>

#include <array>
#include <cstdint>

namespace dusk::fifo_report {

// One reading of everything, taken by the caller (read_now() on the console).
struct Snapshot {
    AuroraFifoStats fifo{};
    AuroraUniformStats uniforms{};
    AuroraFrameWaitStats waits{};
    AuroraUniformReuseMode mode = AURORA_UNIFORM_REUSE_OFF;
    AuroraRenderCpuStats render{};  // aleks2 (deko3d P1)
};

// Reads Aurora now. Inline so that the host tests, which never call it, need no Aurora.
inline Snapshot read_now() {
    Snapshot s;
    aurora_gx_fifo_stats(&s.fifo);
    aurora_gx_uniform_stats(&s.uniforms);
    aurora_gfx_frame_wait_stats(&s.waits);
    s.mode = aurora_gx_get_uniform_reuse_mode();
    aurora_gfx_render_cpu_stats(&s.render);
    return s;
}

struct DrainWindow {
    std::uint64_t calls = 0;
    std::uint64_t waits = 0;
    double waitMs = 0.0;
};

// A window: end minus start, and the frame count every per-frame figure divides by.
struct Window {
    std::uint64_t frames = 0;
    std::uint32_t processingMode = 0;

    double busyMs = 0.0;
    std::uint64_t processedBytes = 0;
    std::uint64_t processCalls = 0;
    std::uint64_t publishes = 0;
    std::uint64_t wakes = 0;
    std::uint64_t workerSleeps = 0;
    DrainWindow drains;
    std::array<DrainWindow, AURORA_FIFO_DRAIN_SITE_COUNT> sites{};

    std::uint64_t fullBuilds = 0;
    std::uint64_t repeatHits = 0;
    std::uint64_t reuseCandidates = 0;
    std::uint64_t reuses = 0;
    std::uint64_t verificationChecks = 0;
    std::uint64_t verificationMismatches = 0;
    bool disabledAfterMismatch = false;
    std::uint32_t lastMismatchSection = 0;
    std::uint32_t lastMismatchOffset = 0;
    AuroraUniformReuseMode modeAtStart = AURORA_UNIFORM_REUSE_OFF;
    AuroraUniformReuseMode modeAtEnd = AURORA_UNIFORM_REUSE_OFF;
    // Set by the caller when it saw the mode differ from the start on any frame.
    bool modeChangedDuringWindow = false;

    std::uint64_t frameSlotWaits = 0;
    double frameSlotWaitMs = 0.0;
    std::uint64_t stagingWaits = 0;
    double stagingWaitMs = 0.0;
    std::uint64_t renderItems = 0;
    double renderWorkMs = 0.0;
    // aleks2 (deko3d P1): the render worker's thread CPU, the draws it encoded, deko3d's GPU time
    bool deko3d = false;
    double renderCpuMs = 0.0;
    std::uint64_t encodedFrames = 0;
    std::uint64_t encodedDraws = 0;
    double dkGpuMs = 0.0;
    std::uint64_t dkGpuFrames = 0;
};

Window window(const Snapshot& start, const Snapshot& end, std::uint64_t frames);

// total / frames, or 0 for an empty window (never a division by zero).
double per_frame(double total, std::uint64_t frames);

// A zone time minus the wait it contains, never below zero. The two are measured by
// different clocks over slightly different spans (a profiler scope and Aurora's drain), so
// a small negative difference is noise, not a negative cost; `clamped` says it happened.
struct Net {
    double ms = 0.0;
    bool clamped = false;
};
Net net_ms(double zoneMs, double waitMs);

const char* site_key(int site);     // "gx_draw_done", "end_frame", ...
const char* mode_name(AuroraUniformReuseMode mode);  // "off", "verify", "on"
const char* processing_mode_name(std::uint32_t mode);

// The "fifo" object.
void write_fifo(benchmark::JsonWriter& j, const Window& w);
// The "uniforms" object. `sectionName` names last_mismatch_section (Aurora's
// aurora_gx_uniform_section_name on the console, a stand-in in the tests).
void write_uniforms(benchmark::JsonWriter& j, const Window& w, const char* sectionName);
// The "frame_waits" object (frame slot, staging, render worker).
void write_frame_waits(benchmark::JsonWriter& j, const Window& w);

// Zone figures per frame, as the profiler capture gives them. A zone is "had" when it was
// entered in the window: with interpolation (or a time scale) the main loop paints in Draw and
// Painter and TickWait do not run at all, though their profiler slots stay registered.
struct ZoneTimes {
    double painterMs = 0.0;
    double tickWaitMs = 0.0;
    double drawRecMs = 0.0;
    double executeMs = 0.0;
    double actorsMs = 0.0;
    double drawMs = 0.0;
    bool havePainter = false;
    bool haveTickWait = false;
    bool haveDrawRec = false;
    bool haveDraw = false;
};
// The "derived" object: painter_net_ms_per_frame = Painter - TickWait and
// drawrec_net_ms_per_frame = DrawRec - GXDrawDone drain wait, both per frame, never negative.
void write_derived(benchmark::JsonWriter& j, const ZoneTimes& zones, const Window& w);

// Frame kinds over a window (interp_frames.hpp), as members of the caller's
// "interpolation" object. `seconds` is the window's length for the two rates.
void write_interp_frames(benchmark::JsonWriter& j, const interp_frames::Counters& window, double seconds);

}  // namespace dusk::fifo_report
