#!/usr/bin/env bash
# aleks2 (V1-rc3): host test of the NRO's netdb wrapper (source/switch_netdb.cpp), linked with the same
# -Wl,--wrap options as the NRO, under ASan and UBSan: numeric answers built locally, names to the resolver,
# and a SQLite reader running while other threads open, use and close UDP/TCP sockets (netdb_test.cpp).
# usage: test-netdb.sh [WORKDIR]    needs SQLITE_SRC (sqlite3.c/.h)
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/../source/switch_netdb.cpp"
out="${1:-/tmp/netdb-test}"
: "${SQLITE_SRC:?SQLITE_SRC must point at the SQLite amalgamation}"
mkdir -p "$out"
cc -O1 -g -fsanitize=address,undefined -c "$SQLITE_SRC/sqlite3.c" -o "$out/sqlite3.o" -DSQLITE_THREADSAFE=1
c++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -pthread \
  -I"$SQLITE_SRC" "$here/netdb_test.cpp" "$src" "$out/sqlite3.o" -ldl \
  -Wl,--wrap=getaddrinfo -Wl,--wrap=freeaddrinfo -Wl,--wrap=getnameinfo -o "$out/netdb_test"
ASAN_OPTIONS=detect_leaks=1 "$out/netdb_test"
echo "test-netdb.sh: all passed"
