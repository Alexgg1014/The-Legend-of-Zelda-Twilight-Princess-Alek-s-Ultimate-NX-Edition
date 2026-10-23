#!/usr/bin/env bash
# aleks2 (V1): after a Switch link, no hook target of the catalog may be inlined anywhere in the image
# (hook_inline_elf_audit.py tells why). Run from the gate, on the build's own ELF and symbol manifest.
#
#   test-hook-inline-elf.sh BUILD_DIR OUT_DIR PACKAGE_DIR...
#
# PACKAGE_DIR: folders of .dusk packages -- the catalog snapshot catalog_watch.py keeps, the bundled packages.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="$1"
out="$2"
shift 2
bin="${DEVKITA64_BIN:-/opt/devkitpro/devkitA64/bin}"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
symgen="$(find "$build/_deps/symgen" -maxdepth 1 -name 'symgen*' -type f | head -1)"
[ -n "$symgen" ] || { echo "no symgen under $build/_deps/symgen"; exit 2; }
exe=""
if command -v cygpath >/dev/null 2>&1; then
  # Windows: a native Python cannot start /d/... MSYS paths (rc15 gate: FileNotFoundError on symgen).
  bin="$(cygpath -m "$bin")"
  symgen="$(cygpath -m "$(cd "$(dirname "$symgen")" && pwd)/$(basename "$symgen")")"
  exe=.exe
fi
mkdir -p "$out"
# aleks2 (rc17): the newest online catalog snapshot is always audited as well (hook_inline_elf_audit.py
# latest-catalog: catalog_watch.py's snapshots, newest fetched_at), so a release gate covers mods published since a
# snapshot was last named by hand. ALEKS2_CATALOG_RESEARCH says where the snapshots are (default: research/ next to
# the folder that holds this checkout); ALEKS2_CATALOG_SNAPSHOT=none leaves it out (tests only). Not finding one
# is an error: a gate that silently audited an old catalog is what this replaces.
dirs=("$@")
if [ "${ALEKS2_CATALOG_SNAPSHOT:-latest}" != none ]; then
  research="${ALEKS2_CATALOG_RESEARCH:-$(cd "$here/../../.." && cd ../.. && pwd)/research}"
  if command -v cygpath >/dev/null 2>&1; then research="$(cygpath -m "$research")"; fi
  latest="$("$py" -I "$here/hook_inline_elf_audit.py" latest-catalog "$research")" || {
    echo "no online catalog snapshot under $research (set ALEKS2_CATALOG_RESEARCH)"; exit 2; }
  dirs+=("$latest")
fi
packages=()
for dir in "${dirs[@]}"; do
  for p in "$dir"/*.dusk; do [ -e "$p" ] && packages+=("$p"); done
done
[ "${#packages[@]}" -gt 0 ] || { echo "no packages in: $*"; exit 2; }
status=0
"$py" -I "$here/hook_inline_elf_audit.py" targets --readelf "$bin/aarch64-none-elf-readelf$exe" --nm "$bin/aarch64-none-elf-nm$exe" \
  --symgen "$symgen" --elf "$build/dusklight.elf" --symdb "$build/dusklight.symdb.raw" --out "$out/hook-targets.json" \
  "${packages[@]}" || status=1  # V1-PGO: unresolved by-name hooks fail the audit
"$py" -I "$here/hook_inline_elf_audit.py" check --readelf "$bin/aarch64-none-elf-readelf$exe" \
  --targets "$out/hook-targets.json" --report "$out/hook-inline.json" "$build/dusklight.elf" || status=1
exit $status
