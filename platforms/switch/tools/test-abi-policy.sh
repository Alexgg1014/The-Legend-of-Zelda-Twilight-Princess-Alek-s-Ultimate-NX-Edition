#!/usr/bin/env bash
# H37-c6: library selection (switch-arm64 over linux-aarch64), the libstdc++ ABI refusal of Linux-built modules in
# elf_audit.py, and check_switch_module.py, on real AArch64 shared objects made with clang + ld.lld.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
py=python3; command -v "$py" >/dev/null 2>&1 || py=python
"$py" "$here/abi_policy_test.py"
