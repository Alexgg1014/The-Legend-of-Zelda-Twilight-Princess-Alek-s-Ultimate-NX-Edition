#!/usr/bin/env bash
# H37-c8: the console has no en_US.iso88591 locale. The old Text::Capitalize tried to
# construct it while generating hints; the throw escaped the dynamically loaded module
# and terminated the process. Apply the real patch stack, verify that path is gone, and
# exercise the replacement's ASCII and Latin-1 mappings.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-randomizer-locale}"
rm -rf "$out"
mkdir -p "$out/tree"
rando="${RANDOMIZER_SRC:-$root/mods/randomizer}"
[ -e "$rando/.git" ] || [ -d "$rando/generator" ] || { echo "SKIPPED: randomizer sources not found"; exit 2; }
(cd "$rando" && git archive --format=tar HEAD) | tar -x -C "$out/tree"
# Keep `git apply` anchored here even when OUT lives under another checkout.
git -C "$out/tree" init -q
for patch in "$root"/platforms/switch/patches/randomizer/*.patch; do
  git -C "$out/tree" apply "$patch" || { echo "patch $(basename "$patch") does not apply" >&2; exit 1; }
done

body="$(sed -n '/void Text::Capitalize()/,/^[[:space:]]*}/p' "$out/tree/generator/utility/text.cpp")"
if grep -Eq 'std::locale|std::toupper|localeName' <<<"$body"; then
  echo "FAIL: Text::Capitalize still depends on an installed locale or a throwing locale facet" >&2
  exit 1
fi
grep -q 'utility::latin1::uppercase' <<<"$body" || {
  echo "FAIL: Text::Capitalize does not use the deterministic Latin-1 helper" >&2; exit 1;
}

cat > "$out/test.cpp" <<'EOF'
#include "generator/utility/latin1.hpp"
#include <cstdlib>
using randomizer::utility::latin1::uppercase;
int main() {
    if (uppercase('a') != 'A' || uppercase('z') != 'Z' || uppercase('A') != 'A') return 1;
    if (uppercase(0xE0) != 0xC0 || uppercase(0xF6) != 0xD6) return 2;
    if (uppercase(0xF8) != 0xD8 || uppercase(0xFE) != 0xDE) return 3;
    if (uppercase(0xD7) != 0xD7 || uppercase(0xDF) != 0xDF || uppercase(0xFF) != 0xFF) return 4;
    return 0;
}
EOF
"${CXX:-g++}" -std=c++20 -Wall -Wextra -pedantic -I"$out/tree" "$out/test.cpp" -o "$out/test"
"$out/test"
echo "randomizer hint capitalization: locale-free ASCII/Latin-1 mapping: ok"
