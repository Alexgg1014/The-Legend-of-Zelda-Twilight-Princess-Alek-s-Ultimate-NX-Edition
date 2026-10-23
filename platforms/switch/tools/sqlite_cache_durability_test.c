/* aleks2 (SHADERS-c1): which journal_mode / synchronous pair survives a crash or a power cut
 * in the middle of Aurora's cache writes?
 *
 * The console logged "database disk image is malformed" for both caches after forced
 * power-offs; H38 opened them with journal_mode=MEMORY, synchronous=OFF. This measures,
 * instead of assuming, what each alternative does under two failure models:
 *
 *   crash     the process dies at a random write; everything it already wrote reaches
 *             the file (the OS/FS keeps it). An application crash or kill.
 *   powercut  the power goes at a random write; what was made durable by the last sync
 *             is on the card, and of what came after, each write independently may or
 *             may not have reached it. Writes are never torn and metadata (create,
 *             delete, rename) is immediate: a kinder model than real hardware, so a mode
 *             that fails here fails on the console too, not the other way round.
 *
 * Failures are injected by a VFS shim over "unix-none" (the VFS the console uses, locks
 * off) that holds every write since the last xSync in memory; xSync makes them durable.
 * The workload is Aurora's: one transaction per Dawn blob store (REPLACE, 1-48 KiB),
 * batches of 2772-byte pipeline recipes (the size of a GX config), lookups, and now and
 * then the prune (DELETE + VACUUM + wal_checkpoint) that runs after the startup queue
 * drains. After each failure the database is reopened as the next launch would and
 * PRAGMA integrity_check decides.
 *
 * It also counts, on an uninterrupted run, the syncs and bytes each mode issues per
 * committed transaction: on the console every sync is a flush to the SD card, so that is
 * the cost side of the choice, independent of this host's disk.
 *
 * usage: sqlite_cache_durability_test <dir> [trials] [journal/sync that must never fail]
 */
#include "sqlite3.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- random ------------------------------------------------------------------- */
static uint64_t g_rng = 88172645463325252ull;
static uint64_t rnd(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return g_rng;
}
static int rnd_below(int n) { return (int)(rnd() % (uint64_t)n); }

/* ---- the shim ------------------------------------------------------------------ */
typedef struct Op {
  int truncate; /* 0: write, 1: truncate */
  sqlite3_int64 off;
  int len;
  unsigned char* data;
} Op;

typedef struct PcFile {
  sqlite3_file base;
  sqlite3_file* real;
  Op* ops;
  int nops, cap;
  struct PcFile* next;
} PcFile;

static sqlite3_vfs* g_base;
static sqlite3_vfs g_pc;
static PcFile* g_files;
static long g_writeCount, g_cutAt = -1;
static int g_powercut; /* 0: crash (apply all), 1: powercut (random subset) */
static long g_syncs, g_bytes;

static int apply_op(PcFile* f, Op* o) {
  if (o->truncate) return f->real->pMethods->xTruncate(f->real, o->off);
  return f->real->pMethods->xWrite(f->real, o->data, o->len, o->off);
}

static void drop_ops(PcFile* f) {
  for (int i = 0; i < f->nops; i++) free(f->ops[i].data);
  f->nops = 0;
}

static void flush_file(PcFile* f) {
  for (int i = 0; i < f->nops; i++) apply_op(f, &f->ops[i]);
  drop_ops(f);
}

static void cut_now(void) {
  for (PcFile* f = g_files; f; f = f->next) {
    for (int i = 0; i < f->nops; i++) {
      if (!g_powercut || (rnd() & 1)) apply_op(f, &f->ops[i]);
    }
    drop_ops(f);
  }
  _exit(0);
}

static void push_op(PcFile* f, int truncate, const void* data, int len, sqlite3_int64 off) {
  if (f->nops == f->cap) {
    f->cap = f->cap ? f->cap * 2 : 64;
    f->ops = realloc(f->ops, sizeof(Op) * (size_t)f->cap);
  }
  Op* o = &f->ops[f->nops++];
  o->truncate = truncate;
  o->off = off;
  o->len = len;
  o->data = NULL;
  if (!truncate) {
    o->data = malloc((size_t)len);
    memcpy(o->data, data, (size_t)len);
  }
}

