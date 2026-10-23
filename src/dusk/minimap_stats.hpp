#pragma once
// aleks2 (rc9): what the minimap renders and why (d_map.cpp), for the session heartbeat.
#include <cstdint>

namespace dusk::minimap_stats {

// Why hud_fast rendered the minimap instead of keeping it: the first key field that changed.
enum Reason : uint8_t {
    kFirst,       // nothing kept yet (start, hud_fast just turned on)
    kCenter,      // the render window moved
    kZoom,        // cm per texel
    kRoomFloor,
    kPlayer,      // player cursor position or angle
    kRestart,     // restart cursor
    kDisplay,     // mirror mode, render height, second screen
    kRefresh,     // forced every 15 kept frames
    kReasonCount,
};

// minimap_pan: why the base texture was rendered again.
enum BaseReason : uint8_t {
    kBaseFirst,
    kBaseKey,     // room, floor, zoom, render size, mirror, second screen, switch/visited bits
    kBaseMargin,  // the window left the margin
    kBaseReasonCount,
};

struct Counters {
    uint32_t renders = 0;      // frames the minimap was rendered (entry())
    uint32_t kept = 0;         // frames hud_fast kept the last render
    uint32_t composes = 0;     // minimap_pan: renders drawn from the base texture
    uint32_t baseRenders = 0;  // minimap_pan: base texture renders
    uint32_t reasons[kReasonCount] = {};
    uint32_t baseReasons[kBaseReasonCount] = {};
};

// Cumulative since launch; defined in d_map.cpp.
const Counters& counters();
const char* reason_name(int reason);
const char* base_reason_name(int reason);

}  // namespace dusk::minimap_stats
