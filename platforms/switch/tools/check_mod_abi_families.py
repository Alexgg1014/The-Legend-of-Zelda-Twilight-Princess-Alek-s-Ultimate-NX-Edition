#!/usr/bin/env python3
"""1.0.1: the whole-family export lists are really exported by the linked image.

mod_abi_libstdcxx_fs.txt (gen-mod-abi-filesystem.py) names every std::filesystem function of devkitA64's libstdc++;
ModAbiExports.cmake pins and exports them, so a mod from outside the catalog finds any of them (GitHub issue #4:
Twilight HD HUD stopped on equivalent and read_symlink). This checks the ELF's .dynsym holds every one.

usage: check_mod_abi_families.py NM ELF FAMILY_FILE...    exit 0 = all exported, 1 = some are not
"""
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    nm, elf, families = sys.argv[1], sys.argv[2], sys.argv[3:]
    out = subprocess.run([nm, "-D", "--defined-only", elf], check=True, capture_output=True, text=True).stdout
    exported = {line.split()[-1] for line in out.splitlines() if line.strip()}
    bad = False
    for family in families:
        names = [l.strip() for l in open(family, encoding="utf-8") if l.strip() and not l.startswith("#")]
        missing = [n for n in names if n not in exported]
        print(f"{family}: {len(names) - len(missing)}/{len(names)} exported")
        for n in missing[:20]:
            print(f"  NOT EXPORTED {n}")
        bad = bad or bool(missing)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