static int pc_size(PcFile* f, sqlite3_int64* size) {
  int rc = f->real->pMethods->xFileSize(f->real, size);
  if (rc != SQLITE_OK) return rc;
  for (int i = 0; i < f->nops; i++) {
    Op* o = &f->ops[i];
    if (o->truncate) *size = o->off;
    else if (o->off + o->len > *size) *size = o->off + o->len;
  }
  return SQLITE_OK;
}

static int pc_close(sqlite3_file* file) {
  PcFile* f = (PcFile*)file;
  /* A close is not a sync: what is pending stays pending (the OS would still hold it).
   * Apply it so the real file is complete, as a later sync or writeback would. */
  flush_file(f);
  free(f->ops);
  for (PcFile** p = &g_files; *p; p = &(*p)->next) {
    if (*p == f) {
      *p = f->next;
      break;
    }
  }
  return f->real->pMethods->xClose(f->real);
}

static int pc_read(sqlite3_file* file, void* buf, int amt, sqlite3_int64 off) {
  PcFile* f = (PcFile*)file;
  sqlite3_int64 size;
  pc_size(f, &size);
  memset(buf, 0, (size_t)amt);
  int rc = f->real->pMethods->xRead(f->real, buf, amt, off);
  if (rc != SQLITE_OK && rc != SQLITE_IOERR_SHORT_READ) return rc;
  /* Replay pending writes and truncates over what the real file holds. */
  sqlite3_int64 realSize;
  f->real->pMethods->xFileSize(f->real, &realSize);
  sqlite3_int64 cur = realSize;
  for (int i = 0; i < f->nops; i++) {
    Op* o = &f->ops[i];
    if (o->truncate) {
      if (o->off < off + amt) {
        sqlite3_int64 from = o->off > off ? o->off : off;
        memset((char*)buf + (from - off), 0, (size_t)(off + amt - from));
      }
      cur = o->off;
      continue;
    }
    sqlite3_int64 a = o->off > off ? o->off : off;
    sqlite3_int64 b = (o->off + o->len) < (off + amt) ? (o->off + o->len) : (off + amt);
    if (a < b) memcpy((char*)buf + (a - off), o->data + (a - o->off), (size_t)(b - a));
    if (o->off + o->len > cur) cur = o->off + o->len;
  }
  (void)cur;
  return off + amt > size ? SQLITE_IOERR_SHORT_READ : SQLITE_OK;
}

static int pc_write(sqlite3_file* file, const void* buf, int amt, sqlite3_int64 off) {
  PcFile* f = (PcFile*)file;
  if (g_cutAt >= 0 && ++g_writeCount >= g_cutAt) cut_now();
  push_op(f, 0, buf, amt, off);
  g_bytes += amt;
  return SQLITE_OK;
}

static int pc_truncate(sqlite3_file* file, sqlite3_int64 size) {
  PcFile* f = (PcFile*)file;
  push_op(f, 1, NULL, 0, size);
  return SQLITE_OK;
}

static int pc_sync(sqlite3_file* file, int flags) {
  PcFile* f = (PcFile*)file;
  g_syncs++;
  flush_file(f);
  return f->real->pMethods->xSync(f->real, flags);
}

static int pc_file_size(sqlite3_file* file, sqlite3_int64* size) { return pc_size((PcFile*)file, size); }
static int pc_lock(sqlite3_file* file, int l) { return ((PcFile*)file)->real->pMethods->xLock(((PcFile*)file)->real, l); }
static int pc_unlock(sqlite3_file* file, int l) {
  return ((PcFile*)file)->real->pMethods->xUnlock(((PcFile*)file)->real, l);
}
static int pc_check_reserved(sqlite3_file* file, int* r) {
  return ((PcFile*)file)->real->pMethods->xCheckReservedLock(((PcFile*)file)->real, r);
}
static int pc_file_control(sqlite3_file* file, int op, void* arg) {
  /* Size hints and chunk sizes would write behind the shim's back. */
  if (op == SQLITE_FCNTL_SIZE_HINT || op == SQLITE_FCNTL_CHUNK_SIZE) return SQLITE_OK;
  return ((PcFile*)file)->real->pMethods->xFileControl(((PcFile*)file)->real, op, arg);
}
static int pc_sector_size(sqlite3_file* file) { return ((PcFile*)file)->real->pMethods->xSectorSize(((PcFile*)file)->real); }
static int pc_device_characteristics(sqlite3_file* file) {
  (void)file;
  return 0; /* promise nothing: no atomic writes, no safe append */
}

