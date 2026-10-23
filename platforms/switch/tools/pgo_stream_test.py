#!/usr/bin/env python3
"""aleks2 (V1-PGO): host test for pgo_stream.py -- run files built here the way pgo_dump.cpp writes them."""
import pathlib
import struct
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import pgo_stream  # noqa: E402

failures = 0


def check(cond, what):
    global failures
    if not cond:
        print(f"FAIL: {what}")
        failures += 1


def gcda(payload):
    return struct.pack("<IIII", pgo_stream.GCDA_MAGIC, 0x4231362A, 1, 2) + payload


def run_file(units, end=True, declared=None, written=None):
    b = pgo_stream.MAGIC + struct.pack("<II", len(units) if declared is None else declared, 0)
    for name, blob in units:
        n = name.encode()
        b += struct.pack("<I", len(n)) + n + struct.pack("<I", len(blob)) + blob
    if end:
        b += pgo_stream.END + struct.pack("<I", len(units) if written is None else written)
    return b


ROOT = "X:/build/switch-devkitA64-release-h38"
units = [
    (ROOT + "/CMakeFiles/dusklight.dir/src/d/d_a_alink.cpp.gcda", gcda(b"\x01" * 40)),
    ("X:\\build\\switch-devkitA64-release-h38\\extern\\aurora\\CMakeFiles\\aurora_gx.dir\\lib\\deko\\dk_encode.cpp.gcda",
     gcda(b"\x02" * 8)),
    ("D:/elsewhere/foo.cpp.gcda", gcda(b"")),
]

# reading
got = pgo_stream.read_run(run_file(units))
check(len(got) == 3 and got[0][1] == units[0][1], "a whole run file reads back")
for bad, what in ((run_file(units, end=False), "no end marker"), (run_file(units)[:-30], "cut in the data"),
                  (run_file(units, written=2), "end count differs"), (b"nope" * 8, "wrong magic")):
    try:
        pgo_stream.read_run(bad)
        check(False, f"{what} is refused")
    except pgo_stream.StreamError:
        pass

# paths
check(pgo_stream.relative(units[0][0], ROOT) == "CMakeFiles/dusklight.dir/src/d/d_a_alink.cpp.gcda", "relative path")
check(pgo_stream.relative(units[1][0], "x:\\build\\switch-devkitA64-release-h38\\") ==
      "extern/aurora/CMakeFiles/aurora_gx.dir/lib/deko/dk_encode.cpp.gcda", "backslashes and case")
check(pgo_stream.relative(units[2][0], ROOT) is None, "outside the build root")
check(pgo_stream.relative(ROOT + "/../evil.gcda", ROOT) is None, "no escaping the root")
check(pgo_stream.relative(ROOT + "/a.cpp.obj", ROOT) is None, "only .gcda")

# the object summary libgcov would have written
arcs = struct.pack("<Ii", pgo_stream.TAG_COUNTERS_ARCS, 24) + struct.pack("<3q", 5, 70000, 3)
zero = struct.pack("<Ii", pgo_stream.TAG_COUNTERS_ARCS, -16)
fn = struct.pack("<Ii3I", 0x01000000, 12, 1, 2, 3)
g = gcda(fn + arcs + fn + zero)
s = pgo_stream.with_summary(g)
check(s[:16] == g[:16] and struct.unpack_from("<IIII", s, 16) == (pgo_stream.TAG_OBJECT_SUMMARY, 8, 1, 70000),
      "summary after the header: runs 1, sum_max = largest counter")
check(s[32:] == g[16:], "the records follow unchanged")
check(pgo_stream.with_summary(s) == s, "a summary is added once")

# split, the .tmp rule, install
with tempfile.TemporaryDirectory() as tmp:
    t = pathlib.Path(tmp)
    (t / "card").mkdir()
    (t / "card" / "run-20261020-200000.dkpgo").write_bytes(run_file(units))
    (t / "card" / "run-20261020-200000.dkpgo.tmp").write_bytes(run_file(units[:1]))  # superseded
    (t / "card" / "run-20261020-210000.dkpgo.tmp").write_bytes(run_file(units[:1]))  # the run's only copy
    rc = pgo_stream.main(["split", *map(str, sorted((t / "card").iterdir())), "--build-root", ROOT,
                          "--out", str(t / "runs")])
    check(rc == 0, "split succeeds")
    a = t / "runs" / "run-20261020-200000"
    check((a / "CMakeFiles/dusklight.dir/src/d/d_a_alink.cpp.gcda").read_bytes() == pgo_stream.with_summary(units[0][1]),
          "gcda written, with its summary")
    check((a / "extern/aurora/CMakeFiles/aurora_gx.dir/lib/deko/dk_encode.cpp.gcda").exists(), "aurora unit written")
    check(len(list(a.rglob("*.gcda"))) == 2, "the unit outside the root is left out")
    check((t / "runs" / "run-20261020-210000").exists(), "a lone .tmp is used")
    build = t / "build"
    (build / "CMakeFiles/dusklight.dir/src/d").mkdir(parents=True)
    (build / "CMakeFiles/dusklight.dir/src/d/d_a_alink.cpp.obj").write_bytes(b"")
    rc = pgo_stream.main(["install", str(a), "--build", str(build)])
    check(rc == 0 and (build / "CMakeFiles/dusklight.dir/src/d/d_a_alink.cpp.gcda").exists(), "installed by object")
    check(not (build / "extern").exists(), "no object, not installed")
    bad = t / "bad"
    bad.mkdir()
    (bad / "run-x.dkpgo").write_bytes(run_file([(units[0][0], b"notgcda" * 4)]))
    check(pgo_stream.main(["split", str(bad / "run-x.dkpgo"), "--build-root", ROOT, "--out", str(t / "o2")]) == 2,
          "a unit that is not gcda fails the split")

print("pgo_stream_test: all passed" if failures == 0 else f"pgo_stream_test: {failures} failure(s)")
sys.exit(1 if failures else 0)
