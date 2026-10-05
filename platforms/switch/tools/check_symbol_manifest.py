#!/usr/bin/env python3
"""aleks2 (H37): check a linked game image against what its native mods require.

Run after the link and before elf2nro, on the ELF the NRO is made from:

  check_symbol_manifest.py --elf dusklight.elf --symdb dusklight.symdb.raw \\
      --required platforms/switch/mod_required_symbols.txt --report dusklight.symbols.json [--strict]

--symdb is symgen's manifest of that ELF written uncompressed (symgen manifest --no-compress);
the NRO carries the compressed one, made from the same ELF in the same build step.

It checks, and writes to --report:

  identity  the manifest's build id is the ELF's GNU build id. A manifest made from another
            link is stale: every address in it would be wrong. Always an error.
  imports   each `import` line of --required resolves the way the loader resolves it on Switch
            (image_surface.py): a defined name in the ELF's .dynsym -- the exported surface, the
            allowlist -- or, failing that, an entry of the image runtime table the ELF carries
            (aleks2_image_runtime: libgcc/libgcc_eh/libm functions hidden in the image), each
            entry checked to be the address of the image's own function of that name.
  hooks     each `hook` line resolves in the manifest to exactly one address (a name with
            symgen's DupName flag maps to several: overloads or per-file statics), and that
            address is a hook target funchook can patch and something reaches: the start of a
            function of 8 bytes or more, nothing branching into its first two instructions,
            called, branched to or pointed at (image_surface.hook_targets).

Per mod, the report says compatible or not and names every missing or ambiguous symbol. With
--strict any of them fails the build (exit 1); without, the build goes on and the report, which
the NRO also carries, marks those mods incompatible with this build.
"""
import argparse
import json
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import elf_audit  # noqa: E402
import image_surface  # noqa: E402

SYMDB_MAGIC = b"SYMGEN\0\0"
SYMDB_VERSION = 2
FLAG_DUP_NAME = 1 << 4
SHT_NOTE = 7
NT_GNU_BUILD_ID = 3


class CheckError(Exception):
    pass


def elf_build_id(elf: elf_audit.Elf) -> bytes:
    for s in elf.sections:
        if s["type"] != SHT_NOTE:
            continue
        at, end = s["offset"], s["offset"] + s["size"]
        while at + 12 <= end:
            namesz, descsz, ntype = struct.unpack_from("<III", elf.data, at)
            name = elf.data[at + 12:at + 12 + namesz]
            desc = at + 12 + ((namesz + 3) & ~3)
            if ntype == NT_GNU_BUILD_ID and name == b"GNU\0":
                return elf.data[desc:desc + descsz]
            at = desc + ((descsz + 3) & ~3)
    raise CheckError(f"{elf.name}: no GNU build id (link with --build-id)")


def read_symdb(data: bytes) -> tuple:
    """(build id, {name: (flags, rva)}) of an uncompressed symgen v2 manifest."""
    if len(data) < 72:
        raise CheckError("symbol manifest is truncated")
    magic, version, compression, ulen, clen, idlen = struct.unpack_from("<8sIIQQI", data, 0)
    if magic != SYMDB_MAGIC or version != SYMDB_VERSION:
        raise CheckError("not a symgen v2 manifest")
    if compression != 0:
        raise CheckError("symbol manifest is compressed; make it with symgen manifest --no-compress")
    if idlen > 32 or ulen != clen or 72 + ulen > len(data):
        raise CheckError("symbol manifest is malformed")
    build_id = data[36:36 + idlen]
    (count,) = struct.unpack_from("<I", data, 68)
    payload = data[72:72 + ulen]
    if count * 24 > len(payload):
        raise CheckError("symbol manifest is malformed")
    strings = payload[count * 24:]
    names = {}
    for i in range(count):
        _hash, rva, name_off, flags = struct.unpack_from("<QQII", payload, i * 24)
        end = strings.find(b"\0", name_off)
        if end < 0:
            raise CheckError("symbol manifest has a name outside its string table")
        names[strings[name_off:end].decode("utf-8", "replace")] = (flags, rva)
    return build_id, names


def read_required(path: pathlib.Path) -> list:
    lines = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split(" ", 3)
        if len(parts) != 4 or parts[1] not in ("import", "hook"):
            raise CheckError(f"{path}:{number}: expected '<mod id> import|hook <group> <name>'")
        lines.append(parts)
    return lines


