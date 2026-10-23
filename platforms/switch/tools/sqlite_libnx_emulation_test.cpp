/* aleks2 (V1-c3 investigation): what in libnx's fsdev makes SQLite answer "database disk
 * image is malformed" on a brand-new database?
 *
 * libnx (nx/source/runtime/devices/fs_dev.c) translates only four Horizon results to errno
 * (path not found -> ENOENT, exists -> EEXIST, two -> EINVAL/ENAMETOOLONG); everything else
 * becomes EIO. SQLite's unixRead turns a read() failing with EIO into SQLITE_IOERR_CORRUPTFS,
 * which the VDBE reports as SQLITE_CORRUPT ("malformed", xerr=11). So on the console any
 * refused read looks like corruption. Which read? This replaces SQLite's syscalls with
 * wrappers that can each imitate one Horizon rule, traces every call, and runs the exact
 * V1-c2 sequence (new file, journal_mode + synchronous, Dawn's schema in BEGIN IMMEDIATE)
 * under each rule, in MEMORY and in DELETE/TRUNCATE/PERSIST.
 *
 *   rule read_past_eof   read() starting beyond the end of the file fails (EIO)
 *   rule read_at_eof     read() starting exactly at the end fails too (EIO)
 *   rule write_gap       write() starting beyond the end fails (EIO)
 *   rule stat_open       stat() of a file this process has open fails (EIO)
 *
 * usage: sqlite_libnx_emulation_test <dir> [trace]
 */
#include "sqlite3.h"
#include "sqlite_cache_open.hpp"
#include "sqlite_selftest.hpp"

#include <string>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_shimFailures, g_reproFailures;
static int r_read_past, r_read_at, r_write_gap, r_stat_open, r_fallback, r_hole_garbage, g_trace;
static sqlite3_syscall_ptr real_open, real_close, real_read, real_write, real_stat, real_fstat, real_pread,
    real_pwrite;

/* Which fds are open and on what path (to apply stat_open). */
static char g_paths[256][512];

typedef int (*open_fn)(const char*, int, int);
typedef int (*close_fn)(int);
typedef ssize_t (*read_fn)(int, void*, size_t);
typedef ssize_t (*write_fn)(int, const void*, size_t);
typedef int (*stat_fn)(const char*, struct stat*);
typedef int (*fstat_fn)(int, struct stat*);

static const char* base(const char* p) {
  const char* s = strrchr(p, '/');
  return s ? s + 1 : p;
}

static int w_open(const char* p, int f, int m) {
  int fd = ((open_fn)real_open)(p, f, m);
  if (fd >= 0 && fd < 256) snprintf(g_paths[fd], sizeof g_paths[fd], "%s", p);
  if (g_trace) printf("    open(%s, %s%s) = %d\n", base(p), (f & O_CREAT) ? "CREAT|" : "", (f & O_RDWR) ? "RDWR" : "RD", fd);
  return fd;
}
static int w_close(int fd) {
  if (fd >= 0 && fd < 256) g_paths[fd][0] = 0;
  return ((close_fn)real_close)(fd);
}
static off_t size_of(int fd) {
  struct stat st;
  return ((fstat_fn)real_fstat)(fd, &st) == 0 ? st.st_size : -1;
}
static ssize_t w_read(int fd, void* b, size_t n) {
  off_t off = lseek(fd, 0, SEEK_CUR), sz = size_of(fd);
  int refuse = (r_read_past && off > sz) || (r_read_at && off >= sz && n > 0);
  if (g_trace) printf("    read(%s, off=%lld, n=%zu) size=%lld%s\n", base(g_paths[fd]), (long long)off, n, (long long)sz, refuse ? " -> EIO" : "");
  if (refuse) { errno = EIO; return -1; }
  return ((read_fn)real_read)(fd, b, n);
}
static ssize_t w_write(int fd, const void* b, size_t n) {
  off_t off = lseek(fd, 0, SEEK_CUR), sz = size_of(fd);
  int refuse = r_write_gap && off > sz;
  if (r_hole_garbage && off > sz && sz >= 0) {
    /* Horizon (V1-c3 probe): the bytes skipped by a write past the end are not zeros. */
    char junk[4096];
    memset(junk, 0xAA, sizeof junk);
    lseek(fd, sz, SEEK_SET);
    for (off_t at = sz; at < off;) {
      size_t k = (size_t)((off - at) < (off_t)sizeof junk ? (off - at) : (off_t)sizeof junk);
      ssize_t w = ((write_fn)real_write)(fd, junk, k);
      if (w <= 0) break;
      at += w;
    }
    lseek(fd, off, SEEK_SET);
  }
  if (g_trace) printf("    write(%s, off=%lld, n=%zu) size=%lld%s\n", base(g_paths[fd]), (long long)off, n, (long long)sz, refuse ? " -> EIO" : "");
  if (refuse) { errno = EIO; return -1; }
  return ((write_fn)real_write)(fd, b, n);
}
/* Linux builds of SQLite use pread/pwrite; the console's (newlib) uses lseek+read/write.
 * Same rules, by explicit offset. */
