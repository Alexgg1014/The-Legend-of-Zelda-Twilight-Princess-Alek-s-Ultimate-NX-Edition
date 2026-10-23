#!/usr/bin/env bash
# aleks2 (rc21): gate check, next to test-hook-inline-elf.sh -- every C/C++ unit of the Switch build is compiled
# with -fno-ipa-ra and -fno-inline-functions-called-once (check_no_ipa_ra.py says why; it reads build.ninja).
# Run from the gate on the build dir.
#
#   test-no-ipa-ra.sh BUILD_DIR
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
build="$1"
if command -v cygpath >/dev/null 2>&1 && [ "$("$py" -c 'import sys; print(sys.platform)')" = win32 ]; then
  # a native Windows Python: D:/... paths (an MSYS Python takes the /d/... ones as they are)
  build="$(cygpath -m "$build")"
  script="$(cygpath -m "$here/check_no_ipa_ra.py")"
else
  script="$here/check_no_ipa_ra.py"
fi
"$py" "$script" "$build"
