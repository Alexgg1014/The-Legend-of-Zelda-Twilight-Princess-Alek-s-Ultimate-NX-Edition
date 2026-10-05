/*
  aleks2: SDL3 joystick driver for Horizon, written for this port.

  The console's controllers all feed libnx's "default" pad, which merges handheld
  mode, a Pro Controller, a Joy-Con pair and a single Joy-Con held sideways into
  one player-one pad. The game reads player one and nothing else, so SDL gets one
  joystick, always present, and the question of which hardware is behind it
  stays with the system.

  Rumble and motion do not go through SDL on this console; they use libnx
  directly (extern/aurora/lib/device_switch.cpp). Everything here returns
  "unsupported" for them.

  Licensed like the rest of SDL (zlib).
*/
#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_SWITCH

#include <switch.h>

#include "../SDL_sysjoystick.h"

// How many controllers Horizon may keep connected while this program runs. It is
// not how many players there are: the system refuses a pad beyond the number
// declared before the program ever sees it, so with one declared, a Joy-Con will
// not turn on while a Pro Controller is paired. The game still reads only the
// merged player-one pad.
#define NX_CONNECTABLE_PADS 4

// Joystick buttons, in the order SDL numbers them. The first sixteen are the
// physical controls; the last four repeat the shoulder buttons so the gamepad
// mapping can hand them out as MISC1..MISC4 as well. NXJoystickGetGamepadMapping
// below refers to these by index.
static const u64 k_buttons[] = {
    HidNpadButton_A,     HidNpadButton_B,     HidNpadButton_X,      HidNpadButton_Y,
    HidNpadButton_L,     HidNpadButton_R,     HidNpadButton_ZL,     HidNpadButton_ZR,
    HidNpadButton_Minus, HidNpadButton_Plus,  HidNpadButton_StickL, HidNpadButton_StickR,
    HidNpadButton_Up,    HidNpadButton_Down,  HidNpadButton_Left,   HidNpadButton_Right,
    HidNpadButton_L,     HidNpadButton_R,     HidNpadButton_ZL,     HidNpadButton_ZR,
};

enum
{
    kButtonCount = SDL_arraysize(k_buttons),
    kAxisCount = 4,
};

static struct
{
    PadState pad;
    u64 held;  // buttons seen at the previous update
} g_nx;

static bool NXJoystickInit(void)
{
    padConfigureInput(NX_CONNECTABLE_PADS, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_nx.pad);
    g_nx.held = 0;
    SDL_PrivateJoystickAdded(1);
    return true;
}

static int NXJoystickGetCount(void)
{
    return 1;
}

static void NXJoystickDetect(void)
{
}

static bool NXJoystickIsDevicePresent(Uint16 vendor_id, Uint16 product_id, Uint16 version, const char *name)
{
    return false;  // no other driver's device is ours
}

static const char *NXJoystickGetDeviceName(int index)
{
    return "Nintendo Switch Controller";
}

static const char *NXJoystickGetDevicePath(int index)
{
    return NULL;
}

static int NXJoystickGetDeviceSteamVirtualGamepadSlot(int index)
{
    return -1;
}

static int NXJoystickGetDevicePlayerIndex(int index)
{
    return -1;
}

static void NXJoystickSetDevicePlayerIndex(int index, int player)
{
}

static SDL_GUID NXJoystickGetDeviceGUID(int index)
{
    return SDL_CreateJoystickGUIDForName(NXJoystickGetDeviceName(index));
}

static SDL_JoystickID NXJoystickGetDeviceInstanceID(int index)
{
    return (SDL_JoystickID)index + 1;
}

static bool NXJoystickOpen(SDL_Joystick *joystick, int index)
{
    joystick->nbuttons = kButtonCount;
    joystick->naxes = kAxisCount;
    joystick->nhats = 0;
    return true;
}

static bool NXJoystickRumble(SDL_Joystick *joystick, Uint16 low, Uint16 high)
{
    return SDL_Unsupported();
}

static bool NXJoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left, Uint16 right)
{
    return SDL_Unsupported();
}

static bool NXJoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    return SDL_Unsupported();
}

