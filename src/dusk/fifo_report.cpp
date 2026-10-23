#include "dusk/fifo_report.hpp"

#include <algorithm>

namespace dusk::fifo_report {
namespace {

double ns_to_ms(std::uint64_t ns) {
    return static_cast<double>(ns) / 1e6;
}

DrainWindow drain_delta(const AuroraFifoDrainStats& a, const AuroraFifoDrainStats& b) {
    return {b.calls - a.calls, b.waits - a.waits, ns_to_ms(b.wait_ns - a.wait_ns)};
}

void write_drain(benchmark::JsonWriter& j, const DrainWindow& d, std::uint64_t frames) {
    j.integer("drain_calls", static_cast<std::int64_t>(d.calls));
    j.num("drain_calls_per_frame", per_frame(static_cast<double>(d.calls), frames), 2);
    j.integer("drain_waits", static_cast<std::int64_t>(d.waits));
    j.num("drain_wait_ms", d.waitMs, 3);
    j.num("drain_wait_ms_per_frame", per_frame(d.waitMs, frames), 3);
}

}  // namespace

double per_frame(double total, std::uint64_t frames) {
    return frames > 0 ? total / static_cast<double>(frames) : 0.0;
}

Net net_ms(double zoneMs, double waitMs) {
    const double diff = zoneMs - waitMs;
    return diff < 0.0 ? Net{0.0, true} : Net{diff, false};
}

Window window(const Snapshot& start, const Snapshot& end, std::uint64_t frames) {
    Window w;
    w.frames = frames;
    w.processingMode = end.fifo.processing_mode;
    w.busyMs = ns_to_ms(end.fifo.busy_ns - start.fifo.busy_ns);
    w.processedBytes = end.fifo.processed_bytes - start.fifo.processed_bytes;
    w.processCalls = end.fifo.process_calls - start.fifo.process_calls;
    w.publishes = end.fifo.publishes - start.fifo.publishes;
    w.wakes = end.fifo.wake_requests - start.fifo.wake_requests;
    w.workerSleeps = end.fifo.worker_sleeps - start.fifo.worker_sleeps;
    w.drains = drain_delta(start.fifo.drains, end.fifo.drains);
    for (int i = 0; i < AURORA_FIFO_DRAIN_SITE_COUNT; ++i) {
        w.sites[static_cast<std::size_t>(i)] = drain_delta(start.fifo.drain_sites[i], end.fifo.drain_sites[i]);
    }

    w.fullBuilds = end.uniforms.full_builds - start.uniforms.full_builds;
    w.repeatHits = end.uniforms.repeat_hits - start.uniforms.repeat_hits;
    w.reuseCandidates = end.uniforms.reuse_candidates - start.uniforms.reuse_candidates;
    w.reuses = end.uniforms.reuses - start.uniforms.reuses;
    w.verificationChecks = end.uniforms.verification_checks - start.uniforms.verification_checks;
    w.verificationMismatches = end.uniforms.verification_mismatches - start.uniforms.verification_mismatches;
    w.disabledAfterMismatch = end.uniforms.disabled_after_mismatch != 0;
    w.lastMismatchSection = end.uniforms.last_mismatch_section;
    w.lastMismatchOffset = end.uniforms.last_mismatch_offset;
    w.modeAtStart = start.mode;
    w.modeAtEnd = end.mode;
    w.modeChangedDuringWindow = start.mode != end.mode;

    w.frameSlotWaits = end.waits.frame_slot_waits - start.waits.frame_slot_waits;
    w.frameSlotWaitMs = ns_to_ms(end.waits.frame_slot_wait_ns - start.waits.frame_slot_wait_ns);
    w.stagingWaits = end.waits.staging_waits - start.waits.staging_waits;
    w.stagingWaitMs = ns_to_ms(end.waits.staging_wait_ns - start.waits.staging_wait_ns);
    w.renderItems = end.waits.render_items - start.waits.render_items;
    w.renderWorkMs = ns_to_ms(end.waits.render_work_ns - start.waits.render_work_ns);
    w.deko3d = end.render.deko3d;
    w.renderCpuMs = ns_to_ms(end.render.thread_cpu_ns - start.render.thread_cpu_ns);
    w.encodedFrames = end.render.frames - start.render.frames;
    w.encodedDraws = end.render.draws - start.render.draws;
    w.dkGpuMs = ns_to_ms(end.render.gpu_ns - start.render.gpu_ns);
    w.dkGpuFrames = end.render.gpu_frames - start.render.gpu_frames;
    return w;
}

const char* site_key(int site) {
    switch (site) {
    case AURORA_FIFO_DRAIN_DRAW_DONE: return "gx_draw_done";
    case AURORA_FIFO_DRAIN_END_FRAME: return "end_frame";
    case AURORA_FIFO_DRAIN_COMPANION: return "companion_capture";
    case AURORA_FIFO_DRAIN_EXPLICIT: return "explicit";
    case AURORA_FIFO_DRAIN_AUX_PASS: return "aux_pass";
    case AURORA_FIFO_DRAIN_OTHER: return "other";
    default: return "unknown";
    }
}

const char* mode_name(AuroraUniformReuseMode mode) {
    switch (mode) {
    case AURORA_UNIFORM_REUSE_OFF: return "off";
    case AURORA_UNIFORM_REUSE_VERIFY: return "verify";
    case AURORA_UNIFORM_REUSE_ON: return "on";
    }
    return "unknown";
}

const char* processing_mode_name(std::uint32_t mode) {
    switch (mode) {
    case 0: return "drain";
    case 1: return "inline";
    case 2: return "thread";
    default: return "unknown";
    }
}

void write_fifo(benchmark::JsonWriter& j, const Window& w) {
    const auto f = w.frames;
    j.begin_object("fifo");
    j.str("note", "cumulative Aurora counters, end minus start of the window; per_frame divides by the frames "
                  "profiled in the window. worker_busy is command translation on the FIFO thread; drain_wait is "
                  "the game thread blocked until that thread caught up, by the site that asked");
    j.str("processing_mode", processing_mode_name(w.processingMode));
    j.integer("frames", static_cast<std::int64_t>(f));
    j.num("worker_busy_ms_total", w.busyMs, 3);
    j.num("worker_busy_ms_per_frame", per_frame(w.busyMs, f), 3);
    j.integer("processed_bytes_total", static_cast<std::int64_t>(w.processedBytes));
    j.num("processed_bytes_per_frame", per_frame(static_cast<double>(w.processedBytes), f), 1);
    j.integer("process_calls_total", static_cast<std::int64_t>(w.processCalls));
    j.num("process_calls_per_frame", per_frame(static_cast<double>(w.processCalls), f), 2);
    j.integer("publishes_total", static_cast<std::int64_t>(w.publishes));
    j.num("publishes_per_frame", per_frame(static_cast<double>(w.publishes), f), 2);
    j.integer("worker_wakes_total", static_cast<std::int64_t>(w.wakes));
    j.num("worker_wakes_per_frame", per_frame(static_cast<double>(w.wakes), f), 2);
    j.integer("worker_sleeps_total", static_cast<std::int64_t>(w.workerSleeps));
    j.num("worker_sleeps_per_frame", per_frame(static_cast<double>(w.workerSleeps), f), 2);
    j.integer("drain_calls_total", static_cast<std::int64_t>(w.drains.calls));
    j.num("drain_calls_per_frame", per_frame(static_cast<double>(w.drains.calls), f), 2);
    j.integer("drain_waits_total", static_cast<std::int64_t>(w.drains.waits));
    j.num("drain_wait_ms_total", w.drains.waitMs, 3);
    j.num("drain_wait_ms_per_frame", per_frame(w.drains.waitMs, f), 3);
    j.begin_object("drains_by_site");
    for (int i = 0; i < AURORA_FIFO_DRAIN_SITE_COUNT; ++i) {
        j.begin_object(site_key(i));
        write_drain(j, w.sites[static_cast<std::size_t>(i)], f);
        j.end_object();
    }
    j.end_object();
    j.end_object();
}

void write_uniforms(benchmark::JsonWriter& j, const Window& w, const char* sectionName) {
    const auto f = w.frames;
    j.begin_object("uniforms");
    j.str("note", "uniform block reuse when only the pipeline changed (Aurora, H35). off = the H34 path; verify = "
                  "decided and compared byte for byte, the full block always drawn; on = applied. A mismatch "
                  "disables reuse for the rest of the run");
    j.str("mode", mode_name(w.modeAtEnd));
    j.str("mode_at_start", mode_name(w.modeAtStart));
    j.boolean("mode_changed_during_window", w.modeChangedDuringWindow);
    j.integer("full_builds", static_cast<std::int64_t>(w.fullBuilds));
    j.num("full_builds_per_frame", per_frame(static_cast<double>(w.fullBuilds), f), 1);
    j.integer("repeat_hits", static_cast<std::int64_t>(w.repeatHits));
    j.num("repeat_hits_per_frame", per_frame(static_cast<double>(w.repeatHits), f), 1);
    j.integer("reuse_candidates", static_cast<std::int64_t>(w.reuseCandidates));
    j.num("reuse_candidates_per_frame", per_frame(static_cast<double>(w.reuseCandidates), f), 1);
    j.integer("reuses", static_cast<std::int64_t>(w.reuses));
    j.num("reuses_per_frame", per_frame(static_cast<double>(w.reuses), f), 1);
    j.integer("verification_checks", static_cast<std::int64_t>(w.verificationChecks));
    j.integer("verification_mismatches", static_cast<std::int64_t>(w.verificationMismatches));
    j.boolean("disabled_after_mismatch", w.disabledAfterMismatch);
    if (w.disabledAfterMismatch) {
        j.str("last_mismatch_section", sectionName != nullptr ? sectionName : "unknown");
        j.integer("last_mismatch_offset", w.lastMismatchOffset);
    }
    j.end_object();
}

void write_frame_waits(benchmark::JsonWriter& j, const Window& w) {
    const auto f = w.frames;
    j.begin_object("frame_waits");
    j.str("note", "frame_slot = aurora_begin_frame waiting for a slot the render worker still holds; staging = "
                  "waiting for a mapped staging buffer; render_work = the render worker running its queue "
                  "(encode, submit, present), which can block on the display, so it is not pure CPU");
    j.integer("frame_slot_waits", static_cast<std::int64_t>(w.frameSlotWaits));
    j.num("frame_slot_wait_ms_per_frame", per_frame(w.frameSlotWaitMs, f), 3);
    j.integer("staging_waits", static_cast<std::int64_t>(w.stagingWaits));
    j.num("staging_wait_ms_per_frame", per_frame(w.stagingWaitMs, f), 3);
    j.integer("render_items", static_cast<std::int64_t>(w.renderItems));
    j.num("render_work_ms_per_frame", per_frame(w.renderWorkMs, f), 3);
    j.end_object();
    // aleks2 (deko3d P1): the same zone with either backend (Settings > Graphics Backend)
    j.begin_object("render_cpu");
    j.str("note", "render worker THREAD time (kernel tick count of that thread), not wall time; draws = draw "
                  "commands encoded (drawCallCount summed); us_per_draw = thread time / draws. gpu_ms_per_frame: "
                  "deko3d's timestamps (deko3d only; with Dawn see gpu_timing)");
    j.str("backend", w.deko3d ? "deko3d" : "dawn_nvk");
    j.integer("encoded_frames", static_cast<std::int64_t>(w.encodedFrames));
    j.num("thread_cpu_ms_per_frame", per_frame(w.renderCpuMs, w.encodedFrames), 3);
    j.num("draws_per_frame", per_frame(static_cast<double>(w.encodedDraws), w.encodedFrames), 1);
    j.num("us_per_draw", w.encodedDraws > 0 ? w.renderCpuMs * 1000.0 / static_cast<double>(w.encodedDraws) : 0.0, 3);
    if (w.deko3d) {
        j.num("gpu_ms_per_frame", per_frame(w.dkGpuMs, w.dkGpuFrames), 3);
    }
    j.end_object();
}

void write_derived(benchmark::JsonWriter& j, const ZoneTimes& zones, const Window& w) {
    const double drawDoneWait = per_frame(w.sites[AURORA_FIFO_DRAIN_DRAW_DONE].waitMs, w.frames);
    const Net painter = net_ms(zones.painterMs, zones.tickWaitMs);
    const Net drawRec = net_ms(zones.drawRecMs, drawDoneWait);
    const bool painterAvailable = zones.havePainter;
    j.begin_object("derived");
    j.str("note", "per frame. painter_net = Painter - TickWait (the game's tick limiter sleeps inside Painter); "
                  "drawrec_net = DrawRec - the GXDrawDone drain wait (DrawRec ends in JFWDisplay::endFrame, which "
                  "blocks in GXDrawDone until the FIFO thread caught up). Execute holds Painter, Actors and "
                  "DrawRec as siblings. With interpolation (or a time scale) painting moves to the main loop's "
                  "Draw zone and Painter does not run: painter_net_available is then false. The zones and the "
                  "wait come from different clocks over slightly different spans: a difference below zero is "
                  "written as 0 and flagged as clamped. The GXDrawDone wait counts every GXDrawDone in the "
                  "window, nearly all of them inside DrawRec");
    j.num("execute_ms_per_frame", zones.executeMs, 3);
    j.num("painter_ms_per_frame", zones.painterMs, 3);
    j.num("actors_ms_per_frame", zones.actorsMs, 3);
    j.num("tick_wait_ms_per_frame", zones.tickWaitMs, 3);
    j.num("draw_ms_per_frame", zones.drawMs, 3);
    j.num("painter_net_ms_per_frame", painterAvailable ? painter.ms : 0.0, 3);
    j.boolean("painter_net_clamped", painterAvailable && painter.clamped);
    j.boolean("painter_net_available", painterAvailable);
    j.num("drawrec_ms_per_frame", zones.drawRecMs, 3);
    j.num("gx_draw_done_wait_ms_per_frame", drawDoneWait, 3);
    j.num("drawrec_net_ms_per_frame", drawRec.ms, 3);
    j.boolean("drawrec_net_clamped", drawRec.clamped);
    j.boolean("drawrec_net_available", zones.haveDrawRec);
    j.end_object();
}

void write_interp_frames(benchmark::JsonWriter& j, const interp_frames::Counters& d, double seconds) {
    j.integer("frames_total", static_cast<std::int64_t>(d.frames));
    j.integer("frames_normal", static_cast<std::int64_t>(d.normal));
    j.integer("frames_normal_while_stalled", static_cast<std::int64_t>(d.stalled));
    j.integer("frames_interpolated_accepted", static_cast<std::int64_t>(d.interpolatedOnly));
    j.integer("frames_interpolated_with_sim_tick", static_cast<std::int64_t>(d.interpolatedWithTick));
    j.integer("frames_interpolated_rejected", static_cast<std::int64_t>(d.predictiveRejects));
    j.integer("sim_ticks", static_cast<std::int64_t>(d.simTicks));
    j.num("sim_rate_hz", seconds > 0.0 ? static_cast<double>(d.simTicks) / seconds : 0.0, 2);
    j.num("presented_rate_fps", seconds > 0.0 ? static_cast<double>(d.frames) / seconds : 0.0, 2);
    j.str("frames_note", "counted per game-clock frame. accepted = interpolating and no sim tick due (a frame that "
                         "exists only because of interpolation); with_sim_tick = interpolating and a tick ran; "
                         "rejected = the prediction slept to the next tick instead of drawing an interpolated "
                         "frame; normal = no interpolation (off, or the governor stalled it)");
}

}  // namespace dusk::fifo_report
