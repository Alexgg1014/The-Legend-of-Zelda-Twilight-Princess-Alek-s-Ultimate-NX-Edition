// aleks2 (cross-save): SaveSnapshotService, whole-save snapshots for sync mods. A thin layer over
// save_manager: every read, validation, backup, atomic write and remount is the Saves window's own
// code (see save_manager.hpp, "the pieces the Saves window is built from").

#include "registry.hpp"
#include "internal.hpp"
#include "save.hpp"

#include "dusk/main.h"
#include "dusk/mods/loader/loader.hpp"
#include "dusk/mount_path.hpp"
#include "dusk/save_context.hpp"
#include "dusk/save_manager.hpp"
#include "dusk/ui/ui.hpp"
#include "dusk/utilities.hpp"
#include "mods/svc/save_snapshot.h"

#include <borealis/io.hpp>
#include <borealis/log.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dusk::mods::svc {
namespace {

borealis::Log Log{"dusk::mods::save_snapshot"};

namespace sm = save_manager;

std::atomic_uint64_t s_tempSequence = 0;

template <size_t N>
void copy_text(char (&destination)[N], std::string_view text) {
    const size_t size = std::min(text.size(), N - 1);
    std::memcpy(destination, text.data(), size);
    destination[size] = '\0';
}

std::string game_family(std::string_view game) {
    // Twilight Princess on Wii (RZD) and GameCube (GZ2) share one save layout: disc_compatibility
    // calls it a PlatformChange. Both sync under the GameCube family.
    const auto family = game.substr(0, 3);
    return family == "RZD" ? std::string{"GZ2"} : std::string{family};
}

std::string sync_key(const sm::SaveIdentity& identity) {
    return fmt::format("{}-{}-{}", game_family(identity.game), identity.maker, identity.saveName);
}

SaveSnapshotCompat to_compat(sm::DiscCompatibility compatibility) {
    switch (compatibility) {
    case sm::DiscCompatibility::Exact:
        return SAVE_SNAPSHOT_COMPAT_EXACT;
    case sm::DiscCompatibility::RegionChange:
        return SAVE_SNAPSHOT_COMPAT_REGION_CHANGE;
    case sm::DiscCompatibility::PlatformChange:
        return SAVE_SNAPSHOT_COMPAT_PLATFORM_CHANGE;
    case sm::DiscCompatibility::Incompatible:
        break;
    }
    return SAVE_SNAPSHOT_COMPAT_INCOMPATIBLE;
}

struct Resolved {
    SaveSnapshotStatus status = SAVE_SNAPSHOT_OK;
    std::string message;
    sm::SaveContext context;
};

Resolved resolve(const char* saveName) {
    const std::string name = saveName != nullptr ? std::string{saveName} : sm::active_save_name();
    if (!utils::is_valid_save_name(name)) {
        return {.status = SAVE_SNAPSHOT_NO_DISC, .message = "The save name is invalid."};
    }
    auto context = sm::context_for_save(name);
    if (!context) {
        // context_for_save fails for a pending restart/missing disc (no message about storage) or
        // for an unresolvable card location.
        const bool card = context.result.message.find("memory card") != std::string::npos;
        return {.status = card ? SAVE_SNAPSHOT_NO_CARD : SAVE_SNAPSHOT_NO_DISC,
            .message = context.result.message};
    }
    return {.context = std::move(context.value)};
}

std::optional<std::filesystem::path> validate_path(const LoadedMod& mod, const char* rawPath) {
    if (rawPath == nullptr) {
        return std::nullopt;
    }
    std::error_code ec;
    const auto dataRoot =
        !mod.dataDirUtf8.empty() ?
            mount_path::normalized_absolute(borealis::io::fs_path_from_utf8(mod.dataDirUtf8), ec) :
            mount_path::persistent_data_dir(ConfigPath, mod.metadata.id, ec);
    if (ec) {
        return std::nullopt;
    }
    // Same containment as HttpService downloads: inside mod_dir or data_dir, never in the staging
    // area. A sync mod downloads a snapshot there and hands the same path to inspect/apply.
    return mount_path::validate_download_path(
        borealis::io::fs_path_from_utf8(rawPath), mod.dir, dataRoot);
}

std::filesystem::path record_path(const LoadedMod& mod, const sm::SaveIdentity& identity) {
    return ConfigPath / "save_sync" / std::filesystem::path(mod.metadata.id) /
           borealis::io::fs_path_from_utf8(sync_key(identity) + ".rec");
}

void fill_header(SaveSnapshotInfo& out, const sm::GciHeader& header) {
    copy_text(out.game, header.game);
    copy_text(out.maker, header.maker);
    copy_text(out.save_name, header.saveName);
    out.modified_time = header.modifiedTime;
}

// Reads the local save and its digest; status NO_SAVE when there is none.
struct LocalSave {
    SaveSnapshotStatus status = SAVE_SNAPSHOT_OK;
    std::string message;
    std::vector<uint8_t> gci;
    std::map<std::string, std::vector<uint8_t>> modFiles;
    std::string digest;
};

LocalSave read_local(const sm::SaveContext& context) {
    LocalSave local;
    auto gci = sm::read_save_gci(context.storage, context.identity);
    if (!gci) {
        return {.status = SAVE_SNAPSHOT_IO_ERROR, .message = gci.result.message};
    }
    if (gci.value.empty()) {
        return {.status = SAVE_SNAPSHOT_NO_SAVE, .message = "There is no save file."};
    }
    auto mods = sm::read_save_mod_files(context.storage, context.identity);
    if (!mods) {
        return {.status = SAVE_SNAPSHOT_IO_ERROR, .message = mods.result.message};
    }
    local.digest = sm::snapshot_digest(gci.value, mods.value);
    local.gci = std::move(gci.value);
    local.modFiles = std::move(mods.value);
    return local;
}

template <typename T>
bool valid_out(T* out) {
    return out != nullptr && out->struct_size >= sizeof(T);
}

template <typename T>
void reset_out(T* out) {
    const uint32_t structSize = out->struct_size;
    std::memset(static_cast<void*>(out), 0, sizeof(T));
    out->struct_size = structSize;
}

ModResult snapshot_get_identity(
    ModContext* context, const char* saveName, SaveSnapshotIdentity* out) {
    auto* mod = mod_from_context(context);
    if (mod == nullptr || !valid_out(out)) {
        return MOD_INVALID_ARGUMENT;
    }
    reset_out(out);
    auto resolved = resolve(saveName);
    if (resolved.status != SAVE_SNAPSHOT_OK) {
        out->status = resolved.status;
        copy_text(out->message, resolved.message);
        return MOD_OK;
    }
    const auto& identity = resolved.context.identity;
    copy_text(out->game, identity.game);
    copy_text(out->maker, identity.maker);
    copy_text(out->save_name, identity.saveName);
    copy_text(out->key, sync_key(identity));
    out->change_count = save_change_count(identity.saveName);
    auto local = read_local(resolved.context);
    if (local.status == SAVE_SNAPSHOT_NO_SAVE) {
        out->status = SAVE_SNAPSHOT_OK;
        return MOD_OK;
    }
    out->status = local.status;
    if (local.status != SAVE_SNAPSHOT_OK) {
        copy_text(out->message, local.message);
        return MOD_OK;
    }
    out->has_save = true;
    copy_text(out->digest, local.digest);
    if (auto parsed = sm::parse_gci(local.gci)) {
        out->modified_time = parsed.value.modifiedTime;
    }
    return MOD_OK;
}

ModResult snapshot_capture(
    ModContext* context, const char* saveName, const char* destPath, SaveSnapshotInfo* out) {
    auto* mod = mod_from_context(context);
    if (mod == nullptr || !valid_out(out)) {
        return MOD_INVALID_ARGUMENT;
    }
    const auto destination = validate_path(*mod, destPath);
    if (!destination) {
        return MOD_INVALID_ARGUMENT;
    }
    reset_out(out);
    auto resolved = resolve(saveName);
    if (resolved.status != SAVE_SNAPSHOT_OK) {
        out->status = resolved.status;
        copy_text(out->message, resolved.message);
        return MOD_OK;
    }
    // Never read a save the game is writing: on_save_written fires after SaveSync, so a capture
    // scheduled from it (after a debounce) finds the card idle.
    if (const auto idle = sm::ensure_card_idle(resolved.context.storage); !idle) {
        out->status = SAVE_SNAPSHOT_CARD_BUSY;
        copy_text(out->message, idle.message);
        return MOD_OK;
    }
    auto local = read_local(resolved.context);
    if (local.status != SAVE_SNAPSHOT_OK) {
        out->status = local.status;
        copy_text(out->message, local.message);
        return MOD_OK;
    }
    if (const auto written = sm::write_dusksave(*destination, local.gci, local.modFiles); !written)
    {
        out->status = SAVE_SNAPSHOT_IO_ERROR;
        copy_text(out->message, written.message);
        return MOD_OK;
    }
    std::error_code ec;
    out->file_size = std::filesystem::file_size(*destination, ec);
    copy_text(out->digest, local.digest);
    if (auto parsed = sm::parse_gci(local.gci)) {
        fill_header(*out, parsed.value);
    }
    out->compat = SAVE_SNAPSHOT_COMPAT_EXACT;
    return MOD_OK;
}

// Reads and validates a snapshot file for resolved; fills out and returns the artifact on OK.
std::optional<sm::Artifact> check_artifact(
    const Resolved& resolved, const std::filesystem::path& path, SaveSnapshotInfo& out) {
    auto artifact = sm::read_artifact(borealis::io::fs_path_to_string(path));
    if (!artifact || (artifact.value.kind != sm::ArtifactKind::Gci &&
                         artifact.value.kind != sm::ArtifactKind::DuskSave))
    {
        out.status = SAVE_SNAPSHOT_INVALID;
        copy_text(out.message, artifact ? std::string{"The snapshot is not an individual save."} :
                                          artifact.result.message);
        return std::nullopt;
    }
    std::error_code ec;
    out.file_size = std::filesystem::file_size(path, ec);
    fill_header(out, artifact.value.header);
    out.digest[0] = '\0';
    copy_text(out.digest, sm::snapshot_digest(artifact.value.gci, artifact.value.modFiles));
    const auto& identity = resolved.context.identity;
    const auto compatibility = sm::disc_compatibility(artifact.value.header, identity);
    out.compat = to_compat(compatibility);
    if (compatibility == sm::DiscCompatibility::Incompatible ||
        artifact.value.header.saveName != identity.saveName)
    {
        out.status = SAVE_SNAPSHOT_INCOMPATIBLE;
        copy_text(out.message,
            fmt::format("The snapshot is {}-{} '{}', but this save is {}-{} '{}'.",
                artifact.value.header.maker, artifact.value.header.game,
                artifact.value.header.saveName, identity.maker, identity.game,
                identity.saveName));
        return std::nullopt;
    }
    return std::move(artifact.value);
}

ModResult snapshot_inspect(
    ModContext* context, const char* saveName, const char* path, SaveSnapshotInfo* out) {
    auto* mod = mod_from_context(context);
    if (mod == nullptr || !valid_out(out)) {
        return MOD_INVALID_ARGUMENT;
    }
    const auto source = validate_path(*mod, path);
    if (!source) {
        return MOD_INVALID_ARGUMENT;
    }
    reset_out(out);
    auto resolved = resolve(saveName);
    if (resolved.status != SAVE_SNAPSHOT_OK) {
        out->status = resolved.status;
        copy_text(out->message, resolved.message);
        return MOD_OK;
    }
    if (!check_artifact(resolved, *source, *out)) {
        return MOD_OK;
    }
    if (out->compat != SAVE_SNAPSHOT_COMPAT_EXACT) {
        out->status = SAVE_SNAPSHOT_NEEDS_CONVERSION;
        copy_text(out->message, "The snapshot comes from another region or platform.");
    }
    return MOD_OK;
}

SaveSnapshotStatus status_for_stage(sm::ApplyStage stage) {
    switch (stage) {
    case sm::ApplyStage::Busy:
        return SAVE_SNAPSHOT_CARD_BUSY;
    case sm::ApplyStage::Validate:
        return SAVE_SNAPSHOT_INVALID;
    case sm::ApplyStage::Backup:
        return SAVE_SNAPSHOT_BACKUP_FAILED;
    case sm::ApplyStage::Write:
        return SAVE_SNAPSHOT_WRITE_FAILED;
    case sm::ApplyStage::ModData:
        return SAVE_SNAPSHOT_MOD_DATA_FAILED;
    case sm::ApplyStage::Remount:
        return SAVE_SNAPSHOT_REMOUNT_FAILED;
    case sm::ApplyStage::None:
        break;
    }
    return SAVE_SNAPSHOT_IO_ERROR;
}

ModResult snapshot_apply(ModContext* context, const char* saveName, const char* path,
    const SaveSnapshotApplyDesc* desc, SaveSnapshotInfo* out) {
    auto* mod = mod_from_context(context);
    if (mod == nullptr || !valid_out(out) ||
        (desc != nullptr && desc->struct_size < sizeof(SaveSnapshotApplyDesc)))
    {
        return MOD_INVALID_ARGUMENT;
    }
    const auto source = validate_path(*mod, path);
    if (!source) {
        return MOD_INVALID_ARGUMENT;
    }
    reset_out(out);
    const auto finish = [out](SaveSnapshotStatus status, std::string_view message) {
        out->status = status;
        copy_text(out->message, message);
        return MOD_OK;
    };
    // The game keeps the loaded save in memory; replacing it under a running game would be undone
    // by its next save. Applying is limited to the pre-launch menu.
    if (!dusk::ui::is_prelaunch_open() || save_slot_active()) {
        return finish(SAVE_SNAPSHOT_IN_GAME,
            "Snapshots can only be applied from the pre-launch menu.");
    }
    auto resolved = resolve(saveName);
    if (resolved.status != SAVE_SNAPSHOT_OK) {
        return finish(resolved.status, resolved.message);
    }
    auto artifact = check_artifact(resolved, *source, *out);
    if (!artifact) {
        return MOD_OK;
    }
    const bool allowConversion =
        desc != nullptr && (desc->flags & SAVE_SNAPSHOT_APPLY_ALLOW_CONVERSION) != 0;
    if (out->compat != SAVE_SNAPSHOT_COMPAT_EXACT && !allowConversion) {
        return finish(SAVE_SNAPSHOT_NEEDS_CONVERSION,
            "The snapshot comes from another region or platform.");
    }
    if (desc != nullptr && desc->expected_digest != nullptr) {
        auto local = read_local(resolved.context);
        if (local.status != SAVE_SNAPSHOT_OK && local.status != SAVE_SNAPSHOT_NO_SAVE) {
            return finish(local.status, local.message);
        }
        if (local.digest != desc->expected_digest) {
            return finish(SAVE_SNAPSHOT_LOCAL_CHANGED,
                "The local save changed since it was last checked.");
        }
    }
    const auto outcome = sm::apply_snapshot(
        resolved.context.storage, resolved.context.identity, *artifact, allowConversion);
    copy_text(out->backup_name,
        outcome.backup.empty() ? std::string{} :
                                 borealis::io::fs_path_to_string(outcome.backup.filename()));
    if (!outcome.result) {
        Log.warn("[{}] snapshot apply failed at stage {}: {}", mod->metadata.id,
            static_cast<int>(outcome.failedStage), outcome.result.message);
        return finish(status_for_stage(outcome.failedStage), outcome.result.message);
    }
    auto written = read_local(resolved.context);
    if (written.status != SAVE_SNAPSHOT_OK) {
        return finish(written.status, written.message);
    }
    copy_text(out->digest, written.digest);
    Log.info("[{}] snapshot applied to {} (backup '{}')", mod->metadata.id,
        sync_key(resolved.context.identity), out->backup_name);
    return finish(SAVE_SNAPSHOT_OK, {});
}

ModResult snapshot_read_record(
    ModContext* context, const char* saveName, void* buf, size_t* inoutSize) {
    auto* mod = mod_from_context(context);
    if (mod == nullptr || inoutSize == nullptr) {
        return MOD_INVALID_ARGUMENT;
    }
    auto resolved = resolve(saveName);
    if (resolved.status != SAVE_SNAPSHOT_OK) {
        return MOD_UNAVAILABLE;
    }
    const auto path = record_path(*mod, resolved.context.identity);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > SAVE_SNAPSHOT_RECORD_MAX_BYTES) {
        return MOD_UNAVAILABLE;
    }
    if (buf == nullptr) {
        *inoutSize = static_cast<size_t>(size);
        return MOD_OK;
    }
    if (*inoutSize < size) {
        return MOD_INVALID_ARGUMENT;
    }
    std::FILE* file = std::fopen(borealis::io::fs_path_to_string(path).c_str(), "rb");
    if (file == nullptr) {
        return MOD_UNAVAILABLE;
    }
    const size_t read = size == 0 ? 0 : std::fread(buf, 1, static_cast<size_t>(size), file);
    std::fclose(file);
    if (read != size) {
        return MOD_UNAVAILABLE;
    }
    *inoutSize = static_cast<size_t>(size);
    return MOD_OK;
}