typedef ssize_t (*pread_fn)(int, void*, size_t, off_t);
typedef ssize_t (*pwrite_fn)(int, const void*, size_t, off_t);
static ssize_t w_pread(int fd, void* b, size_t n, off_t off) {
  off_t sz = size_of(fd);
  int refuse = (r_read_past && off > sz) || (r_read_at && off >= sz && n > 0);
  if (g_trace) printf("    pread(%s, off=%lld, n=%zu) size=%lld%s\n", base(g_paths[fd]), (long long)off, n, (long long)sz, refuse ? " -> EIO" : "");
  if (refuse) { errno = EIO; return -1; }
  return ((pread_fn)real_pread)(fd, b, n, off);
}
static ssize_t w_pwrite(int fd, const void* b, size_t n, off_t off) {
  off_t sz = size_of(fd);
  int refuse = r_write_gap && off > sz;
  if (r_hole_garbage && off > sz && sz >= 0) {
    char junk[4096];
    memset(junk, 0xAA, sizeof junk);
    for (off_t at = sz; at < off;) {
      size_t k = (size_t)((off - at) < (off_t)sizeof junk ? (off - at) : (off_t)sizeof junk);
      ssize_t w = ((pwrite_fn)real_pwrite)(fd, junk, k, at);
      if (w <= 0) break;
      at += w;
    }
  }
  if (g_trace) printf("    pwrite(%s, off=%lld, n=%zu) size=%lld%s\n", base(g_paths[fd]), (long long)off, n, (long long)sz, refuse ? " -> EIO" : "");
  if (refuse) { errno = EIO; return -1; }
  return ((pwrite_fn)real_pwrite)(fd, b, n, off);
}

static int w_stat(const char* p, struct stat* st) {
  int open_here = 0;
  for (int i = 0; i < 256; i++) if (g_paths[i][0] && strcmp(g_paths[i], p) == 0) open_here = 1;
  int rc;
  if (r_stat_open && open_here) { errno = EIO; rc = -1; }
  else rc = ((stat_fn)real_stat)(p, st);
  /* V1-c2's busy_tolerant_stat on top: an existing-but-refused file is a plain 0644 file. */
  if (rc != 0 && r_fallback && errno != ENOENT && errno != ENOTDIR && errno != ENAMETOOLONG) {
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFREG | 0644;
    st->st_nlink = 1;
    rc = 0;
  }
  if (g_trace) printf("    stat(%s) = %d%s\n", base(p), rc, rc ? (errno == ENOENT ? " ENOENT" : " EIO") : "");
  return rc;
}

static void install(void) {
  sqlite3_vfs* v = sqlite3_vfs_find("unix-none");
  real_open = v->xGetSystemCall(v, "open");
  real_close = v->xGetSystemCall(v, "close");
  real_read = v->xGetSystemCall(v, "read");
  real_write = v->xGetSystemCall(v, "write");
  real_stat = v->xGetSystemCall(v, "stat");
  real_fstat = v->xGetSystemCall(v, "fstat");
  real_pread = v->xGetSystemCall(v, "pread");
  real_pwrite = v->xGetSystemCall(v, "pwrite");
  if (real_pread) v->xSetSystemCall(v, "pread", (sqlite3_syscall_ptr)w_pread);
  if (real_pwrite) v->xSetSystemCall(v, "pwrite", (sqlite3_syscall_ptr)w_pwrite);
  v->xSetSystemCall(v, "open", (sqlite3_syscall_ptr)w_open);
  v->xSetSystemCall(v, "close", (sqlite3_syscall_ptr)w_close);
  v->xSetSystemCall(v, "read", (sqlite3_syscall_ptr)w_read);
  v->xSetSystemCall(v, "write", (sqlite3_syscall_ptr)w_write);
  v->xSetSystemCall(v, "stat", (sqlite3_syscall_ptr)w_stat);
}

/* gpu_cache.cpp: pragmas, then ensure_schema_up_to_date in BEGIN IMMEDIATE; then one blob
 * store transaction, close, reopen, read it back. */
