#!/usr/bin/env python3
"""1.1.3 (GitHub #14): daNpc_grO_c::CreateHeap must keep its `mdlIdx > 0` check.

CreateHeap reads l_loadObj_list[0][mType] for the four Goron types. Upstream declares the table int[2][2], so
types 2 and 3 read past row 0. That is UB, and GCC (devkitA64 16.1, -O2/-O3) assumes mType < 2, sees only 1 and 2
in row 0 and drops the check. The Goron in Kakariko's Malo Mart (TYPE_MARO) reads -1 and loads
l_bmdGetParamList[-1] (a BCK) as a model: Data Abort in mDoExt_J3DModel__create, entering the shop. Upstream issue
TwilitRealm/dusklight#2695. d_a_npc_gro.cpp declares it int[1][4].

This reads the machine code. In CreateHeap the second dComIfG_getObjectRes call is the extra model's (the pipe or
the donation box). Between the call before it and that call there has to be a conditional branch. With the check
dropped there are only loads.

usage: check_gro_loadobj.py FILE [FILE...]   (a linked ELF or a d_a_npc_gro.cpp object)
exit 0 = the check is there, 1 = it is missing, 2 = cannot check.  OBJDUMP overrides the objdump binary.
"""
import os
import re
import shutil
import subprocess
import sys

FUNC = "daNpc_grO_c::CreateHeap()"
MANGLED = "_ZN11daNpc_grO_c10CreateHeapEv"
CALL = re.compile(r"\bbl\s+\S+\s+<(?P<target>[^>]+)>")
RELOC_CALL = re.compile(r"R_AARCH64_(?:CALL|JUMP)26\s+(?P<target>\S+)")
COND = re.compile(r"\s(?:b\.\w+|cbz|cbnz|tbz|tbnz)\s")


def objdump():
    exe = os.environ.get("OBJDUMP")
    if exe:
        return exe
    for cand in ("aarch64-none-elf-objdump",
                 os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "devkitA64", "bin",
                              "aarch64-none-elf-objdump")):
        found = shutil.which(cand)
        if found:
            return found
    return None


def function_lines(exe, path):
    out = subprocess.run([exe, "--disassemble=" + MANGLED, "-r", "--no-show-raw-insn", path],
                         capture_output=True, text=True, errors="replace")
    if out.returncode != 0:
        raise RuntimeError(out.stderr.strip() or f"objdump exit {out.returncode}")
    lines, inside = [], False
    for line in out.stdout.splitlines():
        head = re.match(r"^[0-9a-f]+ <(?P<name>.+)>:$", line)
        if head:
            if inside:
                break
            inside = head.group("name") == MANGLED
            continue
        if inside:
            lines.append(line)
    return lines


def calls(lines):
    """(index, target) of every call; an object file names the target in the relocation after the bl."""
    found = []
    for i, line in enumerate(lines):
        if not re.search(r"\sbl\s", line):
            continue
        m = CALL.search(line)
        target = m.group("target") if m else ""
        if i + 1 < len(lines):
            r = RELOC_CALL.search(lines[i + 1])
            if r:
                target = r.group("target")
        found.append((i, target))
    return found


def check(path, exe):
    lines = function_lines(exe, path)
    if not lines:
        print(f"{path}: {FUNC} not found")
        return 2
    sites = calls(lines)
    res = [n for n, (_, t) in enumerate(sites) if "getObjectRes" in t]
    if len(res) < 2:
        print(f"{path}: {FUNC} has {len(res)} dComIfG_getObjectRes call(s), expected 2")
        return 2
    n = res[1]
    start = sites[n - 1][0] + 1
    end = sites[n][0]
    window = lines[start:end]
    if any(COND.search(" " + l) for l in window):
        print(f"{path}: OK, the extra model's resource load is behind a check")
        return 0
    print(f"{path}: FAIL, {FUNC} loads the extra model with no check on l_loadObj_list "
          f"({len(window)} instructions, no branch): TYPE_MARO crashes (GitHub #14)")
    return 1


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    exe = objdump()
    if not exe:
        print("check_gro_loadobj: no aarch64 objdump (set OBJDUMP)")
        return 2
    worst = 0
    for path in sys.argv[1:]:
        try:
            rc = check(path, exe)
        except (OSError, RuntimeError) as e:
            print(f"{path}: cannot disassemble: {e}")
            rc = 2
        worst = max(worst, rc)
    return worst


if __name__ == "__main__":
    sys.exit(main())
