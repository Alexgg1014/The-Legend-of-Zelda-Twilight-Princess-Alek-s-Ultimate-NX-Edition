#!/usr/bin/env bash
# H37-c3: a hook target the game reaches only through a callback table is reached.
#
#   test-indirect-references.sh [OUT_DIR]
#
# H37-c2's candidate gate refused five Randomizer targets (dStage_playerInit, daDitem_Execute,
# daE_MD_Create, daKytag08_Execute, daObj_Gb_Create) with 0 calls, 0 data_pointers and 0 formed_in_code:
# all five are callbacks in method/FuncTable tables, never the operand of a BL. image_surface now reads
# the pointers the loader applies (DT_RELA and DT_RELR: RELATIVE, ABS64, GLOB_DAT) and follows a
# stored address through its table to something that uses the table.
#
# Real AArch64 static-PIE links (clang + ld.lld, or aarch64-linux-gnu-gcc + ld when CC says so; no
# libc, no linker script of the console) of one small game, checked with image_surface.hook_targets:
#   cb_local          static callback in a file-local const FuncTable that code indexes     -> reached
#   cb_create/cb_exec static callbacks in a method table <- profile <- profile list <- code -> reached
#   cb_abs            exported callback in a table, relocation rewritten to R_AARCH64_ABS64   -> reached
#   cb_dead           callback in a table nothing uses, and in a dead method table/profile  -> refused
#   cb_mid            a table whose addend is cb_mid + 4, not its start                      -> refused
# and the same expectations for a link with DT_RELR packed relocations.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
out="${1:-$root/build/test-indirect-references}"
rm -rf "$out"; mkdir -p "$out"; out="$(cd "$out" && pwd)"
py=python3; command -v "$py" >/dev/null 2>&1 || py=python
cc="${CC:-clang --target=aarch64-linux-gnu}"
ld="${LD:-ld.lld}"

cat > "$out/game.c" <<'CEOF'
typedef int (*fn)(int);
#define NI __attribute__((noinline, used))
typedef struct { fn create; fn execute; } Methods;
typedef struct { const Methods *m; int id; } Profile;

NI static int cb_local(int v) { return v * 3 + 1 + (v >> 2); }
NI static int cb_local2(int v) { return v * 29 + 3 + (v >> 9); }
static const fn FuncTable[2] = { cb_local, cb_local2 };
NI int run_functable(int i, int v) { return FuncTable[i](v); }

NI static int cb_create(int v) { return v * 5 + 2 + (v >> 3); }
NI static int cb_exec(int v) { return v * 7 + 3 + (v >> 1); }
NI static int cb_other(int v) { return v * 11 + 5 + (v >> 5); }
static const Methods l_Method = { cb_create, cb_exec };
static const Methods l_Method2 = { cb_other, cb_other };
static const Profile g_profile = { &l_Method, 4 };
static const Profile g_profile2 = { &l_Method2, 5 };
static const Profile *const g_list[] = { &g_profile, &g_profile2 };
NI int run_profile(int i, int v) { return g_list[i]->m->create(v) + g_list[i]->m->execute(v); }

int cb_abs(int v) { return v * 13 + 7 + (v >> 4); }
NI int cb_abs_keep(int v) { return v + 1; }
static const fn abs_tbl[2] = { cb_abs, cb_abs_keep };
NI int run_abs(int i, int v) { return abs_tbl[i](v); }

NI static int cb_dead(int v) { return v * 17 + 9 + (v >> 6); }
NI static int cb_dead2(int v) { return v * 19 + 11 + (v >> 7); }
__attribute__((used)) static const fn dead_tbl[2] = { cb_dead, cb_dead };
static const Methods l_DeadMethod = { cb_dead2, cb_dead2 };
__attribute__((used)) static const Profile dead_profile = { &l_DeadMethod, 9 };

NI static int cb_mid(int v) { return v * 23 + 13 + (v >> 8) + (v << 2); }
static const fn mid_tbl[2] = { (fn)((char *)cb_mid + 4), cb_abs_keep };
NI int run_mid(int i, int v) { return mid_tbl[i](v); }

int main(void) { return run_functable(0, 1) + run_profile(1, 2) + run_abs(1, 3) + run_mid(1, 4); }
void _start(void) { main(); for (;;) {} }
CEOF
printf '{ cb_abs; };\n' > "$out/dynamic.list"
$cc -O2 -fPIE -ffreestanding -fno-stack-protector -c "$out/game.c" -o "$out/game.o"
"$ld" -pie --no-dynamic-linker -static -e _start --build-id=sha1 --dynamic-list="$out/dynamic.list" -o "$out/game.elf" "$out/game.o"
"$ld" -pie --no-dynamic-linker -static -e _start --build-id=sha1 --dynamic-list="$out/dynamic.list" \
  --pack-dyn-relocs=relr -o "$out/game-relr.elf" "$out/game.o"

"$py" - "$here" "$out" <<'PYEOF'
import pathlib, struct, sys
sys.path.insert(0, sys.argv[1])
import elf_audit, image_surface as S

out = pathlib.Path(sys.argv[2])
failures = 0


def check(cond, what):
    global failures
    if not cond:
        print(f"FAIL: {what}")
        failures += 1


