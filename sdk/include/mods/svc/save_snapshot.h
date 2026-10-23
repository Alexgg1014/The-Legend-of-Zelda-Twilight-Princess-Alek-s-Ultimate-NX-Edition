#pragma once

#include <mods/api.h>

#ifdef __cplusplus
#include <mods/service.hpp>
#endif

/*
 * aleks2 (cross-save): whole-save snapshots for sync and backup mods.
 *
 * A snapshot is the complete GameCube save file (all three slots) plus the mod sidecars stored with
 * it, packed as a .dusksave version 1 -- the same artifact the Saves window imports and exports, read
 * and written by the same code (parser, disc compatibility, backups, atomic writes, card remount).
 *
 * Unlike SaveService, which stores a mod's own per-slot blobs, this service reads and replaces the
 * player's save. Every replacement backs up the current save first.
 *
 * Calls run on the game thread and are synchronous file I/O on a small file (a Twilight Princess
 * save is tens of KiB). None of them touches the network.
 *
 * Every function returns MOD_OK when it ran -- the outcome is then in `status` -- and
 * MOD_INVALID_ARGUMENT for a bad argument (NULL output, wrong struct_size, path outside the mod's
 * data_dir). `save_name` selects the CARD file; NULL means the current game mode's save.
 */

#define SAVE_SNAPSHOT_SERVICE_ID DUSKLIGHT_SERVICE_ID_PREFIX "save_snapshot"
#define SAVE_SNAPSHOT_SERVICE_MAJOR 1u
#define SAVE_SNAPSHOT_SERVICE_MINOR 0u

/* Maximum size of the per-mod sync record (read_record/write_record). */
#define SAVE_SNAPSHOT_RECORD_MAX_BYTES 16384u

typedef enum SaveSnapshotStatus {
    SAVE_SNAPSHOT_OK = 0,
    SAVE_SNAPSHOT_NO_DISC = 1,          /* no configured disc, or a restart is pending */
    SAVE_SNAPSHOT_NO_CARD = 2,          /* the memory card location is unavailable */
    SAVE_SNAPSHOT_CARD_BUSY = 3,        /* the game is reading or writing the card: retry later */
    SAVE_SNAPSHOT_IN_GAME = 4,          /* apply: only from the pre-launch menu */
    SAVE_SNAPSHOT_NO_SAVE = 5,          /* capture: there is no save to capture */
    SAVE_SNAPSHOT_INVALID = 6,          /* not a valid .gci/.dusksave */
    SAVE_SNAPSHOT_INCOMPATIBLE = 7,     /* another game, maker or save name: never applied */
    SAVE_SNAPSHOT_NEEDS_CONVERSION = 8, /* region/platform change without ALLOW_CONVERSION */
    SAVE_SNAPSHOT_LOCAL_CHANGED = 9,    /* apply: the local save no longer has expected_digest */
    SAVE_SNAPSHOT_BACKUP_FAILED = 10,   /* nothing was written */
    SAVE_SNAPSHOT_WRITE_FAILED = 11,    /* the previous save is still in place */
    SAVE_SNAPSHOT_MOD_DATA_FAILED = 12, /* the previous save was put back (see message) */
    SAVE_SNAPSHOT_REMOUNT_FAILED = 13,  /* the new save is on disk; restart before playing */
    SAVE_SNAPSHOT_IO_ERROR = 14,        /* reading the save or writing the snapshot file failed */
} SaveSnapshotStatus;

typedef enum SaveSnapshotCompat {
    SAVE_SNAPSHOT_COMPAT_EXACT = 0,
    SAVE_SNAPSHOT_COMPAT_REGION_CHANGE = 1,   /* same game, other region: rewritten on apply */
    SAVE_SNAPSHOT_COMPAT_PLATFORM_CHANGE = 2, /* GameCube <-> Wii Twilight Princess */
    SAVE_SNAPSHOT_COMPAT_INCOMPATIBLE = 3,
} SaveSnapshotCompat;

/* SHA-256 as 64 lowercase hex digits plus NUL. */
#define SAVE_SNAPSHOT_DIGEST_SIZE 65u

typedef struct SaveSnapshotIdentity {
    uint32_t struct_size;
    SaveSnapshotStatus status; /* OK, NO_DISC, NO_CARD or IO_ERROR */
    char game[5];              /* e.g. "GZ2E" */
    char maker[3];             /* e.g. "01" */
    char save_name[33];        /* e.g. "gczelda2" */
    /* Stable sync key: game family (GZ2 for both GameCube and Wii), maker and save name, e.g.
     * "GZ2-01-gczelda2". Saves with the same key may need a conversion between regions/platforms. */
    char key[48];
    bool has_save;
    /* Content digest of the local save (see SaveSnapshotInfo.digest); empty without a save. */
    char digest[SAVE_SNAPSHOT_DIGEST_SIZE];
    /* Changes counted this session (game writes, imports, restores, deletes); 0 at start. Cheap:
     * compare it before asking for anything that reads the save. */
    uint64_t change_count;
    /* GCI header time of the local save (seconds since 2000-01-01); informative only. */
    uint32_t modified_time;
    char message[256];
} SaveSnapshotIdentity;

