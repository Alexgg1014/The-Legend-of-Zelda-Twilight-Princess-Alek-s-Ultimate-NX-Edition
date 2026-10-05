#!/usr/bin/env python3
"""How much of souldbminer's code is still in the tree, and what is known about each part.

Two instruments, because each one is wrong in a different direction.

  content   (default)  lines that v2.0.2 -> nx/main adds (his fork of upstream), counted if
                       they still appear verbatim in our file. It cannot tell a line he wrote
                       from a line that can only be written one way, so it is a ceiling.
                       It reads src/, platforms/ and include/ of the main repository only.

  --blame              lines whose LAST writer was a commit of his (git blame -w -M -C), in
                       the main repository and in extern/aurora. A line that was rewritten
                       is no longer his; a line that was only re-indented or moved still is.
                       This is what catches a file that was "rewritten" by editing it in place.

Neither number is a measure of authorship by itself. The ledger is: a JSON list saying, for
each file that still has lines of his, WHY those lines may stay -- or that they may not.

  class         meaning
  upstream      the same text is in a public upstream (name the source, tag and license)
  trivial       boilerplate or one-line hooks that can only be written one way
  api           dictated by an interface we do not own (a header's signatures, a libnx call)
  independent   rewritten from behaviour; what matches is what the interface forces
  (no entry)    pending: nobody has argued it yet

An entry may carry "refs": files or directories (environment variables such as $DEVKITPRO are
expanded) whose lines are the interface or the public reference the file follows. The report
then states how many of the file's substantive lines are found in them, and --explain FILE
prints the ones that are not, so the claim can be read instead of trusted.

The report keeps the four apart so that a percentage is never mistaken for a verdict. The
tool exits 1 while anything is pending, or when a ledger entry no longer matches the tree.

  python platforms/switch/tools/souldb-residue.py [--blame] [--ledger FILE] [--min N] [--json] [path ...]

Run it from the repository checkout (dusklight-aleks2).
"""
import argparse
import collections
import json
import os
import subprocess
import sys

BASE = "v2.0.2"
FORK = "nx/main"
UPSTREAM_REF = "v2.0.3"       # what "upstream" means for the main repository
AUTHOR = "souldb"             # substring of the commit author name

# Lines too common to mean anything.
NOISE = {
    "", "{", "}", "};", "}", "#endif", "#else", "#endif  // __SWITCH__",
    "#ifdef __SWITCH__", "#if defined(__SWITCH__)", "#ifndef __SWITCH__", "return;", "break;",
    ")", ");", "*/", "/*", "//", "#pragma once", "endif()", "else()",
}

CLASSES = ("upstream", "trivial", "api", "independent")


def git(repo, *args):
    out = subprocess.run(["git", "-C", repo] + list(args), capture_output=True, text=True,
                         errors="replace")
    return out.stdout


# ---- content instrument (the original) ------------------------------------------------

def his_added_lines(path):
    diff = git(".", "diff", "-U0", BASE, FORK, "--", path)
    added = [l[1:].strip() for l in diff.splitlines() if l.startswith("+") and not l.startswith("+++")]
    return [l for l in added if l and l not in NOISE and len(l) > 3]


def content_report(paths, floor):
    touched = git(".", "diff", "--name-only", BASE, FORK, "--", *paths).split()
    rows = []
    for path in touched:
        his = his_added_lines(path)
        if not his:
            continue
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                ours = fh.read()
        except OSError:
            rows.append((path, len(his), 0, "gone"))
            continue
        present = collections.Counter(l.strip() for l in ours.splitlines())
        rows.append((path, len(his), sum(1 for l in his if present[l] > 0), ""))
    rows.sort(key=lambda r: -r[2])
    print(f"{'residue':>8} {'his':>6}  file   (content match against his net diff)")
    for path, his, residue, note in rows:
        if residue >= floor or note:
            print(f"{residue:>8} {his:>6}  {path}{(' (' + note + ')') if note else ''}")
    total_his = sum(r[1] for r in rows)
    total_res = sum(r[2] for r in rows)
    print(f"\n{total_res} of {total_his} of his lines still present ({100.0 * total_res / max(1, total_his):.0f}%)")
    return rows


# ---- blame instrument -------------------------------------------------------------------

def blamed_lines(repo, path):
    # Without a revision, blame reads the working tree: lines not committed yet are
    # attributed to nobody, which is what a check of a rewrite in progress needs.
    out = git(repo, "blame", "-w", "-M", "-C", "--line-porcelain", "--", path)
    author = None
    lines = []
    for line in out.split("\n"):
        if line.startswith("author "):
            author = line[7:]
        elif line.startswith("\t") and AUTHOR in (author or "").lower():
            lines.append(line[1:])
    return lines


def survey(repo, prefix, upstream_ref):
    """(path, blamed lines, substantive lines) for every file of `repo` that still has
    lines last written by him."""
    names = git(repo, "log", "--author=" + AUTHOR, "--name-only", "--format=").split("\n")
    tracked = set(git(repo, "ls-files").split("\n"))
    rows = []
    for name in sorted({n for n in names if n}):
        if name not in tracked:
            continue
        mine = blamed_lines(repo, name)
        if not mine:
            continue
        upstream = {l.strip() for l in git(repo, "show", f"{upstream_ref}:{name}").split("\n")} if upstream_ref else set()
        substantive = [l for l in mine if l.strip() not in NOISE and len(l.strip()) > 3 and l.strip() not in upstream]
        rows.append((prefix + name, len(mine), len(substantive)))
    return rows


