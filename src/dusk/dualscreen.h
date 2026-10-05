#pragma once

// Companion frame capture (the on-device screenshot aid). OFF by default: it
// is a development tool, and its Android trigger is a broadcast receiver that
// has to be exported to be reachable from `adb`, so it must not ship.
//
// Flip to 1 (here or with -DDUSK_COMPANION_CAPTURE=1) to build it in; that is
// the intended way to use it, since build-dusklight-apk.sh produces a RELEASE
// apk and there is no separate debug build of these files.
//
// NOTE: do NOT write `DEBUG` here. Every dusk translation unit compiles with
// NDEBUG=1 and no -DDEBUG, and include/global.h:31 then defines DEBUG as 0 —
// so `#define DUSK_COMPANION_CAPTURE DEBUG` was silently 0 in *every* build
// and compiled the whole feature away, including the JNI entry point the
// Android receiver calls (it failed with UnsatisfiedLinkError and a swallowed
// log line, which is exactly why it went unnoticed).
#ifndef DUSK_COMPANION_CAPTURE
#define DUSK_COMPANION_CAPTURE 0
#endif

namespace dusk::dualscreen {

// Renders the companion dashboard (HUD, map, inventory, ...) into an offscreen
// texture for the second screen when game.dualScreen is enabled. Call from
// mDoGph_Painter, bracketing the 2D draw-list flush; the dashboard is drawn
// during endHudCapture, after the main screen's own 2D pass.
void beginHudCapture();
void endHudCapture();

#if DUSK_COMPANION_CAPTURE
// Ask for the next presented companion frame to be written to `path` as a
// PNG (exactly what the panel shows: letterbox, dim and all). One-shot and
// asynchronous — the file appears a frame or two later. Verification aid:
// on Android it is wired to the DUMP broadcast, on desktop to F12.
// Passing an empty path uses the default location.
void requestScreenshot(const char* path);
#endif

// Report whether a physical second display exists (Android reports this
// from its DisplayManager; defaults to available on desktop). When
// unavailable, the dual-screen setting is inert and the HUD stays on the
// main screen.
void setDisplayAvailable(bool available);

/// Force the next frame to redraw the companion, bypassing the redraw
/// cadence. Call it whenever the user does something that must show up at
/// once -- a tap, a page change -- so the cadence never costs responsiveness.
/// Cheap and thread-safe; it only sets a flag.
void requestRedraw();

/// Game frames covered by the redraw happening now: 1 when the companion is
/// drawn every frame, up to the divisor otherwise. The companion's animation
/// ramps are per-FRAME rates evaluated once per REDRAW, so they need this to
/// stay at their intended speed instead of running `divisor` times too slow.
/// Never returns 0.
u32 redrawStride();

/// Whether the companion is being redrawn this frame. Anything that only
/// feeds the dashboard -- the live dungeon-map render, for one -- is pure
/// waste on the frames this returns false, because nothing will sample it.
/// Decided once in beginHudCapture and stable for the rest of the frame.
bool isRedrawFrame();

// --- Companion capture profiler (Switch only) -------------------------------
//
// The first version of this only wrapped drawDashboard, and its answer -- 94%
// of the cost in one undifferentiated "layout" blob -- was true but useless,
// and it also MISSED the render pass either side of the draw. create_pass and
// resolve_pass live in endHudCapture, outside anything drawDashboard can see,
// and this port translates GX to Vulkan, so the resolve is exactly where a
// recorded draw list turns into real work. Measuring only the recording and
// concluding the companion is cheap would be the same class of mistake as
// reading the clock request instead of the clock.
//
// So the whole capture is one timeline now, split where a fix could actually
// go. profBegin() starts it, profMark() closes the section that just ran, and
// profEndBatch() counts one redraw and logs the average every 60.
enum CompanionProfSlot {
    kProfPass = 0,     // create_pass: getting the offscreen target
    kProfChrome,       // state reset, backdrop, top bar
    kProfWindow,       // the content window's own frame and background
    kProfPage,         // the page body inside it -- map, items, collect, guide
    kProfPanels,       // corner boxes, left column, the I/II item buttons
    kProfTabs,         // tab strip and the floor picker
    kProfTouch,        // hit-testing and the drag ghost
    kProfDrain,        // pushing the recorded GX list through the command processor
    kProfResolve,      // the snapshot, the pass bookkeeping and set_source
    kProfSlotCount,
};

#ifdef __SWITCH__
void profBegin();
void profMark(int slot);
void profEndBatch();
#else
inline void profBegin() {}
inline void profMark(int /*slot*/) {}
inline void profEndBatch() {}
#endif

// Android: copy the Swap Screens setting into SharedPreferences, where
// DuskLauncherActivity can read it. The launcher has to choose a display
// BEFORE the native library exists, so it cannot ask the config — and having
// Java parse config.json instead would duplicate the data-folder path logic,
// which is exactly the drift that once sent saved guide pages somewhere the
// importer never looked. One-way mirror; config.json stays authoritative.
// No-op off Android.
void publishSwapPreference(bool swapped);

// True when the HUD actually lives on the second screen this frame: the
// setting is on AND a physical second display exists. Game-side gates must
// use this (never the raw setting) so single-screen devices keep their HUD.
// Stays the gate for everything that is on the companion in BOTH modes
// (rupees/keys, minimap, X/Y buttons).
bool hudOnCompanion();

// True while the Switch panel is split Flip Grip style (companion on, Flip
// layout, handheld). This is the value actually pushed to aurora this frame,
// so the docked fallback to Normal reads false here. Always false off Switch.
bool flipActive();

// H33: whether Flip Grip is in effect, from the settings alone (companion on, Flip layout,
// handheld, a display available). Unlike flipActive() it is right before the first frame and
// in the pre-launch screen, so startup and the resolution policy can use it. Always false off
// Switch.
bool flipWanted();

// H33: the aspect lock the picture really uses: the user's saved preference OR Flip Grip.
// Flip Grip is built around the game's native 4:3 picture, so it forces 4:3 while it is in
// effect; leaving it hands the saved preference back. Nothing is written: video.lockAspectRatio
// is never touched by entering or leaving Flip Grip, so there is nothing to restore.
bool effectiveAspectLock();

// Pushes effectiveAspectLock() to the viewport policy when it changed since the last push.
// Cheap and idempotent: safe every frame, from startup and from settings handlers.
void applyAspectPolicy();

// True when the HUD is on the companion AND the mode moves the
// gameplay-critical subset back to the main screen (Functional): hearts,
// vessel of light, A/B/Z + Midna, d-pad and the special-action panels.
// (The lantern oil gauge and oxygen bar stay on the companion's status strip
// in both modes.) Those elements are then drawn ONLY on the main screen —
// the companion stops drawing them. Game-side gates must use this helper
// rather than reading game.dualScreenHudMode directly, for the same reason
// hudOnCompanion() exists.
bool mainHudRestored();

// True when the status cluster (hearts, A/B/Z, d-pad, contextual panels)
// renders on the MAIN screen this frame: dual-screen off/unavailable, or the
// Functional mode. Exactly `!hudOnCompanion() || mainHudRestored()` — use
// this instead of re-deriving that compound at each gate.
bool mainHudActive();

// Cinematic safety net: latched true while Link's health is low enough that
// the hearts pop back onto the MAIN screen (with hysteresis, so the boundary
// doesn't flicker). Always false in Functional / single-screen — hearts are
// already there.
bool lowLifePopIn();

// Scene-change hook (called from fopScnM_ChangeReq): any change to a
// non-play scene (quit to title, game over to menu, file select) sends the
// companion back to the boot splash once the HUD meter dies; play-to-play
// stage transitions keep the last dashboard frame instead.
void onSceneChangeReq(short procName);

// True while the most recently requested scene is NOT the play scene — quit
// to title, game over, file select. NOTE: a location change requests
// PLAY_SCENE again (d_s_play.cpp), so this stays FALSE throughout one; the
// dim's load latch keys on fopOvlpM_IsDoingReq for that.
bool leftGameplay();

}  // namespace dusk::dualscreen
