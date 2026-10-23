// aleks2 (V1-c3) host check for extern/aurora/lib/sqlite_selftest.cpp: on Linux every probe
// and the preferred pair must pass, and the report must name the choice.
#include "sqlite_selftest.hpp"

#include <sqlite3.h>

#include <iostream>

int main(int argc, char** argv) {
  if (argc < 2) {
    return 2;
  }
  sqlite3_initialize();
  const auto r = aurora::sqlite::run_sqlite_selftest(argv[1], "unix-none");
  std::cout << r.report;
  const bool ok = r.journal == "truncate" && r.sync == "normal" && r.report.find("FAIL") == std::string::npos;
  std::cout << (ok ? "sqlite_selftest: all checks passed\n" : "sqlite_selftest: FAILED\n");
  return ok ? 0 : 1;
}
