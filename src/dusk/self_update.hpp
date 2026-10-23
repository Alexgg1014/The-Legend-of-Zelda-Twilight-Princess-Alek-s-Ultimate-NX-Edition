#pragma once

// aleks2 (V1-rc8): the in-app updater (Switch). It checks the port's GitHub releases -- by hand from Settings, or at
// start at most once a day when "Check for Dusklight Updates" is on -- and, for a release that publishes a .nro with
// its SHA-256, downloads it next to the running NRO, checks it, and puts it in place when the program closes (the
// running NRO's file is held open for its romfs until then). The previous NRO is kept as <name>.nro.bak until the
// new one has run for a while. Nothing else on the card is touched: the shader caches reconcile with a new build's
// seed on their own, as after a manual update.

#include "dusk/config_var.hpp"

#include <string>

namespace dusk::self_update {

enum class State { Idle, Checking, UpToDate, Available, ManualOnly, Downloading, Verifying, Ready, Failed };

void update();             // every UI frame: finished checks, downloads and hash checks
void game_frame();         // every game frame: drops the .bak once the new NRO has run a while
void maybe_auto_check();   // the start-up check, at most once a day
void check_now();
void install();            // download and check the available release
void restart_now();        // close (installing the update) and start again

State state();
config::ConfigVar<bool>& include_betas();  // "Include Beta Versions" (GitHub prereleases)
std::string status_value();   // a few words, for the settings row
std::string status_detail();  // a sentence
std::string latest_label();   // "1.0.0-rc.9"

}  // namespace dusk::self_update

// At exit, before the process ends (main.cpp): puts a checked download in place of the running NRO.
extern "C" void aleks2_apply_pending_update(void);
