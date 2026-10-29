#!/usr/bin/env python3
"""V1-rc4: the game's data layout against upstream Dusklight's, the one published mods are compiled against.

Mods read the game's globals through upstream's headers: an inline accessor such as dComIfGp_getPlayer is a load at
an offset baked into the mod. A member that exists upstream and not here (or the reverse) moves everything after it,
and the mod reads the wrong field without any error until it dereferences one. Found on the console with V1-rc3:
upstream had added PARTIAL_DEBUG storage to dVibration_c and mDoHIO_entry_c, and Crests of Courage 1.10.0 walked
g_dComIfG_gameInfo's 2D draw list 0x20 bytes off, past the end of the image.

What it measures, with the build's own compiler and flags (read from build.ninja):
  - offsetof every annotated member ("/* 0x.. */ Type mName;") of dComIfG_play_c and dComIfG_inf_c;
  - sizeof the type of every global the game headers declare (extern [DUSK_GAME_DATA] Type g_name;);
  - V1-rc8, --wide DIRS: sizeof and the annotated members of every class and struct defined in the headers of those
    include/ subdirectories (default d,m_Do,f_op,f_pc,f_ap). Two mods crashed in the map classes (dMeterMap_c from
    Twilight Compass, dMenu_Fmap_c from Twilit Essentials), which a global's type does not reach: dMeter2_c, the
    menus and the actors are only ever reached through pointers.
  - 1.0.3, --player HEADER:CLASS,...: offsetof the annotated members of the player classes (default daPy_py_c and
    daAlink_c), taken from BOTH headers, and an upstream member this tree does not have is an error, not a note.
    Mods use Link the most, through upstream's inline accessors (doTrigger() reads mItemTrigger at 0x2F8D); the port
    had renamed those bytes and kept the masks elsewhere, so every such read saw 0 -- and --wide never looked at the
    actor headers. sizeof is not compared: members appended at the end are allowed.
It compiles that probe twice -- this tree's include/ first, then an upstream checkout's include/ first -- and
prints every difference. Exit 1 when there is one.

  abi_parity.py --build BUILD_DIR --upstream UPSTREAM_CHECKOUT --cxx aarch64-none-elf-g++ --objdump ...objdump
                [--tu src/d/d_com_inf_game.cpp] [--work DIR]
"""
import argparse
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[3]
CLASSES = ("dComIfG_play_c", "dComIfG_inf_c")


MEMBER = re.compile(r"\s*/\*\s*0x[0-9A-Fa-f]+\s*\*/\s*(?:[\w:<>,\s\*&]+?)\s+\**(\w+)\s*(\[[^\]]*\])*\s*;")


def class_members(lines, cls):
    """offsetof facts for the annotated members ("/* 0x.. */ Type mName;") of class or struct `cls`."""
    starts = [i for i, l in enumerate(lines) if re.match(r"\s*(class|struct)\s+" + re.escape(cls) + r"\b[^;]*$", l)]
    out = []
    for start in starts[:1]:
        end = next((k for k in range(start, len(lines)) if re.match(r"};", lines[k])), len(lines))
        for l in lines[start:end]:
            m = MEMBER.match(l)
            if m:
                out.append(("offsetof", cls, m.group(1)))
    return out


def members(header: pathlib.Path):
    lines = header.read_text(encoding="utf-8", errors="replace").splitlines()
    return [f for cls in CLASSES for f in class_members(lines, cls)]


HEADERS = []  # headers that declare the globals, included by the probe


def globals_(include: pathlib.Path):
    """sizeof each global's type, and offsetof the annotated members of that type when its header defines it."""
    seen = {}
    texts = {}
    for h in sorted(include.rglob("*.h")):
        if "aleks2" in h.parts:
            continue
        text = h.read_text(encoding="utf-8", errors="replace")
        texts[h] = text
        found = False
        for m in re.finditer(r"^\s*(?:extern|DUSK_GAME_EXTERN)\s+(?:DUSK_GAME_DATA\s+)?([A-Za-z_][\w:]*)\s+(g_\w+)\s*;",
                             text, re.M):
            seen.setdefault(m.group(2), (m.group(1), h))
            found = True
        if found:
            HEADERS.append(h.relative_to(include).as_posix())
    facts = [("sizeof", t, g) for g, (t, _) in sorted(seen.items())]
    done = set(CLASSES)
    for g, (t, h) in sorted(seen.items()):
        if t in done or "::" in t:
            continue
        done.add(t)
        for hh, text in texts.items():
            if re.search(r"^\s*(class|struct)\s+" + re.escape(t) + r"\b[^;]*$", text, re.M):
                facts += class_members(text.splitlines(), t)
                break
    return facts


