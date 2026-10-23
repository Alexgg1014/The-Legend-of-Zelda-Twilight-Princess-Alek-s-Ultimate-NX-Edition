#include "dusk/perf_core.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace dusk::perf {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool parse_uint(std::string_view s, unsigned long& out) {
    if (s.empty() || s.size() > 9) {
        return false;
    }
    unsigned long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + static_cast<unsigned long>(c - '0');
    }
    out = v;
    return true;
}

bool parse_bool(std::string_view s, bool& out) {
    const std::string v = lower(s);
    if (v == "1" || v == "on" || v == "true" || v == "yes") {
        out = true;
        return true;
    }
    if (v == "0" || v == "off" || v == "false" || v == "no") {
        out = false;
        return true;
    }
    return false;
}

void note(std::string& unknown, std::string_view key, std::string_view value) {
    if (!unknown.empty()) {
        unknown += ", ";
    }
    unknown += std::string(key);
    unknown += '=';
    unknown += std::string(value);
}

bool parse_steps(std::string_view s, std::vector<int>& out) {
    std::vector<int> steps;
    while (!s.empty()) {
        const auto comma = s.find(',');
        const auto item = trim(s.substr(0, comma));
        unsigned long v = 0;
        if (!parse_uint(item, v) || v < 50 || v > 100) {
            return false;
        }
        steps.push_back(static_cast<int>(v));
        if (comma == std::string_view::npos) {
            break;
        }
        s.remove_prefix(comma + 1);
    }
    if (steps.size() < 2 || steps.size() > 5) {
        return false;
    }
    for (std::size_t i = 1; i < steps.size(); ++i) {
        if (steps[i] >= steps[i - 1]) {
            return false;  // strictly descending
        }
    }
    out = std::move(steps);
    return true;
}

}  // namespace

static bool parse_fifo_fast(std::string_view v, unsigned& out);

