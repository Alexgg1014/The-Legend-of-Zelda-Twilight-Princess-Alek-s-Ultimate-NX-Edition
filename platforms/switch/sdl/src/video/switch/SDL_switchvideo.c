/*
  aleks2: SDL3 video driver for Horizon, written for this port.

  This replaces the EGL-based driver in devkitPro's SDL branch. Here the console
  has exactly one thing to scan out -- libnx's default NWindow -- and the game
  does not draw into it through SDL at all: Dawn takes the NWindow itself, as a
  VK_NN_vi_surface swapchain. So the driver has nothing to do with GL or EGL and
  does not link them. All it provides is what SDL's own window logic needs to
  hold a window: one display, one fullscreen-only window, the dock/undock
  switch, and the applet's request to quit.

  Licensed like the rest of SDL (zlib).
*/
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_SWITCH

#include <switch.h>

#include "../SDL_sysvideo.h"
#include "../../events/SDL_events_c.h"
#include "../../events/SDL_windowevents_c.h"

// Handheld scans out 720p, a docked console 1080p. These are the only two
// sizes the system will ever composite.
typedef struct PanelSize
{
    int w;
    int h;
} PanelSize;

static PanelSize QueryPanel(void)
{
    const PanelSize docked = { 1920, 1080 };
    const PanelSize handheld = { 1280, 720 };
    return (appletGetOperationMode() == AppletOperationMode_Console) ? docked : handheld;
}

static SDL_DisplayMode ModeFor(PanelSize panel)
{
    SDL_DisplayMode mode;
    SDL_zero(mode);
    mode.w = panel.w;
    mode.h = panel.h;
    mode.refresh_rate = 60.0f;
    mode.format = SDL_PIXELFORMAT_RGBA8888;
    return mode;
}

// How often the applet is asked whether the console was docked or undocked.
#define DOCK_CHECK_INTERVAL_NS SDL_MS_TO_NS(500)

struct SDL_VideoData
{
    SDL_DisplayID display;
    PanelSize shown;       // what the display currently advertises
    Uint64 next_check_ns;  // earliest time of the next dock check
};

struct SDL_WindowData
{
    int unused;  // SDL wants a per-window record; there is nothing to keep in it
};

static bool NXVideoInit(SDL_VideoDevice *device)
{
    SDL_VideoData *data = device->internal;
    const PanelSize panel = QueryPanel();
    const SDL_DisplayMode mode = ModeFor(panel);

    data->display = SDL_AddBasicVideoDisplay(&mode);
    if (data->display == 0) {
        return false;
    }
    data->shown = panel;
    data->next_check_ns = SDL_GetTicksNS() + DOCK_CHECK_INTERVAL_NS;
    return true;
}

static void NXVideoQuit(SDL_VideoDevice *device)
{
    (void)device;
}

static bool NXCreateWindow(SDL_VideoDevice *device, SDL_Window *window, SDL_PropertiesID props)
{
    (void)props;

    SDL_WindowData *wd = (SDL_WindowData *)SDL_calloc(1, sizeof(*wd));
    if (wd == NULL) {
        return false;
    }
    window->internal = wd;

    // There is one surface and it has the size of the panel, whatever size was
    // asked for.
    const PanelSize panel = device->internal->shown;
    window->w = panel.w;
    window->h = panel.h;

    SDL_SetWindowFocusable(window, true);
    SDL_SetKeyboardFocus(window);
    SDL_SendWindowEvent(window, SDL_EVENT_WINDOW_RESIZED, panel.w, panel.h);
    return true;
}

static void NXDestroyWindow(SDL_VideoDevice *device, SDL_Window *window)
{
    (void)device;
    SDL_free(window->internal);
    window->internal = NULL;
}

// The panel changed (dock or undock): advertise the new size and move every
// window to it.
static void ApplyPanel(SDL_VideoDevice *device, PanelSize panel)
{
    SDL_VideoData *data = device->internal;
    SDL_VideoDisplay *display = SDL_GetVideoDisplay(data->display);
    if (display == NULL) {
        return;
    }

    SDL_DisplayMode mode = ModeFor(panel);
    mode.format = display->desktop_mode.format;
    mode.refresh_rate = display->desktop_mode.refresh_rate;

    SDL_ResetFullscreenDisplayModes(display);
    SDL_AddFullscreenDisplayMode(display, &mode);
    SDL_SetDesktopDisplayMode(display, &mode);
    SDL_SetCurrentDisplayMode(display, &display->desktop_mode);
    data->shown = panel;

    for (SDL_Window *w = device->windows; w != NULL; w = w->next) {
        SDL_SetWindowSize(w, panel.w, panel.h);
    }
}

static void NXPumpEvents(SDL_VideoDevice *device)
{
    // The applet asks the program to leave by making this return false.
    if (!appletMainLoop()) {
        SDL_SendQuit();
        return;
    }

    SDL_VideoData *data = device->internal;
    const Uint64 now = SDL_GetTicksNS();
    if (now < data->next_check_ns) {
        return;
    }
    data->next_check_ns = now + DOCK_CHECK_INTERVAL_NS;

    const PanelSize panel = QueryPanel();
    if (panel.w != data->shown.w || panel.h != data->shown.h) {
        ApplyPanel(device, panel);
    }
}

static void NXFreeDevice(SDL_VideoDevice *device)
{
    SDL_free(device->internal);
    SDL_free(device);
}

static SDL_VideoDevice *NXCreateDevice(void)
{
    SDL_VideoDevice *device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(*device));
    if (device == NULL) {
        return NULL;
    }
    device->internal = (SDL_VideoData *)SDL_calloc(1, sizeof(*device->internal));
    if (device->internal == NULL) {
        SDL_free(device);
        return NULL;
    }

    device->VideoInit = NXVideoInit;
    device->VideoQuit = NXVideoQuit;
    device->CreateSDLWindow = NXCreateWindow;
    device->DestroyWindow = NXDestroyWindow;
    device->PumpEvents = NXPumpEvents;
    device->device_caps = VIDEO_DEVICE_CAPS_FULLSCREEN_ONLY;
    device->free = NXFreeDevice;
    return device;
}

VideoBootStrap SWITCH_bootstrap = {
    .name = "switch",
    .desc = "Horizon (libnx) window, no GL",
    .create = NXCreateDevice,
};

#endif // SDL_VIDEO_DRIVER_SWITCH
