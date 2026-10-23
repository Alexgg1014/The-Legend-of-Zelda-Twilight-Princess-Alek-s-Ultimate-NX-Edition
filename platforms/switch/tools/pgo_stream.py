#!/usr/bin/env python3
"""aleks2 (V1-PGO): the profile runs of a PGO build, from the card to the build directory.

A run file (sdmc:/switch/dusklight/pgo/run-<date>-<time>.dkpgo, written by src/dusk/pgo_dump.cpp) holds one gcda
stream per compiled unit, named by the .gcda path the compiler recorded (the object's path in the build directory).

  split RUN.dkpgo... --build-root X:/build/switch-devkitA64-release-h38 --out DIR
        one .gcda tree per run: DIR/<run>/<path relative to the build root>
  merge --gcov-tool aarch64-none-elf-gcov-tool DIR/<run>... --out MERGED
        adds the runs together (gcov-tool merge, one pair at a time)
  install MERGED --build BUILD_DIR
        copies the merged .gcda files next to the objects, where -fprofile-use (ALEKS2_PGO=USE) reads them

Each run file is a snapshot of the counters since that run started: only its latest version counts, never two
snapshots of the same run. A run cut off between the temporary file and the rename leaves run-....dkpgo.tmp, which
is complete if its end marker is there; split takes it when the .dkpgo is missing.
The workflow follows the idea of Chansey's sa2-nx PGO notes (our own code; THIRD_PARTY_NOTICES).
"""
import argparse
import pathlib
import shutil
import struct
import subprocess
import sys

MAGIC = b"DKPGO\x01\x00\x00"
END = b"DKPGOEND"
GCDA_MAGIC = 0x67636461  # "gcda"
TAG_OBJECT_SUMMARY = 0xA1000000
TAG_COUNTERS_ARCS = 0x01A10000


class StreamError(Exception):
    pass


def read_run(data):
    """[(recorded name, gcda bytes)] of a run file; raises StreamError if it is cut short or malformed."""
    if data[:8] != MAGIC:
        raise StreamError("not a DKPGO run file")
    units, _ = struct.unpack_from("<II", data, 8)
    pos = 16
    out = []
    while pos + 8 <= len(data) and data[pos:pos + 8] != END:
        if pos + 4 > len(data):
            raise StreamError("cut short in a unit name")
        (n,) = struct.unpack_from("<I", data, pos)
        pos += 4
        name = data[pos:pos + n].decode("utf-8", "replace")
        pos += n
        if pos + 4 > len(data):
            raise StreamError(f"cut short after {name}")
        (m,) = struct.unpack_from("<I", data, pos)
        pos += 4
        if pos + m > len(data):
            raise StreamError(f"cut short in the data of {name}")
        out.append((name, data[pos:pos + m]))
        pos += m
    if data[pos:pos + 8] != END:
        raise StreamError("no end marker: the run file was cut short")
    (written,) = struct.unpack_from("<I", data, pos + 8)
    if written != len(out):
        raise StreamError(f"end marker says {written} units, found {len(out)}")
    if written > units:
        raise StreamError(f"{written} units written of {units} declared")
    return out


def has_counts(blob):
    """Does any arc counter of this gcda hold a non-zero count?"""
    pos = 16
    while pos + 8 <= len(blob):
        tag, length = struct.unpack_from("<Ii", blob, pos)
        pos += 8
        if length > 0:
            if tag == TAG_COUNTERS_ARCS and any(struct.unpack_from(f"<{length // 8}q", blob, pos)):
                return True
            pos += length
    return False


def with_summary(blob):
    """The gcda with the object summary libgcov writes at exit (runs, the largest counter), which the stream that
    __gcov_info_to_gcda produces leaves out. Without it gcov-tool merge kept runs at 0 and dropped units that had
    counts, and -fprofile-use built different code from the merged files. One run file is one run: runs = 1."""
    if len(blob) < 24 or struct.unpack_from("<I", blob, 16)[0] == TAG_OBJECT_SUMMARY:
        return blob
    pos, top = 16, 0
    while pos + 8 <= len(blob):
        tag, length = struct.unpack_from("<Ii", blob, pos)
        pos += 8
        if length > 0:
            if tag == TAG_COUNTERS_ARCS:
                top = max([top, *struct.unpack_from(f"<{length // 8}q", blob, pos)])
            pos += length
    summary = struct.pack("<IIII", TAG_OBJECT_SUMMARY, 8, 1, min(top, 0xFFFFFFFF))
    return blob[:16] + summary + blob[16:]