LabOptions parse_lab_options(std::string_view text) {
    LabOptions o;
    while (!text.empty()) {
        const auto eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text.remove_prefix(eol == std::string_view::npos ? text.size() : eol + 1);
        // V1-rc10: '#' and ';' start a comment only at the start of a line or after a blank, and never in
        // tour_matrix's value (rc9: "hud_fast:1,0; swap_interval:2,1" kept only the first axis).
        line = trim(line);
        const bool matrixLine = lower(trim(line.substr(0, line.find('=')))) == "tour_matrix";
        for (std::size_t i = 0; !matrixLine && i < line.size(); ++i) {
            if ((line[i] == '#' || line[i] == ';') && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
                line = line.substr(0, i);
                break;
            }
        }
        line = trim(line);
        if (line.empty() || line.front() == '[') {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            note(o.unknown, line, "");
            continue;
        }
        const std::string key = lower(trim(line.substr(0, eq)));
        const auto value = trim(line.substr(eq + 1));
        const std::string v = lower(value);
        unsigned long n = 0;
        bool b = false;
        unsigned n32 = 0;
        // aleks2 (V1): renderer toggles read once at start-up from the environment (Aurora, lib/deko): the staging
        // slots CPU-cached and the present's colour clear left out when the picture covers the image (both off by
        // default, after ChanseyIsTheBest/renut_nx c319b29, used with his permission), and the input -> present
        // probe's 30 s log line (on by default; observability only).
        // V1-rc16: dk_zcull (the queue's zcull, COS_DK_ZCULL) and dk_compression (hardware compression of colour
        // render targets, COS_DK_COMPRESSION), both off by default, for A/B on the console (ideas from Chansey's
        // sa2-nx review; our own switches, lib/deko).
        if (key == "dk_cached_upload" || key == "dk_skip_present_clear" || key == "dk_zcull" ||
            key == "dk_compression" || key == "latency_probe") {
            if (parse_bool(v, b)) {
                const char* name = key == "dk_cached_upload"        ? "COS_DK_CACHED_UPLOAD"
                                   : key == "dk_skip_present_clear" ? "COS_DK_SKIP_PRESENT_CLEAR"
                                   : key == "dk_zcull"              ? "COS_DK_ZCULL"
                                   : key == "dk_compression"        ? "COS_DK_COMPRESSION"
                                                                    : "AURORA_LATENCY_PROBE";
                setenv(name, b ? "1" : "0", 1);
            } else {
                note(o.unknown, line, "");
            }
            continue;
        }
        if (key == "gpu_timing") {
            if (v == "auto") {
                o.gpuTiming = GpuTiming::Auto;
            } else if (v == "always" || v == "1" || v == "on") {
                o.gpuTiming = GpuTiming::Always;
            } else if (v == "off" || v == "0") {
                o.gpuTiming = GpuTiming::Off;
            } else {
                note(o.unknown, key, value);
            }
        } else if (key == "rt_pool" && parse_uint(v, n) && n <= 6) {
            o.rtPool = static_cast<unsigned>(n);
        } else if (key == "drs_steps" && parse_steps(v, o.drsSteps)) {
        } else if (key == "drs_target_fps" && parse_uint(v, n) && n >= 15 && n <= 60) {
            o.drsTargetFps = static_cast<int>(n);
        } else if (key == "drs_log" && parse_bool(v, b)) {
            o.drsVerbose = b;
        } else if (key == "drs_stress_transitions" && parse_uint(v, n) && n >= 2 && n <= 100000) {
            o.stressTransitions = static_cast<unsigned>(n);
        } else if (key == "drs_stress_interval_ms" && parse_uint(v, n) && n >= 250 && n <= 60000) {
            o.stressIntervalMs = static_cast<unsigned>(n);
        } else if (key == "drs_stress_pattern" && (v == "steps" || v == "ladder")) {
            o.stressLadder = v == "ladder";
        } else if (key == "fifo_fast" && parse_fifo_fast(v, n32)) {
            o.fifoFast = n32;
        } else if (key == "hud_fast" && parse_bool(v, b)) {
            o.hudFast = b;
        } else if (key == "warmup_fade_boost" && parse_bool(v, b)) {
            o.warmupFadeBoost = b;
        } else if (key == "arg_shield" && parse_bool(v, b)) {
            o.argShield = b;
        } else if (key == "hook_fast" && v == "verify") {
            o.hookFast = true;
            o.hookVerify = true;
        } else if (key == "hook_fast" && parse_bool(v, b)) {
            o.hookFast = b;
            o.hookVerify = false;
        } else if (key == "vsync_relaxed" && parse_bool(v, b)) {
            o.vsyncRelaxed = b;
        } else if (key == "mod_lib_cache" && parse_bool(v, b)) {
            o.modLibCache = b;  // V1-rc19
        } else if (key == "tour_matrix" && !v.empty()) {
            o.tourMatrix = std::string(v);
        } else if (key == "minimap_pan" && parse_bool(v, b)) {
            o.minimapPan = b;
        } else if (key == "dbg_fog_off" && parse_bool(v, b)) {
            o.debugFlags = b ? (o.debugFlags | 1u) : (o.debugFlags & ~1u);
        } else if (key == "dbg_base_mip" && parse_bool(v, b)) {
            o.debugFlags = b ? (o.debugFlags | 2u) : (o.debugFlags & ~2u);
        } else if (key == "dbg_no_aniso" && parse_bool(v, b)) {
            o.debugFlags = b ? (o.debugFlags | 4u) : (o.debugFlags & ~4u);
        } else if (key == "dbg_copy_shape_match" && parse_bool(v, b)) {
            o.debugFlags = b ? (o.debugFlags | 8u) : (o.debugFlags & ~8u);
        } else if (key == "dbg_no_real_shadow" && parse_bool(v, b)) {
            o.dbgNoRealShadow = b;
        } else if (key == "wii_record" && parse_bool(v, b)) {
            o.wiiRecord = b;
        } else if (key == "warmup_keep_all" && parse_bool(v, b)) {
            o.warmupKeepAll = b;
        } else if (key == "profile_main" && parse_bool(v, b)) {
            o.profileMain = b;
        } else if (key == "profile_seconds" && parse_uint(v, n) && n >= 5 && n <= 300) {
            o.profileSeconds = static_cast<unsigned>(n);
        } else if (key == "profile_delay_s" && parse_uint(v, n) && n <= 3600) {
            o.profileDelayS = static_cast<unsigned>(n);
        } else if (key == "interp_floor_fps" && parse_uint(v, n) && (n == 0 || (n >= 31 && n <= 60))) {
            o.interpFloorFps = static_cast<unsigned>(n);
        } else if (key == "hud_cull_alpha0" && parse_bool(v, b)) {
            o.hudCullAlpha0 = b;
        } else if (key == "exit_mode" && (v == "auto" || v == "loader" || v == "process")) {
            o.exitMode = v == "auto" ? 0u : v == "loader" ? 1u : 2u;
        } else if (key == "logic_core" && !v.empty() && v.find_first_not_of("012, ") == std::string::npos) {
            unsigned mask = 0;
            for (const char c : v) {
                if (c >= '0' && c <= '2') {
                    mask |= 1u << (c - '0');
                }
            }
            o.logicCoreMask = mask;
        } else if (key == "texrep_budget_mb" && parse_uint(v, n) && n >= 64 && n <= 2048) {
            o.texrepBudgetMb = static_cast<unsigned>(n);
        } else if (key == "swap_interval" && (v == "0" || v == "1" || v == "2" || v == "3" || v == "adaptive")) {
            o.swapInterval = v == "adaptive" ? 3u : static_cast<unsigned>(v[0] - '0');  // 0: auto
        } else {
            note(o.unknown, key, value);
        }
    }
    return o;
}

