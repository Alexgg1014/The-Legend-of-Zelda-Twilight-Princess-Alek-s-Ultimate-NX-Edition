// aleks2 (SHADERS-c1) host test for extern/aurora/lib/sqlite_cache_open.cpp: a damaged
// cache is detected, set aside under a dated name (never deleted on the spot), recreated
// and reported; a healthy one is left alone; an error that is not damage is not
// "repaired". Built by test-sqlite-cache-repair.sh against the SQLite amalgamation Aurora
// builds (3.51.3), with the console's VFS base ("unix-none").
//
// usage: sqlite_cache_repair_test <scratch dir> [a damaged cache taken off a console]
#include "sqlite_cache_open.hpp"

#include <sqlite3.h>

#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace aurora::sqlite;

namespace {
int g_failures = 0;
std::vector<std::string> g_log;

void check(bool ok, const std::string& what) {
  std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
  if (!ok) {
    ++g_failures;
  }
}

const LogFn kLog = [](LogLevel level, const std::string& text) {
  static const char* names[] = {"debug", "info", "warn", "error"};
  g_log.push_back(std::string{names[static_cast<int>(level)]} + ": " + text);
};

bool logged(const std::string& needle) {
  for (const auto& line : g_log) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// The schema Aurora's Dawn cache makes (gpu_cache.cpp ensure_schema_up_to_date), inside
// BEGIN IMMEDIATE as there: on the console log the damage showed up exactly here
// ("Failed to start transaction: database disk image is malformed (xerr=11)").
int dawn_init(sqlite3* db) {
  int ret = sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
  if (ret != SQLITE_OK) {
    return ret;
  }
  ret = sqlite3_exec(db,
                     "CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER);"
                     "CREATE TABLE IF NOT EXISTS cache (key BLOB PRIMARY KEY NOT NULL, value BLOB NOT NULL,"
                     " size INTEGER NOT NULL, compressed INTEGER NOT NULL);"
                     "INSERT INTO aurora_schema SELECT 2 WHERE NOT EXISTS (SELECT 1 FROM aurora_schema);",
                     nullptr, nullptr, nullptr);
  if (ret != SQLITE_OK) {
    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    return ret;
  }
  return sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
}

int count_rows(sqlite3* db) {
  int n = -1;
  sqlite3_exec(
      db, "SELECT COUNT(*) FROM cache",
      [](void* out, int, char** argv, char**) {
        *static_cast<int*>(out) = std::atoi(argv[0]);
        return 0;
      },
      &n, nullptr);
  return n;
}

// A cache with `rows` blobs of 8 KiB, closed cleanly.
void make_cache(const std::string& path, int rows, const CacheOpenOptions& options) {
  sqlite3* db = nullptr;
  CacheOpenReport report;
  open_cache_checked(path, &db, options, dawn_init, report, kLog);
  std::vector<unsigned char> blob(8192);
  sqlite3_stmt* st = nullptr;
  sqlite3_prepare_v2(db, "REPLACE INTO cache VALUES (?, ?, ?, 0)", -1, &st, nullptr);
  for (int i = 0; i < rows; ++i) {
    for (size_t b = 0; b < blob.size(); ++b) {
      blob[b] = static_cast<unsigned char>((i * 131 + b * 7) & 0xff);
    }
    unsigned char key[16] = {};
    std::memcpy(key, &i, sizeof i);
    sqlite3_bind_blob(st, 1, key, 16, SQLITE_TRANSIENT);
    sqlite3_bind_blob(st, 2, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, static_cast<int>(blob.size()));
    sqlite3_step(st);
    sqlite3_reset(st);
  }
  sqlite3_finalize(st);
  close_cache(db, path, true);
}

std::vector<std::string> quarantined(const std::string& dir, const std::string& stem) {
  std::vector<std::string> out;
  for (const auto& e : fs::directory_iterator(dir)) {
    const auto name = e.path().filename().string();
    if (name.rfind(stem + ".corrupt-", 0) == 0 && name.size() > 3 && name.substr(name.size() - 3) == ".db") {
      out.push_back(name);
    }
  }
  return out;
}

void overwrite(const std::string& path, long offset, const std::string& bytes) {
  std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
  f.seekp(offset);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void reset_dir(const std::string& dir) {
  fs::remove_all(dir);
  fs::create_directories(dir);
  g_log.clear();
}

sqlite3_syscall_ptr g_realStatForTest = nullptr;

// What libnx does: stat() on an existing file this process may have open fails (EBUSY);
// a missing file is still ENOENT.
int libnx_like_stat(const char* path, struct stat* out) {
  using StatFn = int (*)(const char*, struct stat*);
  const int rc = reinterpret_cast<StatFn>(g_realStatForTest)(path, out);
  const std::string p{path};
  if (rc == 0 && p.size() > 3 && p.compare(p.size() - 3, 3, ".db") == 0) {
    errno = EBUSY;
    return -1;
  }
  return rc;
}

struct Case {
  std::string name;
  CacheOpenOptions options;
};

// Opens `path` and expects it repaired (or not) with a usable, empty-or-intact cache after.
void expect_open(const std::string& label, const std::string& path, const CacheOpenOptions& options,
                 bool expectRepair, int expectRows) {
  sqlite3* db = nullptr;
  CacheOpenReport report;
  const int ret = open_cache_checked(path, &db, options, dawn_init, report, kLog);
  check(ret == SQLITE_OK && db != nullptr, label + ": cache usable after open");
  check(report.repaired == expectRepair, label + ": repaired=" + (report.repaired ? "yes" : "no") + " (expected " +
                                             (expectRepair ? "yes" : "no") + ")" +
                                             (report.reason.empty() ? "" : " reason: " + report.reason));
  if (db != nullptr) {
    check(count_rows(db) == expectRows,
          label + ": rows " + std::to_string(count_rows(db)) + " (expected " + std::to_string(expectRows) + ")");
    close_cache(db, path, true);
  }
  if (expectRepair) {
    check(!report.quarantinedAs.empty() && fs::exists(report.quarantinedAs),
          label + ": damaged file kept as " + report.quarantinedAs);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: sqlite_cache_repair_test <scratch dir> [console cache]\n";
    return 2;
  }
  const std::string root = argv[1];
  sqlite3_initialize();

  const Case cases[] = {
      {"wal/normal", {JournalChoice::Wal, SyncChoice::Normal, CheckPolicy::Dirty, 2, "unix-none"}},
      {"delete/normal", {JournalChoice::Delete, SyncChoice::Normal, CheckPolicy::Dirty, 2, "unix-none"}},
  };

  for (const auto& c : cases) {
    std::cout << "== " << c.name << "\n";
    const std::string dir = root + "/" + (c.options.journal == JournalChoice::Wal ? "wal" : "delete");
    const std::string path = dir + "/dawn_cache.db";
    auto options = c.options;

    // 1. A fresh cache: created, marked open, the asked journal mode in force.
    reset_dir(dir);
    {
      sqlite3* db = nullptr;
      CacheOpenReport report;
      const int ret = open_cache_checked(path, &db, options, dawn_init, report, kLog);
      check(ret == SQLITE_OK, "fresh: opens");
      check(!report.repaired && !report.checked, "fresh: no repair, no check");
      check(report.journalMode == journal_name(options.journal), "fresh: journal_mode=" + report.journalMode);
      check(fs::exists(path + ".open"), "fresh: open marker present while in use");
      close_cache(db, path, true);
      check(!fs::exists(path + ".open"), "fresh: open marker removed by a clean close");
    }

    // 2. A healthy cache after a clean close: not checked (policy dirty), rows intact.
    reset_dir(dir);
    make_cache(path, 50, options);
    expect_open("healthy", path, options, false, 50);

    // 3. A healthy cache whose last session did not close (crash): checked, kept.
    reset_dir(dir);
    make_cache(path, 50, options);
    std::ofstream(path + ".open") << "1";
    {
      sqlite3* db = nullptr;
      CacheOpenReport report;
      open_cache_checked(path, &db, options, dawn_init, report, kLog);
      check(report.dirtyAtOpen && report.checked && !report.repaired,
            "unclosed but healthy: quick_check ran (" + std::to_string(report.checkMs) + " ms) and kept it");
      check(count_rows(db) == 50, "unclosed but healthy: rows intact");
      close_cache(db, path, true);
    }

    // 4. Header garbage: "file is not a database" at the first statement.
    reset_dir(dir);
    make_cache(path, 50, options);
    overwrite(path, 0, std::string(100, '\x5a'));
    expect_open("header garbage", path, options, true, 0);
    check(logged("set aside as"), "header garbage: the repair is logged");

    // 5. Truncated to half (an interrupted copy, a cut write): detected and repaired.
    reset_dir(dir);
    make_cache(path, 200, options);
    fs::resize_file(path, fs::file_size(path) / 2);
    std::ofstream(path + ".open") << "1";
    expect_open("truncated", path, options, true, 0);

    // 6. Interior pages scribbled over (what a half-written commit leaves without a
    //    journal), with the previous session unclosed: quick_check finds it.
    reset_dir(dir);
    make_cache(path, 200, options);
    {
      const auto size = static_cast<long>(fs::file_size(path));
      for (long off = 4096 * 2; off < size; off += 4096 * 5) {
        overwrite(path, off, std::string(64, '\x00') + std::string(32, '\xff'));
      }
    }
    std::ofstream(path + ".open") << "1";
    expect_open("scribbled pages", path, options, true, 0);

    // 7. Damage found by init even when checks are off (policy never): BEGIN on a
    //    malformed schema page, the console's exact failure.
    reset_dir(dir);
    make_cache(path, 20, options);
    overwrite(path, 100, std::string(200, '\x11'));  // page 1 b-tree header and cell pointers
    {
      auto never = options;
      never.check = CheckPolicy::Never;
      expect_open("damage at init, checks off", path, never, true, 0);
    }

    // 8. A zero-length file (the console card had one): a valid empty database.
    reset_dir(dir);
    std::ofstream(path).close();
    expect_open("zero-length", path, options, false, 0);

    // 9. Not damage: the directory does not exist. Nothing is renamed or created.
    reset_dir(dir);
    {
      sqlite3* db = nullptr;
      CacheOpenReport report;
      const std::string missing = dir + "/no-such-dir/dawn_cache.db";
      const int ret = open_cache_checked(missing, &db, options, dawn_init, report, kLog);
      check(ret != SQLITE_OK && db == nullptr && !report.repaired, "missing directory: an error, not a repair");
      check(quarantined(dir, "dawn_cache").empty(), "missing directory: nothing quarantined");
    }

    // 10. Siblings go with the damaged file, so the new database never sees them. (A
    //     -journal or -wal present at open is SQLite's own business first -- a valid hot
    //     journal is replayed, which is exactly the recovery a journal exists for -- so
    //     the move itself is tested here, on the files as the repair finds them.)
    reset_dir(dir);
    make_cache(path, 10, options);
    overwrite(path, 0, std::string(100, '\x5a'));
    std::ofstream(path + "-journal") << "journal of the damaged file";
    std::ofstream(path + "-wal") << "wal of the damaged file";
    {
      const std::string moved = quarantine_file(path, "20261015-120000", kLog);
      check(!moved.empty() && fs::exists(moved + "-journal") && fs::exists(moved + "-wal") &&
                !fs::exists(path + "-journal") && !fs::exists(path + "-wal"),
            "siblings: -journal and -wal moved with it as " + moved);
      const std::string again = quarantine_file(path, "20261015-120000", kLog);
      check(again.empty(), "siblings: nothing left to move the second time");
      expect_open("after the move", path, options, false, 0);
    }

    // 11. Quarantine is bounded: three repairs in a row keep the newest two.
    reset_dir(dir);
    for (int i = 0; i < 3; ++i) {
      make_cache(path, 5, options);
      overwrite(path, 0, std::string(100, '\x5a'));
      sqlite3* db = nullptr;
      CacheOpenReport report;
      open_cache_checked(path, &db, options, dawn_init, report, kLog);
      close_cache(db, path, true);
    }
    check(quarantined(dir, "dawn_cache").size() == 2,
          "bounded: " + std::to_string(quarantined(dir, "dawn_cache").size()) + " quarantined copies kept (2)");
    check(logged("removed an older quarantined cache"), "bounded: the removal is logged");

    // 12. An unclean close after the session saw damage keeps the marker: next open checks.
    reset_dir(dir);
    make_cache(path, 5, options);
    {
      sqlite3* db = nullptr;
      CacheOpenReport report;
      open_cache_checked(path, &db, options, dawn_init, report, kLog);
      close_cache(db, path, false);
      check(fs::exists(path + ".open"), "damage seen in session: marker kept for the next open");
    }
  }

  // 15. V1-c2, the console's failure: libnx's stat() refuses a file this process has open
  //     for writing, and SQLite stats the database to create its -journal. Reproduced with
  //     a stat() that fails with EBUSY on any existing .db; then fixed by the fallback.
  {
    std::cout << "== libnx stat (V1-c2)\n";
    const std::string dir = root + "/libnx-stat";
    reset_dir(dir);
    const std::string path = dir + "/pipeline_cache.db";
    sqlite3_vfs* base = sqlite3_vfs_find("unix-none");
    g_realStatForTest = base->xGetSystemCall(base, "stat");
    base->xSetSystemCall(base, "stat", reinterpret_cast<sqlite3_syscall_ptr>(libnx_like_stat));
    CacheOpenOptions del{JournalChoice::Delete, SyncChoice::Normal, CheckPolicy::Dirty, 2, "unix-none"};
    {
      sqlite3* db = nullptr;
      CacheOpenReport report;
      const int ret = open_cache_checked(path, &db, del, dawn_init, report, kLog);
      check(ret == SQLITE_IOERR_FSTAT && db == nullptr,
            "libnx stat, no fallback: fails with IOERR_FSTAT (" + std::to_string(ret) + "), as on the console");
      check(report.repaired, "libnx stat, no fallback: the unusable file was set aside and retried once");
    }
    reset_dir(dir);
    check(install_busy_stat_fallback("unix-none", kLog), "libnx stat: fallback installed");
    // A -wal left by V1-c1's refused WAL attempt is removed when WAL is not in use.
    std::ofstream(path + "-wal") << "orphan";
    {
      sqlite3* db = nullptr;
      CacheOpenReport report;
      const int ret = open_cache_checked(path, &db, del, dawn_init, report, kLog);
      check(ret == SQLITE_OK && db != nullptr && !report.repaired,
            "libnx stat + fallback: opens, journal_mode=" + report.journalMode);
      check(!fs::exists(path + "-wal"), "orphan -wal removed");
      sqlite3_exec(db, "INSERT INTO cache VALUES (x'01', x'02', 1, 0)", nullptr, nullptr, nullptr);
      check(count_rows(db) == 1, "libnx stat + fallback: a write transaction commits");
      close_cache(db, path, true);
      sqlite3* again = nullptr;
      open_cache_checked(path, &again, del, dawn_init, report, kLog);
      check(count_rows(again) == 1, "libnx stat + fallback: the row is there after reopening");
      close_cache(again, path, true);
    }
    // Back to the real stat for anything after this.
    base->xSetSystemCall(base, "stat", g_realStatForTest);
  }

  // 13. Options from the environment.
  {
    std::cout << "== environment\n";
    setenv("AURORA_CACHE_JOURNAL", "delete", 1);
    setenv("AURORA_CACHE_SYNC", "full", 1);
    setenv("AURORA_CACHE_CHECK", "always", 1);
    auto o = cache_options_from_env(kLog);
    check(o.journal == JournalChoice::Delete && o.sync == SyncChoice::Full && o.check == CheckPolicy::Always,
          "env: delete/full/always read");
    setenv("AURORA_CACHE_JOURNAL", "bogus", 1);
    g_log.clear();
    o = cache_options_from_env(kLog);
    check(o.journal == CacheOpenOptions{}.journal && logged("not a known value"), "env: unknown value -> default + log");
    unsetenv("AURORA_CACHE_JOURNAL");
    unsetenv("AURORA_CACHE_SYNC");
    unsetenv("AURORA_CACHE_CHECK");
  }

  // 14. The damaged pipeline cache taken off the console (2026-10-05 card archive).
  if (argc > 2) {
    std::cout << "== console file " << argv[2] << "\n";
    const std::string dir = root + "/console";
    reset_dir(dir);
    const std::string path = dir + "/pipeline_cache.db";
    fs::copy_file(argv[2], path);
    const auto originalSize = fs::file_size(path);
    CacheOpenOptions options{JournalChoice::Wal, SyncChoice::Normal, CheckPolicy::Dirty, 2, "unix-none"};
    sqlite3* db = nullptr;
    CacheOpenReport report;
    const int ret = open_cache_checked(
        path, &db, options,
        [](sqlite3* d) {
          int r = sqlite3_exec(d, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
          if (r != SQLITE_OK) {
            return r;
          }
          r = sqlite3_exec(d, "CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER);", nullptr, nullptr, nullptr);
          sqlite3_exec(d, r == SQLITE_OK ? "COMMIT" : "ROLLBACK", nullptr, nullptr, nullptr);
          return r;
        },
        report, kLog);
    check(ret == SQLITE_OK && report.repaired, "console file: repaired (" + report.reason + ")");
    check(!report.quarantinedAs.empty() && fs::file_size(report.quarantinedAs) == originalSize,
          "console file: kept byte for byte as " + report.quarantinedAs);
    close_cache(db, path, true);
  }

  for (const auto& line : g_log) {
    std::cout << "    log " << line << "\n";
  }
  std::cout << (g_failures == 0 ? "sqlite_cache_repair: all checks passed\n" : "sqlite_cache_repair: FAILED\n");
  return g_failures == 0 ? 0 : 1;
}
