#!/usr/bin/env python3
"""aleks2 (H37): what a set of native mods requires of the game image, from the mods themselves.

For each mod library (a loose .so or the lib/<platform>/mod.so inside a .dusk) it lists:

  import  every symbol its relocations bind to that the mod does not define, is not weak, and
          that the loader does not supply itself (its shims) -- what the NRO must export, in
          its .dynsym, for the loader's import resolution (src/dusk/mods/loader/
          elf_runtime_policy.cpp) to find it;
  hook    every hook target it declares by name (DEFINE_HOOK_SYMBOL: a mangled name, a
          qualified name or a file-local "path#name"), read from the mod's own metadata
          records with symgen modmeta -- what the post-link symbol manifest must resolve,
          unambiguously.

The output is committed (platforms/switch/mod_required_symbols.txt). The Switch build adds the
imports to the linker's --dynamic-list, by name, and check_symbol_manifest.py verifies after
the link that every line is satisfied by that exact build. A mod that imports a name the loader
refuses outright (elf_audit.DENIED) is an error here: no build can satisfy it.

  gen_required_symbols.py --symgen SYMGEN --out FILE [--merge] ID=PATH [ID=PATH ...]

--merge (H37-c5) keeps the lines FILE already has for mods not named on the command line, so a mod
whose package is not at hand (the catalog mods) is added or refreshed without the others.
"""
import argparse
import json
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import elf_audit  # noqa: E402

GROUPS = {"game-or-sdk": "game", "libc": "libc", "libstdc++": "libstdc++", "libgcc": "libgcc",
          "glibc-only": "glibc-only"}


def requirements(mod_id: str, path: pathlib.Path, symgen: str) -> tuple:
    # The library the loader itself would take (switch-arm64, else linux-aarch64): a catalog package
    # ships a build for every platform, and only that one is what the Switch build must satisfy.
    label, data = elf_audit.select_library(path)
    audit = elf_audit.audit_elf(elf_audit.Elf(data, label))
    weak = {w.split("@")[0] for w in audit.get("weak_imports", [])}
    lines, denied = [], []
    for group, names in audit["imports"].items():
        for versioned in names:
            name = versioned.split("@")[0]
            if name in weak or name in elf_audit.LOADER_SHIMS:
                continue
            if elf_audit.matches(name, elf_audit.DENIED):
                denied.append(name)
                continue
            lines.append((mod_id, "import", GROUPS[group], name))
    with tempfile.TemporaryDirectory() as tmp:
        lib = pathlib.Path(tmp) / "mod.so"
        lib.write_bytes(data)
        meta = json.loads(subprocess.run([symgen, "modmeta", str(lib)], check=True,
                                         capture_output=True, text=True).stdout)
    for hook in meta["files"][0]["hooks"]:
        if hook["kind"] == "name":
            lines.append((mod_id, "hook", "name", hook["name"]))
    return lines, denied


def service_imports(path: pathlib.Path, symgen: str) -> list:
    """The services the mod's library (the one the loader takes) imports, as the mod lists them:
    [{service_id, major, min_minor, optional}], read with symgen modmeta -- nothing is run."""
    label, data = elf_audit.select_library(path)
    with tempfile.TemporaryDirectory() as tmp:
        lib = pathlib.Path(tmp) / "mod.so"
        lib.write_bytes(data)
        meta = json.loads(subprocess.run([symgen, "modmeta", str(lib)], check=True,
                                         capture_output=True, text=True).stdout)
    return meta["files"][0]["imports"]


def member_hooks(path: pathlib.Path, symgen: str) -> list:
    """H38-c6: the member-function hook records of the library the loader takes, as symgen modmeta lists them:
    [{kind: "member", vtable, display, symbol, virtual_slot}] (virtual_slot is None for a non-virtual member)."""
    _label, data = elf_audit.select_library(path)
    with tempfile.TemporaryDirectory() as tmp:
        lib = pathlib.Path(tmp) / "mod.so"
        lib.write_bytes(data)
        meta = json.loads(subprocess.run([symgen, "modmeta", str(lib)], check=True,
                                         capture_output=True, text=True).stdout)
    return [h for h in meta["files"][0]["hooks"] if h["kind"] == "member"]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--symgen", required=True, help="symgen executable (cmake/SymbolManifest.cmake pins it)")
    ap.add_argument("--out", required=True, type=pathlib.Path)
    ap.add_argument("--merge", action="store_true", help="keep FILE's lines of mods not named here")
    ap.add_argument("mods", nargs="+", help="MOD_ID=PATH to a .so or .dusk")
    args = ap.parse_args(argv)
    lines, problems = [], []
    for spec in args.mods:
        mod_id, _, path = spec.partition("=")
        if not path:
            ap.error(f"{spec}: expected MOD_ID=PATH")
        got, denied = requirements(mod_id, pathlib.Path(path), args.symgen)
        lines += got
        problems += [f"{mod_id} imports {name}, which the loader refuses" for name in denied]
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1
    if args.merge and args.out.is_file():
        named = {spec.partition("=")[0] for spec in args.mods}
        for old in args.out.read_text().splitlines():
            fields = old.split(" ")
            if old and not old.startswith("#") and len(fields) == 4 and fields[0] not in named:
                lines.append(tuple(fields))
    lines = sorted(set(lines))
    mods = sorted({line[0] for line in lines})
    header = [
        "# Generated by platforms/switch/tools/gen_required_symbols.py. Do not edit by hand.",
        "#",
        "# What the native mods below require of the game image: `import` lines are names the",
        "# NRO must export (added to the --dynamic-list by ModAbiExports.cmake), `hook` lines",
        "# are by-name hook targets the post-link symbol manifest must resolve unambiguously.",
        "# platforms/switch/tools/check_symbol_manifest.py checks both after every link.",
        "#",
        "# <mod id> <kind> <group> <name>",
        f"# mods: {' '.join(mods)}",
    ]
    args.out.write_text("\n".join(header + [" ".join(line) for line in lines]) + "\n")
    counts = {m: (sum(1 for l in lines if l[0] == m and l[1] == "import"),
                  sum(1 for l in lines if l[0] == m and l[1] == "hook")) for m in mods}
    for m, (i, h) in counts.items():
        print(f"{m}: {i} import(s), {h} by-name hook(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
