"""H38 test fixtures: a synthetic AArch64 module, a fake symgen and a fake cmake, so the pipeline and the gate can be
exercised on a host with no devkitA64 and no symgen. Nothing here is a production artifact."""
import json
import pathlib
import struct
import zipfile

MOD_EXPORTS = ("mod_meta", "mod_ctx", "mod_initialize", "mod_update", "mod_shutdown")


def make_elf(imports=("game_function", "memcpy"), tls=False, eh_hdr=True, machine=183, needed=(), seed=0) -> bytes:
    """A minimal ELF64 shared object elf_audit reads: dynsym with the five mod exports and `imports` (undefined),
    GLOB_DAT relocations for the imports, optional PT_TLS (tls), PT_GNU_EH_FRAME + an .eh_frame section (eh_hdr)."""
    names = list(MOD_EXPORTS) + list(imports)
    dynstr = b"\0"
    offsets = {}
    for n in list(needed) + names:
        offsets[n] = len(dynstr)
        dynstr += n.encode() + b"\0"
    syms = struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0)
    for n in names:
        defined = n in MOD_EXPORTS
        syms += struct.pack("<IBBHQQ", offsets[n], (1 << 4) | (1 if defined else 0), 0, 1 if defined else 0,
                            0x1000 + 16 * names.index(n) if defined else 0, 16 if defined else 0)
    if tls:
        offsets["tls_var"] = len(dynstr)
        dynstr += b"tls_var\0"
        syms += struct.pack("<IBBHQQ", offsets["tls_var"], (1 << 4) | 6, 0, 1, 0, 4)
    nsyms = len(syms) // 24
    relas = b"".join(struct.pack("<QQq", 0x3000 + 8 * i, ((names.index(n) + 1) << 32) | 1025, 0)
                     for i, n in enumerate(imports))
    shstr = b"\0.dynsym\0.dynstr\0.eh_frame\0.shstrtab\0"
    sh_off = {n: shstr.index(b"\0" + n.encode() + b"\0") + 1 for n in (".dynsym", ".dynstr", ".eh_frame", ".shstrtab")}
    phnum = 3 + (1 if tls else 0) + (1 if eh_hdr else 0)
    base = 64 + 56 * phnum
    dynstr_at = base
    dynsym_at = (dynstr_at + len(dynstr) + 7) & ~7
    rela_at = dynsym_at + len(syms)
    ehf_at = rela_at + len(relas)
    ehf = struct.pack("<I", 0)  # a terminator-only .eh_frame
    dyn_entries = [(1, offsets[n]) for n in needed] + [(5, dynstr_at), (6, dynsym_at), (10, len(dynstr)), (11, 24)]
    if relas:
        dyn_entries += [(7, rela_at), (8, len(relas)), (9, 24)]
    dyn_entries.append((0, 0))
    dyn = b"".join(struct.pack("<qQ", t, v) for t, v in dyn_entries)
    dyn_at = (ehf_at + len(ehf) + 7) & ~7
    shstr_at = dyn_at + len(dyn)
    sh_at = (shstr_at + len(shstr) + 7) & ~7
    total = sh_at + 64 * 5

    def sh(name, typ, addr, off, size, link=0, ent=0):
        return struct.pack("<IIQQQQIIQQ", name, typ, 2 if addr else 0, addr, off, size, link, 0, 8, ent)
    shdrs = (sh(0, 0, 0, 0, 0) + sh(sh_off[".dynsym"], 11, dynsym_at, dynsym_at, len(syms), 2, 24)
             + sh(sh_off[".dynstr"], 3, dynstr_at, dynstr_at, len(dynstr))
             + sh(sh_off[".eh_frame"], 1, ehf_at, ehf_at, len(ehf))
             + sh(sh_off[".shstrtab"], 3, 0, shstr_at, len(shstr)))
    phdrs = struct.pack("<IIQQQQQQ", 1, 4, 0, 0, 0, total, total, 0x1000)                      # PT_LOAD R
    phdrs += struct.pack("<IIQQQQQQ", 2, 4, dyn_at, dyn_at, dyn_at, len(dyn), len(dyn), 8)    # PT_DYNAMIC
    phdrs += struct.pack("<IIQQQQQQ", 0x6474E551, 6, 0, 0, 0, 0, 0, 16)                       # PT_GNU_STACK
    if tls:
        phdrs += struct.pack("<IIQQQQQQ", 7, 4, 0, 0, 0, 4, 4, 4)                             # PT_TLS
    if eh_hdr:
        phdrs += struct.pack("<IIQQQQQQ", 0x6474E550, 4, ehf_at, ehf_at, ehf_at, len(ehf), len(ehf), 4)
    ehdr = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8) + struct.pack(
        "<HHIQQQIHHHHHH", 3, machine, 1, 0, 64, sh_at, 0, 64, 56, phnum, 64, 5, 4)
    body = bytearray(total)
    body[0:64] = ehdr
    body[64:64 + len(phdrs)] = phdrs
    for at, blob in ((dynstr_at, dynstr), (dynsym_at, syms), (rela_at, relas), (ehf_at, ehf), (dyn_at, dyn),
                     (shstr_at, shstr), (sh_at, shdrs)):
        body[at:at + len(blob)] = blob
    # `seed` makes otherwise equal libraries differ (a distinct build id for a distinct fixture).
    return bytes(body) + struct.pack("<I", seed)


