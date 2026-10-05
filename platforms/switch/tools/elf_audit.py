#!/usr/bin/env python3
"""aleks2 (H37): what a native mod library actually asks of whoever loads it.

Reads ELF64 shared objects -- loose `.so` files or the `lib/<platform>/*.so` inside `.dusk`
packages -- with nothing but the standard library, and reports, per library:

  class, endianness, machine, type; PT_LOAD/PT_DYNAMIC/PT_TLS/GNU_RELRO/GNU_EH_FRAME/
  GNU_STACK; the dynamic section (DT_NEEDED, SONAME, hash tables, flags, init/fini arrays,
  version tables); relocations counted by type; imports and exports (with symbol versions);
  TLS; unwind and exception tables; the C runtime it was built against; memory footprint and
  alignment; and a verdict for Horizon, with the reasons.

The verdict is about loading the file as it is with platforms/switch's loader
(src/dusk/mods/loader/elf_loader.cpp): which relocations it would need, which dependencies,
and which imports only a glibc/Linux runtime defines. It does not guess at the game: an
import that is not a C or C++ runtime name is counted as "game or SDK", to be resolved
against the game's symbol manifest at load time.

  elf_audit.py [--json OUT] FILE.so|PACKAGE.dusk ...
"""
import argparse
import hashlib
import json
import pathlib
import struct
import sys
import zipfile

PT = {0: "NULL", 1: "LOAD", 2: "DYNAMIC", 3: "INTERP", 4: "NOTE", 6: "PHDR", 7: "TLS",
      0x6474E550: "GNU_EH_FRAME", 0x6474E551: "GNU_STACK", 0x6474E552: "GNU_RELRO",
      0x6474E553: "GNU_PROPERTY"}
ET = {1: "REL", 2: "EXEC", 3: "DYN", 4: "CORE"}
EM = {62: "x86_64", 183: "aarch64", 40: "arm", 3: "x86"}
DT = {1: "NEEDED", 2: "PLTRELSZ", 3: "PLTGOT", 4: "HASH", 5: "STRTAB", 6: "SYMTAB", 7: "RELA",
      8: "RELASZ", 9: "RELAENT", 10: "STRSZ", 11: "SYMENT", 12: "INIT", 13: "FINI",
      14: "SONAME", 15: "RPATH", 16: "SYMBOLIC", 17: "REL", 18: "RELSZ", 19: "RELENT",
      20: "PLTREL", 21: "DEBUG", 22: "TEXTREL", 23: "JMPREL", 24: "BIND_NOW", 25: "INIT_ARRAY",
      26: "FINI_ARRAY", 27: "INIT_ARRAYSZ", 28: "FINI_ARRAYSZ", 29: "RUNPATH", 30: "FLAGS",
      32: "PREINIT_ARRAY", 33: "PREINIT_ARRAYSZ", 35: "RELRSZ", 36: "RELR", 37: "RELRENT",
      0x6FFFFEF5: "GNU_HASH", 0x6FFFFFF0: "VERSYM", 0x6FFFFFF9: "RELACOUNT",
      0x6FFFFFFA: "RELCOUNT", 0x6FFFFFFB: "FLAGS_1", 0x6FFFFFFC: "VERDEF", 0x6FFFFFFD: "VERDEFNUM",
      0x6FFFFFFE: "VERNEED", 0x6FFFFFFF: "VERNEEDNUM", 0x6FFFFEF6: "TLSDESC_PLT",
      0x6FFFFEF7: "TLSDESC_GOT"}
R_AARCH64 = {0: "NONE", 257: "ABS64", 258: "ABS32", 259: "ABS16", 260: "PREL64", 261: "PREL32",
             1024: "COPY", 1025: "GLOB_DAT", 1026: "JUMP_SLOT", 1027: "RELATIVE",
             1028: "TLS_DTPMOD64", 1029: "TLS_DTPREL64", 1030: "TLS_TPREL64", 1031: "TLSDESC",
             1032: "IRELATIVE"}
STB = {0: "LOCAL", 1: "GLOBAL", 2: "WEAK", 10: "GNU_UNIQUE"}
STT = {0: "NOTYPE", 1: "OBJECT", 2: "FUNC", 3: "SECTION", 4: "FILE", 5: "COMMON", 6: "TLS",
       10: "GNU_IFUNC"}

