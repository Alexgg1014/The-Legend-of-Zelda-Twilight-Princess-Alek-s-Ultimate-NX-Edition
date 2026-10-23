#!/usr/bin/env python3
"""aleks2 (H37-c5): what a package is, as the loader will see it, before any ELF is involved.

For one .dusk (or a loose .so) it reports, from the package alone:

  * which library the loader takes (lib/switch-arm64, else lib/linux-aarch64 in compatibility mode --
    never android, ios, macos or windows), its size and its relocations;
  * what the loader would refuse in it structurally (PT_TLS, relocation types it does not apply,
    DT_NEEDED, refused imports...), from elf_audit.verdict;
  * how many hooks it declares and so how many CodeMemory pages and arena objects it needs: funchook
    uses one trampoline page per hook target (H37 milestone notes, 6.3) and an arena is
    CODE_ARENA_PAGES (64) pages in one kernel object. A mod declares its targets by name, by
    function and by member pointer; the count here is the number of records, an upper bound (a
    target two records share uses one page, and a declared hook the mod never installs costs none);
  * the services it imports, required and optional.

"Fits" is a static estimate of capacity, not a promise: what a mod installs at run time is decided
by the mod. Combinations are judged on the sum of their records (an upper bound again); a target two
packages share is composed by the hook service or refused as a conflict when both are enabled, which
only the loader can decide.

  package_audit.py --symgen SYMGEN PACKAGE...
"""
import argparse
import json
import pathlib
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import elf_audit  # noqa: E402

CODE_ARENA_PAGES = 64          # platforms/switch/source/code_arena.h
# DM2-a: hooks per arena object. funchook's 12-byte patch lets several trampolines share a page: 16 slots of
# 256 bytes a page (code_arena.h CODE_ARENA_SLOTS), so one CodeMemory object holds 1024 hooks, not 64.
HOOKS_PER_ARENA = 1024
PLANNED_ARENA_OBJECTS = 2      # what H37 plans for (docs 6.3): 128 hooks across at most two kernel objects
CODE_ARENA_MAX = 8             # the arena table's size; the kernel's pool of CodeMemory objects is smaller (10 observed)


def manifest_of(path: pathlib.Path) -> dict:
    """{id, name, version} from the package's mod.json ({} for a loose .so)."""
    if not zipfile.is_zipfile(path):
        return {}
    with zipfile.ZipFile(path) as z:
        meta = json.loads(z.read("mod.json"))
    return {"id": meta.get("id"), "name": meta.get("name"), "version": meta.get("version")}


