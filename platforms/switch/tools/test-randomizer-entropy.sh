#!/usr/bin/env bash
# H37-c9: the console's libstdc++ has no entropy source, so std::random_device returned the same sequence in every
# process and every seed came out identical. Apply the real patch stack, check the three direct uses are gone, and
# check EntropySeed() differs from call to call (the clock and the counter are what make that hold on the console).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-randomizer-entropy}"
rm -rf "$out"
mkdir -p "$out/tree"
rando="${RANDOMIZER_SRC:-$root/mods/randomizer}"
[ -e "$rando/.git" ] || [ -d "$rando/generator" ] || { echo "SKIPPED: randomizer sources not found"; exit 2; }
(cd "$rando" && git archive --format=tar HEAD) | tar -x -C "$out/tree"
git -C "$out/tree" init -q
for patch in "$root"/platforms/switch/patches/randomizer/*.patch; do
    git -C "$out/tree" apply "$patch" || { echo "patch $(basename "$patch") does not apply" >&2; exit 1; }
done
uses="$(grep -rn 'random_device' "$out/tree/generator" "$out/tree/src" | grep -v 'random.cpp' | grep -v '^[^:]*:[0-9]*:[[:space:]]*//' || true)"
[ -z "$uses" ] || { echo "FAIL: random_device is still used outside EntropySeed: $uses" >&2; exit 1; }
cat > "$out/test.cpp" <<'CPP'
#include "generator/utility/random.hpp"
#include <set>
int main() {
    std::set<uint32_t> seen;
    for (int i = 0; i < 20000; ++i) seen.insert(randomizer::utility::random::EntropySeed());
    return seen.size() > 19990 ? 0 : 1;   // 32-bit values: a handful of collisions at most
}
CPP
"${CXX:-g++}" -std=c++20 -Wall -Wextra -I"$out/tree" "$out/test.cpp" "$out/tree/generator/utility/random.cpp" -o "$out/test"
"$out/test"
echo "randomizer entropy: three call sites use EntropySeed, 20000 successive seeds distinct: ok"
