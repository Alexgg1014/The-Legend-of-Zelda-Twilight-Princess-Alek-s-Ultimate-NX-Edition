#!/usr/bin/env python3
"""aleks2 (H36): does each hook of the mods compiled into an NRO have anything to catch?

A hook patches the entry of one function. On Switch the game is optimised, and a
function whose every caller was inlined, or redirected to a `.part.0`/`.isra.0`/
`.constprop.0` clone, has an entry nobody jumps to: the hook installs and catches
nothing, with no error anywhere (CMakeLists.txt tells how Roll and Sprint found that
out). This reads the linked ELF -- not the NRO, which has no symbols -- and the hook
target list static_mod_link.py writes next to the build (switch-mods/hook_targets.txt),
and prints for every target whether its symbol is in the executable, how many direct
calls (bl/b) reach it, and which clones exist with how many calls of their own.

Hooks on virtual members are reached through the vtable, which no call count shows;
for those it only checks the vtable symbol is there.

  hook_call_sites.py --objdump aarch64-none-elf-objdump --nm aarch64-none-elf-nm \\
      build/switch-devkitA64-release-h36/switch-mods/hook_targets.txt \\
      build/switch-devkitA64-release-h36/dusklight.elf

Exit status 1 when some target is missing or has no direct caller, so a build can be
held back on it. Run it after building; it reads, never writes.
"""
import argparse
import pathlib
import re
import subprocess
import sys

CALL = re.compile(r"^\s*([0-9a-f]+):\s+(bl|b|call|jmp)\s+([0-9a-f]+)\b")  # aarch64; x86 for the host test
CLONE = re.compile(r"^(?P<base>.+?)\.(part|isra|constprop|cold|lto_priv)\.\d+")


def read_targets(path: pathlib.Path):
    rows = []
    for line in path.read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        mod, kind, symbol, display = (line.split("\t") + ["-"] * 4)[:4]
        rows.append({"mod": mod, "kind": kind, "symbol": symbol, "display": display})
    return rows


def symbol_addresses(nm: str, elf: pathlib.Path):
    out = subprocess.run([nm, "--defined-only", str(elf)], capture_output=True, text=True,
                         check=True).stdout
    by_name = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            by_name.setdefault(parts[2], int(parts[0], 16))
    return by_name


def call_counts(objdump: str, elf: pathlib.Path):
    counts = {}
    with subprocess.Popen([objdump, "-d", "--no-show-raw-insn", str(elf)],
                          stdout=subprocess.PIPE, text=True) as proc:
        for line in proc.stdout:
            match = CALL.match(line)
            if match:
                target = int(match.group(3), 16)
                counts[target] = counts.get(target, 0) + 1
    return counts


def report(targets, addresses, counts):
    clones = {}
    for name, address in addresses.items():
        match = CLONE.match(name)
        if match:
            clones.setdefault(match.group("base"), []).append((name, address))

    problems = 0
    lines = []
    for target in targets:
        symbol = target["symbol"]
        label = target["display"] if target["display"] != "-" else symbol
        if target["kind"] == "vtable":
            state = "ok (virtual, via vtable)" if symbol in addresses else "MISSING vtable"
            problems += symbol not in addresses
            lines.append(f"{target['mod']:<18} {label:<44} {state}")
            continue
        if symbol == "-" or symbol not in addresses:
            problems += 1
            lines.append(f"{target['mod']:<18} {label:<44} MISSING (not in this executable)")
            continue
        direct = counts.get(addresses[symbol], 0)
        clone_text = ", ".join(f"{name} {counts.get(address, 0)} call(s)"
                               for name, address in sorted(clones.get(symbol, [])))
        state = f"{direct} direct call(s)"
        if direct == 0:
            problems += 1
            state = "NO DIRECT CALLER -- the hook would catch nothing"
        if clone_text:
            state += f"; clones: {clone_text}"
        lines.append(f"{target['mod']:<18} {label:<44} {state}")
    return problems, lines


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--objdump", default="aarch64-none-elf-objdump")
    ap.add_argument("--nm", default="aarch64-none-elf-nm")
    ap.add_argument("targets", type=pathlib.Path)
    ap.add_argument("elf", type=pathlib.Path)
    args = ap.parse_args(argv)

    targets = read_targets(args.targets)
    addresses = symbol_addresses(args.nm, args.elf)
    counts = call_counts(args.objdump, args.elf)
    problems, lines = report(targets, addresses, counts)
    print("\n".join(lines))
    print(f"{len(targets)} hook target(s), {problems} with nothing to catch or missing")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