/* Version 1: no shared memory, so WAL only works in EXCLUSIVE locking mode -- as on the
 * console, where Aurora must not depend on a -shm file it cannot lock. */
static const sqlite3_io_methods g_pcMethods = {
    1, pc_close, pc_read, pc_write, pc_truncate, pc_sync, pc_file_size, pc_lock, pc_unlock,
    pc_check_reserved, pc_file_control, pc_sector_size, pc_device_characteristics,
};

static int pc_open(sqlite3_vfs* vfs, const char* name, sqlite3_file* file, int flags, int* outFlags) {
  (void)vfs;
  PcFile* f = (PcFile*)file;
  memset(f, 0, sizeof(*f));
  f->real = (sqlite3_file*)(f + 1);
  int rc = g_base->xOpen(g_base, name, f->real, flags, outFlags);
  if (rc != SQLITE_OK) {
    f->base.pMethods = NULL;
    return rc;
  }
  f->base.pMethods = &g_pcMethods;
  f->next = g_files;
  g_files = f;
  return SQLITE_OK;
}

static int pc_delete(sqlite3_vfs* vfs, const char* name, int dirSync) {
  (void)vfs;
  return g_base->xDelete(g_base, name, dirSync);
}

static void install_shim(void) {
  g_base = sqlite3_vfs_find("unix-none");
  g_pc = *g_base;
  g_pc.iVersion = 1;
  g_pc.zName = "powercut";
  g_pc.szOsFile = (int)sizeof(PcFile) + g_base->szOsFile;
  g_pc.xOpen = pc_open;
  g_pc.xDelete = pc_delete;
  sqlite3_vfs_register(&g_pc, 0);
}

/* ---- the workload --------------------------------------------------------------- */
typedef struct Mode {
  const char* journal;
  const char* sync;
} Mode;

static int exec(sqlite3* db, const char* sql) { return sqlite3_exec(db, sql, NULL, NULL, NULL); }

static int set_pragmas(sqlite3* db, const Mode* m) {
  char sql[160];
  if (strcmp(m->journal, "wal") == 0 && exec(db, "PRAGMA locking_mode=EXCLUSIVE;") != SQLITE_OK) return 1;
  snprintf(sql, sizeof sql, "PRAGMA journal_mode=%s; PRAGMA synchronous=%s;", m->journal, m->sync);
  return exec(db, sql) != SQLITE_OK;
}

static const char* kSchema =
    "CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER);"
    "CREATE TABLE IF NOT EXISTS cache(key BLOB PRIMARY KEY NOT NULL, value BLOB NOT NULL,"
    " size INTEGER NOT NULL, compressed INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS pipeline_cache(type INTEGER NOT NULL, hash INTEGER NOT NULL,"
    " config_version INTEGER NOT NULL, config_size INTEGER NOT NULL, config BLOB NOT NULL,"
    " first_frame_used INTEGER NOT NULL, PRIMARY KEY(type, hash));"
    "CREATE INDEX IF NOT EXISTS pipeline_cache_load_order_idx ON pipeline_cache(type, config_version, first_frame_used);";

static unsigned char g_blob[48 * 1024];

static void fill(int n) {
  for (int i = 0; i < n; i += 8) {
    uint64_t v = rnd();
    memcpy(g_blob + i, &v, (size_t)(n - i < 8 ? n - i : 8));
  }
}

static long g_commits;

static void store_blob(sqlite3* db, sqlite3_stmt* st) {
  exec(db, "BEGIN IMMEDIATE");
  unsigned char key[16];
  uint64_t k = (uint64_t)rnd_below(1000);
  memset(key, 0, sizeof key);
  memcpy(key, &k, sizeof k);
  int n = 1024 + rnd_below(47 * 1024);
  fill(n);
  sqlite3_bind_blob(st, 1, key, 16, SQLITE_TRANSIENT);
  sqlite3_bind_blob(st, 2, g_blob, n, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, 3, n);
  sqlite3_bind_int(st, 4, 0);
  sqlite3_step(st);
  sqlite3_reset(st);
  if (exec(db, "COMMIT") == SQLITE_OK) g_commits++;
}

