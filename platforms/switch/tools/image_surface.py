"""aleks2 (H37-c2): what a linked game image offers native mods, read from the ELF itself.

Shared by check_symbol_manifest.py (after every link), hook_target_audit.py and the H37
candidate check. Three questions, answered the way the loader answers them on the console:

  exports        the image's .dynsym: defined GLOBAL/WEAK names (ImageSymbols, image_symbols.cpp).
  image runtime  the table src/dusk/mods/loader/elf_image_runtime.c compiles into the image --
                 toolchain functions the image links but cannot export (hidden in libgcc,
                 libgcc_eh, libm). Read out of the ELF: symbol aleks2_image_runtime, three
                 pointers per entry, each relocated by an R_AARCH64_RELATIVE (or stored in place,
                 as RELR leaves it). Every entry is checked against the ELF's own .symtab: its
                 address must be where the function of that name starts.
  hook targets   an address from the symbol manifest must be the start of a real function with
                 room for funchook's two-instruction patch before the next function (a one-
                 instruction function is fine when alignment padding -- NOPs -- follows it), with
                 nothing branching into the patched pair, and reached by something -- a direct call or a branch (also
                 through a linker veneer), a function pointer in data, an address formed in code
                 (adrp + add), or the image's exports (mods call it). A hook on code nobody
                 reaches installs and intercepts nothing.
"""
import bisect
import collections
import re
import struct

SHT_SYMTAB = 2
SHT_RELA = 4
SHT_DYNSYM = 11
SHT_RELR = 19
SHF_EXECINSTR = 0x4
PF_X = 1
STT_FUNC = 2
R_AARCH64_ABS64 = 257
R_AARCH64_GLOB_DAT = 1025
R_AARCH64_RELATIVE = 1027
STT_OBJECT = 1
PT_LOAD = 1
CHAIN_DEPTH = 8
IMAGE_RUNTIME_SYMBOL = "aleks2_image_runtime"
IMAGE_RUNTIME_ENTRY = 24  # struct Aleks2ImageRuntimeEntry: name, address, library
# funchook_arm64.h: JUMP32_SIZE, the instructions overwritten at the entry.
HOOK_PATCH_BYTES = 8
NOP = 0xD503201F
# DM2-c: the PC-only stubs of platforms/switch/mod_compat_symbols.txt exist so that a hook on them
# installs and never fires; "nothing calls it" is what they are for, not a problem.
MOD_COMPAT_SYMBOLS = __import__("pathlib").Path(__file__).resolve().parents[1] / "mod_compat_symbols.txt"


def pc_only_symbols() -> frozenset:
    if not MOD_COMPAT_SYMBOLS.exists():
        return frozenset()
    return frozenset(line.split()[3] for line in MOD_COMPAT_SYMBOLS.read_text(encoding="utf-8").splitlines()
                     if line.startswith("pc-only ") and len(line.split()) == 5)


class SurfaceError(Exception):
    pass


