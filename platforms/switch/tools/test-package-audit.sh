#!/usr/bin/env bash
# H37-c5: package_audit.py and the gate's per-package / static-only / catalog logic (package_audit_test.py).
# H37_CATALOG_DIR and SYMGEN add the real catalog packages; without them that part is printed as SKIPPED.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
py=python3; command -v "$py" >/dev/null 2>&1 || py=python
"$py" "$here/package_audit_test.py"
