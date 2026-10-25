#!/usr/bin/env python3
"""aleks2 (rc21): every C/C++ unit of the Switch build must be compiled with the flags mods' hooks rely on.

- -fno-ipa-ra (rc21). Mods hook game functions at run time. A hook is an ordinary AAPCS64 call that may clobber
  any caller-saved register, while GCC's IPA register allocation lets a caller keep values in caller-saved
  registers across a call to a function it has seen leave them alone. rc19/rc20 crashed that way (Twilit
  Essentials hooks dMenu_Collect2D_c::getItemTag; cursorMove kept x4 across the call).
- -fno-inline-functions-called-once (1.0.0). A function with a single caller stays out of line, so a mod update
  that hooks a new file-local function finds it without a new build (Dawnlight 5.3.0: d_a_e_sf.cpp#action).

- -fno-inline-small-functions (1.0.1), actor sources only (src/d/actor). The small-function inliner still folded
  short file-local functions into their caller (Dawnlight 5.3.1: d_a_e_wb.cpp#e_wb_f_wait); actors hold most of the
  "file#function" hooks.

Any function can become a hook target after a catalog update, so the whole build gets the first two and the actors
the third (CMakeLists.txt), and
this checks it. It is the same family as the hook-inline gate (test-hook-inline-elf.sh).

rc22: read from build.ninja, every C/C++ compile edge. compile_commands.json lists only part of the build (the
game's own units were missing from it), so a check on it alone passed without seeing the game.

usage: check_no_ipa_ra.py BUILD_DIR    exit 0 = every unit has every flag, 1 = some do not, 2 = cannot check
"""
import os
import re
import sys

EDGE = re.compile(r"^build (?P<out>.+?\.(?:o|obj)): (?P<rule>\S*(?:C|CXX)_COMPILER\S*) (?P<src>\S+)")
SOURCE = (".c", ".cc", ".cpp", ".cxx")
# the -fno-<name> flags a unit must end up with, and which units (a regex on the source path; None = every unit)
REQUIRED = (
    ("ipa-ra", None),
    ("inline-functions-called-once", None),
    # 1.0.1: the actors' file-local functions, which mods hook by "file#function", stay out of line
    ("inline-small-functions", re.compile(r"[/\\]src[/\\]d[/\\]actor[/\\][^/\\]+\.cpp$")),
    # 1.0.1: and with PGO the hot-call inliner (Dawnlight 5.3.3: daB_GG_c::setBaseMtx, daTitle_c::Draw)
    ("inline-functions", re.compile(r"[/\\]src[/\\]d[/\\]actor[/\\][^/\\]+\.cpp$")),
)


def turned_off(text, name):
    # the last -f<name>/-fno-<name> on the line wins, as in GCC
    state = None
    for m in re.finditer(r"(?<!\S)-f(no-)?" + re.escape(name) + r"(?!\S)", text):
        state = m.group(1) is not None
    return state is True


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    path = os.path.join(sys.argv[1], "build.ninja")
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError as e:
        print(f"hook flags: cannot read {path}: {e}")
        return 2
    units = {name: 0 for name, _ in REQUIRED}
    missing = {name: [] for name, _ in REQUIRED}
    i = 0
    while i < len(lines):
        m = EDGE.match(lines[i])
        i += 1
        if not m:
            continue
        src = m.group("src").replace("$:", ":")
        if not src.lower().endswith(SOURCE):
            continue  # a precompiled header and the like
        variables = []
        while i < len(lines) and lines[i].startswith("  "):
            variables.append(lines[i].strip())
            i += 1
        flags = " ".join(v.split("=", 1)[1] for v in variables if v.startswith(("FLAGS =", "DEFINES =")))
        for name, scope in REQUIRED:
            if scope is not None and not scope.search(src):
                continue
            units[name] += 1
            if not turned_off(flags, name):
                missing[name].append(src)
    if units[REQUIRED[0][0]] == 0:
        print(f"hook flags: no C/C++ compile edges in {path}")
        return 2
    bad = False
    for name, scope in REQUIRED:
        what = "C/C++ units" if scope is None else "actor units"
        if scope is not None and units[name] == 0:
            print(f"-fno-{name}: no actor units found in build.ninja")
            bad = True
            continue
        print(f"-fno-{name}: {units[name] - len(missing[name])}/{units[name]} {what} (build.ninja)")
        groups = {}
        for src in missing[name]:
            key = "/".join(src.replace("\\", "/").split("/")[:-1])
            groups.setdefault(key, []).append(src)
        for key in sorted(groups):
            print(f"  MISSING {len(groups[key])} in {key}/ (e.g. {groups[key][0].rsplit('/', 1)[-1]})")
        bad = bad or bool(missing[name])
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