ModResult snapshot_write_record(
    ModContext* context, const char* saveName, const void* data, size_t size) {
    auto* mod = mod_from_context(context);
    if (mod == nullptr || (data == nullptr && size != 0) || size > SAVE_SNAPSHOT_RECORD_MAX_BYTES) {
        return MOD_INVALID_ARGUMENT;
    }
    auto resolved = resolve(saveName);
    if (resolved.status != SAVE_SNAPSHOT_OK) {
        return MOD_UNAVAILABLE;
    }
    const auto path = record_path(*mod, resolved.context.identity);
    std::error_code ec;
    if (size == 0) {
        std::filesystem::remove(path, ec);
        return ec ? MOD_UNAVAILABLE : MOD_OK;
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    auto temporary = path;
    temporary += fmt::format(".{}.tmp", s_tempSequence.fetch_add(1));
    std::FILE* file = std::fopen(borealis::io::fs_path_to_string(temporary).c_str(), "wb");
    if (file == nullptr) {
        return MOD_UNAVAILABLE;
    }
    const bool written = std::fwrite(data, 1, size, file) == size;
    const bool closed = std::fclose(file) == 0;
    std::string error;
    if (!written || !closed || !borealis::io::atomic_replace(temporary, path, error)) {
        std::filesystem::remove(temporary, ec);
        Log.error("[{}] sync record not written: {}", mod->metadata.id,
            error.empty() ? std::string{"write failed"} : error);
        return MOD_UNAVAILABLE;
    }
    return MOD_OK;
}

constexpr SaveSnapshotService s_saveSnapshotService{
    .header = SERVICE_HEADER(
        SaveSnapshotService, SAVE_SNAPSHOT_SERVICE_MAJOR, SAVE_SNAPSHOT_SERVICE_MINOR),
    .get_identity = SERVICE_FUNCTION(snapshot_get_identity),
    .capture = SERVICE_FUNCTION(snapshot_capture),
    .inspect = SERVICE_FUNCTION(snapshot_inspect),
    .apply = SERVICE_FUNCTION(snapshot_apply),
    .read_record = SERVICE_FUNCTION(snapshot_read_record),
    .write_record = SERVICE_FUNCTION(snapshot_write_record),
};

}  // namespace

constinit const ServiceModule g_saveSnapshotModule{
    .id = SAVE_SNAPSHOT_SERVICE_ID,
    .majorVersion = SAVE_SNAPSHOT_SERVICE_MAJOR,
    .minorVersion = SAVE_SNAPSHOT_SERVICE_MINOR,
    .service = &s_saveSnapshotService,
};

}  // namespace dusk::mods::svc
