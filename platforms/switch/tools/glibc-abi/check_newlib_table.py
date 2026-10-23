"""DM2-d: the newlib side of src/dusk/mods/loader/glibc_abi_table.inc.

Each entry's native value is the macro of the C library the table is compiled against. This compiles the
table with devkitA64 (newlib/libnx, the headers the NRO is built with) and static_asserts every entry
against the value measured on newlib (research/dm2d/abi-newlib.json, the same number as the entry's
"// newlib:" comment). A failure names the entry.

Two passes:
 1. With _GNU_SOURCE, as the probe measured: every entry must match.
 2. As the NRO compiles glibc_abi.cpp (no _GNU_SOURCE): newlib hides some names behind __GNU_VISIBLE, which
    then read kNotDefined. Allowed only in the informational clock group (clock_from_glibc maps glibc clock
    ids explicitly); errno, open/fcntl flags and fcntl commands feed the translation maps.

  python check_newlib_table.py TABLE_INC NEWLIB_JSON OUT_DIR
"""
import json
import os
import re
import subprocess
import sys

NA = -999999
# The toolchain the NRO is built with (aleks2/tools/env.sh DEVKITPRO), overridable.
DEVKITPRO = os.environ.get("DEVKITPRO", "/opt/devkitpro")
CXX = os.path.join(DEVKITPRO, "devkitA64", "bin", "aarch64-none-elf-g++.exe")
LIBNX_INCLUDE = os.path.join(DEVKITPRO, "libnx", "include")


def main():
    inc, newlib_json, out = sys.argv[1:4]
    newlib = json.load(open(newlib_json, encoding="utf-8"))
    text = open(inc, encoding="utf-8").read()
    entries = re.findall(r'\{"(\w+)", "(\w+)", (-?\d+),\s*// newlib: (n/a|-?\d+)', text)
    problems = []
    lines = ["#include <dirent.h>", "#include <fcntl.h>", "#include <pthread.h>", "#include <wchar.h>",
             "#include <cerrno>", "#include <ctime>", "#include <cstdio>", "#include <unistd.h>",
             "#include <arpa/inet.h>", "#include <net/if.h>", "#include <netdb.h>", "#include <netinet/in.h>",
             "#include <netinet/tcp.h>", "#include <poll.h>", "#include <sys/ioctl.h>", "#include <sys/socket.h>",
             "struct Entry { const char* group; const char* name; long long glibc; long long native; };",
             f"constexpr long long kNotDefined = {NA};",
             "constexpr Entry kTable[] = {", f'#include "{os.path.abspath(inc)}"', "};",
             f"static_assert(sizeof kTable / sizeof kTable[0] == {len(entries)});"]
    for i, (group, name, _glibc, comment) in enumerate(entries):
        measured = newlib.get(name)
        expect = NA if comment == "n/a" else int(comment)
        if measured is None or measured != expect:
            problems.append(f"{group} {name}: comment says {comment}, abi-newlib.json says {measured}")
        lines.append(f'static_assert(kTable[{i}].native == {expect}LL, "{group} {name}: newlib measured {comment}");')
    os.makedirs(out, exist_ok=True)
    src = os.path.join(out, "check_newlib_table.cpp")
    open(src, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")

    print(f"{len(entries)} entries; comment vs abi-newlib.json: {len(problems)} disagree")
    for p in problems:
        print("  " + p)
    ok = not problems
    for label, defs in (("with _GNU_SOURCE", ["-D_GNU_SOURCE=1"]), ("as the NRO builds it", [])):
        r = subprocess.run([CXX, "-std=gnu++20", "-D__SWITCH__", *defs, "-I", LIBNX_INCLUDE, "-fsyntax-only", src],
                           capture_output=True, text=True)
        failed = re.findall(r"static assertion failed: (\w+) (\w+)", r.stderr)
        other = [l for l in r.stderr.splitlines() if "error" in l and "static assertion failed" not in l]
        bad = [f for f in failed if defs or f[0] != "clock"]
        print(f"devkitA64 {label}: rc={r.returncode}, {len(failed)} entr(y/ies) undefined or different, "
              f"{len(bad)} not allowed, {len(other)} other error(s)")
        for g, n in failed:
            print(f"  {g} {n}" + ("  <-- not allowed" if (g, n) in bad else "  (clock, informational)"))
        for l in other[:20]:
            print("  " + l)
        ok = ok and not bad and not other
    print("newlib table check: " + ("all entries match" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
