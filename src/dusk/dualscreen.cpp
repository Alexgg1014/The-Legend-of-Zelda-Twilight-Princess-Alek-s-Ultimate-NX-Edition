#include "dusk/dualscreen.h"
#include "dusk/session_log.hpp"

#include "dusk/companion.h"
#include "dusk/main.h"
#include "dusk/settings.h"
#include "d/d_com_inf_game.h"
#include "f_pc/f_pc_name.h"
#include "JSystem/J2DGraph/J2DOrthoGraph.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_mtx.h"

#include "data.hpp"
#include "dusk/logging.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <aurora/aux_window.hpp>
#include <aurora/tuning.h>
#include <dolphin/gx/GXAurora.h>

#include "dusk/perf.hpp"
#ifdef __SWITCH__
#include <aurora/aux_layout_switch.hpp>
#include <switch.h>
// The SD log lives on the platform side; this TU cannot include its header.
extern "C" void dusk_switch_log(const char* msg);
#endif
#include <aurora/gfx.hpp>

#if DUSK_COMPANION_CAPTURE
// Only the development capture path below uses miniz, and it ships disabled.
// The Switch build has no miniz, so including it unconditionally breaks a
// build that never calls it.
#include "miniz.h"
#endif

namespace dusk::dualscreen {
namespace {

bool s_active = false;
bool s_flipActive = false;
bool s_auxWindowFailed = false;
// Whether the boot/loading splash should draw this frame (dual screen on,
// but the dashboard has never presented yet).
bool s_splash = false;
// Written from the Android UI thread (display add/remove), read by the
// game thread's per-frame gates — hence atomic.
std::atomic<bool> s_everPresented{false};
// Set when the current scene change leaves gameplay (title / file select):
// the dashboard keeps drawing through the fade-out, and once the HUD meter
// dies the splash returns instead of a frozen last frame. Play-to-play
// stage transitions clear it, keeping the existing hold-last-frame skip.
bool s_leftGameplay = false;
// Redraw cadence. The companion is a status surface: redrawing it every frame
// spends GX draw work -- CPU, the co-limiter on this hardware -- on a picture
// that mostly has not changed. s_redrawTick counts frames; s_forceRedraw is
// the escape hatch interaction uses so the cadence never costs responsiveness.
u32 s_redrawTick = 0;
// Game frames elapsed since the previous redraw, and the running counter that
// feeds it. See the stride comment in beginHudCapture.
u32 s_framesSinceRedraw = 0;
u32 s_redrawStride = 1;
std::atomic<bool> s_forceRedraw{true};
// Decided once per frame in beginHudCapture, read by everything downstream
// inside the same frame -- dmapUpdate (via dMeter2_c::_draw, which runs
// between the two capture hooks) and endHudCapture itself.
bool s_redrawThisFrame = true;
// Low-health hearts pop-in latch (Cinematic only); see lowLifePopIn().
bool s_lowLifeLatch = false;
// Second physical display present? Android reports via setDisplayAvailable;
// desktop opens its own window on demand.
#if defined(TARGET_ANDROID) || defined(__ANDROID__) || defined(ANDROID)
std::atomic<bool> s_displayAvailable{false};
#else
std::atomic<bool> s_displayAvailable{true};
#endif

bool isEnabled() {
    // The logo scene draws its 2D lists through the same pass; only redirect
    // once actual gameplay is running. Without a physical second display
    // the setting is inert (single-screen devices keep their HUD).
    // "Gameplay is running" is asked TWO ways on purpose. IsGameLaunched is the
    // intended signal, but it is set in exactly two places, both inside
    // prelaunch.cpp — so any route that reaches gameplay without that screen
    // leaves it false forever, and the second screen then sits on the boot
    // splash for the whole session while the game plays normally on the main
    // display. companion::hudReady() is direct evidence: the game's own HUD
    // meter exists, which it does not during the logo scene, so it cannot
    // re-introduce the problem the flag was added to avoid.
    return getSettings().game.dualScreen.getValue() && s_displayAvailable &&
           (dusk::IsGameLaunched || companion::hudReady());
}

// The game leaves alpha writes disabled for most 2D drawing, so the capture's
// alpha channel stays at the pass-clear value (0) and alpha-blended consumers
// render it invisible. Stamp alpha=1 across the target, leaving RGB untouched,
// before resolving.
void stampOpaqueAlpha() {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE,
        GX_AF_NONE);
    GXSetChanMatColor(GX_COLOR0A0, {0xFF, 0xFF, 0xFF, 0xFF});
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_SET);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetZMode(GX_DISABLE, GX_ALWAYS, GX_DISABLE);
    GXSetCullMode(GX_CULL_NONE);
    GXSetClipMode(GX_CLIP_DISABLE);
    GXSetColorUpdate(GX_FALSE);
    GXSetAlphaUpdate(GX_TRUE);
    GXLoadPosMtxImm(cMtx_getIdentity(), GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3s16(-0x4000, -0x4000, 0);
    GXPosition3s16(0x4000, -0x4000, 0);
    GXPosition3s16(0x4000, 0x4000, 0);
    GXPosition3s16(-0x4000, 0x4000, 0);
    GXEnd();
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    dComIfGp_getCurrentGrafPort()->setup2D();
}

// Open or close the second OS window to match the setting, and honor a
// user-initiated close of that window.
void updateAuxWindow(bool enabled) {
#if defined(TARGET_ANDROID) || defined(__ANDROID__) || defined(ANDROID)
    // On Android the aux surface lifecycle is driven by DuskActivity's
    // Presentation on the secondary display; just blank it when disabled.
    if (!enabled) {
        aurora::auxwin::set_source({}, 0, 0);
    }
#else
    if (aurora::auxwin::consume_close_request()) {
        getSettings().game.dualScreen.setValue(false);
        enabled = false;
    }
    if (enabled && !aurora::auxwin::is_open() && !s_auxWindowFailed) {
        const aurora::auxwin::CreateInfo info{
            .title = "Dusklight — Second Screen",
            .width = FB_WIDTH * 2,
            .height = FB_HEIGHT * 2,
            .posX = getSettings().game.dualScreenPosX.getValue(),
            .posY = getSettings().game.dualScreenPosY.getValue(),
            .displayIndex = getSettings().game.dualScreenDisplay.getValue(),
        };
        s_auxWindowFailed = !aurora::auxwin::create(info);
    } else if (!enabled) {
        if (aurora::auxwin::is_open()) {
            aurora::auxwin::destroy();
        }
        s_auxWindowFailed = false;
    }
#endif
}

// Canvas geometry limits: logical canvas width clamp and the accepted
// native aux-surface size range (fallback to 2x supersample outside it).
constexpr u32 kMinCanvasW = 320;
constexpr u32 kMaxCanvasW = 1024;
constexpr u32 kMinNativeDim = 320;
constexpr u32 kMaxNativeDim = 2048;

// Match the dashboard canvas to the second screen's aspect ratio (e.g. 8:7
// on the AYN Thor) so the blit fills it edge to edge without skew, and
// render at the aux panel's native resolution (1:1 blit, no scaling blur);
// logical layout units stay at canvas size via the decoupled ortho
// projection. Falls back to 2x supersample when the surface size is
// unknown or out of range.
void computeAuxCanvas(u32& canvasW, u32& canvasH, u32& texW, u32& texH) {
    f32 aspect = (f32)FB_WIDTH / (f32)FB_HEIGHT;
    u32 auxWidth = 0;
    u32 auxHeight = 0;
    if (aurora::auxwin::get_surface_size(&auxWidth, &auxHeight) && auxHeight != 0) {
        aspect = (f32)auxWidth / (f32)auxHeight;
    }
    canvasH = FB_HEIGHT;
    canvasW = (u32)((f32)canvasH * aspect + 0.5f);
    if (canvasW < kMinCanvasW) {
        canvasW = kMinCanvasW;
    } else if (canvasW > kMaxCanvasW) {
        canvasW = kMaxCanvasW;
    }
    texW = canvasW * 2;
    texH = canvasH * 2;
    if (auxWidth >= kMinNativeDim && auxHeight >= kMinNativeDim &&
        auxWidth <= kMaxNativeDim && auxHeight <= kMaxNativeDim)
    {
        texW = auxWidth;
        texH = auxHeight;
    }
}

#if DUSK_COMPANION_CAPTURE
// Pending screenshot destination. Written from whichever thread asked (the
// Android UI thread for the DUMP broadcast), read on the game thread.
std::mutex s_shotMutex;
std::string s_shotPath;
bool s_shotPending = false;
// Frames the request has been outstanding. The capture can be abandoned
// without ever completing (surface torn down, display detached), and without
// this the request latched forever and polled silently for the rest of the
// session.
int s_shotWaited = 0;
constexpr int kShotTimeoutFrames = 240;
#endif

}  // namespace

