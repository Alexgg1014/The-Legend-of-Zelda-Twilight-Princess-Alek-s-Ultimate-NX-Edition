#!/usr/bin/env bash
# aleks2 (1.0.1): host test for aurora::time::skip_suspended (extern/aurora/lib/time.cpp), with ASan/UBSan when the
# compiler has them.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-suspend-clock-test"
mkdir -p "$out"
cd "$out"
flags=(-std=c++20 -Wall -Wextra -O1 -g -I"$root/extern/aurora/include" -I"$root/extern/aurora/lib")
if [ "${SANITIZE:-1}" = "1" ] && echo 'int main(){}' | g++ -x c++ -fsanitize=address,undefined - -o /dev/null 2>/dev/null; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
g++ "${flags[@]}" "$here/suspend_clock_test.cpp" "$root/extern/aurora/lib/time.cpp" -o suspend_clock_test
./suspend_clock_test
