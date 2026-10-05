#!/usr/bin/env python3
"""aleks2 (H36): pack a mod folder (mod.json plus res/) into a .dusk package.

A .dusk is a zip with mod.json at its root, which is what the loader scans for in the
mods folder on the card (sdmc:/switch/dusklight/mods). The archive is reproducible:
entries are sorted, timestamps fixed and permissions normalised, so the same folder
always gives the same bytes and the same SHA-256.

H37: the package is canonical -- the same bytes on Windows and Linux, whatever the
checkout, the Python or its zlib:
  - entries are stored, not deflated: deflate output is not specified byte for byte and
    differs between zlib builds (Linux's zlib and zlib-ng, which recent Windows Pythons ship,
    give two different archives for one folder);
  - text files (TEXT_SUFFIXES) are packed with LF line endings: a CRLF checkout packs the same
    bytes as an LF one. Every other file is packed exactly as it is;
  - entries are ordered by their name as a string, case-sensitively (sorting Windows paths
    ignores case).
.gitattributes also keeps platforms/switch/test-mods LF in every checkout.
test-dusk-reproducible.sh checks all of it.

  make_dusk.py platforms/switch/test-mods/luau_probe out/luau_probe.dusk
"""
import argparse
import hashlib
import json
import pathlib
import sys
import zipfile

FIXED_TIME = (1980, 1, 1, 0, 0, 0)


# Packed with LF line endings. Anything else is packed as its bytes.
TEXT_SUFFIXES = {".json", ".luau", ".lua", ".txt", ".md", ".rml", ".rcss", ".css", ".yaml",
                 ".yml", ".csv", ".ini", ".toml", ".xml", ".html", ".glsl", ".wgsl", ".c", ".cpp",
                 ".h", ".hpp", ".cmake"}


def canonical_bytes(path: pathlib.Path) -> bytes:
    data = path.read_bytes()
    if path.suffix.lower() in TEXT_SUFFIXES:
        data = data.replace(b"\r\n", b"\n")
    return data


def normalised_files(source: pathlib.Path) -> list:
    """The text files whose line endings packing changes (CRLF in the folder)."""
    return sorted(p.relative_to(source).as_posix() for p in source.rglob("*")
                  if p.is_file() and canonical_bytes(p) != p.read_bytes())


def pack(source: pathlib.Path, output: pathlib.Path) -> str:
    manifest = source / "mod.json"
    if not manifest.is_file():
        raise ValueError(f"{source}: no mod.json")
    json.loads(manifest.read_text())  # malformed JSON fails here, not on the console
    files = sorted((p.relative_to(source).as_posix(), p) for p in source.rglob("*") if p.is_file())
    output.parent.mkdir(parents=True, exist_ok=True)
    staged = output.with_suffix(output.suffix + ".part")
    with zipfile.ZipFile(staged, "w", compression=zipfile.ZIP_STORED) as archive:
        for name, path in files:
            info = zipfile.ZipInfo(name, date_time=FIXED_TIME)
            info.compress_type = zipfile.ZIP_STORED
            info.create_system = 3  # Unix, whatever host packs it
            info.external_attr = 0o644 << 16
            archive.writestr(info, canonical_bytes(path))
    staged.replace(output)
    return hashlib.sha256(output.read_bytes()).hexdigest()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", type=pathlib.Path, help="the mod folder (mod.json and res/)")
    ap.add_argument("output", type=pathlib.Path, help="the .dusk to write")
    args = ap.parse_args(argv)
    try:
        normalised = normalised_files(args.source)
        digest = pack(args.source, args.output)
    except (ValueError, OSError, json.JSONDecodeError) as error:
        print(f"make_dusk: {error}", file=sys.stderr)
        return 1
    if normalised:
        print(f"make_dusk: note: {len(normalised)} text file(s) had CRLF line endings and were "
              f"packed with LF ({', '.join(normalised)})", file=sys.stderr)
    print(f"{args.output} sha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
