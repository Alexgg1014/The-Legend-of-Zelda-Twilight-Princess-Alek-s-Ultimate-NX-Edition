#!/usr/bin/env bash
# H38: the Wii-style gesture detector on synthetic accelerometer data: rest and noise, one pulse per gesture, no
# repeat inside the cooldown, two separate gestures, blocking, resets and the single-sensor case. No hardware.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-wii-gestures-test"
mkdir -p "$out"
"${CXX:-g++}" -std=c++20 -Wall -Wextra -O1 -g -I"$root/src" "$here/wii_gestures_test.cpp" -o "$out/wii_gestures_test"
"$out/wii_gestures_test"
# V1-rc9: the sword classification replayed on the rc7/rc8 console logs (wii_replay_test.cpp).
"${CXX:-g++}" -std=c++20 -Wall -Wextra -O1 -g -I"$root/src" "$here/wii_replay_test.cpp" -o "$out/wii_replay_test"
"$out/wii_replay_test" "$here/data"