# Mirrors of the loader's policy (src/dusk/mods/loader/elf_loader.cpp, elf_runtime_policy.cpp).
# Kept in step by platforms/switch/tools/test-elf-loader.sh, which compares this tool's verdict
# with the loader's own Dry Run on the same files.
LOADER_RELOCATIONS = {"NONE", "ABS64", "GLOB_DAT", "JUMP_SLOT", "RELATIVE"}
# DT_NEEDED a Linux-built package may name: the game image stands in for these five.
PROVIDED_NEEDED = {"libc.so.6", "libm.so.6", "libstdc++.so.6", "libgcc_s.so.1", "ld-linux-aarch64.so.1"}
# The loader's own shims.
LOADER_SHIMS = {"__cxa_finalize", "__errno_location", "__libc_single_threaded", "__stack_chk_guard",
                "stdin", "stdout", "stderr"}
# glibc-only names the NRO itself defines (src/dusk/switch_mod_libc.cpp, switch_mod_libstdcxx.cpp).
NRO_GLIBC_COMPAT = {"__ctype_b_loc", "__ctype_tolower_loc", "__ctype_toupper_loc", "__isoc23_strtol",
                    "__isoc23_strtoll", "__isoc23_strtoul", "__isoc23_strtoull", "__isoc23_sscanf",
                    "__isoc99_sscanf", "__printf_chk", "__fprintf_chk", "__vfprintf_chk", "__getauxval",
                    "getrandom", "pthread_cond_clockwait", "pipe", "prctl", "getifaddrs", "freeifaddrs",
                    "dlopen", "dlsym", "dlerror"}
# Refused by name for Linux-built packages (exact, or prefix when ending in '*').
DENIED = ["_IO_*", "_dl_*", "dl_iterate_phdr", "__tls_get_addr",
          "__cxa_thread_atexit_impl", "__register_atfork", "fork", "syscall", "__libc_*",
          "gnu_get_libc_version"]
CAVEATS = ["pthread_*", "sem_*"]
# H37-c6: refused by name for Linux-built packages, and only for them: objects whose C++ layout depends
# on the libstdc++ the module was compiled with. A Linux module is built against GCC 13's headers and its
# inline constructors lay std::ios_base/basic_ios and the streams built on them out as GCC 13 does; the game runs
# devkitA64's libstdc++ (16), whose code expects another layout -- H37-c5's Randomizer died in
# std::locale::operator= from basic_ofstream's constructor. std::locale itself is NOT on the list: an isolated
# locale import (Cosmetics' six: locale(), locale(const&), ~locale(), locale::id::_M_id, locale::facet) is a
# plain library call on a small object, Cosmetics ran on the console, and the crash evidence implicates ios_base. Substrings of the Itanium-mangled names (a
# vtable, a VTT, a constructor and a member of the family all contain them); ios_base::Init, the static
# initialiser <iostream> adds to every translation unit, is a plain library call and stays allowed.
UNSAFE_LIBSTDCXX_ABI = ("8ios_base", "9basic_ios", "basic_ifstream", "basic_ofstream", "basic_fstream",
                        "basic_stringstream", "basic_ostringstream", "basic_istringstream")
UNSAFE_LIBSTDCXX_ABI_EXEMPT = ("ios_base4Init",)
UNSAFE_LIBSTDCXX_ABI_REASON = (
    "libstdc++ ABI not safe: a Linux-built module's inline std::ios_base/basic_ios/fstream/stringstream "
    "code is laid out for the libstdc++ it was compiled with, not the game's; a switch-arm64 library is required")
# Version prefixes that only a glibc/GNU userland defines.
LINUX_VERSIONS = ("GLIBC_", "GLIBCXX_", "CXXABI_", "GCC_")
# Imports that are glibc's own (not ISO C/POSIX), whatever their version.
GLIBC_ONLY = {"__cxa_finalize", "__libc_start_main", "__errno_location", "__stack_chk_guard",
              "__ctype_b_loc", "__ctype_tolower_loc", "__ctype_toupper_loc", "_ITM_registerTMCloneTable",
              "_ITM_deregisterTMCloneTable", "__gmon_start__", "__cxa_thread_atexit_impl",
              "__libc_single_threaded", "__tls_get_addr", "__assert_fail", "__isoc23_strtol",
              "__isoc23_strtoul", "__isoc23_sscanf", "__isoc99_sscanf"}


