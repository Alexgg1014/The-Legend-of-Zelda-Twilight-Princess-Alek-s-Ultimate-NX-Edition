#!/usr/bin/env python3
"""aleks2 (V1-rc12): symbolize a main-thread profile written by src/dusk/sampler.cpp (perf-lab profile_main = 1).

    profile_report.py PROFILE (--elf dusklight.elf [--nm aarch64-none-elf-nm] | --symbols nm.txt) [--top 40]

The profile's header gives the NRO base; every address is made relative to it and looked up in the ELF's symbol table
(nm -C -n --defined-only: the ELF is linked at 0, like the NRO's first byte). Prints:
  * self: the function the PC was in (where the time is spent);
  * inclusive: every function on the sampled chain (PC, LR, two frame records), once per sample;
  * stacks: function(PC) <- caller <- caller's caller.
Addresses outside the NRO (mods' code, the kernel) are counted as [outside NRO].
"""
import argparse
import bisect
import collections
import re
import subprocess
import sys

OUTSIDE = "[outside NRO]"


def read_symbols(lines):
    """nm -C -n output -> sorted (addresses, names) of code symbols."""
    addrs, names = [], []
    for line in lines:
        parts = line.rstrip("\n").split(" ", 2)
        if len(parts) < 3 or parts[1] not in ("T", "t", "W", "w"):
            continue
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        addrs.append(addr)
        names.append(parts[2])
    order = sorted(range(len(addrs)), key=lambda i: addrs[i])
    return [addrs[i] for i in order], [names[i] for i in order]


def read_profile(lines):
    base, samples = None, []
    for line in lines:
        if line.startswith("#"):
            m = re.search(r"NRO base 0x([0-9a-fA-F]+)", line)
            if m:
                base = int(m.group(1), 16)
            continue
        parts = line.split()
        if len(parts) != 4:
            continue
        try:
            samples.append(tuple(int(p, 16) for p in parts))
        except ValueError:
            continue
    if base is None:
        raise SystemExit("no 'NRO base' in the profile header")
    return base, samples


class Symbolizer:
    def __init__(self, addrs, names, base, size_limit=0x10000000):
        self.addrs, self.names, self.base, self.limit = addrs, names, base, size_limit

    def __call__(self, absolute):
        if absolute == 0:
            return None
        off = absolute - self.base
        if off < 0 or off >= self.limit or not self.addrs:
            return OUTSIDE
        i = bisect.bisect_right(self.addrs, off) - 1
        return self.names[i] if i >= 0 else OUTSIDE


def report(samples, sym, top=40, out=sys.stdout):
    self_count = collections.Counter()
    incl = collections.Counter()
    stacks = collections.Counter()
    for pc, lr, f1, f2 in samples:
        fpc = sym(pc) or OUTSIDE
        # The caller: the first frame record's return address when there is one, else LR (a leaf).
        callers = [sym(a) for a in (f1 or lr, f2)]
        callers = [c for c in callers if c]
        self_count[fpc] += 1
        for name in {fpc, *(c for c in callers), sym(lr) or fpc}:
            incl[name] += 1
        stacks[" <- ".join([fpc] + callers[:2])] += 1
    n = max(len(samples), 1)
    print(f"{len(samples)} samples", file=out)
    print(f"\n== self (top {top}) ==", file=out)
    for name, c in self_count.most_common(top):
        print(f"{100.0 * c / n:6.2f}%  {c:7d}  {name}", file=out)
    print(f"\n== inclusive (top {top}) ==", file=out)
    for name, c in incl.most_common(top):
        print(f"{100.0 * c / n:6.2f}%  {c:7d}  {name}", file=out)
    print(f"\n== stacks (top {top}) ==", file=out)
    for name, c in stacks.most_common(top):
        print(f"{100.0 * c / n:6.2f}%  {c:7d}  {name}", file=out)
    return self_count, incl, stacks


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("profile")
    ap.add_argument("--elf")
    ap.add_argument("--nm", default="aarch64-none-elf-nm")
    ap.add_argument("--symbols", help="a saved 'nm -C -n --defined-only' listing instead of --elf")
    ap.add_argument("--top", type=int, default=40)
    a = ap.parse_args(argv)
    if a.symbols:
        with open(a.symbols, encoding="utf-8", errors="replace") as f:
            addrs, names = read_symbols(f)
    elif a.elf:
        nm = subprocess.run([a.nm, "-C", "-n", "--defined-only", a.elf], check=True, capture_output=True, text=True)
        addrs, names = read_symbols(nm.stdout.splitlines())
    else:
        ap.error("--elf or --symbols")
    with open(a.profile, encoding="utf-8", errors="replace") as f:
        base, samples = read_profile(f)
    report(samples, Symbolizer(addrs, names, base), a.top)


if __name__ == "__main__":
    main()
