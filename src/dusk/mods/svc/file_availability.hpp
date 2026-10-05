#pragma once
// aleks2 (H37-c4): when the file service exists.
//
// dev.twilitrealm.dusklight.file is two things: pickers (open a file, open a folder, export one)
// and access by location (open, size, read, write, seek, flush, close, read_all, write_all, free,
// list, join, create_child), which is borealis::io and works wherever the game has a file system.
// A mod that imports the service for the second half -- Randomizer reads and writes its seed files
// through it -- must not lose it because there is no dialog to show. H37-c1..c3 made the service
// depend on the pickers alone, so on Switch (no picker: borealis_file_select_switch.cpp) a mod that
// needs it failed at activation with "missing: dev.twilitrealm.dusklight.file".
//
// Where there is no picker the pickers stay in the service and answer "canceled" -- exactly what a
// dismissed desktop dialog answers -- so a mod learns the same way it always does that nobody chose.
namespace dusk::mods::svc {

// A platform whose pickers always cancel but whose borealis::io works on the card.
#if defined(__SWITCH__)
inline constexpr bool kFileServiceWithoutPicker = true;
#else
inline constexpr bool kFileServiceWithoutPicker = false;
#endif

constexpr bool file_service_available(bool canOpenFile, bool canOpenFolder, bool canExportFile,
    bool platformIoWithoutPicker = kFileServiceWithoutPicker) noexcept {
    return platformIoWithoutPicker || canOpenFile || canOpenFolder || canExportFile;
}

}  // namespace dusk::mods::svc
