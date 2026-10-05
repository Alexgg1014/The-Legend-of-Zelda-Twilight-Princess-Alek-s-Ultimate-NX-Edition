#!/usr/bin/env python3
"""aleks2 (H36): the checks and the one table that need every sealed mod at once.

seal_mod.py makes one mod's merged object safe to link beside other mods. Two
things are still left that only show up when several sealed mods meet in one
executable, and this script handles both, reading the sealed objects directly
(plain ELF64, no third-party module):

  * Shared template code bound to one mod. The SDK's header templates use the
    per-mod globals by name: HookImpl<Tag, ...>::dispatch_pre passes `mod_ctx`,
    mods::log::write<...> calls `svc_log`. Each sealed mod keeps its own copy of
    those globals (seal_mod.py localises them), but the template instantiations
    stay weak and the linker keeps ONE copy of each. When two mods instantiate the
    same one -- the same hook target, the same alias name, the same log call
    shape -- every mod gets the first mod's copy, and with it the first mod's
    context and services. Nothing fails; hooks and log lines are silently
    attributed to the wrong mod. That is refused here: a weak symbol defined by
    two or more sealed mods whose code refers to anything that belongs to one
    mod (a localised global or a renamed export) stops the build, naming both.

  * Names the host has to resolve. On desktop a symbol manifest, embedded after
    linking, maps names to addresses. The Switch build has none, so hooks declared
    by name (DEFINE_HOOK_SYMBOL) and hooks on virtual members (resolved through
    the class vtable symbol) cannot resolve and the mod's initialisation fails.
    Every mod is linked into the same executable, though, so the linker can fill
    those addresses in: this script lists the names the sealed mods' metadata
    asks for and writes a table of weak references to them, which
    dusk::mods::manifest::resolve falls back to on Switch. A weak reference that
    the link cannot satisfy is null and resolves as "not found" at run time,
    exactly as a missing manifest entry would; it never breaks the link.
    Qualified display names ("Class::method") are not link names, so they are
    mapped to their mangled names through a reviewed alias file.

Usage:
  static_mod_link.py --aliases FILE --output TABLE.cpp NAME=SEALED.o ...
  static_mod_link.py --list NAME=SEALED.o ...    (prints the requests, writes nothing)
"""
import argparse
import pathlib
import re
import struct
import sys

# ModMetaKind (sdk/include/mods/api.h)
META_PAD = 0
META_HOOK_FN = 4
META_HOOK_MEM = 5
META_HOOK_NAME = 6
META_HOOK_MEM_EXT = 7

EM_X86_64 = 62
EM_AARCH64 = 183

SHT_SYMTAB = 2
SHT_RELA = 4
SHT_REL = 9
SHT_GROUP = 17
SHN_UNDEF = 0
SHN_LORESERVE = 0xFF00
SHF_WRITE = 0x1
SHF_EXECINSTR = 0x4
SHF_MERGE = 0x10

STB_LOCAL = 0
STB_GLOBAL = 1
STB_WEAK = 2
STT_FUNC = 2
STT_SECTION = 3
STT_FILE = 4

EXPORTS = ("mod_meta", "mod_ctx", "mod_initialize", "mod_update", "mod_shutdown")

MANGLED = re.compile(r"^_Z[0-9A-Za-z_.$]+$")
C_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


class ElfError(Exception):
    pass


