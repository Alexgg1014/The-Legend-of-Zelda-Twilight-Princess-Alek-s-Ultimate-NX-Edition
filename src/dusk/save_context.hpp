#pragma once

#include "dusk/save_manager.hpp"

#include <string>

namespace dusk::save_manager {

// aleks2 (cross-save): which save a save name means right now (configured disc, card storage),
// shared by the Saves window and the save snapshot service so both resolve it the same way.
struct SaveContext {
    SaveIdentity identity;
    Storage storage;
};

// Fails while a restart is pending or no disc is configured; migrates legacy mod data first.
ValueResult<SaveContext> context_for_save(const std::string& saveName);

// The save name of the current game mode.
std::string active_save_name();

}  // namespace dusk::save_manager