#if DUSK_COMPANION_CAPTURE
void requestScreenshot(const char* path) {
    {
        std::lock_guard lock{s_shotMutex};
        if (path != nullptr && path[0] != '\0') {
            s_shotPath = path;
        } else {
            // Desktop default; on Android the caller passes the app's
            // external files dir, which is the only place adb can reach.
            s_shotPath = (data::configured_data_path() / "companion-screenshot.png").string();
        }
        s_shotPending = true;
        s_shotWaited = 0;
    }
    aurora::auxwin::request_capture();
}

// Writes the PNG once the read-back lands; no-op while nothing is pending.
// File-local: one caller (beginHudCapture), no header declaration.
static void pollScreenshot() {
    {
        std::lock_guard lock{s_shotMutex};
        if (!s_shotPending) {
            return;
        }
    }
    std::vector<u8> pixels;
    u32 width = 0;
    u32 height = 0;
    if (!aurora::auxwin::take_capture(pixels, &width, &height) || width == 0 || height == 0) {
        std::lock_guard lock{s_shotMutex};
        if (++s_shotWaited > kShotTimeoutFrames) {
            s_shotPending = false;
            DuskLog.warn("companion screenshot: no frame captured, giving up");
        }
        return;  // still in flight
    }
    std::string path;
    {
        std::lock_guard lock{s_shotMutex};
        s_shotPending = false;
        path = s_shotPath;
    }
    // Level 1, not 6: this encodes a full-panel RGBA frame (~4 MB at 1240x1080)
    // synchronously on the GAME thread inside beginHudCapture, so the hitch is
    // paid by the running game. Acceptable for a debug aid, but not worth
    // several hundred extra milliseconds for a smaller file.
    size_t pngSize = 0;
    void* png = tdefl_write_image_to_png_file_in_memory_ex(pixels.data(), (int)width, (int)height,
        4, &pngSize, 1, MZ_FALSE);
    if (png == NULL) {
        DuskLog.warn("companion screenshot: PNG encode failed");
        return;
    }
    FILE* file = fopen(path.c_str(), "wb");
    if (file != NULL) {
        const bool ok = fwrite(png, 1, pngSize, file) == pngSize;
        fclose(file);
        if (ok) {
            DuskLog.info("companion screenshot: wrote {}x{} to {}", width, height, path);
        } else {
            DuskLog.warn("companion screenshot: short write to {}", path);
        }
    } else {
        DuskLog.warn("companion screenshot: cannot open {}", path);
    }
    mz_free(png);
}