def retired(paths):
    """Files in his diff that are no longer in the tree."""
    gone = []
    for path in git(".", "diff", "--name-only", BASE, FORK, "--", *paths).split():
        if not os.path.exists(path):
            gone.append(path)
    return gone


def corpus(refs):
    """Stripped lines of every text file under the given paths."""
    lines = set()
    for ref in refs:
        ref = os.path.expandvars(ref)
        files = []
        if os.path.isdir(ref):
            for root, _, names in os.walk(ref):
                files += [os.path.join(root, n) for n in names]
        elif os.path.exists(ref):
            files = [ref]
        for f in files:
            try:
                with open(f, encoding="utf-8", errors="replace") as fh:
                    lines.update(l.strip() for l in fh.read().splitlines())
            except OSError:
                pass
    return lines


def unexplained(path, entry):
    """Substantive blamed lines of `path` that none of the ledger entry's refs contain."""
    repo, name = ("extern/aurora", path[len("extern/aurora/"):]) if path.startswith("extern/aurora/") else (".", path)
    known = corpus(entry.get("refs", []))
    out = []
    for l in blamed_lines(repo, name):
        t = l.strip()
        if t in NOISE or len(t) <= 3 or t in known:
            continue
        out.append(l)
    return out


def load_ledger(path):
    if not path or not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8") as fh:
        entries = json.load(fh)
    ledger = {}
    for e in entries:
        if e.get("class") not in CLASSES:
            sys.exit(f"ledger: {e.get('path')}: class must be one of {CLASSES}")
        if not e.get("why"):
            sys.exit(f"ledger: {e.get('path')}: every entry needs a 'why'")
        ledger[e["path"]] = e
    return ledger


def blame_report(args):
    pin = git(".", "rev-parse", f"{UPSTREAM_REF}:extern/aurora").strip()
    rows = survey(".", "", UPSTREAM_REF) + survey("extern/aurora", "extern/aurora/", pin or None)
    ledger = load_ledger(args.ledger)

    by_class = collections.defaultdict(lambda: [0, 0, 0])   # files, blamed, substantive
    detail = []
    problems = []
    for path, blamed, substantive in sorted(rows, key=lambda r: -r[1]):
        entry = ledger.get(path)
        klass = entry["class"] if entry else "pending"
        if entry is None and substantive == 0:
            # Every line he last wrote here is a brace, an include or a line that is also
            # in upstream: nothing to argue.
            klass = "trivial"
        by_class[klass][0] += 1
        by_class[klass][1] += blamed
        by_class[klass][2] += substantive
        detail.append((path, blamed, substantive, klass, entry))
        if entry and blamed > entry.get("max_lines", blamed):
            problems.append(f"{path}: {blamed} lines blamed, ledger allows {entry['max_lines']}")
    for path in ledger:
        if path not in {r[0] for r in rows}:
            problems.append(f"{path}: in the ledger but no longer has lines of his (remove the entry)")

    if args.json:
        print(json.dumps({
            "by_class": {k: {"files": v[0], "blamed": v[1], "substantive": v[2]} for k, v in by_class.items()},
            "files": [{"path": p, "blamed": b, "substantive": s, "class": c} for p, b, s, c, _ in detail],
            "retired": retired(args.paths),
            "problems": problems,
        }, indent=2))
    else:
        print(f"{'blamed':>7} {'subst.':>7}  {'class':<11} file")
        for path, blamed, substantive, klass, entry in detail:
            if blamed >= args.min:
                note = ""
                if entry and entry.get("refs") and substantive:
                    left = len(unexplained(path, entry))
                    note = f"   [in refs: {substantive - min(left, substantive)}/{substantive}]"
                print(f"{blamed:>7} {substantive:>7}  {klass:<11} {path}{note}")
        print()
        print("by class (files / lines blamed / substantive lines):")
        for klass in ("pending",) + CLASSES:
            f, b, s = by_class.get(klass, [0, 0, 0])
            print(f"  {klass:<12} {f:>4} files {b:>6} blamed {s:>6} substantive")
        gone = retired(args.paths)
        print(f"\nretired: {len(gone)} files of his diff are no longer in the tree")
        for p in problems:
            print("PROBLEM:", p)
    return 1 if (by_class.get("pending", [0])[0] or problems) else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--min", type=int, default=1, help="only report files with at least this many lines")
    ap.add_argument("--blame", action="store_true", help="history instrument over the tree and extern/aurora")
    ap.add_argument("--ledger", default=None, help="JSON ledger of justified files (with --blame)")
    ap.add_argument("--json", action="store_true", help="machine-readable output (with --blame)")
    ap.add_argument("--explain", default=None, help="print the lines of FILE its ledger refs do not contain")
    ap.add_argument("paths", nargs="*", default=["src/", "platforms/", "include/"])
    args = ap.parse_args()
    if args.explain:
        ledger = load_ledger(args.ledger)
        entry = ledger.get(args.explain)
        if not entry:
            sys.exit("no ledger entry for " + args.explain)
        for line in unexplained(args.explain, entry):
            print(line)
        return 0
    if args.blame:
        return blame_report(args)
    content_report(args.paths, args.min)
    return 0


if __name__ == "__main__":
    sys.exit(main())