def unsafe_libstdcxx_abi(name):
    """True for an import whose layout depends on the libstdc++ version (see UNSAFE_LIBSTDCXX_ABI)."""
    return (name.startswith("_Z") and any(p in name for p in UNSAFE_LIBSTDCXX_ABI)
            and not any(e in name for e in UNSAFE_LIBSTDCXX_ABI_EXEMPT))


def library_platform(label):
    """The lib/<platform>/ a library label names ('pkg.dusk:lib/linux-aarch64/mod.so'), else None."""
    parts = label.split(":", 1)[-1].split("/")
    return parts[1] if len(parts) == 3 and parts[0] == "lib" else None


def matches(name, patterns):
    return any(name.startswith(p[:-1]) if p.endswith("*") else name == p for p in patterns)


class ElfError(Exception):
    pass


class Elf:
    def __init__(self, data: bytes, name: str):
        self.name = name
        self.data = data
        if len(data) < 64 or data[:4] != b"\x7fELF":
            raise ElfError("not an ELF file")
        self.elfclass = {1: "ELF32", 2: "ELF64"}.get(data[4], f"class {data[4]}")
        self.endian = {1: "little", 2: "big"}.get(data[5], f"data {data[5]}")
        if self.elfclass != "ELF64" or self.endian != "little":
            raise ElfError(f"{self.elfclass} {self.endian}-endian: only ELF64 little-endian is read")
        (self.type, self.machine, _, self.entry, self.phoff, self.shoff, self.flags, _, phentsize,
         phnum, shentsize, shnum, shstrndx) = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
        self.phdrs = []
        for i in range(phnum):
            at = self.phoff + i * phentsize
            if at + 56 > len(data):
                raise ElfError("program header table runs past the end of the file")
            ptype, flags, offset, vaddr, paddr, filesz, memsz, align = struct.unpack_from(
                "<IIQQQQQQ", data, at)
            self.phdrs.append({"type": PT.get(ptype, hex(ptype)), "flags": flags, "offset": offset,
                               "vaddr": vaddr, "filesz": filesz, "memsz": memsz, "align": align})
        self.sections = []
        if self.shoff and shnum:
            for i in range(shnum):
                at = self.shoff + i * shentsize
                if at + 64 > len(data):
                    raise ElfError("section header table runs past the end of the file")
                n, t, f, addr, off, size, link, info, align, ent = struct.unpack_from(
                    "<IIQQQQIIQQ", data, at)
                self.sections.append({"name_off": n, "type": t, "flags": f, "addr": addr, "offset": off,
                                      "size": size, "link": link, "info": info, "entsize": ent})
            if shstrndx < len(self.sections):
                strtab = self.sections[shstrndx]
                for s in self.sections:
                    s["name"] = self._cstr(strtab["offset"] + s["name_off"])

    def _cstr(self, at):
        end = self.data.find(b"\0", at)
        return self.data[at:end if end >= 0 else len(self.data)].decode("utf-8", "replace")

    def vaddr_to_offset(self, vaddr):
        for p in self.phdrs:
            if p["type"] == "LOAD" and p["vaddr"] <= vaddr < p["vaddr"] + p["filesz"]:
                return p["offset"] + (vaddr - p["vaddr"])
        raise ElfError(f"address {vaddr:#x} is not backed by the file")

    def section(self, name):
        for s in self.sections:
            if s.get("name") == name:
                return s
        return None