def norm(path):
    return path.replace("\\", "/").rstrip("/").lower()


def relative(name, build_root):
    n = name.replace("\\", "/")
    root = norm(build_root) + "/"
    if not n.lower().startswith(root):
        return None
    rel = n[len(root):]
    if rel.startswith("/") or ".." in rel.split("/") or not rel.endswith(".gcda"):
        return None
    return rel


def check_gcda(blob):
    return len(blob) >= 16 and struct.unpack_from("<I", blob, 0)[0] == GCDA_MAGIC


def pick_runs(paths):
    """The run files to use: a .tmp only when its .dkpgo is missing."""
    chosen = []
    names = {pathlib.Path(p).name for p in paths}
    for p in paths:
        p = pathlib.Path(p)
        if p.name.endswith(".dkpgo.tmp") and p.name[:-4] in names:
            continue
        chosen.append(p)
    return chosen


def cmd_split(args):
    out = pathlib.Path(args.out)
    total = 0
    for run in pick_runs(args.runs):
        stem = run.name.split(".dkpgo")[0]
        units = read_run(run.read_bytes())
        dest = out / stem
        kept = skipped = bad = 0
        for name, blob in units:
            rel = relative(name, args.build_root)
            if rel is None:
                skipped += 1
                continue
            if not check_gcda(blob):
                bad += 1
                continue
            f = dest / rel
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_bytes(with_summary(blob))
            kept += 1
        print(f"{run.name}: {len(units)} units, {kept} written to {dest}, {skipped} outside the build root, "
              f"{bad} not gcda")
        if bad:
            raise StreamError(f"{run.name}: {bad} units are not gcda streams")
        total += kept
    return 0 if total else 1


def cmd_merge(args):
    runs = [pathlib.Path(r) for r in args.runs]
    out = pathlib.Path(args.out)
    if out.exists():
        raise StreamError(f"{out} exists: choose a new directory")
    if len(runs) == 1:
        shutil.copytree(runs[0], out)
        print(f"one run: copied to {out}")
        return 0
    # The steps beside the result, not under %TEMP%: gcov-tool skips files whose path is over Windows' MAX_PATH
    # without a word (the JSystem units, under a long temporary directory).
    steps = []
    acc = runs[0]
    for i, r in enumerate(runs[1:], 1):
        dst = out if i == len(runs) - 1 else out.with_name(f"{out.name}.step{i}")
        if dst != out and dst.exists():
            raise StreamError(f"{dst} exists: remove it or choose another --out")
        subprocess.run([args.gcov_tool, "merge", str(acc), str(r), "-o", str(dst)], check=True)
        steps.append(dst)
        acc = dst
    for s in steps[:-1]:
        shutil.rmtree(s)
    # Every unit with counts in a run must be in the result.
    lost = sorted({str(f.relative_to(r)) for r in runs for f in r.rglob("*.gcda") if has_counts(f.read_bytes())}
                  - {str(f.relative_to(out)) for f in out.rglob("*.gcda")})
    if lost:
        raise StreamError(f"gcov-tool left out {len(lost)} unit(s) that have counts, e.g. {lost[0]}; "
                          "merge in a short directory (e.g. X:/pgo/...): long paths are skipped")
    print(f"{len(runs)} runs merged into {out}")
    return 0


def cmd_install(args):
    src = pathlib.Path(args.merged)
    build = pathlib.Path(args.build)
    n = 0
    for f in src.rglob("*.gcda"):
        rel = f.relative_to(src)
        obj = build / str(rel)[: -len(".gcda")]
        if not (obj.with_name(obj.name + ".obj").exists() or obj.with_name(obj.name + ".o").exists()):
            print(f"no object for {rel}: skipped")
            continue
        dst = build / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(f, dst)
        n += 1
    print(f"{n} .gcda files installed in {build}")
    return 0 if n else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("split")
    s.add_argument("runs", nargs="+")
    s.add_argument("--build-root", required=True)
    s.add_argument("--out", required=True)
    m = sub.add_parser("merge")
    m.add_argument("runs", nargs="+")
    m.add_argument("--gcov-tool", required=True)
    m.add_argument("--out", required=True)
    i = sub.add_parser("install")
    i.add_argument("merged")
    i.add_argument("--build", required=True)
    args = ap.parse_args(argv)
    try:
        return {"split": cmd_split, "merge": cmd_merge, "install": cmd_install}[args.cmd](args)
    except StreamError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