#endif  // DUSK_COMPANION_CAPTURE

void beginHudCapture() {
#if DUSK_COMPANION_CAPTURE
    pollScreenshot();
#endif
    const bool wanted = getSettings().game.dualScreen.getValue() && s_displayAvailable;
    const bool enabled = isEnabled();
    {
        int divisor = getSettings().game.dualScreenRedrawDivisor.getValue();
        if (divisor < 1) {
            divisor = 1;
        } else if (divisor > 12) {
            divisor = 12;
        }
        const bool forced = s_forceRedraw.exchange(false);
        s_redrawThisFrame = forced || (s_redrawTick % (u32)divisor) == 0;
        ++s_redrawTick;
        // How many GAME frames this redraw has to account for.
        //
        // Every animation ramp in the companion advances once per call to
        // drawDashboard -- which is once per REDRAW, not once per frame. With
        // the cadence at 6 that made every page transition, zoom, fade and
        // ghost fly-out run at a sixth of its intended speed: the game stayed
        // at full speed and only the companion's animations crawled, which is
        // exactly how it was reported. The ramps are per-frame rates, so they
        // have to be told how many frames to cover. Never zero -- a stride of
        // 0 would freeze them outright.
        ++s_framesSinceRedraw;
        if (s_redrawThisFrame) {
            s_redrawStride = s_framesSinceRedraw;
            s_framesSinceRedraw = 0;
        }
    }
#ifdef __SWITCH__
    // Push the panel split to aurora every frame rather than caching it, so a
    // settings change takes effect on the next frame with no state to
    // invalidate. Flip Grip needs the console physically turned, so it is
    // handheld only -- docked falls back to Normal.
    {
        const bool flip = flipWanted();
        aurora::auxwin::layout::set_mode(flip ? aurora::auxwin::layout::kFlip270
                                              : aurora::auxwin::layout::kNormal);
        s_flipActive = flip;
        // H33: Flip Grip forces 4:3 while it lasts; leaving it hands the saved preference back.
        applyAspectPolicy();
    }
    // Same reasoning, same place: pushed every frame so the settings row takes
    // effect immediately. get_surface_size reports the capped size, and
    // computeAuxCanvas below renders the dashboard at exactly what it reports,
    // so this IS the companion's render resolution. aurora clamps it.
    aurora::auxwin::set_target_long_side_cap(
        (uint32_t)getSettings().game.dualScreenResolution.getValue());
#endif
    updateAuxWindow(wanted);
    s_active = enabled;
    // Boot/loading: keep the second screen on the branded splash instead of
    // black until the dashboard has real content. Leaving gameplay (quit to
    // title, game over) re-arms it — endHudCapture keeps the dashboard while
    // the meter still lives (the fade-out), then falls back to the splash.
    s_splash = wanted && (!s_everPresented || s_leftGameplay);

    // Reconcile the Java-side mirror with the config. The settings toggle
    // publishes on change, so this is only for the cases the toggle never saw:
    // a first run, or a config.json carried in from another install (a moved
    // data folder) whose swap value the SharedPreferences file knows nothing
    // about. Compare-and-skip, so the steady state costs one bool test.
    {
        const bool swap = getSettings().game.dualScreenSwap.getValue();
        static int s_lastPublished = -1;
        if (s_lastPublished != (swap ? 1 : 0)) {
            s_lastPublished = swap ? 1 : 0;
            publishSwapPreference(swap);
        }
    }

    // One line whenever the second screen's state changes. "The bottom screen
    // just shows the logo" is impossible to triage from a description, because
    // three very different faults look identical from outside:
    //   displayAvailable=0 -> nothing is presented; the user is seeing the
    //                         HANDHELD's own app icon, not ours
    //   launched=0         -> our splash forever (isEnabled needs IsGameLaunched)
    //   hudReady=0         -> the game's HUD meter does not exist
    // Logged on change only, so it costs nothing per frame.
    {
        const bool ready = companion::hudReady();
        const int state = (wanted ? 1 : 0) | (enabled ? 2 : 0) | (ready ? 4 : 0) |
            (s_splash ? 8 : 0) | (s_displayAvailable ? 16 : 0) |
            (s_everPresented ? 32 : 0) | (s_leftGameplay ? 64 : 0);
        static int s_lastState = -1;
        if (state != s_lastState) {
            s_lastState = state;
            DuskLog.info("dualscreen: displayAvailable={} setting={} launched={} wanted={} "
                         "enabled={} hudReady={} splash={} everPresented={} leftGameplay={} swap={}",
                (bool)s_displayAvailable, getSettings().game.dualScreen.getValue(),
                (bool)dusk::IsGameLaunched, wanted, enabled, ready, s_splash,
                (bool)s_everPresented, s_leftGameplay,
                getSettings().game.dualScreenSwap.getValue());
        }
    }

    // Low-health hearts pop-in (Cinematic only). Life is 4 units/heart, max
    // is 5 units/heart (drawLife's own divisors). Threshold scales with max
    // hearts, clamped to 1..2; the release point is one full heart higher so
    // the boundary can't flicker.
    if (!enabled || mainHudRestored() || !companion::hudReady()) {
        s_lowLifeLatch = false;
    } else {
        int showHearts = (dComIfGs_getMaxLife() / 5) / 4;
        if (showHearts < 1) {
            showHearts = 1;
        } else if (showHearts > 2) {
            showHearts = 2;
        }
        const u16 life = dComIfGs_getLife();
        if (life <= (u16)(showHearts * 4)) {
            s_lowLifeLatch = true;
        } else if (life > (u16)((showHearts + 1) * 4)) {
            s_lowLifeLatch = false;
        }
    }

    if (enabled) {
        companion::update();
    }
}