def wide_classes(include: pathlib.Path, dirs):
    """sizeof and annotated members of every class/struct defined (not just declared) in include/<dir>/**.h."""
    facts = []
    seen = set()
    for d in dirs:
        for h in sorted((include / d).glob("*.h")):  # top level: actor headers redefine each other
            text = h.read_text(encoding="utf-8", errors="replace")
            lines = text.splitlines()
            rel = h.relative_to(include).as_posix()
            names = []
            for m in re.finditer(r"^(?:class|struct)\s+(?:DUSK_\w+\s+)?([A-Za-z_]\w*)\b[^;()]*$", text, re.M):
                name = m.group(1)
                if name in seen or name in CLASSES:
                    continue
                seen.add(name)
                names.append(name)
            if not names:
                continue
            if rel not in HEADERS:
                HEADERS.append(rel)
            for name in names:
                WIDE_HEADER[name] = rel
                facts.append(("sizeof", name, name))
                facts += class_members(lines, name)
    return facts


WIDE_HEADER = {}  # --wide class -> its header (a header that does not compile here takes its classes with it)


def error_include_lines(stderr: str, probe_name: str):
    """Probe lines (#include) through which an error was reached, from each diagnostic's "included from" chain."""
    out, chain = set(), []
    pat = re.compile(r"(?:In file included from|from)\s+\S*" + re.escape(probe_name) + r":(\d+)")
    for line in stderr.splitlines():
        m = pat.search(line)
        if m:
            chain.append(int(m.group(1)))
            continue
        if re.search(r":\d+:\d+: (?:fatal )?error", line):
            out.update(chain)
            chain = []
        elif re.search(r":\d+:\d+: (?:warning|note)", line):
            chain = []
    return out


def write_probe(path: pathlib.Path, facts, headers=None):
    with path.open("w") as f:
        # newlib's <ctype.h> defines _C, a member name upstream's d_event_lib.h uses (this tree renames it).
        f.write('#undef _C\n#define private public\n#define protected public\n#include "d/d_com_inf_game.h"\n')
        for h in (HEADERS if headers is None else headers):
            f.write(f'#include "{h}"\n')
        f.write('extern "C" const unsigned long long aleks2_layout_probe[] = {\n')
        for kind, a, b in facts:
            f.write(f"  __builtin_offsetof({a}, {b}),\n" if kind == "offsetof" else f"  sizeof({a}),  // {b}\n")
        f.write("};\n")


def player_classes(ours: pathlib.Path, upstream: pathlib.Path, spec: str):
    """offsetof facts for --player: the annotated members in this tree's header and in upstream's."""
    facts = []
    for item in filter(None, spec.split(",")):
        rel, cls = item.split(":")
        for side in (ours, upstream):
            lines = (side / rel).read_text(encoding="utf-8", errors="replace").splitlines()
            facts += [f for f in class_members(lines, cls) if f not in facts]
        if rel not in HEADERS:
            HEADERS.append(rel)
    return facts


def ninja_flags(build: pathlib.Path, tu: str):
    lines = (build / "build.ninja").read_text(encoding="utf-8", errors="replace").splitlines()
    i = next(k for k, l in enumerate(lines) if l.startswith("build ") and tu.replace("/", "/") in l and l.split(":")[0].endswith(".obj"))
    vals = {}
    for l in lines[i + 1:i + 10]:
        m = re.match(r"\s+(DEFINES|FLAGS|INCLUDES) = (.*)", l)
        if m:
            vals[m.group(1)] = m.group(2)
    flags = re.sub(r"-include \S*cmake_pch\.hxx", "", vals["FLAGS"]).replace("-g1", "")
    return vals["DEFINES"].split() + flags.split() + vals["INCLUDES"].split()