// fifo_fast: "off"/"0", "all"/"on", a number 0-7, or names joined by commas.
static bool parse_fifo_fast(std::string_view v, unsigned& out) {
    if (v == "off" || v == "0") {
        out = 0;
        return true;
    }
    if (v == "all" || v == "on") {
        out = 31;
        return true;
    }
    if (!v.empty() && v.size() <= 2 && v.find_first_not_of("0123456789") == std::string_view::npos) {
        const unsigned n = static_cast<unsigned>(std::stoul(std::string(v)));
        if (n >= 1 && n <= 31) {
            out = n;
            return true;
        }
        return false;
    }
    unsigned mask = 0;
    while (!v.empty()) {
        const auto comma = v.find(',');
        auto name = v.substr(0, comma);
        while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
        while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
        if (name == "bind") {
            mask |= 1;
        } else if (name == "idx") {
            mask |= 2;
        } else if (name == "urep") {
            mask |= 4;
        } else if (name == "pipe") {
            mask |= 8;
        } else if (name == "pop") {
            mask |= 16;
        } else {
            return false;
        }
        v.remove_prefix(comma == std::string_view::npos ? v.size() : comma + 1);
    }
    out = mask;
    return true;
}

static std::string fifo_fast_name(unsigned mask) {
    if (mask == 0) {
        return "off";
    }
    std::string s;
    if (mask & 1) s += "bind,";
    if (mask & 2) s += "idx,";
    if (mask & 4) s += "urep,";
    if (mask & 8) s += "pipe,";
    if (mask & 16) s += "pop,";
    s.pop_back();
    return s;
}