void endHudCapture() {
    const bool splash = s_splash && (!s_active || !companion::hudReady());
    s_splash = false;
    if (!s_active && !splash) {
        return;
    }
    s_active = false;

#ifdef __SWITCH__
    // Nobody is looking: in Normal mode the companion is summoned, and while it
    // is dismissed the compositor draws the game alone -- aux_window_switch's
    // rect resolver returns false on !normal_overlay_visible(). The PRESENT
    // side has always known that. The CAPTURE side did not, so a player in
    // Normal mode with the panel dismissed still paid for the whole offscreen
    // render, the GX drain and the resolve, every cadence tick, to produce a
    // texture that was then never sampled.
    //
    // Flip Grip is exempt because the panel is always on screen there, which is
    // also why this was invisible in testing: the mode being measured never had
    // a hidden companion.
    if (aurora::auxwin::layout::mode() != aurora::auxwin::layout::kFlip270 &&
        !aurora::auxwin::layout::normal_overlay_visible()) {
        return;
    }
#endif

    // The dim is applied by the aux window to the PRESENT blit, not painted
    // into the dashboard. That is the whole point: on the frames below where
    // the capture is skipped, the second screen keeps presenting its last
    // texture and the fade still advances over it. The splash owns its own
    // look and is never dimmed.
    if (splash) {
        // The splash owns the panel and carries no fade of its own. Clear the
        // dashboard's ramp too, or the level it had already reached is still
        // sitting there when the splash hands back and snaps a lit logo to
        // black in one frame.
        companion::resetDim();
        aurora::auxwin::set_dim(0.0f);
    } else {
        aurora::auxwin::set_dim(companion::currentDim());
    }

    // During stage transitions the HUD meter is destroyed and rebuilt; skip
    // the capture entirely so the second screen keeps its last frame instead
    // of flashing a half-empty dashboard (the splash covers first boot).
    if (!splash && !companion::hudReady()) {
        return;
    }

    // Redraw cadence (decided in beginHudCapture). Skipping lands on the same
    // path the donor already uses for stage transitions: no set_source this
    // frame, so the panel keeps presenting what it has while the dim above
    // keeps advancing over it. The splash is exempt -- it is brief and its
    // fade must not stutter.
    //
    // Safe only because the Switch compositor owns its own copy of the
    // dashboard; gfx::resolve_pass views are pooled and valid for the current
    // frame only. See aux_window_switch.cpp.
    if (!splash && !s_redrawThisFrame) {
        return;
    }

    profBegin();
    // aleks2 (H35): every FIFO drain of the capture (create_pass, drain_fifo, resolve_pass)
    // is counted as companion capture in the benchmark's drain telemetry. Accounting only.
    struct CompanionDrainTag {
        AuroraFifoDrainSite previous = aurora_gx_fifo_set_drain_tag(AURORA_FIFO_DRAIN_COMPANION);
        ~CompanionDrainTag() { aurora_gx_fifo_set_drain_tag(previous); }
    } drainTag;
    u32 canvasW, canvasH, texW, texH;
    computeAuxCanvas(canvasW, canvasH, texW, texH);
    if (!aurora::gfx::create_pass(texW, texH)) {
        return;
    }
    profMark(kProfPass);

    // Pixel-exact 2D context for the dashboard, independent of the main
    // screen's (possibly widescreen) ortho extents.
    J2DGrafContext* prevPort = dComIfGp_getCurrentGrafPort();
    if (prevPort == NULL) {
        // Should never happen during mDoGph_Painter — bail out clean.
        aurora::gfx::ResolvedTargets discarded;
        aurora::gfx::resolve_pass({.color = false, .depth = false}, discarded);
        return;
    }
    // The ortho context provides the canvas-unit projection; the actual
    // full-texture viewport/scissor are applied via the aurora passthrough
    // commands, which bypass the logical-fb mapping deterministically.
    J2DOrthoGraph ortho(0.0f, 0.0f, (f32)canvasW, (f32)canvasH, -1.0f, 1.0f);
    companion::setNativeCanvas(texW, texH, (f32)texH / (f32)canvasH);
    dComIfGp_setCurrentGrafPort(&ortho);
    ortho.setPort();
    companion::applyNativeViewport();

    if (splash) {
        companion::drawSplash((f32)canvasW, (f32)canvasH);
    } else {
        companion::drawDashboard((f32)canvasW, (f32)canvasH);
        s_everPresented = true;
    }
    stampOpaqueAlpha();

    dComIfGp_setCurrentGrafPort((J2DOrthoGraph*)prevPort);

    // Drained explicitly so it can be timed apart from everything else
    // resolve_pass does. resolve_pass drains as its first act anyway, so this
    // moves no work -- it only makes the split visible. Hardware put the two
    // together at 13-27 ms per redraw, an order of magnitude above the ~1.6 ms
    // of recording, and the fix depends entirely on which half that is:
    // translating a recorded draw list means draw fewer things, while snapshot
    // and pass bookkeeping means something else entirely.
    aurora::gfx::drain_fifo();
    profMark(kProfDrain);

    aurora::gfx::ResolvedTargets targets;
    if (aurora::gfx::resolve_pass({.color = true, .depth = false}, targets) && targets.color) {
        if (aurora::auxwin::is_open()) {
            aurora::auxwin::set_source(targets.color, targets.width, targets.height);
        }
    }
    // Closed after the resolve. This is the section the first profiler could
    // not see AT ALL, and in a GX-to-Vulkan port it is exactly where a recorded
    // draw list turns into real work. A small number here would be evidence
    // that the draw calls themselves are the cost; a large one means the
    // opposite, and that no amount of trimming the dashboard's contents helps.
    profMark(kProfResolve);
    profEndBatch();

    prevPort->setPort();  // never NULL — the capture bails out above
}

