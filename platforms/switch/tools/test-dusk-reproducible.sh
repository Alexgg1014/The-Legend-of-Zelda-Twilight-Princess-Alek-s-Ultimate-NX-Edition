#!/usr/bin/env bash
# H37: .dusk packages are the same bytes whatever line endings the checkout has, on any host.
#
# A Windows build of H36 packed the Luau probe to a different SHA-256 than the cloud build
# (c67b708e... against 9c8a996c...): Git had checked its text files out with CRLF and make_dusk.py
# packed bytes as they were, deflated by whatever zlib the Python had. Now make_dusk.py packs
# canonically -- stored entries, text files with LF, entries ordered by name case-sensitively,
# Unix attributes -- and .gitattributes keeps platforms/switch/test-mods LF in every checkout.
#
# This test clones the package folders the way Git would on Linux and on Windows (core.autocrlf
# false, true and input, core.eol=crlf) and requires one SHA-256 for all of them, equal to the
# working tree's, and no CR byte in any of their text files. Then the control, which depends on no
# Git setting: a copy of each package whose text files are rewritten here with CRLF line endings
# -- checked byte by byte to really have them -- must pack to the same SHA-256 too: make_dusk.py
# alone makes it canonical. (A clone with only H36's attributes, * text=auto, under
# core.autocrlf=true, is packed as well, and reported, but whether Git converts is Git's business.)
# Then, on a made-up package: a binary file with CRLF bytes is packed untouched, mixed-case names
# are in the same order on every host, every entry is stored with Unix attributes, and packing
# with zlib-ng (when the module is installed) changes nothing.
#
#   test-dusk-reproducible.sh [OUT_DIR]
#
# Works in Git Bash on Windows too (H37-c2): every byte check is done by Python on the files'
# bytes. Git for Windows' grep drops the CR of a CRLF line before matching unless told -U, so
# grep $'\r' found nothing there and the test reported "no CRLF files". Everything happens in
# scratch repositories under OUT_DIR; the real repository, its index and its working tree are
# only read.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-dusk-reproducible}"
rm -rf "$out"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
py=python3
command -v "$py" >/dev/null 2>&1 || py=python
cd "$root"

failures=0
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }

# Clones are what a player or a builder actually gets, so each case is a real git clone of a
# scratch repository holding the working tree's package folders and .gitattributes (committed
# through Git's clean filter, so a CRLF working tree is LF there, as it would be once
# committed). The clone's checkout applies the committed attributes, as on any machine.
packages=()
for dir in platforms/switch/test-mods/*/; do
  [ -f "$dir/mod.json" ] && packages+=("${dir%/}")
done
[ "${#packages[@]}" -gt 0 ] || { echo "no package folders under platforms/switch/test-mods"; exit 1; }

g() { git -c user.name=test -c user.email=test@invalid -c core.autocrlf=false "$@"; }
make_repo() {  # make_repo DIR ATTRIBUTES_FILE
  local repo="$1"
  g init -q "$repo"
  cp "$2" "$repo/.gitattributes"
  for pkg in "${packages[@]}"; do
    mkdir -p "$repo/$(dirname "$pkg")"
    cp -R "$pkg" "$repo/$pkg"
  done
  g -C "$repo" add -A
  g -C "$repo" commit -q -m packages
}
make_repo "$out/repo" .gitattributes
# The control: the attributes these files had in H36 (the catch-all * text=auto only).
printf '* text=auto\n' > "$out/old.gitattributes"
make_repo "$out/repo-old" "$out/old.gitattributes"

pack() {  # pack PACKAGE_DIR -> prints the sha256
  "$py" "$here/make_dusk.py" "$1" "$out/scratch.dusk" 2>/dev/null | sed 's/.* sha256 //' | tr -d '\r'
}
# (Every Python output read here goes through tr -d '\r': Python on Windows ends lines with CRLF,
# and $(...) keeps the CR.)
# The text files (as make_dusk.py classes them) of DIR with a CR byte, one per line; nothing if none.
cr_files() {
  "$py" - "$here" "$1" <<'PYEOF2' | tr -d '\r'
import pathlib, sys
sys.path.insert(0, sys.argv[1])
import make_dusk
root = pathlib.Path(sys.argv[2])
for p in sorted(root.rglob("*")):
    if p.is_file() and p.suffix.lower() in make_dusk.TEXT_SUFFIXES and b"\r" in p.read_bytes():
        print(p.relative_to(root).as_posix())
PYEOF2
}

# Copies DIR to DEST with every text file's line endings made CRLF, by rewriting its bytes.
to_crlf() {
  "$py" - "$here" "$1" "$2" <<'PYEOF2'
import pathlib, shutil, sys
sys.path.insert(0, sys.argv[1])
import make_dusk
source, dest = pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
shutil.copytree(source, dest)
for p in dest.rglob("*"):
    if p.is_file() and p.suffix.lower() in make_dusk.TEXT_SUFFIXES:
        data = p.read_bytes().replace(b"\r\n", b"\n")
        p.write_bytes(data.replace(b"\n", b"\r\n"))
PYEOF2
}

configs=(
  "lf:-c core.autocrlf=false -c core.eol=lf"
  "autocrlf-true:-c core.autocrlf=true"
  "autocrlf-input:-c core.autocrlf=input"
  "eol-crlf:-c core.autocrlf=false -c core.eol=crlf"
)
for entry in "${configs[@]}"; do
  name="${entry%%:*}"
  # shellcheck disable=SC2086
  git ${entry#*:} clone -q --no-hardlinks "$out/repo" "$out/$name"
done
git -c core.autocrlf=true clone -q --no-hardlinks "$out/repo-old" "$out/old-autocrlf-true"

for pkg in "${packages[@]}"; do
  id="$(basename "$pkg")"
  reference="$(pack "$pkg")"
  for entry in "${configs[@]}"; do
    name="${entry%%:*}"
    got="$(pack "$out/$name/$pkg")"
    if [ "$got" != "$reference" ]; then
      fail "$id: the $name checkout packs to $got, the working tree to $reference"
    fi
    cr="$(cr_files "$out/$name/$pkg")"
    [ -z "$cr" ] || fail "$id: the $name checkout has CR bytes in: $(echo "$cr" | tr '\n' ' ')"
  done
  # The control: CRLF text files made here, not by Git, and checked to be CRLF.
  to_crlf "$pkg" "$out/crlf/$id"
  text_files="$("$py" -c "import pathlib, sys; sys.path.insert(0, sys.argv[1]); import make_dusk; print(sum(1 for p in pathlib.Path(sys.argv[2]).rglob('*') if p.is_file() and p.suffix.lower() in make_dusk.TEXT_SUFFIXES))" "$here" "$pkg" | tr -d '\r')"
  crlf="$(cr_files "$out/crlf/$id" | wc -l | tr -d ' ')"
  if [ "$text_files" -eq 0 ] || [ "$crlf" -ne "$text_files" ]; then
    fail "$id: the CRLF copy has $crlf CRLF text file(s) of $text_files; it does not test what it should"
  fi
  control="$(pack "$out/crlf/$id")"
  if [ "$control" != "$reference" ]; then
    fail "$id: CRLF text files pack to $control, LF ones to $reference"
  fi
  git_crlf="$(cr_files "$out/old-autocrlf-true/$pkg" | wc -l | tr -d ' ')"
  old="$(pack "$out/old-autocrlf-true/$pkg")"
  [ "$old" = "$reference" ] || fail "$id: the H36-attributes autocrlf clone packs to $old, not $reference"
  echo "$id: $reference (lf, autocrlf true/input, eol crlf; $crlf/$text_files text files made CRLF; the" \
    "H36-attributes autocrlf clone, $git_crlf CRLF file(s) from Git, the same)"
done

# A made-up package for what the test packages do not have.
if ! "$py" - "$here" "$out" <<'PYEOF'
import pathlib, shutil, sys, zipfile
here, out = map(pathlib.Path, sys.argv[1:3])
sys.path.insert(0, str(here))
import make_dusk
bad = 0
def fail(why):
    global bad
    print("FAIL:", why)
    bad += 1
def make(root, crlf):
    if root.exists():
        shutil.rmtree(root)
    (root / "res").mkdir(parents=True)
    nl = "\r\n" if crlf else "\n"
    (root / "mod.json").write_bytes(('{"id": "dev.test.x",' + nl + '"name": "x", "version": "1.0.0"}' + nl).encode())
    (root / "res" / "main.luau").write_bytes(("print('a')" + nl + "print('b')" + nl).encode())
    (root / "res" / "Zeta.txt").write_bytes(b"z\n")
    (root / "res" / "alpha.txt").write_bytes(b"a\n")
    (root / "res" / "blob.png").write_bytes(b"\x89PNG\r\n\x1a\n" + bytes(range(256)) + b"\r\n")
lf, crlf = out / "made-lf", out / "made-crlf"
make(lf, False)
make(crlf, True)
a = make_dusk.pack(lf, out / "made-lf.dusk")
b = make_dusk.pack(crlf, out / "made-crlf.dusk")
if a != b:
    fail(f"CRLF text packs differently ({b}) from LF ({a})")
with zipfile.ZipFile(out / "made-crlf.dusk") as z:
    names = [i.filename for i in z.infolist()]
    if names != sorted(names):
        fail(f"entries not in case-sensitive name order: {names}")
    for i in z.infolist():
        if i.compress_type != zipfile.ZIP_STORED or i.create_system != 3 or i.date_time != (1980, 1, 1, 0, 0, 0):
            fail(f"{i.filename}: not stored with fixed time and Unix attributes")
    if z.read("res/blob.png") != (crlf / "res" / "blob.png").read_bytes():
        fail("a binary file was changed")
    if b"\r" in z.read("res/main.luau") or b"\r" in z.read("mod.json"):
        fail("a text file kept CR bytes")
try:
    from zlib_ng import zlib_ng
except ImportError:
    zlib_ng = None
if zlib_ng is not None:
    saved, zipfile.zlib = zipfile.zlib, zlib_ng
    try:
        c = make_dusk.pack(lf, out / "made-ng.dusk")
    finally:
        zipfile.zlib = saved
    if c != a:
        fail("zlib-ng packs differently")
    else:
        print("made-up package: same with zlib-ng")
print(f"made-up package: {a} from LF and CRLF folders; binary untouched; stored, ordered, Unix attributes")
sys.exit(1 if bad else 0)
PYEOF
then
  failures=$((failures + 1))
fi

# make_dusk.py says when it changed line endings, so a CRLF folder is visible in the build log.
if ! "$py" "$here/make_dusk.py" "$out/crlf/luau_probe" "$out/scratch.dusk" 2>&1 >/dev/null | grep -q "packed with LF"; then
  fail "make_dusk.py does not say it normalised a CRLF package folder"
fi

if [ "$failures" -gt 0 ]; then
  echo "$failures check(s) failed"
  exit 1
fi
echo "dusk packages are reproducible across LF and CRLF checkouts"