std::string describe(const LabOptions& o) {
    std::string steps;
    for (std::size_t i = 0; i < o.drsSteps.size(); ++i) {
        steps += (i ? "," : "") + std::to_string(o.drsSteps[i]);
    }
    char buf[1024];
    std::snprintf(buf, sizeof buf,
        "gpu_timing=%s rt_pool=%u drs_steps=%s drs_target_fps=%d drs_log=%d drs_stress_transitions=%u "
        "drs_stress_interval_ms=%u drs_stress_pattern=%s fifo_fast=%s logic_core_mask=0x%x swap_interval=%u hud_fast=%d minimap_pan=%d texrep_budget_mb=%u warmup_fade_boost=%d arg_shield=%d hook_fast=%d vsync_relaxed=%d tour_matrix='%s' hud_cull_alpha0=%d exit_mode=%s interp_floor_fps=%u dbg=0x%x dbg_no_real_shadow=%d mod_lib_cache=%d",
        o.gpuTiming == GpuTiming::Always ? "always" : o.gpuTiming == GpuTiming::Off ? "off" : "auto", o.rtPool,
        steps.c_str(), o.drsTargetFps, o.drsVerbose ? 1 : 0, o.stressTransitions, o.stressIntervalMs,
        o.stressLadder ? "ladder" : "steps", fifo_fast_name(o.fifoFast).c_str(), o.logicCoreMask, o.swapInterval, o.hudFast ? 1 : 0, o.minimapPan ? 1 : 0, o.texrepBudgetMb,
        o.warmupFadeBoost ? 1 : 0, o.argShield ? 1 : 0, o.hookVerify ? 2 : o.hookFast ? 1 : 0, o.vsyncRelaxed ? 1 : 0, o.tourMatrix.c_str(), o.hudCullAlpha0 ? 1 : 0,
        o.exitMode == 1 ? "loader" : o.exitMode == 2 ? "process" : "auto", o.interpFloorFps, o.debugFlags,
        o.dbgNoRealShadow ? 1 : 0, o.modLibCache ? 1 : 0);
    return buf;
}

const char* bound_name(Bound b) {
    switch (b) {
    case Bound::Met:
        return "met";
    case Bound::Gpu:
        return "gpu";
    case Bound::CpuMain:
        return "cpu_main";
    case Bound::CpuRender:
        return "cpu_render";
    default:
        return "unknown";
    }
}

void SwapGovernor::reset(unsigned interval) {
    m_current = interval == 2 ? 2u : 1u;
    m_count = 0;
    m_pos = 0;
    m_sinceChange = 0;
}

unsigned SwapGovernor::update(const FrameSample& f) {
    const float work = std::max({f.mainWorkMs, f.fifoBusyMs, f.gpuMs > 0.f ? f.gpuMs : 0.f});
    m_work[m_pos] = work;
    m_pos = (m_pos + 1) % kWindow;
    m_count = std::min(m_count + 1, kWindow);
    ++m_sinceChange;
    if (m_count < kWindow || m_sinceChange < kDwell) {
        return m_current;
    }
    float sorted[kWindow];
    std::copy(m_work, m_work + kWindow, sorted);
    const std::size_t k = kWindow * 9 / 10;
    std::nth_element(sorted, sorted + k, sorted + kWindow);
    m_lastP90 = sorted[k];
    const unsigned want = m_current == 1 ? (m_lastP90 > kTo2Ms ? 2u : 1u) : (m_lastP90 < kTo1Ms ? 1u : 2u);
    if (want != m_current) {
        m_current = want;
        m_sinceChange = 0;
        m_count = 0;
        ++m_changes;
    }
    return m_current;
}

Bound classify(const FrameSample& f, float budgetMs) {
    if (f.intervalMs <= budgetMs * kMissSlack) {
        return Bound::Met;
    }
    const float busy = budgetMs * kBusyShare;
    const bool haveGpu = f.gpuMs >= 0.f;
    if (haveGpu && f.gpuMs >= busy && f.gpuMs >= f.mainWorkMs && f.gpuMs >= f.fifoBusyMs) {
        return Bound::Gpu;
    }
    if (f.mainWorkMs >= busy && f.mainWorkMs >= f.fifoBusyMs) {
        return Bound::CpuMain;
    }
    if (f.fifoBusyMs >= busy) {
        return Bound::CpuRender;
    }
    if (haveGpu && f.gpuMs >= busy) {
        return Bound::Gpu;
    }
    return Bound::Unknown;
}

float percentile(std::vector<float> values, double p) {
    if (values.empty()) {
        return 0.f;
    }
    std::sort(values.begin(), values.end());
    const double rank = std::ceil(p / 100.0 * static_cast<double>(values.size()));
    const auto idx = static_cast<std::size_t>(std::clamp(rank, 1.0, static_cast<double>(values.size()))) - 1;
    return values[idx];
}