#define SAVE_SNAPSHOT_IDENTITY_INIT                                                                \
    {sizeof(SaveSnapshotIdentity), SAVE_SNAPSHOT_OK, {0}, {0}, {0}, {0}, false, {0}, 0u, 0u, {0}}

typedef struct SaveSnapshotInfo {
    uint32_t struct_size;
    SaveSnapshotStatus status;
    char message[256]; /* human-readable detail; never contains save bytes */
    /* SHA-256 over the save content: the GCI bytes and each sidecar (framed, sorted by mod ID).
     * The .dusksave container is not hashed, so capturing an unchanged save twice gives the same
     * digest. After apply, this is the digest of the save as written (a conversion rewrites the
     * game ID, so it can differ from the snapshot's). */
    char digest[SAVE_SNAPSHOT_DIGEST_SIZE];
    uint64_t file_size;       /* capture/inspect: snapshot file size in bytes */
    SaveSnapshotCompat compat; /* inspect/apply: against the configured disc */
    char game[5];              /* inspect/apply: of the snapshot */
    char maker[3];
    char save_name[33];
    uint32_t modified_time; /* GCI header time (seconds since 2000); informative only */
    char backup_name[128];  /* apply: backup file name (empty when there was no local save) */
} SaveSnapshotInfo;

#define SAVE_SNAPSHOT_INFO_INIT                                                                    \
    {sizeof(SaveSnapshotInfo), SAVE_SNAPSHOT_OK, {0}, {0}, 0u, SAVE_SNAPSHOT_COMPAT_EXACT, {0},    \
        {0}, {0}, 0u, {0}}

/* Apply a region/platform conversion (the Saves window's "Import Anyway"). Ask the player first. */
#define SAVE_SNAPSHOT_APPLY_ALLOW_CONVERSION 0x1u

typedef struct SaveSnapshotApplyDesc {
    uint32_t struct_size;
    uint32_t flags;
    /* Compare-and-swap on the local side: when non-NULL, apply only if the local save's digest is
     * still this one ("" = there must be no local save); otherwise SAVE_SNAPSHOT_LOCAL_CHANGED. */
    const char* expected_digest;
} SaveSnapshotApplyDesc;

#define SAVE_SNAPSHOT_APPLY_DESC_INIT {sizeof(SaveSnapshotApplyDesc), 0u, NULL}

typedef struct SaveSnapshotService {
    ServiceHeader header;

    /* Which save `save_name` means now, whether it exists, its digest and change count. */
    ModResult (*get_identity)(ModContext* ctx, const char* save_name, SaveSnapshotIdentity* out);

    /* Writes the save as a .dusksave at dest_path (absolute, under this mod's data_dir), atomically.
     * CARD_BUSY while the game is writing; NO_SAVE without a save. */
    ModResult (*capture)(
        ModContext* ctx, const char* save_name, const char* dest_path, SaveSnapshotInfo* out);

    /* Validates a .gci/.dusksave at path (under data_dir) against the configured disc and save
     * name without writing anything: INVALID, INCOMPATIBLE, NEEDS_CONVERSION or OK, with digest,
     * compat and the snapshot's header. */
    ModResult (*inspect)(
        ModContext* ctx, const char* save_name, const char* path, SaveSnapshotInfo* out);

    /* Replaces the save with the snapshot at path: validate, check expected_digest, back up the
     * current save, write the GCI atomically, replace the sidecars, invalidate cached save data and
     * remount the card. Only from the pre-launch menu (IN_GAME otherwise). A failure leaves the
     * previous save in place, except REMOUNT_FAILED (new save written, restart required). */
    ModResult (*apply)(ModContext* ctx, const char* save_name, const char* path,
        const SaveSnapshotApplyDesc* desc, SaveSnapshotInfo* out);

    /* A small persistent record, private to the calling mod and this save's key (e.g. the last
     * synced digest and remote revision). Stored by the host, written atomically, never part of a
     * snapshot. read_record: buf NULL queries the size; MOD_UNAVAILABLE when there is none.
     * write_record: size 0 deletes it; MOD_UNAVAILABLE without a disc/card; at most
     * SAVE_SNAPSHOT_RECORD_MAX_BYTES. */
    ModResult (*read_record)(ModContext* ctx, const char* save_name, void* buf, size_t* inout_size);
    ModResult (*write_record)(
        ModContext* ctx, const char* save_name, const void* data, size_t size);
} SaveSnapshotService;

MOD_DECLARE_SERVICE(SaveSnapshotService, svc_save_snapshot, SAVE_SNAPSHOT_SERVICE_ID,
    SAVE_SNAPSHOT_SERVICE_MAJOR, SAVE_SNAPSHOT_SERVICE_MINOR);
