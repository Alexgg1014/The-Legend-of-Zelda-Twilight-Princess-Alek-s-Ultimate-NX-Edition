#pragma once

#include <cstdint>
#include <string_view>

namespace dusk::save_manager {
struct Result;
struct Storage;
}  // namespace dusk::save_manager

namespace dusk::mods::svc {

void save_slot_new(uint32_t slot);
void save_slot_loaded(uint32_t slot);
void save_slot_written(uint32_t slot);
void save_slot_copied(uint32_t fromSlot, uint32_t toSlot);
void save_slot_erased(uint32_t slot);
void save_no_slot();
void invalidate_save(std::string_view saveName);

// aleks2 (cross-save): counts, for this session, every change to a save file's content: game writes
// (save_slot_written) and every save_manager write (import, restore, delete, mod data), which all go
// through invalidate_save. Lets a mod skip re-reading a save that did not change.
uint64_t save_change_count(std::string_view saveName);
// A save slot is loaded (in game); false at the file select and in the pre-launch menu.
bool save_slot_active();

// Migrates the old mod_saves.json to the new per-file, per-mod JSON.
save_manager::Result migrate_legacy_sidecar(
    const save_manager::Storage& storage, std::string_view maker, std::string_view game);

}  // namespace dusk::mods::svc