def symbols(elf, kind=SHT_SYMTAB):
    """[(name, value, size, type, bind, shndx)] of the first section of `kind`."""
    out = []
    for s in elf.sections:
        if s["type"] != kind:
            continue
        strtab = elf.sections[s["link"]]
        base = strtab["offset"]
        data = elf.data
        for i in range(1, s["size"] // 24):
            name_off, info, _other, shndx, value, size = struct.unpack_from("<IBBHQQ", data, s["offset"] + i * 24)
            end = data.index(b"\0", base + name_off)
            out.append((data[base + name_off:end].decode("utf-8", "replace"), value, size, info & 0xF,
                        info >> 4, shndx))
        break
    return out


def exports(elf) -> dict:
    """{name: address} of the defined GLOBAL/WEAK entries of .dynsym, as ImageSymbols sees them."""
    return {name: value for name, value, _size, _type, bind, shndx in symbols(elf, SHT_DYNSYM)
            if shndx != 0 and bind in (1, 2)}


def _dynamic(elf) -> dict:
    """{tag: value} of PT_DYNAMIC: what the loader reads to find the relocations it applies."""
    out = {}
    dyn = next((p for p in elf.phdrs if p["type"] == "DYNAMIC"), None)
    if dyn is None:
        return out
    at, end = dyn["offset"], dyn["offset"] + dyn["filesz"]
    while at + 16 <= min(end, len(elf.data)):
        tag, val = struct.unpack_from("<qQ", elf.data, at)
        at += 16
        if tag == 0:
            break
        out[tag] = val
    return out


def _dynamic_symbols(elf) -> list:
    """[(value, shndx)] by index of .dynsym."""
    for s in elf.sections:
        if s["type"] == SHT_DYNSYM:
            return [struct.unpack_from("<HQ", elf.data, s["offset"] + i * 24 + 6)[::-1]
                    for i in range(s["size"] // 24)]
    return []


def _section_of(elf, vaddr: int) -> str:
    for s in elf.sections:
        if s["flags"] & 0x2 and s["addr"] <= vaddr < s["addr"] + max(s["size"], 1) and s["type"] != 8:
            return s.get("name", "?")
    return "?"


def pointer_relocations(elf) -> dict:
    """{place: {type, value, section}} of every relocation that stores a code or data address, as
    the loader applies them: DT_RELA (and the allocated SHT_RELA sections), DT_RELR (and SHT_RELR).
      R_AARCH64_RELATIVE   value = addend
      R_AARCH64_ABS64 / GLOB_DAT   value = symbol + addend, symbol defined in the image
      RELR                 value = what the file holds at the place (the addend stays in place)
    A relocation whose place is not inside a loaded segment, or whose symbol is undefined, is not a
    pointer into this image and is left out."""
    dyn = _dynamic(elf)
    rela = []  # (offset in file, byte size)
    relr = []
    if 7 in dyn and 8 in dyn:  # DT_RELA, DT_RELASZ
        try:
            rela.append((elf.vaddr_to_offset(dyn[7]), dyn[8]))
        except Exception:
            pass
    if 36 in dyn and 35 in dyn:  # DT_RELR, DT_RELRSZ
        try:
            relr.append((elf.vaddr_to_offset(dyn[36]), dyn[35]))
        except Exception:
            pass
    for s in elf.sections:
        if s["flags"] & 0x2 == 0:  # SHF_ALLOC: what crt0 applies
            continue
        if s["type"] == SHT_RELA:
            rela.append((s["offset"], s["size"]))
        elif s["type"] == SHT_RELR:
            relr.append((s["offset"], s["size"]))
    symbols_ = _dynamic_symbols(elf)
    loads = [(p["vaddr"], p["vaddr"] + p["memsz"]) for p in elf.phdrs if p["type"] == "LOAD"]

    def placed(place):
        return place % 8 == 0 and any(lo <= place and place + 8 <= hi for lo, hi in loads)

    out = {}

    def add(place, rtype, value):
        if placed(place) and place not in out:
            out[place] = {"type": rtype, "value": value & 0xFFFFFFFFFFFFFFFF, "section": _section_of(elf, place)}

    for at, size in rela:
        for off in range(at, at + size - 23, 24):
            place, info, addend = struct.unpack_from("<QQq", elf.data, off)
            rtype, sym = info & 0xFFFFFFFF, info >> 32
            if rtype == R_AARCH64_RELATIVE:
                add(place, "RELATIVE", addend)
            elif rtype in (R_AARCH64_ABS64, R_AARCH64_GLOB_DAT) and 0 < sym < len(symbols_):
                value, shndx = symbols_[sym]
                if shndx != 0:
                    add(place, "ABS64" if rtype == R_AARCH64_ABS64 else "GLOB_DAT", value + addend)
    for at, size in relr:
        place = 0
        for off in range(at, at + size - 7, 8):
            (entry,) = struct.unpack_from("<Q", elf.data, off)
            if entry & 1 == 0:
                place = entry
                if placed(place):
                    add(place, "RELR", pointer_at_file(elf, place))
                place += 8
            else:
                bits, k = entry >> 1, 0
                while bits:
                    if bits & 1 and placed(place + 8 * k):
                        add(place + 8 * k, "RELR", pointer_at_file(elf, place + 8 * k))
                    bits >>= 1
                    k += 1
                place += 63 * 8
    return out


def pointer_at_file(elf, vaddr: int) -> int:
    try:
        return struct.unpack_from("<Q", elf.data, elf.vaddr_to_offset(vaddr))[0]
    except Exception:
        return 0


def relative_relocations(elf) -> dict:
    """{place: value} of every address-storing relocation (see pointer_relocations); the name is
    kept for the RELATIVE case this began as."""
    return {place: r["value"] for place, r in pointer_relocations(elf).items()}


def pointer_at(elf, relocs: dict, vaddr: int) -> int:
    """The link-time value of the pointer at `vaddr`: its RELATIVE addend, else what the file holds
    (RELR keeps the addend in place, and so does a non-PIE link)."""
    if vaddr in relocs:
        return relocs[vaddr]
    return struct.unpack_from("<Q", elf.data, elf.vaddr_to_offset(vaddr))[0]


def c_string(elf, vaddr: int) -> str:
    at = elf.vaddr_to_offset(vaddr)
    end = elf.data.index(b"\0", at)
    return elf.data[at:end].decode("utf-8", "replace")


def executable_ranges(elf):
    return [(p["vaddr"], p["vaddr"] + p["memsz"]) for p in elf.phdrs
            if p["type"] == "LOAD" and p["flags"] & PF_X]


def functions_by_address(symtab) -> dict:
    """{address: [(name, size)]} of the FUNC symbols, any binding (locals included)."""
    out = collections.defaultdict(list)
    for name, value, size, stype, _bind, shndx in symtab:
        if stype == STT_FUNC and shndx != 0:
            out[value].append((name, size))
    return out


def image_runtime(elf, symtab, relocs) -> tuple:
    """(entries, problems): the image runtime table as the loader will read it, each entry
    {name, address, library}, and what is wrong with it. entries is None when the image has no
    table (a build without the native mod loader)."""
    table = [s for s in symtab if s[0] == IMAGE_RUNTIME_SYMBOL and s[5] != 0]
    if not table:
        return None, []
    _name, start, size, _type, _bind, _shndx = table[0]
    if size == 0 or size % IMAGE_RUNTIME_ENTRY:
        return [], [f"{IMAGE_RUNTIME_SYMBOL} has size {size}, not a whole number of entries"]
    by_name = collections.defaultdict(list)
    for name, value, fsize, stype, _bind, shndx in symtab:
        if stype == STT_FUNC and shndx != 0:
            by_name[name].append(value)
    exec_ranges = executable_ranges(elf)
    entries, problems = [], []
    for at in range(start, start + size, IMAGE_RUNTIME_ENTRY):
        name = c_string(elf, pointer_at(elf, relocs, at))
        address = pointer_at(elf, relocs, at + 8)
        library = c_string(elf, pointer_at(elf, relocs, at + 16))
        entry = {"name": name, "address": address, "library": library}
        if not any(lo <= address < hi for lo, hi in exec_ranges):
            problems.append(f"image runtime {name}: {address:#x} is not in executable code")
        elif address not in by_name.get(name, []):
            problems.append(f"image runtime {name}: {address:#x} is not where the image's own {name} starts")
        entries.append(entry)
    names = [e["name"] for e in entries]
    for name in {n for n in names if names.count(n) > 1}:
        problems.append(f"image runtime {name}: listed twice")
    return entries, problems


def _sign(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


# B and BL: 0x14000000/0x94000000 under mask 0x7C000000 -> top bytes 0x14-0x17, 0x94-0x97.
# ADRP: top bytes 0x90, 0xB0, 0xD0, 0xF0.
_TOP = re.compile(rb"[\x14-\x17\x94-\x97\x90\xb0\xd0\xf0\x10\x30\x50\x70]")


def code_references(elf, wanted: set, veneers: dict) -> tuple:
    """(calls, formed): per wanted address, the B/BL instructions reaching it (directly or through
    a veneer that jumps to it) and the adrp+add pairs that form it."""
    calls, formed = collections.Counter(), collections.Counter()
    via = {}  # veneer address -> wanted target
    for target, addresses in veneers.items():
        for a in addresses:
            via[a] = target
    pages = {t & ~0xFFF for t in wanted}
    data = elf.data
    for s in elf.sections:
        if not s["flags"] & SHF_EXECINSTR or s["type"] != 1:  # SHT_PROGBITS
            continue
        base, off, size = s["addr"], s["offset"], s["size"] & ~3
        tops = data[off + 3:off + size:4]
        for m in _TOP.finditer(tops):
            i = m.start()
            insn = int.from_bytes(data[off + 4 * i:off + 4 * i + 4], "little")
            pc = base + 4 * i
            if insn & 0x7C000000 == 0x14000000:
                target = pc + 4 * _sign(insn & 0x3FFFFFF, 26)
                if target in wanted:
                    calls[target] += 1
                elif target in via:
                    calls[via[target]] += 1
            elif insn & 0x9F000000 == 0x10000000:  # adr
                if _adr_target(insn, pc) in wanted:
                    formed[_adr_target(insn, pc)] += 1
            elif insn & 0x9F000000 == 0x90000000:
                imm = ((insn >> 29) & 3) | (((insn >> 5) & 0x7FFFF) << 2)
                page = (pc & ~0xFFF) + (_sign(imm, 21) << 12)
                if page not in pages:
                    continue
                rd = insn & 0x1F
                for k in range(1, 5):  # the add usually follows within a few instructions
                    at = off + 4 * (i + k)
                    if at + 4 > off + size:
                        break
                    nxt = int.from_bytes(data[at:at + 4], "little")
                    if nxt & 0xFFC00000 == 0x91000000 and (nxt >> 5) & 0x1F == rd:  # add xd, xn, #imm12
                        target = page + ((nxt >> 10) & 0xFFF)
                        if target in wanted:
                            formed[target] += 1
                        break
    return calls, formed


_ADRP = re.compile(rb"[\x90\xb0\xd0\xf0\x10\x30\x50\x70]")  # adrp, and adr (what a linker relaxes adrp+add to)


def _adr_target(insn: int, pc: int) -> int:
    return pc + _sign(((insn >> 29) & 3) | (((insn >> 5) & 0x7FFFF) << 2), 21)


def _page_offset(insn: int, rn: int):
    """The byte offset an instruction adds to the page held in register `rn`, or None: add (imm)
    and the unsigned-offset loads/stores, which GCC uses instead of an add when it can."""
    if (insn >> 5) & 0x1F != rn:
        return None
    if insn & 0xFF800000 == 0x91000000:                     # add xd, xn, #imm12 {, lsl #12}
        return ((insn >> 10) & 0xFFF) << (12 if insn & 0x400000 else 0)
    if insn & 0x3B000000 == 0x39000000:                     # ldr/str (unsigned offset), any size
        size = insn >> 30
        if insn & 0x04000000 and insn & 0x00800000 and size == 0:
            size = 4                                        # 128-bit SIMD
        return ((insn >> 10) & 0xFFF) << size
    if insn & 0x3FC00000 in (0x29400000, 0x29000000):       # ldp/stp (signed offset)
        scale = 2 + (insn >> 31)
        offset = _sign((insn >> 15) & 0x7F, 7) << scale
        return offset if offset >= 0 else None
    return None


def address_forms(elf, pages: set) -> collections.Counter:
    """{address: count} of the addresses code forms in `pages`: adrp, then within a few
    instructions an add or a load/store whose offset completes the address."""
    formed = collections.Counter()
    data = elf.data
    for s in elf.sections:
        if not s["flags"] & SHF_EXECINSTR or s["type"] != 1:
            continue
        base, off, size = s["addr"], s["offset"], s["size"] & ~3
        tops = data[off + 3:off + size:4]
        for m in _ADRP.finditer(tops):
            i = m.start()
            insn = int.from_bytes(data[off + 4 * i:off + 4 * i + 4], "little")
            pc = base + 4 * i
            if insn & 0x9F000000 == 0x10000000:  # adr: the address itself
                if _adr_target(insn, pc) & ~0xFFF in pages:
                    formed[_adr_target(insn, pc)] += 1
                continue
            if insn & 0x9F000000 != 0x90000000:
                continue
            imm = ((insn >> 29) & 3) | (((insn >> 5) & 0x7FFFF) << 2)
            page = (pc & ~0xFFF) + (_sign(imm, 21) << 12)
            if page not in pages:
                continue
            rd = insn & 0x1F
            for k in range(1, 7):
                at = off + 4 * (i + k)
                if at + 4 > off + size:
                    break
                nxt = int.from_bytes(data[at:at + 4], "little")
                delta = _page_offset(nxt, rd)
                if delta is not None:
                    formed[page + delta] += 1
                if nxt & 0x1F == rd and delta is None and nxt & 0x9F000000 != 0x90000000:
                    break  # rd overwritten by something else
    return formed


def _containers(elf, symtab, details: dict):
    """Sorted [(start, end, name)] of the data objects that can hold a table: sized OBJECT/NOTYPE
    symbols outside executable code; GOT slots count as objects of their own."""
    code = executable_ranges(elf)
    items = {}
    for name, value, size, stype, _bind, shndx in symtab:
        if stype in (0, STT_OBJECT) and size > 0 and shndx != 0 and not any(lo <= value < hi for lo, hi in code):
            items.setdefault(value, (value, value + size, name))
    for place, r in details.items():
        if r["section"].startswith(".got") and not any(a <= place < b for a, b, _ in items.values()):
            items[place] = (place, place + 8, f"{r['section']}[{place:#x}]")
    return sorted(items.values())


def live_pointers(elf, symtab, details: dict, wanted: set, exported_addresses=()) -> dict:
    """{target: {"live": [reference], "dead": [place]}} for the addresses in `wanted` that are stored
    in the image by a valid relocation (`details`, see pointer_relocations).

    A stored address is *live* only when the table holding it is itself used: it is exported, code
    forms its address (adrp + add/ldr), or an address stored in a live table points into it --
    method table -> actor profile -> profile list -> code, or a function-local FuncTable -> code.
    Cycles and tables nothing reaches are dead. Each live reference carries its chain, from the
    object the code or the export reaches down to the table that holds the pointer."""
    containers = _containers(elf, symtab, details)
    starts = [c[0] for c in containers]

    def container_of(place):
        i = bisect.bisect_right(starts, place) - 1
        return i if i >= 0 and place < containers[i][1] else None

    by_value = sorted((r["value"], place) for place, r in details.items())
    values = [v for v, _ in by_value]

    def pointers_into(lo, hi):
        return [by_value[k][1] for k in range(bisect.bisect_left(values, lo), bisect.bisect_left(values, hi))]

    # Upward from the targets: every container that can lead to one.
    holders = {t: [p for p in pointers_into(t, t + 1)] for t in wanted}
    closure, frontier, parents = set(), set(), collections.defaultdict(set)  # parents[c]: containers pointing into c
    for t, places in holders.items():
        for place in places:
            c = container_of(place)
            if c is not None:
                frontier.add(c)
    for _ in range(CHAIN_DEPTH):
        closure |= frontier
        nxt = set()
        for c in frontier:
            lo, hi = containers[c][0], containers[c][1]
            for place in pointers_into(lo, hi):
                d = container_of(place)
                if d is not None and d != c:
                    parents[c].add(d)
                    if d not in closure:
                        nxt.add(d)
        frontier = nxt
        if not frontier:
            break
    pages = set()
    for c in closure:
        lo, hi = containers[c][0], containers[c][1]
        pages.update(range(lo & ~0xFFF, hi, 0x1000))
    formed = address_forms(elf, pages)
    form_addresses = sorted(formed)
    exports_sorted = sorted(exported_addresses)

    def root_kind(c):
        lo, hi = containers[c][0], containers[c][1]
        j = bisect.bisect_left(exports_sorted, lo)
        if j < len(exports_sorted) and exports_sorted[j] < hi:
            return "exported"
        j = bisect.bisect_left(form_addresses, lo)
        if j < len(form_addresses) and form_addresses[j] < hi:
            return "code"
        return None

    # Forward from the roots: a container is live when a live container points into it.
    live = {}  # container -> (root kind, chain of container indices root..self)
    queue = collections.deque()
    for c in closure:
        kind = root_kind(c)
        if kind:
            live[c] = (kind, [c])
            queue.append(c)
    children = collections.defaultdict(set)
    for c, ds in parents.items():
        for d in ds:
            children[d].add(c)
    while queue:
        d = queue.popleft()
        kind, chain = live[d]
        for c in children[d]:
            if c not in live and len(chain) < CHAIN_DEPTH:
                live[c] = (kind, chain + [c])
                queue.append(c)

    out = {t: {"live": [], "dead": []} for t in wanted}
    for t, places in holders.items():
        for place in places:
            c = container_of(place)
            if c is None or c not in live:
                out[t]["dead"].append(place)
                continue
            kind, chain = live[c]
            r = details[place]
            out[t]["live"].append({"place": place, "section": r["section"], "relocation": r["type"],
                                   "value": r["value"], "table": containers[c][2],
                                   "chain": [containers[x][2] for x in chain], "reached_by": kind})
    return out


def word_at(elf, vaddr: int) -> int:
    return int.from_bytes(elf.data[elf.vaddr_to_offset(vaddr):elf.vaddr_to_offset(vaddr) + 4], "little")


def branches_into(elf, start: int, size: int, first: int, last: int) -> int:
    """How many branch instructions of the function [start, start+size) target (first, last) --
    funchook refuses a function that jumps back into the instructions it overwrites."""
    count = 0
    off = elf.vaddr_to_offset(start)
    for i in range(size // 4):
        insn = int.from_bytes(elf.data[off + 4 * i:off + 4 * i + 4], "little")
        pc = start + 4 * i
        if insn & 0x7C000000 == 0x14000000:                    # b, bl
            target = pc + 4 * _sign(insn & 0x3FFFFFF, 26)
        elif insn & 0xFF000010 == 0x54000000 or insn & 0x7E000000 == 0x34000000:  # b.cond, cbz/cbnz
            target = pc + 4 * _sign((insn >> 5) & 0x7FFFF, 19)
        elif insn & 0x7E000000 == 0x36000000:                  # tbz/tbnz
            target = pc + 4 * _sign((insn >> 5) & 0x3FFF, 14)
        else:
            continue
        count += first < target < last
    return count


def hook_targets(elf, symtab, relocs, addresses: dict, exported=frozenset()) -> dict:
    """{name: verdict} for {name: address} hook targets, verdict = {address, function, size,
    calls, data_pointers, formed_in_code, exported, problems}. `exported`: the addresses the
    image exports."""
    funcs = functions_by_address(symtab)
    exec_ranges = executable_ranges(elf)
    starts = sorted(funcs)
    wanted = set(addresses.values())
    veneers = collections.defaultdict(list)
    names_at = {a: {n for n, _ in funcs.get(a, [])} for a in wanted}
    for name, value, _size, _stype, _bind, shndx in symtab:
        if shndx != 0 and name.startswith("__") and name.endswith("_veneer"):
            inner = name[2:-len("_veneer")]
            for a, ns in names_at.items():
                if inner in ns:
                    veneers[a].append(value)
    calls, formed = code_references(elf, wanted, veneers)
    details = pointer_relocations(elf)
    stored = live_pointers(elf, symtab, details, wanted, set(exports(elf).values()))
    out = {}
    for name, address in addresses.items():
        problems = []
        candidates = [f for f in funcs.get(address, []) if f[1] > 0]
        function, size = (candidates[0] if candidates else (None, 0))
        segment = [hi for lo, hi in exec_ranges if lo <= address < hi]
        if not segment:
            problems.append("not in executable code")
        elif function is None:
            problems.append("no function starts there")
        else:
            following = bisect.bisect_right(starts, address)
            room = min(starts[following] if following < len(starts) else segment[0], segment[0]) - address
            padding = range(address + size, address + min(HOOK_PATCH_BYTES, room), 4)
            # DM2-e: a function shorter than the patch is hooked over the padding after it, and funchook
            # (cmake/PatchFunchookSlots.cmake) takes only zero words there, never NOPs: its own category.
            short = size < HOOK_PATCH_BYTES
            if room < HOOK_PATCH_BYTES:
                problems.append(("too short: " if short else "") + f"only {room} bytes before the next function: "
                                f"too little for the {HOOK_PATCH_BYTES}-byte patch")
            elif short and any(word_at(elf, a) != 0 for a in padding):
                problems.append(f"too short: a {size}-byte function, and the {HOOK_PATCH_BYTES}-byte patch would "
                                "reach bytes after it that are not zero padding")
            elif any(word_at(elf, a) not in (NOP, 0) for a in padding):
                problems.append("the patch would overwrite bytes after the function that are not padding")
            elif branches_into(elf, address, size, address, address + HOOK_PATCH_BYTES):
                problems.append("branches back into the patched instructions")
        live, dead = stored[address]["live"], stored[address]["dead"]
        reached = calls[address] + len(live) + formed[address] + (address in exported)
        pc_only = name in pc_only_symbols()
        if not problems and reached == 0 and not pc_only:
            problems.append("nothing calls it or takes its address: a hook there intercepts nothing"
                            + (f" ({len(dead)} stored address(es), none in a table anything uses)" if dead else ""))
        out[name] = {"address": address, "function": function, "size": size, "calls": calls[address],
                     "data_pointers": len(live), "dead_pointers": len(dead), "indirect_references": live[:4], "formed_in_code": formed[address],
                     "exported": address in exported, "pc_only": pc_only,
                     "short": bool(function is not None and size < HOOK_PATCH_BYTES), "problems": problems}
    return out