def compile_probe(cxx, objdump, args, include_first, probe, obj, facts):
    """Compiles, dropping facts the compiler rejects (a member or global one side does not have)."""
    facts = list(facts)
    dropped = []
    headers = list(HEADERS)
    fixed = set(HEADERS) - set(WIDE_HEADER.values())  # the globals' headers: these must compile
    for _ in range(400):
        write_probe(probe, facts, headers)
        r = subprocess.run([cxx, f"-I{include_first}", *args, "-include", str(REPO / "include/dusk_pch.hpp"),
                            "-fno-diagnostics-color", "-w", "-pipe", "-c", str(probe), "-o", str(obj)], capture_output=True, text=True)
        if r.returncode == 0:
            break
        first = 5 + len(headers)  # the probe's lines before the facts (write_probe): 4, the headers, the array
        # V1-rc8 (--wide): a header that does not compile in this configuration (Wii-only, or one that clashes
        # with another) is dropped with its classes: found by the probe line the error came through, or by the
        # error being in the header itself.
        hdr = {headers[n - 5] for n in error_include_lines(r.stderr, probe.name) if 5 <= n < first}
        err_files = {p.replace("\\", "/") for p in re.findall(r"^(.*?):\d+:\d+: (?:fatal )?error", r.stderr, re.M)}
        hdr |= {h for h in headers if h not in fixed and any(p.endswith("/include/" + h) for p in err_files)}
        if hdr:
            if hdr & fixed:
                sys.exit(f"probe does not compile ({sorted(hdr & fixed)}):\n{r.stderr[:3000]}")
            headers = [h for h in headers if h not in hdr]
            gone = [f for f in facts if WIDE_HEADER.get(f[1]) in hdr]
            dropped += gone
            facts = [f for f in facts if WIDE_HEADER.get(f[1]) not in hdr]
            continue
        # a fact on a line the compiler rejects (incomplete type, template, a name both sides lack) is dropped by
        # its line in the probe.
        lines_bad = {int(n) for n in re.findall(re.escape(probe.name) + r":(\d+):\d+: (?:fatal )?error", r.stderr)}
        bad = set(re.findall(r"has no member named '(\w+)'", r.stderr)) | set(
            re.findall(r"'(\w+)' was not declared in this scope", r.stderr)) | set(
            re.findall(r"'(\w+)' does not name a type", r.stderr))
        bad |= {m for m in re.findall(r"error: '?(\w+)'? is not a (?:member|type)", r.stderr)}
        by_line = [facts[n - first - 1] for n in lines_bad if 0 <= n - first - 1 < len(facts)]
        if by_line:
            dropped += by_line
            facts = [f for f in facts if f not in set(by_line)]
            continue
        if not bad:
            sys.exit(f"probe does not compile:\n{r.stderr[:3000]}")
        dropped += [f for f in facts if f[2] in bad or f[1] in bad]
        facts = [f for f in facts if f[2] not in bad and f[1] not in bad]
    dump = subprocess.run([objdump, "-s", "-j", ".rodata.aleks2_layout_probe", str(obj)], capture_output=True,
                          text=True).stdout
    data = b"".join(bytes.fromhex("".join(m.group(1).split()))
                    for m in re.finditer(r"^\s*[0-9a-f]+ ((?:[0-9a-f]{2,8} ?){1,4})", dump, re.M))
    vals = [int.from_bytes(data[i:i + 8], "little") for i in range(0, len(facts) * 8, 8)]
    return {f: v for f, v in zip(facts, vals)}, dropped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", type=pathlib.Path, required=True)
    ap.add_argument("--upstream", type=pathlib.Path, required=True)
    ap.add_argument("--cxx", required=True)
    ap.add_argument("--objdump", required=True)
    ap.add_argument("--tu", default="src/d/d_com_inf_game.cpp")
    ap.add_argument("--work", type=pathlib.Path, default=pathlib.Path("abi-parity-work"))
    ap.add_argument("--ours-include", type=pathlib.Path, default=REPO / "include",
                    help="the include/ to measure as this side (default: this tree's)")
    ap.add_argument("--wide", default="d,m_Do,f_op,f_pc,f_ap",
                    help="include/ subdirectories whose classes are all measured ('' for none)")
    ap.add_argument("--player", default="d/actor/d_a_player.h:daPy_py_c,d/actor/d_a_alink.h:daAlink_c",
                    help="HEADER:CLASS,... measured member by member from both sides ('' for none)")
    a = ap.parse_args()
    a.work.mkdir(parents=True, exist_ok=True)
    facts = members(a.ours_include / "d/d_com_inf_game.h") + globals_(a.ours_include)
    if a.wide:
        known = set(facts)
        facts += [f for f in wide_classes(a.ours_include, a.wide.split(",")) if f not in known]
    player_facts = player_classes(a.ours_include, a.upstream / "include", a.player)
    player_set = set(player_facts)
    facts += [f for f in player_facts if f not in set(facts)]
    args = ninja_flags(a.build, a.tu)
    ours, dropped_ours = compile_probe(a.cxx, a.objdump, args, a.ours_include, a.work / "ours.cpp",
                                       a.work / "ours.o", facts)
    theirs, dropped_up = compile_probe(a.cxx, a.objdump, args, a.upstream / "include", a.work / "upstream.cpp",
                                       a.work / "upstream.o", facts)
    diffs = []
    for f in facts:
        if f in ours and f in theirs and ours[f] != theirs[f]:
            diffs.append(f"{f[0]}({f[1]}, {f[2]}): here {ours[f]:#x}, upstream {theirs[f]:#x}")
    only = sorted({f[2] for f in dropped_ours} ^ {f[2] for f in dropped_up})
    # A player-class member upstream has and this tree lacks: a mod reads or writes bytes nothing here uses.
    missing = sorted(f"{f[1]}::{f[2]}" for f in dropped_ours if f in player_set and f in theirs)
    for m in missing:
        diffs.append(f"{m}: upstream's member is missing here")
    print(f"{len(facts)} facts, {len(set(ours) & set(theirs))} measured on both sides")
    for d in diffs:
        print("DIFF", d)
    for n in only:
        print("ONLY ONE SIDE", n)
    if diffs:
        print(f"abi_parity: {len(diffs)} difference(s) with upstream")
        return 1
    print("abi_parity: layouts match upstream")
    return 0


if __name__ == "__main__":
    sys.exit(main())
