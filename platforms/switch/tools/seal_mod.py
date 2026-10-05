#!/usr/bin/env python3
"""aleks2: make one mod's merged object safe to link beside other mods.

Horizon has no dlopen, so a code mod is compiled into the NRO instead of loaded
from its bundle (platforms/switch/cmake/StaticMods.cmake explains the whole
shape of it). Everything that was private to a shared object then has to be made
private by hand, because a static link has no version script to hide behind:

  * The five exports the loader looks up are renamed with the mod's name, so two
    mods can both define mod_initialize.

  * The modmeta section is renamed too. A mod's descriptor points at its
    metadata records through __start_modmeta/__stop_modmeta, which the linker
    synthesises to bracket the whole output's modmeta section. One library per
    mod means one section per mod; merged into one executable they would join up
    and every mod would claim every other mod's imports and hooks. The new name
    stays a valid C identifier so the linker still synthesises the pair.

  * Every other strong global the mod defines is made local. IMPORT_SERVICE
    declares `const LogService* svc_log` -- a pointer to const, not a const
    pointer, so it has external linkage and two mods importing the same service
    collide on it.

Weak symbols are left exactly as they are. Template instantiations are weak and
live in COMDAT groups, and localising one of those makes the linker discard its
group and then fail on the references into it, which is what objcopy
--keep-global-symbol does if you point it at a merged mod.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys

# What the loader looks up (see src/dusk/mods/loader/natives.cpp).
EXPORTS = ("mod_meta", "mod_ctx", "mod_initialize", "mod_update", "mod_shutdown")

# nm type letters for a defined strong global. Weak (W, V) is deliberately
# absent, and lowercase (already local) never reaches us because of -g.
STRONG_GLOBAL = set("TDBRGSAN")


def strong_globals(nm: str, obj: pathlib.Path) -> list[str]:
    out = subprocess.run([nm, "--defined-only", "-g", str(obj)],
                         capture_output=True, text=True, check=True).stdout
    names = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        kind, name = parts[-2], parts[-1]
        if len(kind) == 1 and kind in STRONG_GLOBAL:
            names.append(name)
    return names


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--objcopy", required=True)
    ap.add_argument("--nm", required=True)
    ap.add_argument("--name", required=True, help="the mod's target name, used as the prefix")
    ap.add_argument("input", type=pathlib.Path)
    ap.add_argument("output", type=pathlib.Path)
    args = ap.parse_args()

    if not args.input.is_file():
        print(f"seal_mod: no such object: {args.input}", file=sys.stderr)
        return 1

    defined = strong_globals(args.nm, args.input)
    missing = [name for name in EXPORTS if name not in defined]
    if missing:
        # Better to say so here than to have the loader report a missing export
        # from a build that could never have worked.
        print(f"seal_mod: {args.name} does not define {', '.join(missing)}; "
              "it is not a mod (DEFINE_MOD plus the three entry points)", file=sys.stderr)
        return 1

    options = [
        "--rename-section", f"modmeta=modmeta_{args.name}",
        "--redefine-sym", f"__start_modmeta=__start_modmeta_{args.name}",
        "--redefine-sym", f"__stop_modmeta=__stop_modmeta_{args.name}",
    ]
    for name in EXPORTS:
        options += ["--redefine-sym", f"{name}={args.name}__{name}"]
    localized = 0
    for name in defined:
        if name in EXPORTS:
            continue
        options += ["--localize-symbol", name]
        localized += 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    staged = args.output.with_suffix(".staged.o")
    shutil.copyfile(args.input, staged)
    result = subprocess.run([args.objcopy, *options, str(staged), str(args.output)],
                            capture_output=True, text=True)
    staged.unlink(missing_ok=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        return result.returncode

    print(f"{args.name}: 5 exports renamed, {localized} other global(s) made local, "
          f"metadata section modmeta_{args.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