#if !defined(TARGET_ANDROID) && !defined(__ANDROID__) && !defined(ANDROID)
// Swapping which panel each WINDOW opens on is an Android idea; a desktop aux
// window has no launcher to hand the choice to. Defined here so the call sites
// need no platform guard of their own — android_aux_display.cpp supplies the
// real one.
void publishSwapPreference(bool) {}
#endif

bool hudOnCompanion() {
    return getSettings().game.dualScreen.getValue() && s_displayAvailable;
}

bool flipActive() {
    return s_flipActive;
}

bool flipWanted() {
#ifdef __SWITCH__
    return getSettings().game.dualScreen.getValue() && s_displayAvailable &&
           getSettings().game.dualScreenLayout.getValue() == kDualLayoutFlip &&
           !dusk::perf::is_docked();
#else
    return false;
#endif
}

bool effectiveAspectLock() {
    return getSettings().video.lockAspectRatio.getValue() || flipWanted();
}

void applyAspectPolicy() {
    static int s_lastPushed = -1;
    const int lock = effectiveAspectLock() ? 1 : 0;
    if (lock == s_lastPushed) {
        return;
    }
    s_lastPushed = lock;
    AuroraSetViewportPolicy(lock != 0 ? AURORA_VIEWPORT_FIT : AURORA_VIEWPORT_STRETCH);
}

