#!/usr/bin/env bash
# H38: the general switch-arm64 mod pipeline (build_switch_mods.py, driven by platforms/switch/mods/h38-sources.json).
#
#   build-h38-mods.sh packages OUT_DIR [mod names...]    build, canonicalize and audit (devkitA64 required)
#   build-h38-mods.sh exports  OUT_DIR [mod names...]    OUT_DIR/mod_required_symbols.{txt,diff}; changes nothing else
#
# Env: SYMGEN (hooks, services, exports), RANDOMIZER_SRC / TWILIT_SRC (a checkout with the pinned commit; else the
# submodule, else the pinned commit is fetched), FMT_SRC YAML_CPP_SRC BASE64PP_SRC BATTERY_EMBED_SRC (offline
# dependency mirrors), DUSK_WEBGPU_INCLUDE_DIR (webgpu/webgpu.h for an offline Twilit build), DEVKITPRO.
# H38_MODS limits the build to some manifest names (space-separated), like H37_TEST_MODS did.
# The H37 invocation (build-h37-test-mods.sh) is unchanged and still builds loader_lab, Cosmetics, Roll and Sprint
# and Randomizer for the H37 gate.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
phase="${1:?usage: build-h38-mods.sh packages|exports OUT_DIR [mod names...]}"
out="${2:?usage: build-h38-mods.sh packages|exports OUT_DIR [mod names...]}"
shift 2
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
mods=("$@")
if [ ${#mods[@]} -eq 0 ] && [ -n "${H38_MODS:-}" ]; then read -r -a mods <<< "$H38_MODS"; fi
args=(--phase "$phase" --out "$out")
if [ ${#mods[@]} -gt 0 ]; then args+=(--mods "${mods[@]}"); fi
exec "$py" "$here/build_switch_mods.py" "${args[@]}"
