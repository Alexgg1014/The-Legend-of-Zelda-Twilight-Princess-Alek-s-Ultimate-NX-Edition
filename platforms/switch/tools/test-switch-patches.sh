#!/usr/bin/env bash
# Host test for cmake/SwitchPatches.cmake, the helper that puts the two small Horizon patches
# on Dawn and Tracy. It needs CMake and GNU patch on PATH and nothing else.
#
# What it guards against: the helper once decided a patch was "already applied" by asking
# patch to reverse it as a dry run. In batch mode patch drops -R when the patch is not
# applied and reports success, so a pristine tree was reported as done and never patched.
# The first build with that mistake had no Dawn patch and would have failed on the console.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
helper="$root/cmake/SwitchPatches.cmake"
work="$(mktemp -d "${TMPDIR:-/tmp}/dusk-patches-test.XXXXXX")"
trap 'rm -rf "$work"' EXIT

cmake_bin="${CMAKE:-cmake}"
fail() { echo "FAIL: $*" >&2; exit 1; }

mkdir -p "$work/tree/sub" "$work/orig/sub" "$work/new/sub"
printf 'one\ntwo\nthree\nfour\nfive\n' > "$work/tree/sub/file.txt"
printf 'one\ntwo\nthree\nfour\nfive\n' > "$work/orig/sub/file.txt"
printf 'one\ntwo\nPATCHED\nthree\nfour\nfive\n' > "$work/new/sub/file.txt"
( cd "$work" && diff -u --label a/sub/file.txt --label b/sub/file.txt orig/sub/file.txt new/sub/file.txt > test.patch || true )

run() {
  "$cmake_bin" -DTREE="$work/tree" -DPATCH="$work/test.patch" "$@" -P "$helper" 2>&1
}

out="$(run -DMARKER_FILE=sub/file.txt -DMARKER_TEXT=PATCHED)"
echo "$out" | grep -q "applied test.patch" || fail "first run did not apply: $out"
grep -q PATCHED "$work/tree/sub/file.txt" || fail "file was not patched"

out="$(run -DMARKER_FILE=sub/file.txt -DMARKER_TEXT=PATCHED)"
echo "$out" | grep -q "already applied" || fail "second run did not notice it was applied: $out"
[ "$(grep -c PATCHED "$work/tree/sub/file.txt")" = 1 ] || fail "second run applied the patch twice"

if run -DMARKER_FILE=sub/file.txt -DMARKER_TEXT="NOT THERE" >/dev/null 2>&1; then
  fail "a missing marker text was accepted"
fi

printf 'something\nelse\nentirely\n' > "$work/tree/sub/file.txt"
if run >/dev/null 2>&1; then
  fail "a patch that does not fit the tree was accepted"
fi

echo "switch_patches: all checks passed"
