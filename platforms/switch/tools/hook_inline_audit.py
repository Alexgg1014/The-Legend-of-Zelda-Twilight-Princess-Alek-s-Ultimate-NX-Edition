#!/usr/bin/env python3
"""aleks2 (H37-c2): are the by-name hook targets of the native mods real, whole functions in the
code the Switch build generates?

A hook patches the entry of one function. If the optimiser inlined that function into a caller,
the copy in the caller never passes through the entry: the hook installs and misses those calls,
silently. If it inlined every call, nothing is left at all, and the symbol manifest has no such
name -- what H37-c1's audit found for five Randomizer targets.

Inlining happens inside one translation unit (the Switch build has no LTO), so the question is
answered per unit: each unit that defines a target, compiled with the Switch build's code
generation (test-hook-targets.sh), is linked on its own into a position-independent image with
its undefined references left at zero, and then

  * symgen names its functions exactly as the post-link manifest names the game's (run from the
    source root, so file-local statics get their "path#name" too);
  * image_surface.hook_targets checks the target as check_symbol_manifest.py checks it in the
    game: a function at least funchook's patch long, nothing branching into the patch, reached
    by a call, a function pointer, an address formed in code, or -- a global function -- by
    other units;
  * no caller anywhere has a copy the hook would miss, by two independent readings:
      - the unit's DWARF holds no DW_TAG_inlined_subroutine of the target (compile with -g2:
        -g1 drops the records of inlines that leave no code of their own, and the level of -g
        never changes the code GCC generates);
      - with --reference-dir, the same unit compiled with inlining off has no more direct calls
        to the target than this one: a call that disappeared was inlined.

  hook_inline_audit.py --symgen SYMGEN --readelf READELF --source-root DIR --required FILE \\
      --report JSON [--reference-dir DIR] UNIT.elf...

Exit 1 when a target is missing from every unit, not interceptable, or inlined anywhere.
"""
import argparse
import collections
import json
import pathlib
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_symbol_manifest  # noqa: E402
import elf_audit  # noqa: E402
import image_surface  # noqa: E402

DIE = re.compile(r"^\s*<(\d+)><([0-9a-f]+)>: Abbrev Number: \d+ \((DW_TAG_\w+)\)")
ATTR = re.compile(r"^\s*<[0-9a-f]+>\s+(DW_AT_\w+)\s*:\s*(.*)$")
REF = re.compile(r"<0x([0-9a-f]+)>")


def dwarf_dies(readelf: str, path: pathlib.Path) -> dict:
    """{offset: {tag, abstract_origin, specification, low_pc}} from readelf's dump."""
    dies = {}
    current = None
    out = subprocess.run([readelf, "--debug-dump=info", "-W", str(path)], capture_output=True, text=True,
                         check=True).stdout
    for line in out.splitlines():
        m = DIE.match(line)
        if m:
            current = {"tag": m.group(3)}
            dies[int(m.group(2), 16)] = current
            continue
        m = ATTR.match(line)
        if not m or current is None:
            continue
        attr, value = m.groups()
        if attr in ("DW_AT_abstract_origin", "DW_AT_specification"):
            ref = REF.search(value)
            if ref:
                current[attr[6:]] = int(ref.group(1), 16)
        elif attr == "DW_AT_low_pc":
            address = re.search(r"0x[0-9a-f]+", value)
            if address:
                current["low_pc"] = int(address.group(0), 16)
    return dies


def inlined_copies(dies: dict, address: int) -> int:
    """How many inlined instances the function whose code starts at `address` has."""
    origins = set()
    for offset, die in dies.items():
        if die["tag"] == "DW_TAG_subprogram" and die.get("low_pc") == address:
            stack = [offset]
            while stack:  # the concrete DIE, its abstract origin, their declarations
                o = stack.pop()
                if o in origins or o not in dies:
                    continue
                origins.add(o)
                stack += [dies[o][k] for k in ("abstract_origin", "specification") if k in dies[o]]
    if not origins:
        return -1  # no debug record of that function: cannot tell
    return sum(1 for die in dies.values()
               if die["tag"] == "DW_TAG_inlined_subroutine" and die.get("abstract_origin") in origins)


def manifest_of(path: pathlib.Path, symgen: str, root: pathlib.Path) -> dict:
    with tempfile.TemporaryDirectory() as tmp:
        raw = pathlib.Path(tmp) / "unit.symdb.raw"
        subprocess.run([symgen, "-C", str(root), "manifest", "--binary", str(path), "-o", str(raw),
                        "--no-compress"], check=True, capture_output=True)
        return check_symbol_manifest.read_symdb(raw.read_bytes())[1]


