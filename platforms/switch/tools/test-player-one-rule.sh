#!/usr/bin/env bash
# aleks2 (rc17): host test for the rule that picks which npad is player 1 for vibration and six-axis
# (extern/aurora/lib/player_one_rule.hpp), with ASan/UBSan when the compiler has them.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-player-one-rule-test"
mkdir -p "$out"
cd "$out"
flags=(-std=c++20 -Wall -Wextra -O1 -g -I"$root/extern/aurora/lib")
if [ "${SANITIZE:-1}" = "1" ] && echo 'int main(){}' | g++ -x c++ -fsanitize=address,undefined - -o /dev/null 2>/dev/null; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
g++ "${flags[@]}" "$here/player_one_rule_test.cpp" -o player_one_rule_test
./player_one_rule_test