class Elf:
    """Just enough of an ELF64 little-endian relocatable object."""

    def __init__(self, path: pathlib.Path):
        try:
            self._parse(path)
        except (struct.error, IndexError, ValueError, UnicodeDecodeError) as error:
            raise ElfError(f"{path}: malformed ELF object ({error})") from None

    def _parse(self, path: pathlib.Path):
        self.path = path
        data = path.read_bytes()
        if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
            raise ElfError(f"{path}: not a little-endian ELF64 object")
        self.data = data
        self.machine = struct.unpack_from("<H", data, 0x12)[0]
        shoff = struct.unpack_from("<Q", data, 0x28)[0]
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
        self.sections = []
        for i in range(shnum):
            (name, kind, flags, addr, offset, size, link, info, align,
             entsize) = struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)
            self.sections.append({"name_off": name, "type": kind, "flags": flags,
                                  "offset": offset, "size": size, "link": link, "info": info,
                                  "entsize": entsize})
        names = self.sections[shstrndx]
        for section in self.sections:
            section["name"] = self._string(names, section["name_off"])
        self.symbols = []
        self.symtab_index = None
        for index, section in enumerate(self.sections):
            if section["type"] == SHT_SYMTAB:
                self.symtab_index = index
                strings = self.sections[section["link"]]
                for at in range(section["offset"], section["offset"] + section["size"], 24):
                    name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", data, at)
                    self.symbols.append({"name": self._string(strings, name), "bind": info >> 4,
                                         "type": info & 0xF, "shndx": shndx, "value": value,
                                         "size": size})
                break

    def _string(self, section, offset):
        start = section["offset"] + offset
        end = self.data.index(b"\0", start)
        return self.data[start:end].decode("utf-8", "replace")

    def section_bytes(self, index):
        section = self.sections[index]
        return self.data[section["offset"]:section["offset"] + section["size"]]

    def section_index(self, name):
        for index, section in enumerate(self.sections):
            if section["name"] == name:
                return index
        return None

    def relocations(self, target_index, with_addend=False):
        """(offset, symbol index[, addend]) of every relocation applied to one section."""
        out = []
        for section in self.sections:
            if section["type"] not in (SHT_RELA, SHT_REL) or section["info"] != target_index:
                continue
            step = 24 if section["type"] == SHT_RELA else 16
            for at in range(section["offset"], section["offset"] + section["size"], step):
                offset, info = struct.unpack_from("<QQ", self.data, at)
                addend = struct.unpack_from("<q", self.data, at + 16)[0] \
                    if section["type"] == SHT_RELA else 0
                out.append((offset, info >> 32, addend) if with_addend else (offset, info >> 32))
        return out

    def groups(self):
        """Section index -> the indices of every section in its COMDAT group."""
        member_of = {}
        for section in self.sections:
            if section["type"] != SHT_GROUP:
                continue
            words = struct.unpack_from(f"<{section['size'] // 4}I", self.data, section["offset"])
            members = list(words[1:])
            for member in members:
                member_of[member] = members
        return member_of


# --- metadata requests ---------------------------------------------------------

def _cstrings(blob: bytes, start: int, end: int):
    out = []
    at = start
    while at < end:
        stop = blob.find(b"\0", at, end)
        if stop < 0:
            stop = end
        out.append(blob[at:stop].decode("utf-8", "replace"))
        at = stop + 1
    return out


def metadata_requests(elf: Elf, mod: str):
    """What one mod's metadata asks the host to hook.

    Returns a list of dicts {"kind", "name", "display"}: "name" for a hook declared by
    name, "vtable" for a virtual member (name is the class vtable symbol), and "direct"
    for a hook whose address the linker already gives the mod (name is the target
    symbol, for the call-site check; nothing has to resolve it by name).
    """
    index = elf.section_index(f"modmeta_{mod}")
    if index is None:
        index = elf.section_index("modmeta")
    if index is None:
        return []
    blob = elf.section_bytes(index)
    relocations = elf.relocations(index)
    relocated = {offset for offset, _ in relocations}
    relocation_names = {offset: elf.symbols[symbol]["name"] for offset, symbol in relocations}
    requests = []
    at = 0
    while at + 8 <= len(blob):
        if struct.unpack_from("<Q", blob, at)[0] == 0:  # padding between objects
            at += 8
            continue
        size, kind = struct.unpack_from("<HB", blob, at)
        if size < 8 or size % 8 or at + size > len(blob):
            raise ElfError(f"{elf.path}: malformed metadata record at offset {at} in {mod}")
        if kind == META_HOOK_NAME:
            name = _cstrings(blob, at + 16, at + size)[0]
            requests.append({"kind": "name", "name": name, "display": name})
        elif kind == META_HOOK_FN:
            requests.append({"kind": "direct", "name": relocation_names.get(at + 8, ""),
                             "display": ""})
        elif kind == META_HOOK_MEM:
            words = struct.unpack_from("<QQ", blob, at + 8)
            if elf.machine == EM_AARCH64:
                # AAPCS64 C++ ABI: the virtual flag is bit 0 of the adjustment word.
                virtual = (words[1] & 1) != 0
                adjust = words[1] >> 1
            else:
                # Itanium: a virtual member pointer is the slot offset plus one, a literal;
                # a non-virtual one is a function address, a relocation in an object file.
                virtual = (at + 8) not in relocated and (words[0] & 1) != 0
                adjust = words[1]
            strings = _cstrings(blob, at + 32, at + size)
            vtable = strings[0] if strings else ""
            display = strings[1] if len(strings) > 1 else ""
            if virtual:
                requests.append({"kind": "vtable", "name": vtable, "display": display,
                                 "adjust": adjust})
            else:
                requests.append({"kind": "direct", "name": relocation_names.get(at + 8, ""),
                                 "display": display})
        elif kind == META_HOOK_MEM_EXT:
            raise ElfError(f"{elf.path}: {mod} has an extended member hook record, which only the "
                           "Microsoft ABI produces")
        at += size
    return requests