static void store_recipes(sqlite3* db, sqlite3_stmt* st) {
  exec(db, "BEGIN IMMEDIATE");
  int n = 1 + rnd_below(20);
  for (int i = 0; i < n; i++) {
    fill(2772);
    sqlite3_bind_int(st, 1, 1);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)rnd_below(4000));
    sqlite3_bind_int(st, 3, 13);
    sqlite3_bind_int(st, 4, 2772);
    sqlite3_bind_blob(st, 5, g_blob, 2772, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, rnd_below(100000));
    sqlite3_step(st);
    sqlite3_reset(st);
  }
  if (exec(db, "COMMIT") == SQLITE_OK) g_commits++;
}

static void prune(sqlite3* db) {
  char sql[96];
  snprintf(sql, sizeof sql, "BEGIN IMMEDIATE; DELETE FROM cache WHERE (rowid %% 7) = %d; COMMIT;", rnd_below(7));
  if (exec(db, sql) == SQLITE_OK) g_commits++;
  exec(db, "VACUUM;");
  exec(db, "PRAGMA wal_checkpoint(TRUNCATE);");
}

static void lookup(sqlite3* db) {
  sqlite3_stmt* st;
  sqlite3_prepare_v2(db, "SELECT value FROM cache WHERE rowid = ?", -1, &st, NULL);
  sqlite3_bind_int(st, 1, 1 + rnd_below(300));
  sqlite3_step(st);
  sqlite3_finalize(st);
}

static int workload(sqlite3* db, int steps) {
  sqlite3_stmt *blob, *recipe;
  if (sqlite3_prepare_v2(db, "REPLACE INTO cache (key, value, size, compressed) VALUES (?, ?, ?, ?)", -1, &blob,
                         NULL) != SQLITE_OK)
    return 1;
  if (sqlite3_prepare_v2(db,
                         "INSERT INTO pipeline_cache (type, hash, config_version, config_size, config, first_frame_used)"
                         " VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT(type, hash) DO UPDATE SET config = excluded.config,"
                         " first_frame_used = MIN(pipeline_cache.first_frame_used, excluded.first_frame_used)",
                         -1, &recipe, NULL) != SQLITE_OK)
    return 1;
  for (int i = 0; steps < 0 || i < steps; i++) {
    int r = rnd_below(100);
    if (r < 68) store_blob(db, blob);
    else if (r < 88) store_recipes(db, recipe);
    else if (r < 98) lookup(db);
    else prune(db);
  }
  sqlite3_finalize(blob);
  sqlite3_finalize(recipe);
  return 0;
}

static void remove_db(const char* path) {
  char p[512];
  unlink(path);
  snprintf(p, sizeof p, "%s-journal", path);
  unlink(p);
  snprintf(p, sizeof p, "%s-wal", path);
  unlink(p);
  snprintf(p, sizeof p, "%s-shm", path);
  unlink(p);
}

/* A cache that has been in use for a while: 300 blobs and 400 recipes, made durable. */
static int make_base(const char* path, const Mode* m) {
  remove_db(path);
  sqlite3* db;
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "unix-none") != SQLITE_OK) return 1;
  set_pragmas(db, m);
  exec(db, kSchema);
  exec(db, "INSERT INTO aurora_schema VALUES (2);");
  workload(db, 400);
  exec(db, "PRAGMA wal_checkpoint(TRUNCATE);");
  sqlite3_close(db);
  return 0;
}

typedef struct Result {
  int ok, damaged, unopenable;
} Result;

/* Reopen as the next launch does (same mode, so a hot journal or WAL is replayed). */
static int verify(const char* path, const Mode* m, char* why, size_t whyLen) {
  sqlite3* db;
  why[0] = 0;
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, "unix-none") != SQLITE_OK) {
    snprintf(why, whyLen, "open: %s", sqlite3_errmsg(db));
    sqlite3_close(db);
    return 2;
  }
  if (set_pragmas(db, m)) {
    snprintf(why, whyLen, "pragmas: %s", sqlite3_errmsg(db));
    sqlite3_close(db);
    return 1;
  }
  sqlite3_stmt* st;
  int verdict = 1;
  if (sqlite3_prepare_v2(db, "PRAGMA integrity_check(1)", -1, &st, NULL) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) {
      const char* t = (const char*)sqlite3_column_text(st, 0);
      if (t && strcmp(t, "ok") == 0) verdict = 0;
      else snprintf(why, whyLen, "integrity: %s", t ? t : "?");
    } else {
      snprintf(why, whyLen, "integrity step: %s", sqlite3_errmsg(db));
    }
    sqlite3_finalize(st);
  } else {
    snprintf(why, whyLen, "integrity prepare: %s", sqlite3_errmsg(db));
  }
  sqlite3_close(db);
  return verdict;
}