bool mainHudRestored() {
    return hudOnCompanion() &&
           getSettings().game.dualScreenHudMode.getValue() == kDualHudFunctional;
}

bool mainHudActive() {
    return !hudOnCompanion() || mainHudRestored();
}

bool lowLifePopIn() {
    return s_lowLifeLatch;
}

void onSceneChangeReq(short procName) {
    s_leftGameplay = procName != fpcNm_PLAY_SCENE_e;
#ifdef __SWITCH__
    dusk::session_log::stampStage(dComIfGp_getNextStageName(), procName);
#endif
}

bool leftGameplay() {
    return s_leftGameplay;
}

#ifdef __SWITCH__
namespace {
u64 s_profLast = 0;
u64 s_profAccum[kProfSlotCount] = {};
int s_profBatch = 0;
constexpr int kProfBatchSize = 60;
const char* const kProfNames[kProfSlotCount] = {
    "pass", "chrome", "window", "page", "panels", "tabs", "touch", "drain", "resolve",
};
}  // namespace

void profBegin() { s_profLast = armGetSystemTick(); }

void profMark(int slot) {
    const u64 now = armGetSystemTick();
    if (slot >= 0 && slot < kProfSlotCount) {
        s_profAccum[slot] += now - s_profLast;
    }
    s_profLast = now;
}

