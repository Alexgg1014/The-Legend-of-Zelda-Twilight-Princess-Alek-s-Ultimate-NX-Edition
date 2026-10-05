#!/usr/bin/env bash
# H37-c2: the H37 test packages are the same bytes whatever directory they are built in.
#
#   test-h37-mods-reproducible.sh [OUT_DIR]
#
# Locally, H37-c1's packages built in /out and /out2 differed: the mod.so carried its build
# directory in DWARF (.debug_line_str) and, through it, in its GNU build id; and the SDK's .dusk
# carried the time it was zipped. build-h37-test-mods.sh now maps the paths away and delivers the
# canonical make_dusk.py package. This builds everything twice, in two directories of different
# names and lengths, and requires:
#   1. every .so and every .dusk identical between the two (SHA256SUMS equal, file for file);
#   2. no build path left in any .so;
#   3. every .dusk canonical: entries stored, in case-sensitive name order, at 1980-01-01 00:00,
#      with Unix attributes -- and repacking it with make_dusk.py gives the same bytes;
#   4. each .so the very bytes of the lib/linux-aarch64/mod.so inside its .dusk.
#
# Takes the environment build-h37-test-mods.sh takes (FMT_SRC, LUAU_SRC, LUABRIDGE_SRC,
# COSMETICS_SRC, RANDOMIZER_SRC, H37_TEST_MODS); the two builds run in parallel.
set -euo pipefail
# H37-c6: these host tests run on Linux without devkitA64; the linux-aarch64 Randomizer they need is a host-test-only
# package (randomizer-linux-host-test.dusk), which build-h37-test-mods.sh builds only when asked.
export H37_ALLOW_UNSAFE_LINUX_RANDOMIZER_FOR_HOST_TESTS=1
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-h37-mods-reproducible}"
rm -rf "$out"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python

a="$out/out"
b="$out/a-longer-second-output-directory"
"$here/build-h37-test-mods.sh" "$a" > "$out/a.log" 2>&1 & pa=$!
"$here/build-h37-test-mods.sh" "$b" > "$out/b.log" 2>&1 & pb=$!
wait "$pa" || { tail -20 "$out/a.log"; exit 1; }
wait "$pb" || { tail -20 "$out/b.log"; exit 1; }

"$py" - "$here" "$a" "$b" <<'PYEOF'
import hashlib, pathlib, sys, tempfile, zipfile
here, a, b = sys.argv[1], pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
sys.path.insert(0, here)
import make_dusk
bad = 0
def fail(why):
    global bad
    print("FAIL:", why)
    bad += 1
def sums(d):
    return dict(line.split()[::-1] for line in (d / "SHA256SUMS").read_text().splitlines())
sa, sb = sums(a), sums(b)
if not sa or set(sa) != set(sb):
    fail(f"different package sets: {sorted(sa)} / {sorted(sb)}")
for name in sorted(sa):
    if sa[name] != sb.get(name):
        fail(f"{name}: {sa[name]} in {a.name}, {sb.get(name)} in {b.name}")
    data = (a / name).read_bytes()
    if hashlib.sha256(data).hexdigest() != sa[name]:
        fail(f"{name}: SHA256SUMS does not match the file")
    if name.endswith(".so"):
        for d in (str(a).encode(), str(b).encode(), b"/tmp/", b"/home/"):
            if d in data:
                fail(f"{name}: contains the path {d.decode()}")
        dusk = a / (name[:-3] + ".dusk")
        if dusk.exists():
            with zipfile.ZipFile(dusk) as z:
                if z.read("lib/linux-aarch64/mod.so") != data:
                    fail(f"{name}: not the library inside {dusk.name}")
    if name.endswith(".dusk"):
        with zipfile.ZipFile(a / name) as z:
            infos = z.infolist()
            names = [i.filename for i in infos]
            if names != sorted(names):
                fail(f"{name}: entries not in name order")
            for i in infos:
                if i.compress_type != zipfile.ZIP_STORED or i.date_time != (1980, 1, 1, 0, 0, 0) or i.create_system != 3:
                    fail(f"{name}: {i.filename} is not stored at a fixed time with Unix attributes")
                    break
            with tempfile.TemporaryDirectory() as tmp:
                stage = pathlib.Path(tmp) / "stage"
                z.extractall(stage)
                repacked = make_dusk.pack(stage, pathlib.Path(tmp) / "again.dusk")
                if repacked != sa[name]:
                    fail(f"{name}: repacking it gives {repacked}")
for name in sorted(sa):
    print(f"{sa[name]}  {name}")
print(f"{len(sa)} files, identical from {a.name}/ and {b.name}/" if not bad else f"{bad} problem(s)")
sys.exit(1 if bad else 0)
PYEOF
echo "test-h37-mods-reproducible: all checks passed"
