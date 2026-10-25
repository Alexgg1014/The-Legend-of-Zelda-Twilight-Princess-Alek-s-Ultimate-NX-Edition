#!/usr/bin/env python3
"""aleks2 (V1): is any hook target of the catalog inlined anywhere in the code the Switch build generated?

A hook patches the entry of one function; a copy of that function inlined into a caller never passes through the
entry, so the hook installs and misses those calls without an error. Upstream's GCC build turns automatic inlining off
for the whole game (-fno-inline-functions, -fno-inline-small-functions, ... in CMakeLists.txt, DUSK_ENABLE_CODE_MODS);
the Switch build keeps -O3 for the frame rate and only turns the clone passes off, so every target a mod hooks has to
be out of line on its own (DUSK_NOINLINE). hook_inline_audit.py checks that per unit for the by-name hooks of known
mods; this checks the linked image, for every target of every package given:

  * the address hooks: game functions a mod library binds by R_AARCH64_ABS64/GLOB_DAT -- the address is taken,
    which is what DEFINE_HOOK(&Class::method, ...) leaves (a call goes through the PLT, JUMP_SLOT). Dawnlight 5.0.0's
    Survivors arena was the case: dRes_control_c::setRes (its D_DLBR0 -> D_MN06B path alias) was inlined into
    dRes_control_c::setStageRes, the hook never ran, and the stage loaded nothing (rc14 console, black screen);
  * the by-name hooks (symgen modmeta), resolved through the symbol manifest as the loader resolves them.

Then the image's DWARF: every DW_TAG_inlined_subroutine whose origin is one of those functions and was inlined by the
compiler's own choice (DW_AT_inline 1, "inlined"; 3, declared inline, upstream inlines too and is left alone).

  hook_inline_elf_audit.py targets --readelf R --nm N --symgen S --elf ELF --symdb SYMDB --out TARGETS.json PKG...
  hook_inline_elf_audit.py check --readelf R --targets TARGETS.json --report REPORT.json IMAGE...

`check` reads the final ELF or single objects (a pre-build check of the units a change touches). Exit 1 when any
target has a compiler-inlined copy.
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

DIE = re.compile(r"^\s*<(\d+)><([0-9a-f]+)>: Abbrev Number: \d+ \((DW_TAG_\w+)\)")
ATTR = re.compile(r"^\s*<[0-9a-f]+>\s+(DW_AT_\w+)\s*:\s*(.*)$")
REF = re.compile(r"<0x([0-9a-f]+)>")
INLINE = re.compile(r"\)\s*(\d)")
ADDRESS_RELOCS = ("R_AARCH64_ABS64", "R_AARCH64_GLOB_DAT")


def game_functions(nm: str, elf: str) -> dict:
    """{address: [names]} of the image's defined functions."""
    by_addr = collections.defaultdict(list)
    out = subprocess.run([nm, "--defined-only", elf], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[1] in "TtWw":
            by_addr[int(p[0], 16)].append(p[2])
    return by_addr


def collect_targets(args) -> int:
    by_addr = game_functions(args.nm, args.elf)
    funcs = {n for names in by_addr.values() for n in names}
    _, manifest = check_symbol_manifest.read_symdb(pathlib.Path(args.symdb).read_bytes())
    targets = collections.defaultdict(set)
    unresolved = []
    for pkg in map(pathlib.Path, args.packages):
        try:
            label, data = elf_audit.select_library(pkg)
        except Exception as error:  # a package with no ARM64 library: nothing of it runs
            print(f"skip {pkg.name}: {error}", file=sys.stderr)
            continue
        linux = elf_audit.library_platform(label) == "linux-aarch64"
        with tempfile.TemporaryDirectory() as tmp:
            lib = pathlib.Path(tmp) / "mod.so"
            lib.write_bytes(data)
            relocs = subprocess.run([args.readelf, "-W", "-r", str(lib)], capture_output=True, text=True,
                                    check=True).stdout
            meta = json.loads(subprocess.run([args.symgen, "modmeta", str(lib)], capture_output=True, text=True,
                                             check=True).stdout)
        for line in relocs.splitlines():
            p = line.split()
            if len(p) >= 5 and p[2] in ADDRESS_RELOCS:
                name = p[4].split("@")[0]
                name = elf_audit.newlib_spelling(name) if linux else name
                if name in funcs:
                    targets[name].add(pkg.stem)
        for hook in meta["files"][0]["hooks"]:
            if hook["kind"] != "name":
                continue
            entry = check_symbol_manifest.lookup(manifest, hook["name"])
            names = by_addr.get(entry[1], []) if entry else []
            if not names:
                unresolved.append(f"{pkg.stem}: {hook['name']}")
            for name in names:
                targets[name].add(f"{pkg.stem} (by name)")
    pathlib.Path(args.out).write_text(json.dumps({k: sorted(v) for k, v in sorted(targets.items())}, indent=1))
    print(f"{len(targets)} hook targets from {len(args.packages)} packages; "
          f"{len(unresolved)} by-name hooks not in the manifest (check_symbol_manifest.py reports those)")
    # V1-PGO: a target the compiler folded into its only caller has no symbol left: the mod's hook fails to install
    # (the profile build lost Dawnlight 5's d_a_e_mm_mt.cpp#action). That is as bad as an inlined copy.
    for u in unresolved:
        print(f"UNRESOLVED {u}")
    return 1 if unresolved else 0


def inlined_copies(readelf: str, image: str, targets: dict) -> dict:
    link, ref, inline_kind, name = {}, {}, {}, {}
    inlined = []  # [offset, origin, enclosing subprogram]
    stack, current = [], None
    proc = subprocess.Popen([readelf, "--debug-dump=info", "-W", image], stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace")
    for line in proc.stdout:
        if "Abbrev Number" in line:
            m = DIE.match(line)
            if not m:
                current = None
                continue
            depth, off, tag = int(m.group(1)), int(m.group(2), 16), m.group(3)
            while stack and stack[-1][0] >= depth:
                stack.pop()
            current = (off, tag)
            if tag == "DW_TAG_inlined_subroutine":
                enclosing = next((o for d, o, t in reversed(stack) if t == "DW_TAG_subprogram"), None)
                inlined.append([off, None, enclosing])
            stack.append((depth, off, tag))
            continue
        if current is None or "DW_AT_" not in line:
            continue
        a = ATTR.match(line)
        if not a:
            continue
        key, val = a.group(1), a.group(2)
        off, tag = current
        if key == "DW_AT_linkage_name":
            link[off] = val.rsplit(": ", 1)[-1].strip()
        elif key == "DW_AT_name":
            name[off] = val.rsplit(": ", 1)[-1].strip()
        elif key in ("DW_AT_specification", "DW_AT_abstract_origin"):
            r = REF.search(val)
            if r:
                ref[off] = int(r.group(1), 16)
                if key == "DW_AT_abstract_origin" and tag == "DW_TAG_inlined_subroutine" and inlined[-1][0] == off:
                    inlined[-1][1] = ref[off]
        elif key == "DW_AT_inline":
            m = INLINE.search(val)
            inline_kind[off] = m.group(1) if m else val
    if proc.wait() != 0:
        raise SystemExit(f"{readelf} failed on {image}")

    def resolve(off):
        kind = None
        for _ in range(8):
            if off is None:
                break
            kind = kind or inline_kind.get(off)
            if off in link:
                return link[off], kind
            off = ref.get(off)
        return None, kind

    found = collections.defaultdict(lambda: {"copies": 0, "inline": None, "callers": collections.Counter()})
    for _off, origin, enclosing in inlined:
        if origin is None:
            continue
        target, kind = resolve(origin)
        if target in targets and kind == "1":
            entry = found[target]
            entry["copies"] += 1
            entry["inline"] = kind
            caller = resolve(enclosing)[0] if enclosing is not None else None
            entry["callers"][caller or name.get(enclosing, "?")] += 1
    return found


def check(args) -> int:
    targets = json.loads(pathlib.Path(args.targets).read_text())
    report, total = {}, 0
    for image in args.images:
        found = inlined_copies(args.readelf, image, targets)
        for target, entry in sorted(found.items()):
            report.setdefault(target, {"mods": targets[target], "images": {}})["images"][pathlib.Path(image).name] = {
                "copies": entry["copies"], "callers": dict(entry["callers"].most_common(8))}
            total += entry["copies"]
    pathlib.Path(args.report).write_text(json.dumps(report, indent=1))
    names = subprocess.run(["c++filt"], input="\n".join(report), capture_output=True, text=True).stdout.splitlines() \
        if report else []
    for display, (target, entry) in zip(names, report.items()):
        where = ", ".join(f"{img}: {e['copies']} in {', '.join(e['callers'])}" for img, e in entry["images"].items())
        print(f"INLINED {display} ({', '.join(entry['mods'][:3])}): {where}")
    print(f"hook targets: {len(targets)}; inlined by the compiler somewhere: {len(report)} ({total} copies) "
          f"in {len(args.images)} image(s)")
    return 1 if report else 0


def latest_catalog(args) -> int:
    """aleks2 (rc17): the packages folder of the newest online catalog snapshot under RESEARCH_DIR -- the rule
    catalog_watch.py uses: a catalog-snapshot-*/ with snapshot.json and download-index.json, newest fetched_at.
    A release gate audits against it, so a mod published today is covered without anyone editing a path."""
    found = []
    for d in pathlib.Path(args.research).glob("catalog-snapshot-*"):
        if (d / "snapshot.json").is_file() and (d / "download-index.json").is_file() and (d / "packages").is_dir():
            fetched = json.loads((d / "snapshot.json").read_text(encoding="utf-8")).get("fetched_at", "")
            found.append((fetched, d.name, d))
    if not found:
        print(f"no catalog snapshot under {args.research}", file=sys.stderr)
        return 2
    fetched, name, d = max(found)
    print(f"catalog snapshot {name} (fetched {fetched})", file=sys.stderr)
    print((d / "packages").as_posix())
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    lc = sub.add_parser("latest-catalog")
    lc.add_argument("research")
    t = sub.add_parser("targets")
    t.add_argument("--readelf", required=True)
    t.add_argument("--nm", required=True)
    t.add_argument("--symgen", required=True)
    t.add_argument("--elf", required=True)
    t.add_argument("--symdb", required=True)
    t.add_argument("--out", required=True)
    t.add_argument("packages", nargs="+")
    c = sub.add_parser("check")
    c.add_argument("--readelf", required=True)
    c.add_argument("--targets", required=True)
    c.add_argument("--report", required=True)
    c.add_argument("images", nargs="+")
    args = ap.parse_args(argv)
    if args.command == "latest-catalog":
        return latest_catalog(args)
    return collect_targets(args) if args.command == "targets" else check(args)


if __name__ == "__main__":
    sys.exit(main())
