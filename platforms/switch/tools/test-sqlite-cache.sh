#!/usr/bin/env bash
# aleks2 (SHADERS-c1): the cache repair test (ASan/UBSan) and the journal-mode durability
# measurement, against the SQLite amalgamation Aurora builds. Linux only (fork, unix-none).
#   SQLITE_SRC   directory with sqlite3.c/sqlite3.h (the build's _deps/sqlite3-src)
#   CONSOLE_DB   optional: a damaged cache taken off a console, for case 14
#   TRIALS       durability trials per cell (default 200)
#   MUST_PASS    journal/sync pair that must never be damaged (default wal/normal)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
: "${SQLITE_SRC:?set SQLITE_SRC to the directory with sqlite3.c}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

# Same compile-time options as the console build: the amalgamation's defaults.
gcc -O2 -c "$SQLITE_SRC/sqlite3.c" -o sqlite3.o -DSQLITE_THREADSAFE=1
gcc -O1 -g -fsanitize=address,undefined -c "$SQLITE_SRC/sqlite3.c" -o sqlite3-asan.o -DSQLITE_THREADSAFE=1

g++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra \
    -I"$SQLITE_SRC" -I"$root/extern/aurora/lib" \
    "$root/extern/aurora/lib/sqlite_cache_open.cpp" "$here/sqlite_cache_repair_test.cpp" sqlite3-asan.o \
    -o sqlite_cache_repair_test -lpthread -ldl
./sqlite_cache_repair_test "$work/repair" ${CONSOLE_DB:+"$CONSOLE_DB"}

# V1-c3: the on-console self-test, run on this host.
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Wall -I"$SQLITE_SRC" -I"$root/extern/aurora/lib"     "$root/extern/aurora/lib/sqlite_selftest.cpp" "$here/sqlite_selftest_test.cpp" sqlite3-asan.o     -o sqlite_selftest_test -lpthread -ldl -DAURORA_SELFTEST_NO_C_API
./sqlite_selftest_test "$work/selftest"

# V1-c4: libnx semantics as measured on the console (reads past the end fail, gaps are not
# zeros, stat of an open file fails): the console's failure must reproduce without the shims
# and disappear with them, through pread/pwrite (Linux) and lseek+read/write (the console).
gcc -O1 -c "$SQLITE_SRC/sqlite3.c" -o sqlite3-posix.o
gcc -O1 -U__linux__ -c "$SQLITE_SRC/sqlite3.c" -o sqlite3-noprw.o
for obj in sqlite3-posix.o sqlite3-noprw.o; do
  g++ -std=c++20 -O1 -I"$SQLITE_SRC" -I"$root/extern/aurora/lib" -DAURORA_SELFTEST_NO_C_API       "$here/sqlite_libnx_emulation_test.cpp" "$root/extern/aurora/lib/sqlite_cache_open.cpp"       "$root/extern/aurora/lib/sqlite_selftest.cpp" "$obj" -o emu -lpthread -ldl
  mkdir -p "$work/emu-$obj"
  echo "== libnx emulation with $obj"
  ./emu "$work/emu-$obj" | tail -3
done

gcc -O2 -Wall -I"$SQLITE_SRC" "$here/sqlite_cache_durability_test.c" sqlite3.o -o durability -lpthread -ldl -lm
mkdir -p "$work/dur"
./durability "$work/dur" "${TRIALS:-200}" "${MUST_PASS:-wal/normal}"