def load_alias_entries(path: pathlib.Path):
    """[(display name, mangled name, header)] from the alias file, in file order."""
    entries = []
    seen = {}
    for number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 3 or not MANGLED.match(parts[1]):
            raise ValueError(f"{path}:{number}: expected '<display name> <mangled name> <header>'")
        if parts[0] in seen and seen[parts[0]] != parts[1]:
            raise ValueError(f"{path}:{number}: '{parts[0]}' is mapped twice")
        seen[parts[0]] = parts[1]
        entries.append((parts[0], parts[1], parts[2]))
    return entries


def load_aliases(path: pathlib.Path):
    return {display: mangled for display, mangled, _ in load_alias_entries(path)}


def link_name(request, aliases):
    """The symbol the linker knows a request by, or None if there is none to give it."""
    name = request["name"]
    if not name:
        return None
    if name in aliases:
        return aliases[name]
    if MANGLED.match(name) or C_IDENTIFIER.match(name):
        return name
    return None


# --- shared template code ------------------------------------------------------

class ModState:
    """What, in one sealed mod, belongs to that mod alone -- and what code reaches it.

    A mod's own state is the renamed exports (<mod>__mod_ctx ...) and whatever lives in a
    writable section outside the COMDAT groups: the globals seal_mod.py localised (svc_log,
    the hook records) and the mod's statics. Code reaches it directly, or through the mod's
    local functions -- including the .isra/.part clones GCC makes of template code, which
    stay local to the mod and so do read that mod's svc_log -- so local functions are
    followed. Assemblers often point at all of this through the section symbol plus an
    offset rather than by name; both forms are handled. Read-only data (string literals,
    constant pools) and anything inside a group are the same in every copy and do not count.
    """

    def __init__(self, elf: Elf, mod: str):
        self.elf = elf
        self.mod = mod
        self.groups = elf.groups()
        self.renamed = {f"{mod}__{name}" for name in EXPORTS}
        self.functions = {}  # section index -> [(start, end, symbol)]
        for symbol in elf.symbols:
            if symbol["type"] == STT_FUNC and symbol["bind"] == STB_LOCAL and \
                    0 < symbol["shndx"] < SHN_LORESERVE:
                self.functions.setdefault(symbol["shndx"], []).append(
                    (symbol["value"], symbol["value"] + max(symbol["size"], 1), symbol))
        self.relocs = {}
        self.memo = {}

    def _relocations(self, shndx):
        if shndx not in self.relocs:
            self.relocs[shndx] = self.elf.relocations(shndx, with_addend=True)
        return self.relocs[shndx]

    def _function_at(self, shndx, offset):
        for start, end, symbol in self.functions.get(shndx, []):
            if start <= offset < end:
                return start, end, symbol
        return None

    def _data_name(self, shndx, offset):
        for symbol in self.elf.symbols:
            name = symbol["name"]
            # "$x"/"$d" are AArch64 mapping symbols (code/data markers), not names.
            if symbol["shndx"] == shndx and name and not name.startswith(("$", ".")) and \
                    symbol["type"] not in (STT_SECTION, STT_FILE, STT_FUNC) and \
                    symbol["value"] <= offset < symbol["value"] + max(symbol["size"], 1):
                return name
        return f"{self.elf.sections[shndx]['name']}+{offset:#x}"

    def reached(self, shndx, start=None, end=None, visiting=None):
        """Names of this mod's state reached from [start, end) of a section, transitively."""
        key = (shndx, start, end)
        if key in self.memo:
            return self.memo[key]
        visiting = set() if visiting is None else visiting
        if key in visiting:
            return set()
        visiting.add(key)
        found = set()
        for offset, target, addend in self._relocations(shndx):
            if start is not None and not start <= offset < end:
                continue
            symbol = self.elf.symbols[target]
            if symbol["name"] in self.renamed:
                found.add(symbol["name"])
                continue
            where = symbol["shndx"]
            if symbol["bind"] != STB_LOCAL or where == SHN_UNDEF or where >= SHN_LORESERVE or \
                    where in self.groups:
                continue
            flags = self.elf.sections[where]["flags"]
            position = addend if symbol["type"] == STT_SECTION else symbol["value"] + addend
            if flags & SHF_WRITE and not flags & SHF_MERGE:
                found.add(symbol["name"] if symbol["type"] != STT_SECTION and
                          not symbol["name"].startswith(("$", ".")) else
                          self._data_name(where, position))
            elif flags & SHF_EXECINSTR:
                function = self._function_at(where, position)
                if function is not None:
                    found |= self.reached(where, function[0], function[1], visiting)
        visiting.discard(key)
        self.memo[key] = found
        return found


