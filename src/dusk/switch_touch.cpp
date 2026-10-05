/*
 * switch_touch.cpp — libnx touchscreen -> the companion's touch layer.
 *
 * The donor's 1,248-line touch layer is used UNMODIFIED. That is possible
 * because companion::touchEvent takes NORMALISED (u, v) coordinates, which is
 * the single most fortunate API decision in the donor for this port: anything
 * that can produce a normalised point inside the companion's rect drives the
 * whole thing -- drag-and-drop equipping, map panning, the wrapped readers,
 * press-and-hold on the item buttons -- with no changes at all.
 *
 * So this file is coordinate mapping and gesture timing, and nothing else.
 *
 *      hidGetTouchScreenStates            (physical panel pixels)
 *            |  undo the Flip Grip rotation
 *      point in the mode's LOGICAL space
 *            |  reject unless inside the companion rect
 *      normalise to (u, v) in that rect
 *            |
 *      companion::touchEvent(action, u, v)
 *
 * TOUCH EXISTS ONLY IN HANDHELD MODE. Docked, the panel is off and hid reports
 * no touchscreen at all -- that is hardware, not a gap here. Docked navigation
 * is the pointer's job, and it will synthesise into this same touchEvent path
 * so there is only ever one interaction model to keep working.
 */

#ifdef __SWITCH__

#include "switch/touch.hpp"
#include "dusk/companion.h"
#include "dusk/dualscreen.h"
#include "dusk/perf.hpp"
#include "dusk/settings.h"

#include <aurora/aux_layout_switch.hpp>
#include <aurora/aux_window.hpp>

#include <switch.h>

#include <SDL3/SDL.h>
#include <aurora/lib/window.hpp>

#include <algorithm>
#include <cmath>

