#include "dusk/dynres.hpp"

#include "dusk/dualscreen.h"
#include "dusk/dusk.h"
#include "dusk/logging.h"
#include "dusk/perf.hpp"
#include "dusk/settings.h"
#include "dusk/switch_display.hpp"

#include <algorithm>
#include <array>
#include <chrono>

namespace dusk::dynres {
namespace {

using clock = std::chrono::steady_clock;

// internalResolutionScale values, low to high, with the row count each one
// renders in handheld 16:9 (1x is the GameCube frame buffer, 528 rows).
struct Rung {
    int value;
    int rows;
};
constexpr std::array kRungs = {
    Rung{1, 528},
    Rung{ladder::kFirstExact + 0, 540},
    Rung{ladder::kFirstExact + 1, 720},
    Rung{ladder::kFirstExact + 2, 900},
    Rung{ladder::kFirstExact + 3, 1080},
};

// Decision cadence and thresholds. FIFO vsync quantizes the counter to
// 60/30/20/15, so "missing" is a whole notch down, not a few frames.
constexpr auto kWindow = std::chrono::milliseconds(2000);
constexpr auto kLoadingGap = std::chrono::milliseconds(250);
constexpr auto kSettleAfterChange = std::chrono::milliseconds(500);
constexpr float kMissRatio = 0.90f;   // fps < target*0.90 -> a rung down
constexpr float kMeetRatio = 0.97f;   // fps >= target*0.97 -> may climb
constexpr float kHelpedRatio = 1.10f; // a drop must buy >= 10% or it is undone
constexpr auto kUpBackoffFirst = std::chrono::seconds(5);
constexpr auto kUpBackoffMax = std::chrono::seconds(60);
constexpr auto kDownBackoffCpuBound = std::chrono::seconds(20);

bool s_enabledLast = false;
int s_baseLast = -1;     // settings value the ceiling was derived from
int s_autoLast = -1;     // what "auto" resolved to (dock/Flip dependent)
int s_ceiling = 0;       // rung index we may not exceed
int s_level = 0;         // rung index in effect
bool s_overriding = false;
unsigned s_transitions = 0;  // H28: resolution changes since launch, for the benchmark

clock::time_point s_windowStart{};
clock::time_point s_lastFrame{};
clock::time_point s_changedAt{};
int s_windowFrames = 0;
bool s_windowTainted = false;
bool s_haveLastFrame = false;

// One-window follow-ups after a move.
bool s_checkDrop = false;   // did the last drop buy frames?
float s_fpsBeforeDrop = 0.f;
bool s_checkClimb = false;  // did the last climb hold?
clock::duration s_upBackoff = kUpBackoffFirst;
clock::time_point s_upAllowedAt{};
clock::time_point s_downAllowedAt{};

// What "Auto" resolves to right now, folded together with the display mode so that a change of
// either re-anchors the ceiling (a manual value can be limited by the mode as well).
int autoValue() {
#ifdef __SWITCH__
    return ladder::exact_value(display::auto_rows()) + 100 * static_cast<int>(display::mode());
#else
    return ladder::exact_value(720);
#endif
}

// The value the ceiling is derived from: what the setting actually renders after the
// resolution policy (limited by the mode), so a rung is never above what the console allows.
int effectiveValue(int base) {
#ifdef __SWITCH__
    return respol::resolve(base, getSettings().game.autoResolutionPolicy.getValue(), display::mode()).value;
#else
    return base > 0 ? base : ladder::exact_value(720);
#endif
}

int rowsForValue(int value) {
    const int locked = ladder::exact_height(value);
    if (locked > 0) {
        return locked;
    }
    return std::max(1, value) * 528;
}

int ceilingFor(int baseValue) {
    const int rows = rowsForValue(baseValue);
    int best = 0;
    for (int i = 0; i < static_cast<int>(kRungs.size()); ++i) {
        if (kRungs[i].rows <= rows) {
            best = i;
        }
    }
    return best;
}

void applyLevel(int level, const char* why, float fps) {
    level = std::clamp(level, 0, s_ceiling);
    if (level == s_level) {
        return;
    }
    s_level = level;
    ++s_transitions;
    s_overriding = level != s_ceiling;
    // At the ceiling hand the user's own value back (it may be "auto" or a
    // scale the rungs only approximate); below it, the rung.
    applyInternalResolutionScale(
        s_overriding ? kRungs[level].value : getSettings().game.internalResolutionScale.getValue());
    s_changedAt = clock::now();
    DuskLog.info("dynres: {} -> {} rows ({:.1f} fps, target {})", why, kRungs[level].rows, fps,
                 getSettings().game.dynamicResolutionTarget.getValue());
}

void restoreBase() {
    if (s_overriding) {
        applyInternalResolutionScale(getSettings().game.internalResolutionScale.getValue());
        s_overriding = false;
    }
}

void beginWindow(clock::time_point now) {
    s_windowStart = now;
    s_windowFrames = 0;
    s_windowTainted = false;
}

}  // namespace

void reset() {
    restoreBase();
    s_baseLast = -1;
    s_autoLast = -1;
    s_level = 0;
    s_ceiling = 0;
    s_haveLastFrame = false;
    s_checkDrop = false;
    s_checkClimb = false;
    s_upBackoff = kUpBackoffFirst;
    s_upAllowedAt = {};
    s_downAllowedAt = {};
}

unsigned transitionCount() {
    return s_transitions;
}

int currentOverride() {
    return s_overriding ? kRungs[s_level].value : 0;
}

void update() {
    const bool enabled = getSettings().game.dynamicResolution.getValue();
    if (!enabled) {
        if (s_enabledLast) {
            reset();
        }
        s_enabledLast = false;
        return;
    }
    s_enabledLast = true;

    const auto now = clock::now();

    // Ceiling follows the user's own resolution and what "auto" means right
    // now (dock, Flip). Any change re-anchors at the top: a new ceiling is a
    // new situation, not a data point.
    const int base = getSettings().game.internalResolutionScale.getValue();
    const int autoNow = autoValue();
    if (base != s_baseLast || autoNow != s_autoLast) {
        s_baseLast = base;
        s_autoLast = autoNow;
        s_overriding = false;
        s_ceiling = ceilingFor(effectiveValue(base));
        s_level = s_ceiling;
        s_checkDrop = false;
        s_checkClimb = false;
        s_haveLastFrame = false;
        beginWindow(now);
        return;
    }
    if (s_ceiling == 0) {
        return;  // already at 1x, nothing below it
    }

    // Frame accounting. A gap over 250 ms is a load, and any frame with
    // pipelines queued is a compile stall: neither says anything about the
    // resolution, so the window is thrown away.
    if (s_haveLastFrame) {
        if (now - s_lastFrame > kLoadingGap) {
            s_windowTainted = true;
        }
    }
    s_lastFrame = now;
    s_haveLastFrame = true;
    if (dusk::lastFrameAuroraStats.queuedPipelines > 0) {
        s_windowTainted = true;
    }
    if (now - s_changedAt < kSettleAfterChange) {
        // The resize itself hitches; don't count it against the new rung.
        return;
    }
    ++s_windowFrames;
    if (now - s_windowStart < kWindow) {
        return;
    }

    const float seconds = std::chrono::duration<float>(now - s_windowStart).count();
    const float fps = seconds > 0.f ? static_cast<float>(s_windowFrames) / seconds : 0.f;
    const bool tainted = s_windowTainted || s_windowFrames < 10;
    beginWindow(now);
    if (tainted) {
        return;
    }

    const float target = static_cast<float>(std::max(getSettings().game.dynamicResolutionTarget.getValue(), 1));
    const float miss = target * kMissRatio;
    const float meet = target * kMeetRatio;

    if (s_checkDrop) {
        s_checkDrop = false;
        if (fps < s_fpsBeforeDrop * kHelpedRatio) {
            // Fewer pixels bought nothing: the main thread is the limit here.
            // Give the pixels back and stop trying for a while.
            applyLevel(s_level + 1, "drop bought nothing, back up", fps);
            s_downAllowedAt = now + kDownBackoffCpuBound;
            return;
        }
    }
    if (s_checkClimb) {
        s_checkClimb = false;
        if (fps < miss) {
            applyLevel(s_level - 1, "climb did not hold, back down", fps);
            s_upBackoff = std::min(s_upBackoff * 2, clock::duration(kUpBackoffMax));
            s_upAllowedAt = now + s_upBackoff;
            return;
        }
        // Held: next climb may try sooner again.
        s_upBackoff = kUpBackoffFirst;
    }

    if (fps < miss) {
        if (s_level > 0 && now >= s_downAllowedAt) {
            s_fpsBeforeDrop = fps;
            s_checkDrop = true;
            applyLevel(s_level - 1, "missing target, down", fps);
        }
    } else if (fps >= meet) {
        if (s_level < s_ceiling && now >= s_upAllowedAt) {
            s_checkClimb = true;
            applyLevel(s_level + 1, "headroom, up", fps);
        }
    }
}

}  // namespace dusk::dynres