void profEndBatch() {
    if (++s_profBatch < kProfBatchSize) {
        return;
    }
    s_profBatch = 0;
    const u64 freq = armGetSystemTickFreq();
    // Microseconds per redraw, integer throughout -- this runs on the game
    // thread and must stay too cheap to think about.
    char line[256];
    int n = snprintf(line, sizeof line, "[comp-prof] over %d redraws (avg us):", kProfBatchSize);
    u64 total = 0;
    for (int i = 0; i < kProfSlotCount; i++) {
        const u64 us = freq > 0 ? (s_profAccum[i] * 1000000ull) / (freq * kProfBatchSize) : 0;
        total += us;
        s_profAccum[i] = 0;
        if (n >= 0 && n < static_cast<int>(sizeof line)) {
            n += snprintf(line + n, sizeof line - n, " %s=%llu", kProfNames[i],
                          static_cast<unsigned long long>(us));
        }
    }
    if (n >= 0 && n < static_cast<int>(sizeof line)) {
        snprintf(line + n, sizeof line - n, " | total=%llu\n",
                 static_cast<unsigned long long>(total));
    }
    ::dusk_switch_log(line);
}
#endif

void requestRedraw() { s_forceRedraw.store(true, std::memory_order_relaxed); }

bool isRedrawFrame() { return s_redrawThisFrame; }

u32 redrawStride() { return s_redrawStride < 1u ? 1u : s_redrawStride; }

void setDisplayAvailable(bool available) {
    s_displayAvailable = available;
    if (!available) {
        s_everPresented = false;
    }
}

}  // namespace dusk::dualscreen
