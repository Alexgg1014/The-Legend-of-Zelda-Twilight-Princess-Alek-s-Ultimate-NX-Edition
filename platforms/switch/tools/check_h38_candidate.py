#!/usr/bin/env python3
"""aleks2 (H38): may this build be handed over as an H38 candidate?

check_h37_candidate.py with --profile h38 (see its docstring and h38_policy.py): the four official packages, the
switch-arm64 Randomizer, and -- with --require-twilit or ALEKS2_H38_REQUIRE_TWILIT=ON -- the switch-arm64 Twilit
Essentials 2.5.0, judged against the final ELF; combined hook capacity against the loader's arena table; no TLS in
any switch-arm64 module. `ready` stays false until a final ELF (--elf/--symdb/--nro) is given.

  check_h38_candidate.py --elf dusklight.elf --symdb dusklight.symdb.raw --nro dusklight.nro \\
      --packages DIR --symgen SYMGEN --source-root REPO --tag H38-c1 --report h38-candidate.json [--require-twilit]
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_h37_candidate  # noqa: E402

if __name__ == "__main__":
    sys.exit(check_h37_candidate.main(["--profile", "h38", *sys.argv[1:]]))
