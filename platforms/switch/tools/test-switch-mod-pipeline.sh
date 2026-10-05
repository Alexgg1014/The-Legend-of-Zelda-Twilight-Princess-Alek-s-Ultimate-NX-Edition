#!/usr/bin/env bash
# H38: build_switch_mods.py and the H38 gate with fixtures and fake cmake/symgen (switch_pipeline_test.py): the pinned
# manifest, deterministic canonical packages, patch order, refusals, the separate exports diff, Twilit required only
# with ALEKS2_H38_REQUIRE_TWILIT, Linux TLS refused, the combined hook capacity. Needs python3 and git; nothing else.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
py=python3; command -v "$py" >/dev/null 2>&1 || py=python
"$py" "$here/switch_pipeline_test.py"