def check(elf_path: pathlib.Path, symdb_path: pathlib.Path, required_path: pathlib.Path,
          require_image_runtime: bool = False) -> dict:
    elf = elf_audit.Elf(elf_path.read_bytes(), elf_path.name)
    elf_id = elf_build_id(elf)
    symdb_id, manifest = read_symdb(symdb_path.read_bytes())
    report = {
        "schema": "dusklight-aleks-symbol-check", "version": 1,
        "elf": elf_path.name, "elf_build_id": elf_id.hex(), "manifest_build_id": symdb_id.hex(),
        "stale": symdb_id != elf_id,
    }
    if report["stale"]:
        raise CheckError(f"stale symbol manifest: made for build {symdb_id.hex()}, "
                         f"the ELF is {elf_id.hex()}")
    exports = image_surface.exports(elf)
    symtab = image_surface.symbols(elf)
    relocs = image_surface.relative_relocations(elf)
    runtime, problems = image_surface.image_runtime(elf, symtab, relocs)
    if runtime is None:
        if require_image_runtime:
            problems.append(f"no {image_surface.IMAGE_RUNTIME_SYMBOL} table in the image "
                            "(src/dusk/mods/loader/elf_image_runtime.c not linked)")
        runtime = []
    runtime_names = {e["name"] for e in runtime}
    for name in sorted(runtime_names & exports.keys()):
        problems.append(f"image runtime {name}: also exported, so the table entry would never be used")
    if problems:
        raise CheckError("; ".join(problems))
    report["exported_symbols"] = len(exports)
    report["manifest_symbols"] = len(manifest)
    report["image_runtime"] = [{"name": e["name"], "address": f"{e['address']:#x}", "library": e["library"]}
                               for e in runtime]
    return check_requirements(elf, symtab, relocs, exports, runtime_names, manifest,
                              read_required(required_path), report)


def check_requirements(elf, symtab, relocs, exports, runtime_names, manifest, required, report) -> dict:
    """Per mod: every import and every by-name hook resolved as the loader will resolve it."""
    mods = {}
    hook_addresses = {}
    for mod_id, kind, group, name in required:
        m = mods.setdefault(mod_id, {"imports": {"required": 0, "found": 0, "missing": [], "image_runtime": []},
                                     "hooks": {"required": 0, "found": 0, "missing": [], "ambiguous": [],
                                               "not_interceptable": []}})
        if kind == "import":
            m["imports"]["required"] += 1
            if name in exports:
                m["imports"]["found"] += 1
            elif name in runtime_names:
                m["imports"]["found"] += 1
                m["imports"]["image_runtime"].append(name)
            else:
                m["imports"]["missing"].append(f"{name} ({group})")
        else:
            m["hooks"]["required"] += 1
            entry = manifest.get(name)
            if entry is None:
                m["hooks"]["missing"].append(name)
            elif entry[0] & FLAG_DUP_NAME:
                m["hooks"]["ambiguous"].append(name)
            else:
                hook_addresses[name] = entry[1]
    targets = (image_surface.hook_targets(elf, symtab, relocs, hook_addresses, set(exports.values()))
               if hook_addresses else {})
    report["hook_targets"] = {name: {**v, "address": f"{v['address']:#x}"} for name, v in sorted(targets.items())}
    for mod_id, kind, _group, name in required:
        if kind == "hook" and name in targets:
            m = mods[mod_id]["hooks"]
            if targets[name]["problems"]:
                m["not_interceptable"].append(f"{name}: {', '.join(targets[name]['problems'])}")
            else:
                m["found"] += 1
    for m in mods.values():
        m["compatible"] = not (m["imports"]["missing"] or m["hooks"]["missing"] or m["hooks"]["ambiguous"]
                               or m["hooks"]["not_interceptable"])
    report["mods"] = mods
    return report


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", required=True, type=pathlib.Path)
    ap.add_argument("--symdb", required=True, type=pathlib.Path)
    ap.add_argument("--required", required=True, type=pathlib.Path)
    ap.add_argument("--report", required=True, type=pathlib.Path)
    ap.add_argument("--strict", action="store_true", help="fail when a mod is incompatible")
    ap.add_argument("--require-image-runtime", action="store_true",
                    help="the image must carry the image runtime table (builds with the native mod loader)")
    args = ap.parse_args(argv)
    try:
        report = check(args.elf, args.symdb, args.required, args.require_image_runtime)
    except (CheckError, elf_audit.ElfError, image_surface.SurfaceError, OSError, struct.error) as error:
        print(f"check_symbol_manifest: {error}", file=sys.stderr)
        return 2
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=1, sort_keys=True) + "\n")
    print(f"symbol manifest: build {report['elf_build_id']}, {report['manifest_symbols']} names, "
          f"{report['exported_symbols']} exported, {len(report['image_runtime'])} in the image runtime table")
    incompatible = 0
    for mod_id, m in sorted(report["mods"].items()):
        i, h = m["imports"], m["hooks"]
        state = "compatible" if m["compatible"] else "INCOMPATIBLE"
        print(f"  {mod_id}: {state}: imports {i['found']}/{i['required']}, "
              f"by-name hooks {h['found']}/{h['required']}")
        for what, names in (("missing import", i["missing"]), ("missing hook target", h["missing"]),
                            ("ambiguous hook target", h["ambiguous"]),
                            ("hook target not interceptable", h["not_interceptable"])):
            for name in names:
                print(f"    {what}: {name}")
        incompatible += not m["compatible"]
    if incompatible and args.strict:
        print(f"check_symbol_manifest: {incompatible} mod(s) incompatible with this build", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