def write_package(path: pathlib.Path, mod_id: str, version: str, libs: dict, extra=None) -> None:
    """A .dusk the way an SDK build leaves it: deflated, real timestamps. libs: {platform: elf bytes}."""
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr("mod.json", json.dumps({"id": mod_id, "name": mod_id, "version": version}))
        z.writestr("res/readme.txt", "hello\r\n")
        for platform, data in sorted(libs.items()):
            z.writestr(f"lib/{platform}/mod.so", data)
        for name, data in (extra or {}).items():
            z.writestr(name, data)


FAKE_SYMGEN = '''#!/usr/bin/env python3
# Fake symgen: `symgen modmeta LIB` answers from the table in $FAKE_SYMGEN_TABLE, keyed by the library's sha256.
import hashlib, json, os, sys
assert sys.argv[1] == "modmeta", sys.argv
lib = open(sys.argv[2], "rb").read()
table = json.load(open(os.environ["FAKE_SYMGEN_TABLE"]))
entry = table.get(hashlib.sha256(lib).hexdigest(), {"hooks": [], "imports": []})
print(json.dumps({"files": [entry]}))
'''

FAKE_CMAKE = '''#!/usr/bin/env python3
# Fake cmake for build_switch_mods.py: records the configure line and "builds" the package target from
# $FAKE_CMAKE_PACKAGES = {target: {mod_id, version, libs: {platform: {elf args}}}}.
import json, os, pathlib, sys
sys.path.insert(0, os.environ["FAKE_CMAKE_FIXTURES"])
import pipeline_fixtures as fx
argv = sys.argv[1:]
if argv[0] == "--build":
    build = pathlib.Path(argv[1])
    target = argv[argv.index("--target") + 1]
    spec = json.load(open(os.environ["FAKE_CMAKE_PACKAGES"]))[target]
    libs = {p: fx.make_elf(**kw) for p, kw in spec["libs"].items()}
    (build / "mods").mkdir(parents=True, exist_ok=True)
    fx.write_package(build / "mods" / (target.replace("_package", "") + ".dusk"), spec["mod_id"], spec["version"], libs)
else:
    src = pathlib.Path(argv[argv.index("-S") + 1])
    build = pathlib.Path(argv[argv.index("-B") + 1])
    build.mkdir(parents=True, exist_ok=True)
    (build / "configure.json").write_text(json.dumps({
        "args": [a for a in argv if a.startswith("-D")], "ci": os.environ.get("CI"), "path": os.environ["PATH"],
        "has_toolchain_patch": (src / "PATCHED").exists()}))
'''