def describe(path: pathlib.Path, symgen: str) -> dict:
    label, data = elf_audit.select_library(path)
    audit = elf_audit.audit_elf(elf_audit.Elf(data, label))
    names = sorted({n.split("@")[0] for g in audit["imports"].values() for n in g})
    streams = [n for n in names if elf_audit.unsafe_libstdcxx_abi(n)]
    with tempfile.TemporaryDirectory() as tmp:
        lib = pathlib.Path(tmp) / "mod.so"
        lib.write_bytes(data)
        meta = json.loads(subprocess.run([symgen, "modmeta", str(lib)], check=True, capture_output=True,
                                         text=True).stdout)["files"][0]
    kinds = {"name": 0, "fn": 0, "member": 0}
    names = set()
    for hook in meta["hooks"]:
        kinds[hook["kind"]] = kinds.get(hook["kind"], 0) + 1
        if hook["kind"] == "name":
            names.add(hook["name"])
    # A hook by name declared twice is one target; fn and member records are one each (their targets
    # are only known at run time).
    pages = len(names) + kinds.get("fn", 0) + kinds.get("member", 0)
    platform = label.split("lib/", 1)[-1].split("/", 1)[0]
    services = meta["imports"]
    prefix = "dev.twilitrealm.dusklight."
    mode = "native (switch-arm64)" if platform == "switch-arm64" else f"compatibility ({platform})"
    return {
        **manifest_of(path),
        "file": path.name, "package_bytes": path.stat().st_size,
        "library": label.split(":", 1)[-1], "platform": platform, "mode": mode, "library_bytes": len(data),
        "relocations": audit["relocations"], "init_array": audit["init"]["init_array"],
        # H37-c6: imports of the stream/locale families whose layout depends on the libstdc++ headers. Refused in a
        # linux-aarch64 library (loader_blockers); in a switch-arm64 one they are fine to bind but nothing here has
        # executed them, so the report says so.
        "iostream_imports": len(streams),
        "unvalidated": (["uses iostreams (std::ios_base/basic_ios/fstream/stringstream): not validated "
                         "by execution on the console"] if streams and platform == "switch-arm64" else []),
        "loader_blockers": (audit["verdict"]["switch_package"]["blockers"] if platform == "switch-arm64"
                            else audit["verdict"]["linux_compat"]["blockers"]),
        "hooks_declared": {"by_name": kinds["name"], "by_name_targets": len(names), "by_function": kinds["fn"],
                           "by_member": kinds["member"]},
        "estimated_pages": pages,
        "estimated_arena_objects": -(-pages // HOOKS_PER_ARENA),
        "services": {"required": sorted(f"{s['service_id']}@{s['major']}.{s['min_minor']}" for s in services
                                        if not s["optional"]),
                     "optional": sorted(f"{s['service_id']}@{s['major']}.{s['min_minor']}" for s in services
                                        if s["optional"]),
                     "dusklight_prefix": prefix},
    }


def blocks(tier: str, require_catalog: bool = False, present: bool = True) -> bool:
    """Does a rejection (or, with present=False, an absence) of a package of this tier block ready?

      required   always: missing, unreadable or incompatible.
      optional   when provided (absence never blocks); a provided one must pass.
      catalog    only with --require-catalog, which demands all three present AND compatible.
    """
    if tier == "required":
        return True
    if tier == "optional":
        return present
    if tier == "catalog":
        return bool(require_catalog)
    raise ValueError(f"unknown tier {tier!r}")


def capacity(pages: int) -> dict:
    objects = -(-pages // HOOKS_PER_ARENA)
    return {"pages": pages, "arena_objects": objects,
            "within_planned_budget": objects <= PLANNED_ARENA_OBJECTS,
            "within_arena_table": objects <= CODE_ARENA_MAX}


def capacity_notes(entries: dict) -> list:
    """Human sentences for what does not fit, one package at a time and for the known combinations."""
    notes = []
    for mod_id, e in sorted(entries.items()):
        cap = capacity(e["estimated_pages"])
        if not cap["within_arena_table"]:
            notes.append(f"{mod_id}: up to {cap['pages']} hook pages = {cap['arena_objects']} arena objects, "
                         f"more than the arena table holds ({CODE_ARENA_MAX}): cannot be hooked")
        elif not cap["within_planned_budget"]:
            notes.append(f"{mod_id}: up to {cap['pages']} hook pages = {cap['arena_objects']} CodeMemory objects; "
                         f"H37 plans for {PLANNED_ARENA_OBJECTS} ({PLANNED_ARENA_OBJECTS * HOOKS_PER_ARENA} hooks): "
                         "it may exhaust the kernel's CodeMemory pool next to other mods -- test it alone")
    return notes


def combinations(entries: dict, shared: dict = None) -> list:
    """Every pair, and the largest set that fits the planned budget, with the page sum of the records
    (upper bound). `shared`: {(a, b): [targets both declare by name]} for the packages whose by-name
    targets are known."""
    ids = sorted(entries)
    out = []
    for i, a in enumerate(ids):
        for b in ids[i + 1:]:
            pages = entries[a]["estimated_pages"] + entries[b]["estimated_pages"]
            common = (shared or {}).get((a, b), [])
            cap = capacity(pages)
            out.append({"mods": [a, b], "pages": pages, "arena_objects": cap["arena_objects"],
                        "within_planned_budget": cap["within_planned_budget"],
                        "shared_by_name_targets": common})
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--symgen", required=True)
    ap.add_argument("--json", type=pathlib.Path)
    ap.add_argument("packages", nargs="+", type=pathlib.Path)
    args = ap.parse_args(argv)
    result = {}
    for path in args.packages:
        entry = describe(path, args.symgen)
        result[entry.get("id") or path.name] = entry
        print(f"{entry.get('id') or path.name} {entry.get('version', '')}: {entry['mode']}, "
              f"{entry['library_bytes']} bytes, hooks by name/function/member "
              f"{entry['hooks_declared']['by_name_targets']}/{entry['hooks_declared']['by_function']}/"
              f"{entry['hooks_declared']['by_member']} -> up to {entry['estimated_pages']} pages "
              f"({entry['estimated_arena_objects']} objects)")
    for note in capacity_notes(result):
        print(f"  note: {note}")
    if args.json:
        args.json.write_text(json.dumps(result, indent=1, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