WindowStats summarize(const std::vector<FrameSample>& frames, float budgetMs) {
    WindowStats w;
    std::vector<float> gpu, interval, mainWork, fifo;
    gpu.reserve(frames.size());
    interval.reserve(frames.size());
    mainWork.reserve(frames.size());
    fifo.reserve(frames.size());
    double gpuSum = 0.0, gpuIntervalSum = 0.0;
    for (const auto& f : frames) {
        ++w.frames;
        const Bound b = classify(f, budgetMs);
        ++w.byBound[static_cast<int>(b)];
        if (b != Bound::Met) {
            ++w.missed;
        }
        w.seconds += f.intervalMs / 1000.0;
        interval.push_back(f.intervalMs);
        mainWork.push_back(f.mainWorkMs);
        fifo.push_back(f.fifoBusyMs);
        if (f.gpuMs >= 0.f) {
            ++w.gpuFrames;
            gpu.push_back(f.gpuMs);
            gpuSum += f.gpuMs;
            gpuIntervalSum += f.intervalMs;
        }
        w.queuedMax = std::max(w.queuedMax, f.queuedPipelines);
    }
    w.gpuUtil = gpuIntervalSum > 0.0 ? gpuSum / gpuIntervalSum : 0.0;
    w.gpuMedianMs = percentile(gpu, 50);
    w.gpuP90Ms = percentile(gpu, 90);
    w.intervalMedianMs = percentile(interval, 50);
    w.mainWorkMedianMs = percentile(mainWork, 50);
    w.fifoBusyMedianMs = percentile(fifo, 50);
    return w;
}

DrsPolicy::DrsPolicy(std::vector<int> stepPercents) : mSteps(std::move(stepPercents)) {
    if (mSteps.empty()) {
        mSteps = {100};
    }
}

void DrsPolicy::reset() {
    mLevel = 0;
    pressureStreak = relaxedStreak = verifyLeft = 0;
    verifyingDrop = verifyingClimb = false;
    lastChange = downAllowedAt = upAllowedAt = -1e9;
    upBackoff = kUpBackoffFirstS;
    missBeforeDrop = 0.0;
    mState = "idle";
}

int DrsPolicy::percent(int level) const {
    return mSteps[static_cast<std::size_t>(std::clamp(level, 0, steps() - 1))];
}

bool DrsPolicy::pressured(const WindowStats& w) const {
    return w.missedPct() >= kPressureMissPct &&
           (w.gpuMissShare() >= kPressureGpuShare || w.gpuUtil >= kPressureUtil);
}

bool DrsPolicy::relaxed(const WindowStats& w) const {
    if (w.missedPct() >= kRelaxedMissPct || mLevel == 0) {
        return false;
    }
    const double here = percent(mLevel) / 100.0;
    const double up = percent(mLevel - 1) / 100.0;
    // GPU work scales with pixels: the next step up costs (up/here)^2 as much.
    const double scaled = w.gpuUtil * (up * up) / (here * here);
    return scaled <= kUpUtil;
}

Decision DrsPolicy::move(int to, std::string why, double now) {
    mLevel = std::clamp(to, 0, steps() - 1);
    lastChange = now;
    pressureStreak = relaxedStreak = 0;
    return {Action::Hold, mLevel, std::move(why)};
}