static bool NXJoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool NXJoystickSetSensorsEnabled(SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

// libnx reports sticks as signed 16-bit already, but -32768..32767 is not
// symmetric once negated, so Y goes through a clamp.
static Sint16 ToAxis(s32 value, bool invert)
{
    if (invert) {
        value = -value;
    }
    return (Sint16)SDL_clamp(value, SDL_JOYSTICK_AXIS_MIN, SDL_JOYSTICK_AXIS_MAX);
}

static void NXJoystickUpdate(SDL_Joystick *joystick)
{
    const Uint64 now = SDL_GetTicksNS();

    padUpdate(&g_nx.pad);
    const u64 held = padGetButtons(&g_nx.pad);
    const u64 flipped = held ^ g_nx.held;
    g_nx.held = held;

    if (flipped != 0) {
        for (int i = 0; i < kButtonCount; ++i) {
            if ((flipped & k_buttons[i]) != 0) {
                SDL_SendJoystickButton(now, joystick, (Uint8)i, (held & k_buttons[i]) != 0);
            }
        }
    }

    // SDL's Y axes grow downward, libnx's grow upward.
    const HidAnalogStickState left = padGetStickPos(&g_nx.pad, 0);
    const HidAnalogStickState right = padGetStickPos(&g_nx.pad, 1);
    SDL_SendJoystickAxis(now, joystick, 0, ToAxis(left.x, false));
    SDL_SendJoystickAxis(now, joystick, 1, ToAxis(left.y, true));
    SDL_SendJoystickAxis(now, joystick, 2, ToAxis(right.x, false));
    SDL_SendJoystickAxis(now, joystick, 3, ToAxis(right.y, true));
}

static void NXJoystickClose(SDL_Joystick *joystick)
{
}

static void NXJoystickQuit(void)
{
}

// How the gamepad layer reads the joystick above. L and R are the game's
// triggers and ZL/ZR its shoulders: that is the arrangement the game's default
// bindings were built around, and it is kept as is. The four duplicated
// buttons appear as MISC1..MISC4 so a binding can still reach the other reading.
static SDL_InputMapping OnButton(Uint8 index)
{
    SDL_InputMapping m;
    SDL_zero(m);
    m.kind = EMappingKind_Button;
    m.target = index;
    return m;
}

static SDL_InputMapping OnAxis(Uint8 index)
{
    SDL_InputMapping m;
    SDL_zero(m);
    m.kind = EMappingKind_Axis;
    m.target = index;
    return m;
}

static bool NXJoystickGetGamepadMapping(int index, SDL_GamepadMapping *out)
{
    SDL_zerop(out);

    // k_buttons indices: A=0 B=1 X=2 Y=3 L=4 R=5 ZL=6 ZR=7 Minus=8 Plus=9
    // StickL=10 StickR=11 Up=12 Down=13 Left=14 Right=15, then L R ZL ZR again.
    out->a = OnButton(0);
    out->b = OnButton(1);
    out->x = OnButton(2);
    out->y = OnButton(3);
    out->lefttrigger = OnButton(4);
    out->righttrigger = OnButton(5);
    out->leftshoulder = OnButton(6);
    out->rightshoulder = OnButton(7);
    out->back = OnButton(8);
    out->start = OnButton(9);
    out->leftstick = OnButton(10);
    out->rightstick = OnButton(11);
    out->dpup = OnButton(12);
    out->dpdown = OnButton(13);
    out->dpleft = OnButton(14);
    out->dpright = OnButton(15);
    out->misc1 = OnButton(16);
    out->misc2 = OnButton(17);
    out->misc3 = OnButton(18);
    out->misc4 = OnButton(19);
    out->leftx = OnAxis(0);
    out->lefty = OnAxis(1);
    out->rightx = OnAxis(2);
    out->righty = OnAxis(3);
    return true;
}

SDL_JoystickDriver SDL_SWITCH_JoystickDriver = {
    .Init = NXJoystickInit,
    .GetCount = NXJoystickGetCount,
    .Detect = NXJoystickDetect,
    .IsDevicePresent = NXJoystickIsDevicePresent,
    .GetDeviceName = NXJoystickGetDeviceName,
    .GetDevicePath = NXJoystickGetDevicePath,
    .GetDeviceSteamVirtualGamepadSlot = NXJoystickGetDeviceSteamVirtualGamepadSlot,
    .GetDevicePlayerIndex = NXJoystickGetDevicePlayerIndex,
    .SetDevicePlayerIndex = NXJoystickSetDevicePlayerIndex,
    .GetDeviceGUID = NXJoystickGetDeviceGUID,
    .GetDeviceInstanceID = NXJoystickGetDeviceInstanceID,
    .Open = NXJoystickOpen,
    .Rumble = NXJoystickRumble,
    .RumbleTriggers = NXJoystickRumbleTriggers,
    .SetLED = NXJoystickSetLED,
    .SendEffect = NXJoystickSendEffect,
    .SetSensorsEnabled = NXJoystickSetSensorsEnabled,
    .Update = NXJoystickUpdate,
    .Close = NXJoystickClose,
    .Quit = NXJoystickQuit,
    .GetGamepadMapping = NXJoystickGetGamepadMapping,
};

#endif // SDL_JOYSTICK_SWITCH
