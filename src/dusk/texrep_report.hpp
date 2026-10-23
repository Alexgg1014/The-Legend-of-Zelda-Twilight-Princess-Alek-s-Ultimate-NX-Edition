#pragma once
// aleks2 (V1-rc12): texture-replacement usage report, for pack authors tuning a Switch version (which textures each
// stage really loads, how big, the resident peak against texrep_budget_mb, evictions). Developer mode only
// (dev.txt) and only with a texture_replacements pack installed: otherwise Aurora records nothing.
// Writes sdmc:/switch/dusklight/cache/logs/texrep-<stage>.txt when the stage is left and every 30 s while in it;
// a stage visited again adds to its file (rows merged by texture name, rooms noted per row).
namespace dusk::texrep_report {

// Every frame (session_log::tick): samples every 2 s, rewrites the current stage's file every 30 s.
void tick(const char* stage);
// When the stage changes (session_log::stampStage, before its name is taken): the rest of the old stage's window
// and its file; nextStage starts the next one.
void stage_left(const char* nextStage);

}  // namespace dusk::texrep_report