def weak_definitions_using_mod_state(elf: Elf, mod: str):
    """Weak symbols this mod defines whose code reaches state that is this mod's own.

    Returns {weak symbol name: sorted names of the mod's own state it reaches}.
    """
    state = ModState(elf, mod)
    found = {}
    for symbol in elf.symbols:
        if symbol["bind"] != STB_WEAK or symbol["shndx"] == SHN_UNDEF or \
                symbol["shndx"] >= SHN_LORESERVE:
            continue
        uses = set()
        if symbol["shndx"] in state.groups:
            for section in state.groups[symbol["shndx"]]:
                uses |= state.reached(section)
        else:  # a weak definition outside any group: only its own bytes
            uses = state.reached(symbol["shndx"], symbol["value"],
                                 symbol["value"] + max(symbol["size"], 1))
        if uses:
            found[symbol["name"]] = sorted(uses)
    return found


def shared_template_conflicts(objects):
    """[(weak symbol, [(mod, uses)...])] for weak code two or more mods would share."""
    by_symbol = {}
    weak_everywhere = {}
    for mod, elf in objects:
        for symbol in elf.symbols:
            if symbol["bind"] == STB_WEAK and symbol["shndx"] != SHN_UNDEF and \
                    symbol["shndx"] < SHN_LORESERVE:
                weak_everywhere.setdefault(symbol["name"], set()).add(mod)
        for name, uses in weak_definitions_using_mod_state(elf, mod).items():
            by_symbol.setdefault(name, []).append((mod, uses))
    conflicts = []
    for name, users in sorted(by_symbol.items()):
        definers = weak_everywhere.get(name, set())
        if len(definers) > 1:
            conflicts.append((name, users, sorted(definers)))
    return conflicts


# --- output --------------------------------------------------------------------

def render_table(entries):
    """entries: [(lookup name, link name, is_code)] -> the C++ source."""
    lines = [
        "// Generated by platforms/switch/tools/static_mod_link.py. Do not edit.",
        "//",
        "// Names the mods compiled into this build ask the host to resolve by name",
        "// (hooks declared by name, and the vtables of hooked virtual members). The",
        "// Switch build has no symbol manifest; the linker fills these in instead.",
        "// Every reference is weak: a symbol this link does not have is null and",
        "// resolves as not found, it never fails the link.",
        '#include "dusk/switch_builtin_mods.hpp"',
        "",
    ]
    for index, (_, symbol, _) in enumerate(entries):
        lines.append(f'extern "C" const char aleks2_builtin_symbol_{index} __asm__("{symbol}") '
                     "__attribute__((weak));")
    lines += ["", "namespace dusk::mods::loader::detail {", "",
              "const BuiltinSymbol kBuiltinSymbols[] = {"]
    for index, (lookup, _, is_code) in enumerate(entries):
        lines.append(f'    {{"{lookup}", &aleks2_builtin_symbol_{index}, {"true" if is_code else "false"}}},')
    lines += ["    {nullptr, nullptr, false},", "};", "", "}  // namespace dusk::mods::loader::detail",
              ""]
    return "\n".join(lines)