def audit_elf(elf: Elf) -> dict:
    out = {"class": elf.elfclass, "endian": elf.endian, "machine": EM.get(elf.machine, str(elf.machine)),
           "type": ET.get(elf.type, str(elf.type)), "entry": elf.entry}
    loads = [p for p in elf.phdrs if p["type"] == "LOAD"]
    out["segments"] = [{"type": p["type"], "flags": "".join(c for c, bit in (("R", 4), ("W", 2), ("X", 1))
                                                         if p["flags"] & bit),
                        "vaddr": p["vaddr"], "memsz": p["memsz"], "filesz": p["filesz"],
                        "offset": p["offset"], "align": p["align"]} for p in elf.phdrs]
    span_lo = min((p["vaddr"] for p in loads), default=0)
    span_hi = max((p["vaddr"] + p["memsz"] for p in loads), default=0)
    out["image_bytes"] = span_hi - span_lo
    out["max_align"] = max((p["align"] for p in loads), default=0)
    out["has"] = {k: any(p["type"] == k for p in elf.phdrs)
                  for k in ("DYNAMIC", "TLS", "GNU_RELRO", "GNU_EH_FRAME", "GNU_STACK", "INTERP")}
    out["wx_segments"] = [s["vaddr"] for s in out["segments"]
                          if s["type"] == "LOAD" and "W" in s["flags"] and "X" in s["flags"]]

    dyn = {}
    needed = []
    dynamic = next((p for p in elf.phdrs if p["type"] == "DYNAMIC"), None)
    if dynamic is not None:
        at = dynamic["offset"]
        end = at + dynamic["filesz"]
        while at + 16 <= end:
            tag, val = struct.unpack_from("<qQ", elf.data, at)
            at += 16
            if tag == 0:
                break
            name = DT.get(tag, hex(tag))
            if name == "NEEDED":
                needed.append(val)
            else:
                dyn[name] = val
    strtab_off = elf.vaddr_to_offset(dyn["STRTAB"]) if "STRTAB" in dyn else None
    dstr = (lambda o: elf._cstr(strtab_off + o)) if strtab_off is not None else (lambda o: f"<{o}>")
    out["needed"] = [dstr(n) for n in needed]
    out["soname"] = dstr(dyn["SONAME"]) if "SONAME" in dyn else None
    out["dynamic_tags"] = sorted(dyn)
    out["hash"] = [h for h in ("HASH", "GNU_HASH") if h in dyn]
    out["bind_now"] = "BIND_NOW" in dyn or bool(dyn.get("FLAGS", 0) & 0x8) or bool(dyn.get("FLAGS_1", 0) & 1)
    out["textrel"] = "TEXTREL" in dyn or bool(dyn.get("FLAGS", 0) & 0x4)
    out["init"] = {"DT_INIT": "INIT" in dyn, "init_array": dyn.get("INIT_ARRAYSZ", 0) // 8,
                   "preinit_array": dyn.get("PREINIT_ARRAYSZ", 0) // 8,
                   "DT_FINI": "FINI" in dyn, "fini_array": dyn.get("FINI_ARRAYSZ", 0) // 8}

    # Dynamic symbols. The count comes from the section header when there is one (the
    # loader itself gets it from the hash table).
    symbols = []
    dynsym = elf.section(".dynsym")
    versym = None
    if "VERSYM" in dyn:
        versym = elf.vaddr_to_offset(dyn["VERSYM"])
    if dynsym is not None and strtab_off is not None:
        count = dynsym["size"] // 24
        for i in range(count):
            n, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", elf.data,
                                                                     dynsym["offset"] + 24 * i)
            ver = struct.unpack_from("<H", elf.data, versym + 2 * i)[0] if versym is not None else 0
            symbols.append({"name": dstr(n), "bind": STB.get(info >> 4, str(info >> 4)),
                            "type": STT.get(info & 15, str(info & 15)), "defined": shndx != 0,
                            "value": value, "size": size, "verndx": ver & 0x7FFF})
    versions = {}
    if "VERNEED" in dyn:
        at = elf.vaddr_to_offset(dyn["VERNEED"])
        for _ in range(dyn.get("VERNEEDNUM", 0)):
            vn_version, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from("<HHIII", elf.data, at)
            aux = at + vn_aux
            for _ in range(vn_cnt):
                vna_hash, vna_flags, vna_other, vna_name, vna_next = struct.unpack_from(
                    "<IHHII", elf.data, aux)
                versions[vna_other] = {"name": dstr(vna_name), "file": dstr(vn_file)}
                aux += vna_next
                if vna_next == 0:
                    break
            at += vn_next
            if vn_next == 0:
                break
    for s in symbols:
        v = versions.get(s["verndx"])
        s["version"] = v["name"] if v else None
    named = [s for s in symbols if s["name"]]
    imports = [s for s in named if not s["defined"]]
    exports = [s for s in named if s["defined"] and s["bind"] in ("GLOBAL", "WEAK", "GNU_UNIQUE")]
    out["exports_count"] = len(exports)
    out["mod_exports"] = sorted(s["name"] for s in exports if s["name"] in
                                ("mod_meta", "mod_ctx", "mod_initialize", "mod_update", "mod_shutdown"))
    # H37-c6: libstdc++/libgcc symbols the library itself DEFINES and exports -- a private copy of the runtime.
    out["private_runtime_symbols"] = sorted(s["name"] for s in exports if s["name"].startswith(
        ("_ZNSt", "_ZSt", "_ZNKSt", "_ZTVSt", "_ZTISt", "_ZTSSt", "_ZNSs", "_ZNK9__gnu", "_ZN9__gnu", "__cxa_",
         "__gxx_", "_Unwind_", "__gcc_")))
    out["tls_symbols"] = sorted(s["name"] for s in named if s["type"] == "TLS")
    out["ifunc_symbols"] = sorted(s["name"] for s in named if s["type"] == "GNU_IFUNC")
    out["unique_symbols"] = sum(1 for s in exports if s["bind"] == "GNU_UNIQUE")
    out["weak_exports"] = sum(1 for s in exports if s["bind"] == "WEAK")
    out["versions_needed"] = sorted({v["name"] for v in versions.values()})

    def classify(sym):
        version = sym["version"] or ""
        if sym["name"] in GLIBC_ONLY:
            return "glibc-only"
        if version.startswith(("GLIBCXX_", "CXXABI_")):
            return "libstdc++"
        if version.startswith("GCC_"):
            return "libgcc"
        if version.startswith("GLIBC_"):
            return "libc"
        return "game-or-sdk"
    groups = {}
    for s in imports:
        groups.setdefault(classify(s), []).append(s["name"] + (f"@{s['version']}" if s["version"] else ""))
    out["imports"] = {k: sorted(v) for k, v in sorted(groups.items())}
    out["imports_count"] = {k: len(v) for k, v in out["imports"].items()}
    out["weak_imports"] = sorted(s["name"] for s in imports if s["bind"] == "WEAK")

    # Relocations, by type, from the dynamic tags (what a loader actually walks).
    relocs = {}
    for tag, size_tag in (("RELA", "RELASZ"), ("JMPREL", "PLTRELSZ")):
        if tag not in dyn:
            continue
        at = elf.vaddr_to_offset(dyn[tag])
        for i in range(dyn.get(size_tag, 0) // 24):
            offset, info, addend = struct.unpack_from("<QQq", elf.data, at + 24 * i)
            rtype = info & 0xFFFFFFFF
            name = R_AARCH64.get(rtype, str(rtype)) if elf.machine == 183 else str(rtype)
            relocs[name] = relocs.get(name, 0) + 1
    out["relocations"] = dict(sorted(relocs.items()))
    out["relr"] = "RELR" in dyn
    out["unwind"] = {
        "eh_frame": elf.section(".eh_frame") is not None,
        "eh_frame_hdr": out["has"]["GNU_EH_FRAME"],
        "gcc_except_table": elf.section(".gcc_except_table") is not None,
        "throws": any(s["name"] in ("__cxa_throw", "_Unwind_Resume", "__cxa_begin_catch") for s in imports),
    }
    comment = elf.section(".comment")
    out["toolchain"] = (elf.data[comment["offset"]:comment["offset"] + comment["size"]]
                        .replace(b"\0", b" | ").decode("utf-8", "replace").strip(" |")
                        if comment else None)
    out["verdict"] = verdict(out, imports)
    return out


def verdict(a: dict, imports: list) -> dict:
    """What platforms/switch's loader would do with this file.

    Two answers, because the loader has two modes. `switch_package`: the file as a
    lib/switch-arm64 package, every import a plain name the game defines. `linux_compat`: the
    file as a lib/linux-aarch64 package in Linux compatibility mode (provided DT_NEEDED,
    versions dropped, the loader's shims, the NRO's glibc-compatibility names, refusals by name).
    """
    structural = []
    if a["machine"] != "aarch64":
        structural.append(f"machine {a['machine']}, not aarch64")
    if a["type"] != "DYN":
        structural.append(f"type {a['type']}, not a shared object (ET_DYN)")
    if a["has"]["INTERP"]:
        structural.append("PT_INTERP: an executable")
    if a["has"]["TLS"] or a["tls_symbols"]:
        structural.append("thread-local storage (PT_TLS / TLS symbols): the loader provides no TLS blocks")
    if a["ifunc_symbols"] or "IRELATIVE" in a["relocations"]:
        structural.append("GNU indirect functions (IFUNC/IRELATIVE)")
    unsupported = sorted(set(a["relocations"]) - LOADER_RELOCATIONS)
    if unsupported:
        structural.append("relocation types the loader does not apply: " + ", ".join(unsupported))
    if a["relr"]:
        structural.append("DT_RELR packed relocations")
    if a["textrel"]:
        structural.append("text relocations (DT_TEXTREL)")
    if a["wx_segments"]:
        structural.append("a segment that is both writable and executable")
    if a["unwind"]["throws"] and not a["unwind"]["eh_frame"]:
        structural.append("C++ exceptions without .eh_frame unwind tables")
    elif a["unwind"]["eh_frame"] and not a["unwind"]["eh_frame_hdr"]:
        structural.append(".eh_frame has no PT_GNU_EH_FRAME: the loader cannot register its exception handlers")
    missing_exports = sorted({"mod_meta", "mod_ctx", "mod_initialize", "mod_update", "mod_shutdown"}
                             - set(a["mod_exports"]))
    if missing_exports:
        structural.append("missing mod exports: " + ", ".join(missing_exports))

    switch = list(structural)
    if a["needed"]:
        switch.append("DT_NEEDED " + ", ".join(a["needed"]) + ": a Switch package may not depend on a library")
    versioned = sorted({s["version"] for s in imports if s["version"]})
    if versioned:
        switch.append(f"versioned imports ({', '.join(versioned[:3])}{'...' if len(versioned) > 3 else ''}): "
                      "built against glibc, not newlib")

    linux = list(structural)
    extra = [n for n in a["needed"] if n not in PROVIDED_NEEDED]
    if extra:
        linux.append("DT_NEEDED " + ", ".join(extra) + ": not one of the Linux runtime libraries the game stands in for")
    resolution = {"shim": [], "nro_glibc_compat": [], "weak_null_if_absent": [], "denied": [],
                  "caveat": [], "game_must_define": []}
    for s in imports:
        name = s["name"]
        if name in LOADER_SHIMS:
            resolution["shim"].append(name)
        elif matches(name, DENIED) or unsafe_libstdcxx_abi(name):
            resolution["denied"].append(name)
        elif name in NRO_GLIBC_COMPAT:
            resolution["nro_glibc_compat"].append(name)
        elif s["bind"] == "WEAK" and name in GLIBC_ONLY | {"_ITM_registerTMCloneTable", "_ITM_deregisterTMCloneTable"}:
            resolution["weak_null_if_absent"].append(name)
        else:
            if matches(name, CAVEATS):
                resolution["caveat"].append(name)
            resolution["game_must_define"].append(name)
    unsafe = sorted(n for n in resolution["denied"] if unsafe_libstdcxx_abi(n))
    other = sorted(n for n in resolution["denied"] if not unsafe_libstdcxx_abi(n))
    if other:
        linux.append("refused imports: " + ", ".join(other))
    if unsafe:
        linux.append(f"{UNSAFE_LIBSTDCXX_ABI_REASON} ({len(unsafe)} import(s), e.g. {', '.join(unsafe[:3])})")
    notes = []
    if a["unwind"]["throws"] and a["unwind"]["eh_frame_hdr"]:
        notes.append("throws C++ exceptions: its .eh_frame is registered with the game's unwinder before constructors run")
    if a["init"]["init_array"] or a["init"]["DT_INIT"]:
        notes.append(f"{a['init']['init_array']} constructor(s) in .init_array, run after relocation")
    if resolution["caveat"]:
        notes.append("glibc pthread objects on newlib: " + ", ".join(sorted(resolution["caveat"])[:6]) +
                     " (see docs/switch-h37-dynamic-loader.md)")
    return {
        "switch_package": {"loadable": not switch, "blockers": switch},
        "linux_compat": {"loadable_if_game_defines_imports": not linux, "blockers": linux,
                         "resolution": {k: sorted(v) for k, v in resolution.items()},
                         "game_must_define": len(resolution["game_must_define"])},
        "notes": notes,
    }


def libraries(path: pathlib.Path):
    """(label, bytes) for a .so, or for every lib/*/*.so in a .dusk package."""
    if path.suffix == ".dusk" or zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            for name in sorted(z.namelist()):
                if name.startswith("lib/") and name.endswith(".so"):
                    yield f"{path.name}:{name}", z.read(name)
    else:
        yield path.name, path.read_bytes()


# aleks2 (H37-c4): the platforms the Switch loader takes a native library from, best first --
# src/dusk/mods/loader/natives.cpp (k_nativePlatform, k_nativeCompatPlatform) and
# switch_native_loader.cpp check_package. Every other platform's library in a package is ignored.
LOADER_PLATFORMS = ("switch-arm64", "linux-aarch64")


def select_library(path: pathlib.Path):
    """(label, bytes) of the one library the loader takes from `path`.

    A loose .so is itself. In a package: lib/switch-arm64/mod.so if it has one, otherwise
    lib/linux-aarch64/mod.so, whatever other platforms it also ships (catalog packages carry
    android, ios, macos, linux and windows builds). Raises ElfError, naming what the package does
    have, when neither is there -- the loader's own refusal.
    """
    if not (path.suffix == ".dusk" or zipfile.is_zipfile(path)):
        return path.name, path.read_bytes()
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        found = {}
        for name in names:
            parts = name.split("/")
            if len(parts) == 3 and parts[0] == "lib" and parts[2] in ("mod.so", "mod.dll"):
                found.setdefault(parts[1], name)
        for platform in LOADER_PLATFORMS:
            name = found.get(platform)
            if name is not None and name.endswith(".so"):
                return f"{path.name}:{name}", z.read(name)
    if not found:
        raise ElfError(f"{path.name}: no native library (lib/<platform>/mod.so)")
    raise ElfError(f"{path.name}: its native code is built only for {', '.join(sorted(found))}; "
                   "it has no switch-arm64 or linux-aarch64 build")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", type=pathlib.Path, help="write the full report here")
    ap.add_argument("files", nargs="+", type=pathlib.Path)
    args = ap.parse_args(argv)
    report = []
    for path in args.files:
        found = False
        for label, data in libraries(path):
            found = True
            entry = {"file": label, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            try:
                entry.update(audit_elf(Elf(data, label)))
            except (ElfError, struct.error, KeyError, ValueError) as error:
                entry["error"] = str(error)
            report.append(entry)
        if not found:
            report.append({"file": path.name, "error": "no lib/<platform>/*.so in this package"})
    for e in report:
        if "error" in e:
            print(f"{e['file']}: ERROR {e['error']}")
            continue
        v = e["verdict"]
        lc = v["linux_compat"]
        print(f"{e['file']}: {e['machine']} {e['type']} {e['image_bytes']} bytes, relocs {e['relocations']}, "
              f"needed {e['needed']}")
        print(f"    as a Switch package: {'loadable' if v['switch_package']['loadable'] else 'no'}"
              + "".join(f"\n      - {b}" for b in v["switch_package"]["blockers"]))
        print(f"    in Linux compatibility mode: "
              f"{'loadable if the game defines ' + str(lc['game_must_define']) + ' import(s)' if lc['loadable_if_game_defines_imports'] else 'no'}"
              + "".join(f"\n      - {b}" for b in lc["blockers"]))
        r = lc["resolution"]
        print(f"      shims {len(r['shim'])}, NRO glibc-compat {len(r['nro_glibc_compat'])}, "
              f"weak-null {len(r['weak_null_if_absent'])}, caveats {len(r['caveat'])}")
        for n in v["notes"]:
            print(f"    note: {n}")
    if args.json:
        args.json.write_text(json.dumps(report, indent=1, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