static Result run_trials(const char* dir, const Mode* m, int powercut, int trials, char* firstWhy, size_t whyLen) {
  Result r = {0, 0, 0};
  char path[512];
  snprintf(path, sizeof path, "%s/durability-%s-%s-%d.db", dir, m->journal, m->sync, powercut);
  firstWhy[0] = 0;
  for (int t = 0; t < trials; t++) {
    uint64_t seed = 0x9E3779B97F4A7C15ull * (uint64_t)(t + 1) ^ (uint64_t)powercut;
    g_rng = seed ? seed : 1;
    make_base(path, m);
    long cutAt = 1 + rnd_below(6000);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
      g_cutAt = cutAt;
      g_writeCount = 0;
      g_powercut = powercut;
      sqlite3* db;
      if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, "powercut") != SQLITE_OK) _exit(3);
      set_pragmas(db, m);
      workload(db, -1);
      _exit(4);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    char why[256];
    int v = verify(path, m, why, sizeof why);
    if (v == 0) r.ok++;
    else {
      if (v == 2) r.unopenable++;
      else r.damaged++;
      if (!firstWhy[0]) snprintf(firstWhy, whyLen, "%s", why);
    }
  }
  remove_db(path);
  return r;
}

/* Syncs and bytes per committed transaction, uninterrupted. */
static void cost(const char* dir, const Mode* m, double* syncsPerCommit, double* kbPerCommit) {
  char path[512];
  snprintf(path, sizeof path, "%s/cost-%s-%s.db", dir, m->journal, m->sync);
  g_rng = 12345;
  make_base(path, m);
  sqlite3* db;
  sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, "powercut");
  set_pragmas(db, m);
  g_syncs = g_bytes = g_commits = 0;
  g_cutAt = -1;
  workload(db, 2000);
  sqlite3_close(db);
  *syncsPerCommit = g_commits ? (double)g_syncs / (double)g_commits : 0.0;
  *kbPerCommit = g_commits ? (double)g_bytes / 1024.0 / (double)g_commits : 0.0;
  remove_db(path);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <dir> [trials] [journal/sync]\n", argv[0]);
    return 2;
  }
  const char* dir = argv[1];
  int trials = argc > 2 ? atoi(argv[2]) : 200;
  const char* must = argc > 3 ? argv[3] : "wal/normal";
  sqlite3_initialize();
  install_shim();
  static const Mode modes[] = {
      {"memory", "off"},   {"memory", "normal"}, {"delete", "off"}, {"delete", "normal"}, {"delete", "full"},
      {"truncate", "normal"}, {"persist", "normal"}, {"wal", "off"}, {"wal", "normal"}, {"wal", "full"},
  };
  printf("sqlite %s, %d trials per cell, cut at a random write in [1, 6000]\n", sqlite3_libversion(), trials);
  printf("%-9s %-7s | %-24s | %-24s | %8s %8s\n", "journal", "sync", "crash ok/damaged/unopen", "powercut ok/dmg/unopen",
         "sync/tx", "KiB/tx");
  int bad = 0;
  for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
    const Mode* m = &modes[i];
    char why1[256], why2[256];
    Result c = run_trials(dir, m, 0, trials, why1, sizeof why1);
    Result p = run_trials(dir, m, 1, trials, why2, sizeof why2);
    double spc, kpc;
    cost(dir, m, &spc, &kpc);
    printf("%-9s %-7s | %6d %6d %6d       | %6d %6d %6d       | %8.2f %8.1f\n", m->journal, m->sync, c.ok, c.damaged,
           c.unopenable, p.ok, p.damaged, p.unopenable, spc, kpc);
    if (why1[0]) printf("    first crash failure:    %s\n", why1);
    if (why2[0]) printf("    first powercut failure: %s\n", why2);
    char name[32];
    snprintf(name, sizeof name, "%s/%s", m->journal, m->sync);
    if (strcmp(name, must) == 0 && (c.damaged + p.damaged + c.unopenable + p.unopenable)) bad = 1;
  }
  printf("%s: %s\n", must, bad ? "FAILED (damage after a crash or power cut)" : "never damaged");
  fflush(stdout);
  return bad;
}