def verdicts_of(path: pathlib.Path, manifest: dict, names) -> dict:
    elf = elf_audit.Elf(path.read_bytes(), path.name)
    symtab = image_surface.symbols(elf)
    relocs = image_surface.relative_relocations(elf)
    # A global function is reached from the other units of the game, which this image lacks.
    external = {value for _n, value, _s, stype, bind, shndx in symtab
                if stype == image_surface.STT_FUNC and bind in (1, 2) and shndx}
    addresses = {n: manifest[n][1] for n in names
                 if n in manifest and not manifest[n][0] & check_symbol_manifest.FLAG_DUP_NAME}
    return image_surface.hook_targets(elf, symtab, relocs, addresses, external)


def audit_unit(path: pathlib.Path, wanted: set, symgen: str, readelf: str, root: pathlib.Path,
               reference: pathlib.Path = None) -> dict:
    manifest = manifest_of(path, symgen, root)
    present = {n: e for n, e in manifest.items() if n in wanted}
    if not present:
        return {}
    verdicts = verdicts_of(path, manifest, present)
    ref_verdicts = verdicts_of(reference, manifest_of(reference, symgen, root), present) if reference else {}
    dies = dwarf_dies(readelf, path)
    out = {}
    for name, entry in present.items():
        if entry[0] & check_symbol_manifest.FLAG_DUP_NAME:
            out[name] = {"unit": path.name, "problems": ["ambiguous within its own unit"]}
            continue
        v = verdicts[name]
        copies = inlined_copies(dies, v["address"])
        problems = list(v["problems"])
        if copies < 0:
            problems.append("no DWARF record of the function")
        elif copies:
            problems.append(f"inlined into {copies} caller(s): those calls never reach the hook")
        reference_calls = None
        if reference is not None:
            if name not in ref_verdicts:
                problems.append("not in the reference unit")
            else:
                reference_calls = ref_verdicts[name]["calls"]
                if v["calls"] < reference_calls:
                    problems.append(f"{reference_calls - v['calls']} of the {reference_calls} direct calls made "
                                    "with inlining off are gone: inlined")
        out[name] = {"unit": path.name, "function": v["function"], "size": v["size"], "calls": v["calls"],
                     "reference_calls": reference_calls, "data_pointers": v["data_pointers"],
                     "formed_in_code": v["formed_in_code"], "external": v["exported"], "inlined_copies": copies,
                     "problems": problems}
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--symgen", required=True)
    ap.add_argument("--readelf", required=True)
    ap.add_argument("--source-root", required=True, type=pathlib.Path)
    ap.add_argument("--required", required=True, type=pathlib.Path, help="mod_required_symbols.txt")
    ap.add_argument("--mods", nargs="*", help="only these mod ids (default: every mod in --required)")
    ap.add_argument("--report", required=True, type=pathlib.Path)
    ap.add_argument("--reference-dir", type=pathlib.Path,
                    help="the same units, under the same names, compiled with inlining off")
    ap.add_argument("units", nargs="+", type=pathlib.Path)
    args = ap.parse_args(argv)
    wanted = collections.defaultdict(set)
    for mod_id, kind, _group, name in check_symbol_manifest.read_required(args.required):
        if kind == "hook" and (not args.mods or mod_id in args.mods):
            wanted[name].add(mod_id)
    found = {}
    for unit in args.units:
        reference = args.reference_dir / unit.name if args.reference_dir else None
        for name, verdict in audit_unit(unit, set(wanted), args.symgen, args.readelf, args.source_root,
                                        reference).items():
            if name in found:
                verdict["problems"] = verdict.get("problems", []) + [f"also defined in {found[name]['unit']}"]
            found[name] = verdict
    bad = 0
    for name in sorted(wanted):
        v = found.get(name)
        if v is None:
            v = found[name] = {"problems": ["not defined in any audited unit"]}
        v["mods"] = sorted(wanted[name])
        state = "ok  " if not v["problems"] else "FAIL"
        bad += bool(v["problems"])
        ref = f" (inlining off: {v['reference_calls']})" if v.get("reference_calls") is not None else ""
        reach = (f"calls {v.get('calls', 0)}{ref}, pointers {v.get('data_pointers', 0)}, "
                 f"formed {v.get('formed_in_code', 0)}, external {'yes' if v.get('external') else 'no'}")
        detail = "; ".join(v["problems"]) if v["problems"] else f"{v['size']} bytes, inlined copies 0, {reach}"
        print(f"{state} {name:45} {v.get('unit', '-'):24} {detail}")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(found, indent=1, sort_keys=True) + "\n")
    print(f"hook targets: {len(wanted) - bad}/{len(wanted)} whole, reached and never inlined")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
