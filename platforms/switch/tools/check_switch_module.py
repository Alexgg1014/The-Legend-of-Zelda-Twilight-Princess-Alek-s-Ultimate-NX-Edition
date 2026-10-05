#!/usr/bin/env python3
"""aleks2 (H37-c6): is this a switch-arm64 native module the way build-h37-test-mods.sh makes one?

A switch-arm64 module is compiled with devkitA64's aarch64-none-elf-g++ -- the compiler and C++ headers the game
itself is built with -- so its inline C++ (iostreams, locale, std::string...) is laid out as the game's libstdc++
expects. It must therefore be a plain loadable module and nothing more:

  * a .dusk stores exactly one library, lib/switch-arm64/mod.so (no other lib/ entry: the loader takes it, and
    a linux-aarch64 copy beside it would only be a way for an old build to be picked up by mistake);
  * AArch64, ET_DYN, 64-bit little-endian;
  * no DT_NEEDED: the loader maps one image and resolves against the game;
  * no PT_TLS, no TLS symbols or relocations, no text relocations (position-independent), no IFUNC;
  * undefined imports left for the loader to resolve against the game (a module with none cannot be a mod that
    uses the game or the C++ runtime);
  * no private runtime: it defines none of libstdc++'s/libgcc's symbols (a statically linked copy would have its
    own, differently-laid-out ios_base and its own unwinder);
  * PT_GNU_EH_FRAME whenever the module has exception/unwind tables: the loader uses it to find .eh_frame and
    register the module with the game's unwinder before any constructor or mod function runs;
  * the mod exports (mod_meta, mod_ctx, mod_initialize, mod_update, mod_shutdown) and nothing elf_audit's
    switch_package verdict refuses.

  check_switch_module.py PACKAGE.dusk | mod.so ...        exit 1 on any failure; prints the reasons.
"""
import argparse
import pathlib
import sys
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import elf_audit  # noqa: E402

EXPECTED = "lib/switch-arm64/mod.so"


def check(path: pathlib.Path) -> list:
    """The reasons `path` is not a switch-arm64 module (empty when it is one)."""
    problems = []
    if path.suffix == ".dusk" or zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            libs = sorted(n for n in z.namelist() if n.startswith("lib/") and not n.endswith("/"))
            if libs != [EXPECTED]:
                problems.append(f"the package must store exactly {EXPECTED}, it stores: {', '.join(libs) or 'no library'}")
            if EXPECTED not in libs:
                return problems
            data, label = z.read(EXPECTED), f"{path.name}:{EXPECTED}"
    else:
        data, label = path.read_bytes(), path.name
    try:
        elf = elf_audit.Elf(data, label)
        audit = elf_audit.audit_elf(elf)
    except elf_audit.ElfError as error:
        return problems + [str(error)]
    if audit["machine"] != "aarch64":
        problems.append(f"machine {audit['machine']}, not aarch64")
    if audit["type"] != "DYN":
        problems.append(f"type {audit['type']}, not ET_DYN")
    if audit["needed"]:
        problems.append("DT_NEEDED " + ", ".join(audit["needed"]) + ": a switch-arm64 module may not depend on a library")
    if audit["has"]["TLS"] or audit["tls_symbols"] or any(r.startswith(("TLS", "R_AARCH64_TLS")) for r in audit["relocations"]):
        problems.append("thread-local storage (PT_TLS / TLS symbols / TLS relocations)")
    if audit["textrel"]:
        problems.append("text relocations: the module is not position-independent")
    if audit["ifunc_symbols"]:
        problems.append("GNU indirect functions")
    if sum(audit["imports_count"].values()) == 0:
        problems.append("no undefined imports: nothing is left for the loader to resolve against the game")
    if audit["private_runtime_symbols"]:
        problems.append("it defines libstdc++/libgcc symbols itself (a private copy of the runtime): "
                        + ", ".join(audit["private_runtime_symbols"][:5]))
    if audit["unwind"]["eh_frame"] and not audit["unwind"]["eh_frame_hdr"]:
        problems.append(".eh_frame has no PT_GNU_EH_FRAME: the loader cannot register its exception handlers")
    problems += audit["verdict"]["switch_package"]["blockers"]
    return list(dict.fromkeys(problems))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", type=pathlib.Path)
    args = ap.parse_args(argv)
    bad = 0
    for path in args.files:
        problems = check(path)
        bad += bool(problems)
        print(f"{'ok  ' if not problems else 'FAIL'} {path}")
        for p in problems:
            print(f"     {p}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