def parse_objects(pairs):
    objects = []
    for pair in pairs:
        if "=" not in pair:
            raise ValueError(f"expected NAME=OBJECT, got '{pair}'")
        mod, path = pair.split("=", 1)
        objects.append((mod, Elf(pathlib.Path(path))))
    return objects


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--aliases", type=pathlib.Path)
    ap.add_argument("--output", type=pathlib.Path)
    ap.add_argument("--targets", type=pathlib.Path,
                    help="also write every hook target, for hook_call_sites.py after linking")
    ap.add_argument("--list", action="store_true", help="print the requests and stop")
    ap.add_argument("objects", nargs="*", help="NAME=SEALED.o, one per mod")
    args = ap.parse_args(argv)

    try:
        objects = parse_objects(args.objects)
        aliases = load_aliases(args.aliases) if args.aliases else {}
    except (ValueError, ElfError, OSError) as error:
        print(f"static_mod_link: {error}", file=sys.stderr)
        return 1

    requests = []
    try:
        for mod, elf in objects:
            for request in metadata_requests(elf, mod):
                requests.append((mod, request))
    except (ElfError, struct.error, IndexError) as error:
        print(f"static_mod_link: {error}", file=sys.stderr)
        return 1

    if args.list:
        for mod, request in requests:
            print(f"{mod}\t{request['kind']}\t{request['name']}\t{request['display']}\t"
                  f"{link_name(request, aliases) or '-'}")
        return 0

    try:
        conflicts = shared_template_conflicts(objects)
    except (struct.error, IndexError) as error:
        print(f"static_mod_link: malformed ELF object ({error})", file=sys.stderr)
        return 1
    if conflicts:
        print("static_mod_link: these mods cannot be compiled into one build together.",
              file=sys.stderr)
        print("Each name below is template code from the SDK headers that more than one mod "
              "instantiates and that uses one mod's own context or services. The linker keeps one "
              "copy for all of them, so the other mods would run with the wrong context.",
              file=sys.stderr)
        for name, users, definers in conflicts:
            detail = "; ".join(f"{mod} uses {', '.join(uses)}" for mod, uses in users)
            print(f"  {name}: defined by {', '.join(definers)} ({detail})", file=sys.stderr)
        return 1

    entries = []
    seen = set()
    unresolved = []
    for mod, request in requests:
        if request["kind"] == "direct":
            continue
        symbol = link_name(request, aliases)
        lookup = request["name"]
        if symbol is None:
            unresolved.append((mod, request))
            continue
        if lookup in seen:
            continue
        seen.add(lookup)
        entries.append((lookup, symbol, request["kind"] == "name"))
    for mod, request in unresolved:
        what = request["display"] or request["name"] or "<unnamed>"
        print(f"static_mod_link: warning: {mod}: no link name for '{what}' "
              f"({request['kind']}); add it to the alias file or this hook cannot install on "
              "Switch", file=sys.stderr)

    if args.output is None:
        print("static_mod_link: --output is required", file=sys.stderr)
        return 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(render_table(entries))
    if args.targets is not None:
        rows = ["# mod\tkind\tlink symbol\tdisplay (static_mod_link.py; read by hook_call_sites.py)"]
        for mod, request in requests:
            symbol = request["name"] if request["kind"] != "name" else link_name(request, aliases)
            rows.append(f"{mod}\t{request['kind']}\t{symbol or '-'}\t{request['display'] or '-'}")
        args.targets.parent.mkdir(parents=True, exist_ok=True)
        args.targets.write_text("\n".join(rows) + "\n")
    print(f"static_mod_link: {len(objects)} mod(s), no shared template code, "
          f"{len(entries)} name(s) for by-name resolution, {len(unresolved)} without a link name")
    return 0


if __name__ == "__main__":
    sys.exit(main())
