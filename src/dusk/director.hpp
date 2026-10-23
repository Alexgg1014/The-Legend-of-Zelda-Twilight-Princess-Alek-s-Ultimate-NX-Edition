#pragma once
// aleks2 (trailer): Director Mode -- sets up each trailer shot on its own so the only thing
// left for the person recording is pressing R in Switch Frame Tap.
//
// Developer mode only (dev.txt) and only when sdmc:/switch/dusklight/director.txt exists
// (written by trailer-v1/director/exportar_director.py from trailer.json). A dev tool for the
// trailer: never part of a release.
//
// Same protections as the Performance Tour (benchmark.cpp): saves blocked, achievements
// suspended, the in-memory save state captured before the first warp and put back, and a
// warp back to where the player was when the session ends.
//
// director.txt -- "[id]" starts a take, then key=value lines ('#' comments):
//   stage=F_SP121 room=0 point=0 layer=-1   where (the warp)
//   hora=0..24          time of day (left as the save has it when absent)
//   lluvia=0|1          rain (logged if the stage will not hold it; the take goes on)
//   forma=link|lobo|epona   Link's form (logged if it does not take; the take goes on as Link / on foot)
//   dur=seconds         how long the take rolls
//   hud=0|1             the HUD (0, hidden, by default)
//   controles=bloqueados|jugador|grabado   input locked; the player's (live, gyro and rumble too); or
//                       the recording in sdmc:/switch/dusklight/director/<id>.pad
//   cam=t,ex,ey,ez,cx,cy,cz,fovy   a camera key at t s (eye, centre, field of view); several make a
//                       Catmull-Rom path, eased into and out of every key
//   cam_follow=dist,height,yaw_speed[,fovy]   no cam keys: an orbit around Link, starting behind him,
//                       turning yaw_speed degrees a second (0: a fixed follow)
//   spawn=proc,x,y,z,params(hex)   an actor created when the take starts rolling
//   The look, for this take only and put back when it ends (never saved):
//   grabacion=0|1       recording mode (1 by default)
//   musica=0|1          0 mutes the music
//   preset=quality|balanced|performance   bloom=off|classic|dusk   dof=off|classic|dusk
// The camera wins every frame: dCamera_c::Set, and if the game camera moves away from it, the game
// camera is stopped for the rest of the take. The log says which of the two each take used.
//
// Settings > Aleks > "Play trailer takes": every take, one after the other; Start stops and restores.
//
// Pad, while the session is armed (L+R held):
//   Right  next take (3 s countdown, then the take runs for its duration)
//   Left   repeat the current take
//   Up     in a controles=jugador take: save what was played so far as director/<id>.pad;
//          otherwise log the current camera as a key frame (director-keys.txt)
//   Down   end the session and restore
//   Start  cancels a running take (as the tour does)
#include <string>

struct interface_of_controller_pad;  // m_Do/m_Do_controller_pad.h (global, as benchmark.hpp)

namespace dusk::director {

bool available();  ///< dev mode and director.txt present
/// V1-rc12: Check Tour -- the takes run by themselves: a fixed list of known spots (or
/// sdmc:/switch/dusklight/check.txt in director.txt's format), each held with input locked and full
/// hearts, then a full heartbeat (fps, memory, live pipelines, efb copies, hooks, texrep), a line in
/// cache/logs/check-<build>-<backend>.txt and a console screenshot (the Album, as the Capture button).
/// Start cancels and restores. Developer mode only; same protections as the tour.
bool can_start_check(std::string& whyNot);
bool start_check(std::string& error);
std::string check_description();
/// Trailer: "Play trailer takes" -- every take of director.txt, one after the other.
bool can_start_reel(std::string& whyNot);
bool start_reel(std::string& error);
std::string reel_description();
/// The Check Tour's (or the reel's) line for the overlay: empty while a take rolls.
std::string check_status_text();
bool active();     ///< a session is armed (protections held)
bool locks_input();  ///< the running take has controles=bloqueados
void tick();       ///< once per frame, after benchmark::tick()
void filter_pad(interface_of_controller_pad* pads, int count);
std::string status_text();

}  // namespace dusk::director