namespace dusk::switch_touch {
namespace {

namespace lay = aurora::auxwin::layout;

// Mirrors the donor's action codes (companion::touchEvent).
constexpr int kActionDown = 0;
constexpr int kActionMove = 1;
constexpr int kActionUp = 2;
constexpr int kActionCancel = 3;

bool s_initialised = false;
bool s_down = false;          // a single-finger gesture is in flight
bool s_pinching = false;      // two fingers took over; the drag was cancelled
float s_prevPinchDist = 0.f;
// Last companion-space point the finger was actually at. The touchscreen
// reports NOTHING on the frame the finger leaves, so the release has no
// coordinates of its own -- and the release is precisely where a drag is
// resolved. See end_gesture.
float s_lastU = 0.f;
float s_lastV = 0.f;
PadState s_pad{};   // own pad handle for the summon chord; see poll_normal_overlay_toggle

/// Panel size for the frame. Taken from the layout's own view of the output so
/// the inverse below is the exact inverse of what the compositor drew, rather
/// than a second guess at the panel size.
int s_outW = 1280;
int s_outH = 720;

/*
 * Undo the presentation rotation, then hit-test and normalise. Every mode
 * shares this path; the unrotated one takes the identity branch.
 *
 * The compositor maps a LOGICAL point to the panel as
 *     phys = (ly, outH - lx)
 * (see layout::to_physical), so the inverse of a physical point is
 *     lx = outH - sy,   ly = sx
 * which at 1280x720 is lx = 720 - sy, ly = sx.
 *
 * THIS IS THE PART TO DISTRUST. The ALEKS port shipped the inverse of a
 * 90-degree turn here -- both axes mirrored -- and every single tap missed its
 * target while looking perfectly plausible in review. Check it against corners
 * on hardware, not by reading: physical (0, outH) must come back as logical
 * (0, 0), the top-left of the canvas.
 */
bool map_to_companion(int sx, int sy, float* outU, float* outV) {
    lay::Rect rect{};
    int lx = sx;
    int ly = sy;

    if (lay::mode() == lay::kFlip270) {
        lay::Layout l{};
        lay::compute(s_outW, s_outH, 4.f / 3.f, &l);
        if (l.mode != lay::kFlip270 || !l.companion.valid()) {
            return false;
        }
        rect = l.companion;
        lx = s_outH - sy;
        ly = sx;
    } else {
        if (!lay::normal_overlay_visible()) {
            return false;
        }
        rect = lay::normal_overlay_rect(s_outW, s_outH, 720.f / 740.f);
        if (!rect.valid()) {
            return false;
        }
    }

    // Always written, even for a point outside the panel: the caller keeps
    // feeding MOVE while a started drag wanders off the companion, and it
    // needs an edge-clamped point for that -- returning without writing left
    // u/v at the caller's zero-initialised locals, which snapped the finger to
    // the top-left corner instead of the nearest edge.
    const float u = static_cast<float>(lx - rect.x) / static_cast<float>(rect.w);
    const float v = static_cast<float>(ly - rect.y) / static_cast<float>(rect.h);
    *outU = std::clamp(u, 0.f, 1.f);
    *outV = std::clamp(v, 0.f, 1.f);

    if (lx < rect.x || lx >= rect.x + rect.w || ly < rect.y || ly >= rect.y + rect.h) {
        return false; // a tap on the game half must never reach the companion
    }
    return true;
}

/// ZL + R3 toggles the summoned companion in Normal mode.
///
/// Flip Grip does not need it -- the companion is always on screen there. Normal
/// does: without a way to summon it, the overlay this port renders is
/// unreachable and Normal is just the stock game. The combo matches the one the
/// ALEKS Zelda3 port already uses for its settings, so the muscle memory
/// carries over, and it is a chord no gameplay binding uses.
void poll_normal_overlay_toggle() {
    if (lay::mode() == lay::kFlip270) {
        return;  // always visible; nothing to toggle
    }
    // Initialised once in initialize(). Re-initialising per frame would reset the
    // pad's previous-state, and padGetButtonsDown would never see an edge.
    padUpdate(&s_pad);
    const u64 held = padGetButtons(&s_pad);
    const u64 down = padGetButtonsDown(&s_pad);
    const bool combo = (held & HidNpadButton_ZL) && (down & HidNpadButton_StickR);
    if (combo) {
        const bool now = !lay::normal_overlay_visible();
        lay::set_normal_overlay_visible(now);
        dualscreen::requestRedraw();
    }
}

void end_gesture(int action) {
    if (!s_down) {
        return;
    }
    s_down = false;
    // The LAST POINT, not (0,0). companion::touchEvent stores the coordinates
    // of every action it is given, including the release, and processDragTouch
    // hit-tests the drop against exactly those -- so passing (0,0) here made
    // every drag release land on the companion's top-left corner. Dragging an
    // item onto I or II therefore always missed: the ghost followed the finger
    // correctly, the release resolved somewhere else entirely, and nothing
    // equipped. It also mis-aimed every deferred tap that resolves on release
    // (guide rows, collect rows, the left-column swipe).
    //
    // Android never hit this because a MotionEvent ACTION_UP carries the
    // release position; libnx's touchscreen reports zero touches instead, so
    // the port has to remember it. A cancel passes it too -- harmless, since
    // that path discards the gesture without hit-testing anything.
    companion::touchEvent(action, s_lastU, s_lastV);
    dualscreen::requestRedraw();
}

} // namespace

void initialize() {
    if (s_initialised) {
        return;
    }
    hidInitializeTouchScreen();
    // No padConfigureInput here. It is process-wide state, not per-handle, and
    // this used to declare one player -- overwriting what input_init() set and
    // quietly putting the limit back. Whoever runs last wins, and that is not
    // a thing to decide by startup order.
    padInitializeDefault(&s_pad);
    // psm is not opened anywhere else in this build. If it fails the battery
    // reads below just return errors and the status strip stays blank, which
    // is the same thing desktop does -- so it is not worth failing over.
    psmInitialize();
    s_initialised = true;
}

// --- Touchscreen -> RmlUi menus (aleks2) -----------------------------------
//
// NX's SDL3-switch patch carries no touch device, so RmlUi menus, the
// prelaunch UI and dusk::ui::input's multi-finger summon gesture never see a
// finger on this console (H0 test: "touch no jala"). Aleks bridged this in its
// own window_switch.cpp; NX pumps real SDL events through
// aurora::window::process_event, so here the same bridge just pushes
// synthetic events into SDL's queue and lets the normal path deliver them
// to ImGui, RmlUi and the dusk input layer.
//
// Mouse rather than finger for RmlUi: its touch path dispatches to a captured
// target the Dusk documents do not listen for; the mouse path is the one their
// buttons, sliders and tabs are written against, and it gets hover and drag
// for free. Finger events are pushed too so the summon gesture keeps working.
//
// Flip Grip IS handled (unlike Aleks): the overlay is composited turned into
// the game rect, so the finger goes through the inverse of that mapping (see
// feed). Touches over a visible companion are the companion's.
namespace menu_bridge {
bool s_down = false;
float s_lastX = 0.f;   // normalised 0..1, panel space
float s_lastY = 0.f;

void push(Uint32 fingerType, float nx, float ny) {
    SDL_Window* window = aurora::window::get_sdl_window();
    if (window == nullptr) {
        return;
    }
    const SDL_WindowID windowId = SDL_GetWindowID(window);
    int winW = 0;
    int winH = 0;
    SDL_GetWindowSize(window, &winW, &winH);
    if (winW <= 0 || winH <= 0) {
        winW = 1280;
        winH = 720;
    }
    const float px = nx * static_cast<float>(winW);
    const float py = ny * static_cast<float>(winH);
    const Uint64 now = SDL_GetTicksNS();

    SDL_Event finger{};
    finger.type = fingerType;
    finger.tfinger.timestamp = now;
    finger.tfinger.touchID = 1;
    finger.tfinger.fingerID = 1;
    finger.tfinger.x = nx;
    finger.tfinger.y = ny;
    finger.tfinger.dx = nx - s_lastX;
    finger.tfinger.dy = ny - s_lastY;
    finger.tfinger.pressure = 1.f;
    finger.tfinger.windowID = windowId;
    SDL_PushEvent(&finger);

    // A press needs the cursor to be over the target first: RmlUi decides
    // what a click hits from the last motion, not from the button event.
    SDL_Event move{};
    move.type = SDL_EVENT_MOUSE_MOTION;
    move.motion.timestamp = now;
    move.motion.windowID = windowId;
    move.motion.which = SDL_TOUCH_MOUSEID;
    move.motion.state = (fingerType == SDL_EVENT_FINGER_UP) ? 0 : SDL_BUTTON_LMASK;
    move.motion.x = px;
    move.motion.y = py;
    SDL_PushEvent(&move);

    if (fingerType == SDL_EVENT_FINGER_DOWN || fingerType == SDL_EVENT_FINGER_UP) {
        SDL_Event button{};
        button.type = fingerType == SDL_EVENT_FINGER_DOWN ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                                          : SDL_EVENT_MOUSE_BUTTON_UP;
        button.button.timestamp = now;
        button.button.windowID = windowId;
        button.button.which = SDL_TOUCH_MOUSEID;
        button.button.button = SDL_BUTTON_LEFT;
        button.button.down = fingerType == SDL_EVENT_FINGER_DOWN;
        button.button.clicks = 1;
        button.button.x = px;
        button.button.y = py;
        SDL_PushEvent(&button);
    }
}

void release() {
    if (s_down) {
        // The release carries the LAST known point, not zero (see s_lastU).
        push(SDL_EVENT_FINGER_UP, s_lastX, s_lastY);
        s_down = false;
    }
}

/// `state` is this frame's touchscreen state (may have count 0);
/// `companionOwnsIt` says the companion took the first finger this frame.
void feed(const HidTouchScreenState& state, bool companionOwnsIt) {
    if (companionOwnsIt || state.count != 1) {
        release();
        return;
    }
    float x = std::clamp(static_cast<float>(state.touches[0].x) / 1280.f, 0.f, 1.f);
    float y = std::clamp(static_cast<float>(state.touches[0].y) / 720.f, 0.f, 1.f);
    if (lay::mode() == lay::kFlip270) {
        // Flip Grip: the RmlUi overlay is composited TURNED into the game's
        // rect (aurora.cpp draws it with the rot270 pipeline over flipRect),
        // so a finger on the panel maps to the overlay through the same
        // inverse the companion uses: physical -> logical portrait canvas
        // (lx = outH - sy, ly = sx), then normalise inside the game rect.
        // Fingers outside the game rect (companion area, gaps) are released,
        // not mapped: they belong to the companion or to nothing.
        const float aspect = dusk::dualscreen::effectiveAspectLock() ? 4.f / 3.f : 16.f / 9.f;
        lay::Layout l{};
        lay::compute(1280, 720, aspect, &l);
        if (l.mode != lay::kFlip270 || !l.game.valid()) {
            release();
            return;
        }
        const float lx = 720.f - static_cast<float>(state.touches[0].y);
        const float ly = static_cast<float>(state.touches[0].x);
        const float u = (lx - static_cast<float>(l.game.x)) / static_cast<float>(l.game.w);
        const float v = (ly - static_cast<float>(l.game.y)) / static_cast<float>(l.game.h);
        if (u < 0.f || u > 1.f || v < 0.f || v > 1.f) {
            release();
            return;
        }
        x = u;
        y = v;
    }
    push(s_down ? SDL_EVENT_FINGER_MOTION : SDL_EVENT_FINGER_DOWN, x, y);
    s_lastX = x;
    s_lastY = y;
    s_down = true;
}
}  // namespace menu_bridge

/// Called once per game frame from duskExecute(), BEFORE the companion latches
/// its input for the frame, so a tap lands in the same frame it happened.
void update() {
    if (!s_initialised) {
        return;
    }
    // Docked has no touchscreen. Ending any in-flight gesture matters: a
    // finger held while the console is docked would otherwise leave a button
    // latched down forever.
    if (perf::is_docked()) {
        end_gesture(kActionCancel);
        s_pinching = false;
        menu_bridge::release();
        return;
    }
    if (!dualscreen::hudOnCompanion()) {
        end_gesture(kActionCancel);
        // Companion off: the whole panel belongs to the menus.
        HidTouchScreenState menuState{};
        if (hidGetTouchScreenStates(&menuState, 1)) {
            menu_bridge::feed(menuState, false);
        } else {
            menu_bridge::release();
        }
        return;
    }
    poll_normal_overlay_toggle();

    uint32_t auxW = 0;
    uint32_t auxH = 0;
    aurora::auxwin::get_surface_size(&auxW, &auxH);
    // The panel is fixed at 1280x720 in handheld; the touchscreen reports in
    // that same space regardless of how the console is being held, which is
    // exactly why the rotation has to be undone in software.
    s_outW = 1280;
    s_outH = 720;

    HidTouchScreenState state{};
    if (!hidGetTouchScreenStates(&state, 1)) {
        end_gesture(kActionCancel);
        s_pinching = false;
        menu_bridge::release();
        return;
    }

    const int count = static_cast<int>(state.count);
    // Whatever the companion does not claim below goes to the menus. Decided
    // up front from the same state so both consumers see one finger history.
    {
        float mu = 0.f;
        float mv = 0.f;
        const bool onCompanion =
            count == 1 && (s_down || map_to_companion(static_cast<int>(state.touches[0].x),
                                                        static_cast<int>(state.touches[0].y), &mu, &mv));
        menu_bridge::feed(state, onCompanion);
    }

    // --- two fingers: pinch owns the gesture ------------------------------
    if (count >= 2) {
        const float dx = static_cast<float>(state.touches[0].x) - static_cast<float>(state.touches[1].x);
        const float dy = static_cast<float>(state.touches[0].y) - static_cast<float>(state.touches[1].y);
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (!s_pinching) {
            // Cancel rather than release: the donor uses action 3 to mean
            // "a pinch took over", so an in-flight drag is abandoned instead
            // of being delivered as a tap.
            end_gesture(kActionCancel);
            s_pinching = true;
            s_prevPinchDist = dist;
            return;
        }
        if (s_prevPinchDist > 1.f && dist > 1.f) {
            const float factor = std::clamp(dist / s_prevPinchDist, 0.5f, 2.0f);
            if (std::fabs(factor - 1.f) > 0.001f) {
                companion::pinchZoom(factor);
                dualscreen::requestRedraw();
            }
        }
        s_prevPinchDist = dist;
        return;
    }
    s_pinching = false;

    // --- one finger or none ------------------------------------------------
    if (count == 0) {
        end_gesture(kActionUp);
        return;
    }

    float u = 0.f;
    float v = 0.f;
    const bool inside = map_to_companion(static_cast<int>(state.touches[0].x),
                                         static_cast<int>(state.touches[0].y), &u, &v);
    if (!inside) {
        // Started inside and dragged out: keep feeding MOVE so the donor's own
        // slop and drag logic decides what that means. Started outside: ignore.
        if (s_down) {
            // map_to_companion edge-clamps u/v even when it answers "outside",
            // so this is still a usable release point.
            s_lastU = u;
            s_lastV = v;
            companion::touchEvent(kActionMove, u, v);
            dualscreen::requestRedraw();
        }
        return;
    }

    s_lastU = u;
    s_lastV = v;
    if (!s_down) {
        s_down = true;
        companion::touchEvent(kActionDown, u, v);
    } else {
        companion::touchEvent(kActionMove, u, v);
    }
    // Interaction must never wait for the redraw cadence.
    dualscreen::requestRedraw();
}

/// Battery for the companion's status strip. Polled rather than pushed (Android
/// got a broadcast); once a second is plenty for a percentage, and psm is a
/// service call we should not make every frame.
void updateBattery() {
    static uint32_t s_lastPollMs = 0;
    const uint32_t nowMs = static_cast<uint32_t>(armTicksToNs(armGetSystemTick()) / 1000000ULL);
    if (s_lastPollMs != 0 && nowMs - s_lastPollMs < 1000) {
        return;
    }
    s_lastPollMs = nowMs;

    uint32_t percent = 0;
    PsmChargerType charger = PsmChargerType_Unconnected;
    const bool okPercent = R_SUCCEEDED(psmGetBatteryChargePercentage(&percent));
    const bool okCharger = R_SUCCEEDED(psmGetChargerType(&charger));
    if (okPercent) {
        companion::setBatteryStatus(static_cast<int>(percent),
                                    okCharger && charger != PsmChargerType_Unconnected);
    }
}

} // namespace dusk::switch_touch

#endif // __SWITCH__
