#!/usr/bin/env bash
# H38-c4: the application version rules (cmake/DuskAppVersion.cmake) and the gate's refusal of UNKNOWN-VERSION / zero versions
# (h38_policy.py). Needs cmake and python3 only: no build, no hardware.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
(cd "$work" && cmake -DDUSK_SOURCE_DIR="$root" -P "$here/app_version_test.cmake")
python3 "$here/app_version_policy_test.py"
