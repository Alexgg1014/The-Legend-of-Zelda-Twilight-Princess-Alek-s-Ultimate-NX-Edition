#!/usr/bin/env bash
# H37-c4: the candidate gate's view of a package (host_services_test.py says what is checked): which
# library the loader takes from a multi-platform package, and the game's services against what the
# Switch build offers. With H37_PACKAGES_DIR and SYMGEN, Randomizer's real service imports too.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
exec "$py" "$here/host_services_test.py" "$@"
