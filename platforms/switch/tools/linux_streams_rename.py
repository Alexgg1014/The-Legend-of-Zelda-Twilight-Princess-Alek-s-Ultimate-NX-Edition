#!/usr/bin/env python3
"""aleks2 (DM2-b): renames the glibc-layout file-stream instances of linux_fstream.cpp and writes the table
the loader binds Linux modules' imports with (platforms/switch/cmake/LinuxStreams.cmake).

Every symbol the object DEFINES whose name belongs to std::basic_filebuf / basic_ifstream / basic_ofstream /
basic_fstream (members, vtables _ZTV, VTTs _ZTT, construction vtables _ZTC, type_info _ZTI and its name _ZTS)
becomes aleks2lx_<name>; references inside the object follow (objcopy --redefine-syms renames both). What the
object defines of other templates (inline basic_ios, basic_streambuf, string... in COMDAT groups) keeps its
name: the same layout as the game's, so whichever copy the linker keeps is right.

The table holds the GLOBAL/WEAK renamed symbols: {original name, address of aleks2lx_<name>}.

Sol's review (respuesta-sol-dm2b.md), checked and applied:
  * point 1: renaming _ZTS does not change the type-name string the type_info points at, and libstdc++'s
    type_info::operator== compares names when the pointers differ (unless the name starts with '*'), so a
    native and a glibc-layout std::ifstream would compare equal and a dynamic_cast across the two layouts could
    succeed. The renamed _ZTS strings are rewritten '*'+name[1:] (same length) in the object: pointer-only
    equality. No catalogue mod defines file-stream RTTI of its own (research/dm2b/sol_checks.py), so the
    modules' casts reach these type_infos by pointer.
  * point 7: every symbol, defined or referenced, that mentions basic_filebuf/basic_[io]fstream/stdio_filebuf
    in any form (thunks _ZTh/_ZTv, std::swap with template parameters, helpers) must be renamed: anything left
    under its own name would bind to the game's native-layout one. The build fails otherwise. An inventory of
    the object (renamed, other globals, locals) is written next to it.

  linux_streams_rename.py --nm NM --objcopy OBJCOPY --in OBJ --out OBJ --table CPP
"""
import argparse
import json
import struct
import pathlib
import re
import subprocess
import sys

FAMILY = re.compile(r"^_Z.*(13basic_filebuf|14basic_ifstream|14basic_ofstream|13basic_fstream)IcSt11char_traitsIcE")
PREFIX = "aleks2lx_"
# Any mention of the classes whose layout differs, however mangled (also inside template arguments).
MENTION = re.compile(r"(13basic_filebuf|14basic_ifstream|14basic_ofstream|13basic_fstream|13stdio_filebuf)")


def mark_type_names(obj):
    """Rewrites each renamed _ZTS string of the relocatable ELF64 `obj` to '*'+name[1:] (same length), after checking
    it holds exactly the type's mangled name. Returns how many were marked."""
    data = bytearray(obj.read_bytes())
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    marked = 0
    for sec in sections:
        if sec[1] != 2:  # SHT_SYMTAB
            continue
        strtab = sections[sec[6]]
        for i in range(sec[5] // 24):
            name_off, _info, _other, shndx, value, size = struct.unpack_from("<IBBHQQ", data, sec[4] + i * 24)
            end = data.index(b"\0", strtab[4] + name_off)
            name = data[strtab[4] + name_off:end].decode()
            if not name.startswith(PREFIX + "_ZTS"):
                continue
            expected = name[len(PREFIX + "_ZTS"):].encode() + b"\0"
            at = sections[shndx][4] + value
            if bytes(data[at:at + len(expected)]) != expected:
                raise SystemExit(f"linux_streams_rename: {name} does not hold its type name")
            data[at] = ord("*")
            marked += 1
    obj.write_bytes(bytes(data))
    return marked
# A module may not import these (they are not in its view of libstdc++), so the table leaves them out.
NOT_IMPORTABLE = ("_ZTC",)


def defined_symbols(nm, obj):
    """[(name, type)] of the symbols obj defines (nm -P: name type value size)."""
    out = subprocess.run([nm, "-P", "--defined-only", obj], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            syms.append((parts[0], parts[1]))
    return syms


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", required=True)
    ap.add_argument("--objcopy", required=True)
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--table", required=True)
    a = ap.parse_args(argv)

    syms = defined_symbols(a.nm, a.inp)
    family = sorted({n for n, _t in syms if FAMILY.match(n)})
    if not family:
        print("linux_streams_rename: no basic_filebuf/fstream symbol defined in", a.inp, file=sys.stderr)
        return 1
    mapfile = pathlib.Path(a.out).with_suffix(".map")
    mapfile.write_text("".join(f"{n} {PREFIX}{n}\n" for n in family), encoding="utf-8")
    subprocess.run([a.objcopy, f"--redefine-syms={mapfile}", a.inp, a.out], check=True)

    # Check: nothing that mentions the family is left under its own name, defined or referenced, in any form.
    after = subprocess.run([a.nm, "-P", a.out], capture_output=True, text=True, check=True).stdout
    leftover = [l.split()[0] for l in after.splitlines()
                if l.split() and MENTION.search(l.split()[0]) and not l.split()[0].startswith(PREFIX)]
    if leftover:
        print("linux_streams_rename: still under their own names:", leftover[:5], file=sys.stderr)
        return 1
    marked = mark_type_names(pathlib.Path(a.out))
    if marked < 4:
        print(f"linux_streams_rename: only {marked} renamed _ZTS strings marked", file=sys.stderr)
        return 1
    inventory = {"renamed": len(family), "marked_type_names": marked,
                 "other_globals": sorted(l.split()[0] for l in after.splitlines()
                                         if len(l.split()) > 1 and l.split()[1] in "TWVRDBu" and not l.split()[0].startswith(PREFIX)),
                 "undefined": sorted(l.split()[0] for l in after.splitlines() if len(l.split()) > 1 and l.split()[1] in "Uw")}
    pathlib.Path(a.out).with_suffix(".inventory.json").write_text(json.dumps(inventory, indent=1), encoding="utf-8")

    exported = sorted(n for n, t in syms if FAMILY.match(n) and t in "TWVRDB" and not n.startswith(NOT_IMPORTABLE))
    lines = [
        "// aleks2 (DM2-b): generated by platforms/switch/tools/linux_streams_rename.py -- do not edit.",
        "// The glibc-layout file streams of src/dusk/mods/loader/linux_fstream.cpp, renamed aleks2lx_<name>,",
        "// under the names a Linux-built module imports. Linux modules only.",
        '#include "dusk/mods/loader/elf_runtime_policy.hpp"',
        "",
        "#include <cstdint>",
        "#include <vector>",
        "",
        'extern "C" {',
    ]
    for i, n in enumerate(exported):
        lines.append(f'extern char aleks2lx_sym{i} __asm__("{PREFIX}{n}");')
    lines += [
        "}",
        "",
        "namespace dusk::mods::elf {",
        "",
        "std::vector<Shim> linux_stream_shims() {",
        '    constexpr std::string_view kReason = "glibc-layout std::basic_filebuf/fstream (DM2-b)";',
        "    return {",
    ]
    for i, n in enumerate(exported):
        lines.append(f'        {{"{n}", reinterpret_cast<uint64_t>(&aleks2lx_sym{i}), kReason, true}},')
    lines += ["    };", "}", "", "}  // namespace dusk::mods::elf", ""]
    pathlib.Path(a.table).write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"linux_streams_rename: {len(family)} renamed, {len(exported)} in the table")
    return 0


if __name__ == "__main__":
    sys.exit(main())