def verdicts(path, patched=None):
    data = bytearray(path.read_bytes())
    if patched:
        patched(data)
    elf = elf_audit.Elf(bytes(data), path.name)
    symtab = S.symbols(elf)
    addr = {n: v for n, v, _s, t, _b, sh in symtab if t == S.STT_FUNC and sh and n.startswith("cb_")}
    exported = set(S.exports(elf).values())
    return elf, S.hook_targets(elf, symtab, S.relative_relocations(elf), addr, exported), addr


def abs64_for_abs_tbl(data):
    """Rewrite the RELATIVE of abs_tbl[0] into R_AARCH64_ABS64 against cb_abs in .dynsym."""
    elf = elf_audit.Elf(bytes(data), "x")
    symtab = S.symbols(elf)
    place = next(v for n, v, *_ in symtab if n == "abs_tbl")
    dynsym = S.symbols(elf, S.SHT_DYNSYM)
    index = 1 + next(i for i, s in enumerate(dynsym) if s[0] == "cb_abs")
    patched = 0
    for sec in elf.sections:
        if sec["type"] != S.SHT_RELA:
            continue
        for at in range(sec["offset"], sec["offset"] + sec["size"], 24):
            off, info, addend = struct.unpack_from("<QQq", data, at)
            if off == place and info & 0xFFFFFFFF == S.R_AARCH64_RELATIVE:
                struct.pack_into("<QQq", data, at, off, (index << 32) | S.R_AARCH64_ABS64, 0)
                patched += 1
    assert patched == 1, "abs_tbl[0] relocation not found"


def expect(v, name, reached_by=None, relocation=None, chain_tail=None, root_table=None):
    r = v[name]
    check(not r["problems"], f"{name}: {r['problems']}")
    check(r["calls"] == 0 and r["formed_in_code"] == 0, f"{name}: no direct reference expected, {r['calls']} calls")
    check(r["data_pointers"] >= 1, f"{name}: no live pointer")
    if r["indirect_references"]:
        ref = r["indirect_references"][0]
        if relocation: check(ref["relocation"] == relocation, f"{name}: {ref['relocation']} != {relocation}")
        if reached_by: check(ref["reached_by"] == reached_by, f"{name}: reached by {ref['reached_by']}")
        if chain_tail: check(ref["chain"][-1] == chain_tail, f"{name}: chain {ref['chain']}")
        if root_table: check(ref["chain"][0] == root_table, f"{name}: chain {ref['chain']}")


def refuse(v, name, text):
    r = v[name]
    check(any(text in p for p in r["problems"]), f"{name}: expected a problem with {text!r}, got {r['problems']}")
    check(r["data_pointers"] == 0, f"{name}: {r['data_pointers']} live pointer(s) in a dead table")


for label, path, patch in (("RELA", out / "game.elf", None), ("RELR", out / "game-relr.elf", None),
                           ("RELA+ABS64", out / "game.elf", abs64_for_abs_tbl)):
    elf, v, addr = verdicts(path, patch)
    print(f"[{label}] " + ", ".join(f"{n}: {len(r['indirect_references'])} live/{r['dead_pointers']} dead"
                                    for n, r in sorted(v.items())))
    rel = {"RELA": "RELATIVE", "RELR": "RELR", "RELA+ABS64": "RELATIVE"}[label]
    expect(v, "cb_local", "code", rel, "FuncTable", "FuncTable")
    expect(v, "cb_create", "code", rel, "l_Method", "g_list")
    expect(v, "cb_exec", "code", rel, "l_Method", "g_list")
    expect(v, "cb_other", "code", rel, "l_Method2", "g_list")
    if label == "RELA+ABS64":
        details = S.pointer_relocations(elf)
        types = {r["type"] for r in details.values()}
        check("ABS64" in types, f"no ABS64 relocation read ({types})")
        expect(v, "cb_abs", "code", "ABS64", "abs_tbl", "abs_tbl")
    else:
        expect(v, "cb_abs", "code", rel, "abs_tbl", "abs_tbl")
    refuse(v, "cb_dead", "none in a table anything uses")
    refuse(v, "cb_dead2", "none in a table anything uses")
    refuse(v, "cb_mid", "nothing calls it")
    check(v["cb_mid"]["dead_pointers"] == 0, "cb_mid + 4 must not count as a stored address of cb_mid")

# The table data must sit in an allocated, loaded section (.data.rel.ro) -- never a section outside the image.
elf, v, _ = verdicts(out / "game.elf")
check([r["section"] for r in v["cb_local"]["indirect_references"]] == [".data.rel.ro"],
      f"section {v['cb_local']['indirect_references']}")
# A relocation whose place lies outside every loaded segment is not applied: not a pointer.
data = bytearray((out / "game.elf").read_bytes())
elf = elf_audit.Elf(bytes(data), "x")
for sec in elf.sections:
    if sec["type"] == S.SHT_RELA:
        for at in range(sec["offset"], sec["offset"] + sec["size"], 24):
            struct.pack_into("<Q", data, at, 0x7FFFFF000)
(out / "game-badplace.elf").write_bytes(data)
elf, v, _ = verdicts(out / "game-badplace.elf")
check(all(r["data_pointers"] == 0 for r in v.values()), "relocations outside the image counted")
print("indirect references: " + ("ok" if not failures else f"{failures} failure(s)"))
sys.exit(1 if failures else 0)
PYEOF
