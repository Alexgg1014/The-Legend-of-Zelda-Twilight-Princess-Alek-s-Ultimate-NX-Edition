#include "save_context.hpp"

#include "dusk/data.hpp"
#include "dusk/game_mode.hpp"
#include "dusk/mods/svc/save.hpp"
#include "dusk/settings.h"
#include "dusk/ui/prelaunch.hpp"

namespace dusk::save_manager {

ValueResult<SaveContext> context_for_save(const std::string& saveName) {
    const auto& state = ui::prelaunch_state();
    if (data::is_data_path_restart_pending() ||
        (!state.activeDiscPath.empty() && state.configuredDiscPath != state.activeDiscPath))
    {
        return {{.message = "Restart required before managing saves."}, {}};
    }
    auto identity = identity_for_disc(state.configuredDiscInfo, saveName);
    if (!state.configuredDiscCanLaunch || !identity) {
        return {{.message = "A disc must be configured before managing saves."}, {}};
    }
    const auto preferredKind = getSettings().backend.cardFileType.getValue() == 0 ?
                                   StorageKind::RawImage :
                                   StorageKind::GciDirectory;
    auto storage = resolve_storage(identity->game, preferredKind);
    if (!storage) {
        return {storage.result, {}};
    }
    if (auto migrated =
            mods::svc::migrate_legacy_sidecar(storage.value, identity->maker, identity->game);
        !migrated)
    {
        return {std::move(migrated), {}};
    }
    return {
        {.ok = true},
        SaveContext{
            .identity = std::move(*identity),
            .storage = std::move(storage.value),
        },
    };
}

std::string active_save_name() {
    return gamemode::getGameModeManager().getCurrentGameMode()->getSaveName();
}

}  // namespace dusk::save_manager