Decision DrsPolicy::evaluate(const WindowStats& w, bool tainted, double now) {
    if (tainted) {
        pressureStreak = relaxedStreak = 0;
        mState = "waiting (load or compile)";
        return {Action::Hold, mLevel, {}};
    }
    if (!w.hasGpu()) {
        pressureStreak = relaxedStreak = 0;
        verifyLeft = 0;
        verifyingDrop = verifyingClimb = false;
        mState = "no GPU timing: resolution left alone";
        return {Action::Hold, mLevel, {}};
    }
    const bool isPressured = pressured(w);
    const bool isRelaxed = relaxed(w);

    if (verifyLeft > 0) {
        --verifyLeft;
        if (verifyingDrop) {
            // Still missing and no longer the GPU's doing: the drop bought nothing.
            if (w.missedPct() >= kPressureMissPct && w.gpuMissShare() < kPressureGpuShare &&
                w.missedPct() >= missBeforeDrop * 0.5) {
                verifyingDrop = false;
                verifyLeft = 0;
                downAllowedAt = now + kCpuBackoffS;
                mState = "CPU-bound: drop undone";
                auto d = move(mLevel - 1, "drop bought nothing (CPU/render worker is the limit), back up", now);
                d.action = Action::Up;
                return d;
            }
            if (verifyLeft == 0) {
                verifyingDrop = false;
            }
        } else if (verifyingClimb) {
            if (isPressured) {
                verifyingClimb = false;
                verifyLeft = 0;
                upAllowedAt = now + upBackoff;
                upBackoff = std::min(upBackoff * 2.0, kUpBackoffMaxS);
                mState = "climb did not hold";
                auto d = move(mLevel + 1, "climb did not hold, back down", now);
                d.action = Action::Down;
                return d;
            }
            if (verifyLeft == 0) {
                verifyingClimb = false;
                upBackoff = kUpBackoffFirstS;
            }
        }
    }

    pressureStreak = isPressured ? pressureStreak + 1 : 0;
    relaxedStreak = isRelaxed ? relaxedStreak + 1 : 0;
    const bool dwelt = now - lastChange >= kMinDwellS;

    if (pressureStreak >= kDownStreak && dwelt && now >= downAllowedAt && mLevel + 1 < steps()) {
        missBeforeDrop = w.missedPct();
        verifyingDrop = true;
        verifyingClimb = false;
        verifyLeft = kVerifyWindows;
        mState = "GPU-bound: stepped down";
        auto d = move(mLevel + 1, "GPU is the limit, down", now);
        d.action = Action::Down;
        return d;
    }
    if (relaxedStreak >= kUpStreak && dwelt && now >= upAllowedAt && mLevel > 0) {
        verifyingClimb = true;
        verifyingDrop = false;
        verifyLeft = kVerifyWindows;
        mState = "headroom: stepped up";
        auto d = move(mLevel - 1, "GPU headroom, up", now);
        d.action = Action::Up;
        return d;
    }
    if (isPressured) {
        mState = mLevel + 1 < steps() ? "GPU tight" : "GPU tight at the lowest step";
    } else if (w.missedPct() >= kPressureMissPct) {
        mState = "missing frames, not the GPU: left alone";
    } else {
        mState = mLevel == 0 ? "at the ceiling" : "below the ceiling";
    }
    return {Action::Hold, mLevel, {}};
}

int rows_for_percent(int ceilingRows, int percent) {
    const int rows = static_cast<int>(std::lround(static_cast<double>(ceilingRows) * percent / 100.0));
    return std::max(kMinRows, rows & ~1);
}

std::vector<int> stress_rows(int ceilingRows, const LabOptions& o, unsigned transitions) {
    std::vector<int> cycle;
    if (o.stressLadder) {
        for (int rows : {900, 720, 540}) {
            if (rows <= ceilingRows) {
                cycle.push_back(rows);
            }
        }
        if (cycle.empty() || cycle.front() != ceilingRows) {
            cycle.insert(cycle.begin(), ceilingRows);
        }
    } else {
        for (int pct : o.drsSteps) {
            cycle.push_back(rows_for_percent(ceilingRows, pct));
        }
    }
    cycle.erase(std::unique(cycle.begin(), cycle.end()), cycle.end());
    std::vector<int> out;
    if (cycle.size() < 2) {
        return out;
    }
    // Bounce: 0 1 2 1 0 1 2 ... starting from the step after the ceiling.
    const int n = static_cast<int>(cycle.size());
    int idx = 0;
    int dir = 1;
    for (unsigned i = 0; i < transitions; ++i) {
        if (idx + dir < 0 || idx + dir >= n) {
            dir = -dir;
        }
        idx += dir;
        out.push_back(cycle[static_cast<std::size_t>(idx)]);
    }
    return out;
}

}  // namespace dusk::perf
