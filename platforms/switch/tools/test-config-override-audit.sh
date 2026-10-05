#!/usr/bin/env bash
# Builds and runs the audit of the ConfigVar Override layer that Run Profile Matrix depends on.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${TMPDIR:-/tmp}/dusk-config-override-audit"
mkdir -p "$out"
cd "$out"
g++ -std=c++20 -Wall -Wextra -O1 -DTARGET_PC=1 -I"$root/src" -I"$root/libs/dolphin/include" \
    "$here/config_override_audit_test.cpp" -o config_override_audit_test
./config_override_audit_test