static const char* run(const char* path, const char* journal, const char* sync) {
  static char why[512];
  for (const char* s : (const char*[]){"", "-journal", "-wal", "-shm"}) {
    char p[600];
    snprintf(p, sizeof p, "%s%s", path, s);
    unlink(p);
  }
  sqlite3* db = 0;
  char sql[256];
  int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "unix-none");
  if (rc) { snprintf(why, sizeof why, "open: %s", sqlite3_errmsg(db)); sqlite3_close(db); return why; }
  sqlite3_extended_result_codes(db, 1);
  snprintf(sql, sizeof sql, "PRAGMA journal_mode=%s; PRAGMA synchronous=%s;", journal, sync);
  const char* steps[] = {
      sql,
      "BEGIN IMMEDIATE; CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER);"
      "SELECT * FROM aurora_schema WHERE value = 2;"
      "DROP TABLE IF EXISTS cache; CREATE TABLE cache (key BLOB PRIMARY KEY NOT NULL, value BLOB NOT NULL,"
      " size INTEGER NOT NULL, compressed INTEGER NOT NULL); DELETE FROM aurora_schema;"
      " INSERT INTO aurora_schema VALUES (2); COMMIT;",
      "BEGIN IMMEDIATE; REPLACE INTO cache VALUES (x'0102', randomblob(20000), 20000, 0); COMMIT;",
  };
  const char* names[] = {"pragmas", "schema transaction (BEGIN IMMEDIATE ...)", "store transaction"};
  for (int i = 0; i < 3; i++) {
    if (g_trace) printf("  -- %s\n", names[i]);
    rc = sqlite3_exec(db, steps[i], 0, 0, 0);
    if (rc) {
      snprintf(why, sizeof why, "%s: rc=%d (%s) msg='%s' errno=%d", names[i], sqlite3_extended_errcode(db),
               sqlite3_errstr(sqlite3_extended_errcode(db)), sqlite3_errmsg(db), sqlite3_system_errno(db));
      sqlite3_close(db);
      return why;
    }
  }
  sqlite3_close(db);
  if (g_trace) printf("  -- reopen and read\n");
  rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, "unix-none");
  snprintf(sql, sizeof sql, "PRAGMA journal_mode=%s; SELECT length(value) FROM cache;", journal);
  if (!rc) rc = sqlite3_exec(db, sql, 0, 0, 0);
  if (rc) {
    snprintf(why, sizeof why, "reopen/read: rc=%d (%s) msg='%s'", sqlite3_extended_errcode(db),
             sqlite3_errstr(sqlite3_extended_errcode(db)), sqlite3_errmsg(db));
    sqlite3_close(db);
    return why;
  }
  sqlite3_close(db);
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  sqlite3_initialize();
  install();
  char path[512];
  snprintf(path, sizeof path, "%s/emu.db", argv[1]);
  struct { const char* name; int a, b, c, d, e, f, shims; } rules[] = {
      {"none (Linux semantics)", 0, 0, 0, 0, 0, 0, 0},
      {"read_past_eof", 1, 0, 0, 0, 0, 0, 0},
      {"read_at_eof", 1, 1, 0, 0, 0, 0, 0},
      {"write_gap", 0, 0, 1, 0, 0, 0, 0},
      {"stat_open (V1-c1)", 0, 0, 0, 1, 0, 0, 0},
      {"stat_open + fallback (V1-c2)", 0, 0, 0, 1, 1, 0, 0},
      {"libnx as measured on the console (V1-c3), no shims", 1, 0, 0, 1, 0, 1, 0},
      {"libnx as measured + V1-c4 shims", 1, 0, 0, 1, 0, 1, 1},
  };
  const char* modes[][2] = {{"memory", "off"}, {"delete", "normal"}, {"truncate", "normal"}, {"persist", "normal"}};
  for (size_t r = 0; r < sizeof rules / sizeof rules[0]; r++) {
    r_read_past = rules[r].a; r_read_at = rules[r].b; r_write_gap = rules[r].c; r_stat_open = rules[r].d;
    r_fallback = rules[r].e;
    r_hole_garbage = rules[r].f;
    if (rules[r].shims) aurora::sqlite::install_libnx_io_shims("unix-none", {});
    printf("== rule %s\n", rules[r].name);
    for (size_t m = 0; m < sizeof modes / sizeof modes[0]; m++) {
      g_trace = argc > 2 && strcmp(argv[2], "trace") == 0 && m == 1 && r == 0;
      const char* fail = run(path, modes[m][0], modes[m][1]);
      printf("  %-8s/%-6s %s\n", modes[m][0], modes[m][1], fail ? fail : "OK");
      if (rules[r].shims && fail) g_shimFailures++;
      if (r == 6 && !fail) g_reproFailures++; /* without shims it must fail, as on the console */
    }
    if (rules[r].shims) {
      /* The self-test the console runs, under the same emulated libnx. */
      auto st = aurora::sqlite::run_sqlite_selftest(std::string(argv[1]) + "/selftest", "unix-none");
      printf("  selftest under emulated libnx + shims: best journal=%s sync=%s\n",
             st.journal.empty() ? "none" : st.journal.c_str(), st.sync.empty() ? "none" : st.sync.c_str());
      if (st.journal.empty()) g_shimFailures++;
      aurora::sqlite::uninstall_libnx_io_shims("unix-none");
    }
  }
  printf(g_shimFailures == 0 && g_reproFailures == 0
             ? "libnx emulation: console failure reproduced without the shims, fixed with them\n"
             : "libnx emulation: FAILED (%d with shims, %d unexpected passes without)\n",
         g_shimFailures, g_reproFailures);
  return g_shimFailures == 0 && g_reproFailures == 0 ? 0 : 1;
}
