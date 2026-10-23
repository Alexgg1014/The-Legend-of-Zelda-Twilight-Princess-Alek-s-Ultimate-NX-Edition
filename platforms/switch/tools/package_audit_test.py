#!/usr/bin/env python3
"""H37-c5: package_audit.py and the gate's per-package, static-only and catalog logic.

Always run: capacity arithmetic, the combination table, package discovery by mod.json id, and the gate in
static-only mode (it must say the final ELF validation is pending and never be READY, with or without
packages). Run only when H37_CATALOG_DIR (a folder with the catalog .dusk files) and SYMGEN are set: the
three catalog packages read for real -- the library the loader takes, TLS found, hook records, services,
the capacity notes. What is skipped is printed as skipped, not counted as passed.
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import zipfile

here = pathlib.Path(__file__).resolve().parent
root = here.parent.parent.parent
sys.path.insert(0, str(here))
import check_h37_candidate  # noqa: E402
import package_audit  # noqa: E402

failures = 0
skipped = []


def check(cond, what):
    global failures
    if not cond:
        print(f"FAIL: {what}")
        failures += 1


# --- capacity ---------------------------------------------------------------------------------------
cap = package_audit.capacity
# DM2-a: 1024 hooks per CodeMemory object (16 slots a page)
check(cap(0)["arena_objects"] == 0 and cap(1024)["arena_objects"] == 1 and cap(1025)["arena_objects"] == 2, "object count")
check(cap(2048)["within_planned_budget"] and not cap(2049)["within_planned_budget"], "planned budget is two objects")
check(cap(8192)["within_arena_table"] and not cap(8193)["within_arena_table"], "arena table holds eight objects")
entries = {"a": {"estimated_pages": 1600}, "b": {"estimated_pages": 640}, "c": {"estimated_pages": 9000}}
notes = package_audit.capacity_notes(entries)
check(len(notes) == 1 and notes[0].startswith("c:") and "cannot be hooked" in notes[0], f"notes {notes}")
combos = {tuple(c["mods"]): c for c in package_audit.combinations(entries, {("a", "b"): ["dComIfGp_setNextStage"]})}
check(combos[("a", "b")]["pages"] == 2240 and not combos[("a", "b")]["within_planned_budget"], "a+b over 2048")
check(combos[("a", "b")]["shared_by_name_targets"] == ["dComIfGp_setNextStage"], "shared targets listed")
check(combos[("a", "c")]["shared_by_name_targets"] == [], "no shared targets")

# --- discovery by mod.json id ---------------------------------------------------------------------------
with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    for name, mod_id in (("one-1.0.dusk", "dev.albt.albw"), ("two.dusk", "dev.other"), ("three.dusk", "dev.bezide.dawnlight")):
        with zipfile.ZipFile(tmp / name, "w") as z:
            z.writestr("mod.json", json.dumps({"id": mod_id, "version": "1"}))
    (tmp / "broken.dusk").write_bytes(b"not a zip")
    found = check_h37_candidate.find_packages(tmp, check_h37_candidate.CATALOG)
    check(set(found) == {"dev.albt.albw", "dev.bezide.dawnlight"}, f"found {sorted(found)}")

    # --- the gate without an ELF: pending, never ready ---------------------------------------------------
    for extra in ([], ["--require-catalog"]):
        report = tmp / "static.json"
        run = subprocess.run([sys.executable, str(here / "check_h37_candidate.py"), "--packages", str(tmp / "empty"),
                              "--symgen", "/nonexistent", "--source-root", str(root), "--report", str(report),
                              "--tag", "static-test", *extra], capture_output=True, text=True)
        out = json.loads(report.read_text())
        check(run.returncode == 1 and out["ready"] is False, "static-only is never ready")
        check(out["validation"] == "static-only" and "FINAL ELF VALIDATION PENDING" in run.stdout, "says it is pending")
        check(any("FINAL ELF VALIDATION PENDING" in p for p in out["problems"]), "pending is a problem")
        check(all(out["packages"][m]["status"] == "missing" for m in check_h37_candidate.OFFICIAL), "required missing")
        check(out["packages"]["dev.twilitrealm.luau"]["status"] == "not provided", "optional is not provided")
        wanted = "missing" if extra else "not provided"
        check(all(out["packages"][m]["status"] == wanted for m in check_h37_candidate.CATALOG), f"catalog {wanted}")
        check(any("together" in n for n in out["notes"]) or out["notes"], "notes present")
    # one flag without the other is a usage error
    run = subprocess.run([sys.executable, str(here / "check_h37_candidate.py"), "--elf", "x", "--packages", str(tmp),
                          "--symgen", "x", "--source-root", str(root), "--report", str(tmp / "r.json")],
                         capture_output=True, text=True)
    check(run.returncode == 2, "--elf without --symdb is refused")

# --- the tier policy -----------------------------------------------------------------------------------
B = package_audit.blocks
check(B("required", False) and B("required", True) and B("required", False, present=False), "required always blocks")
check(not B("optional", False, present=False) and not B("optional", True, present=False), "optional absent never blocks")
check(B("optional", False) and B("optional", True), "optional provided and refused blocks")
check(not B("catalog", False) and not B("catalog", False, present=False), "catalog does not block without --require-catalog")
check(B("catalog", True) and B("catalog", True, present=False), "catalog blocks with --require-catalog")

# --- the whole report, with the package readers and the ELF replaced by fixtures -----------------------
import contextlib  # noqa: E402
import io  # noqa: E402
import check_symbol_manifest  # noqa: E402
import elf_audit  # noqa: E402
import gen_required_symbols  # noqa: E402
import host_services  # noqa: E402
import image_surface  # noqa: E402

TLS = "thread-local storage (PT_TLS / TLS symbols): the loader provides no TLS blocks"
IDS = {"required": list(check_h37_candidate.OFFICIAL), "optional": list(check_h37_candidate.OPTIONAL),
       "catalog": list(check_h37_candidate.CATALOG)}
FILES = {**check_h37_candidate.OFFICIAL, **check_h37_candidate.OPTIONAL}


def gate(tmp, present, refused, require_catalog, static=False):
    """Run the gate. present: ids with a package on disk; refused: {id: 'tls'|'imports'} for the packages the
    fixtures make incompatible. Returns (exit code, report)."""
    packs, cat = tmp / "packs", tmp / "cat"
    for d in (packs, cat):
        d.mkdir(parents=True, exist_ok=True)
        for f in d.iterdir():
            f.unlink()
    for mod_id in present:
        tier = "catalog" if mod_id in IDS["catalog"] else "other"
        name = f"{mod_id}-1.0.dusk" if tier == "catalog" else FILES[mod_id]
        with zipfile.ZipFile((cat if tier == "catalog" else packs) / name, "w") as z:
            z.writestr("mod.json", json.dumps({"id": mod_id, "version": "1.0"}))
    build_id = bytes(range(20))
    nro = tmp / "fake.nro"
    nro.write_bytes(b"\0" * 0x10 + b"NRO0" + b"\0" * 0x2c + build_id + b"\0" * 0x40)

    def describe(path, symgen):
        mod_id = check_h37_candidate.package_id(path)
        platform = "linux-aarch64" if refused.get(mod_id) == "linux" else "switch-arm64"
        return {"id": mod_id, "file": path.name, "version": "1.0", "mode": platform,
                "platform": platform, "library": f"lib/{platform}/mod.so", "library_bytes": 10,
                "iostream_imports": 3, "unvalidated": ["uses iostreams: not validated by execution on the console"],
                "package_bytes": 10, "relocations": {"RELATIVE": 1},
                "loader_blockers": [TLS] if refused.get(mod_id) == "tls" else [],
                "hooks_declared": {"by_name": 1, "by_name_targets": 1, "by_function": 0, "by_member": 0},
                "estimated_pages": 1, "estimated_arena_objects": 1,
                "services": {"required": [], "optional": []}}

    def check_requirements(elf, symtab, relocs, exports, runtime, manifest, required, report):
        mods = {}
        for mod_id in {line[0] for line in required} | set(present):
            bad = refused.get(mod_id) == "imports"
            mods[mod_id] = {"imports": {"required": 2, "found": 1 if bad else 2, "missing": ["x (game)"] if bad else [],
                                        "image_runtime": []},
                            "hooks": {"required": 0, "found": 0, "missing": [], "ambiguous": [], "not_interceptable": []},
                            "compatible": not bad}
        return {"mods": mods, "hook_targets": {}}

    saved = (elf_audit.Elf, check_symbol_manifest.elf_build_id, check_symbol_manifest.read_symdb,
             check_symbol_manifest.check_requirements, image_surface.exports, image_surface.symbols,
             image_surface.relative_relocations, image_surface.image_runtime, gen_required_symbols.requirements,
             gen_required_symbols.service_imports, host_services.sdk_services, package_audit.describe,
             check_h37_candidate.git_identity)
    try:
        elf_audit.Elf = lambda data, name: object()
        check_symbol_manifest.elf_build_id = lambda elf: build_id
        check_symbol_manifest.read_symdb = lambda data: (build_id, {})
        check_symbol_manifest.check_requirements = check_requirements
        image_surface.exports = lambda elf: {}
        image_surface.symbols = lambda elf: []
        image_surface.relative_relocations = lambda elf: {}
        image_surface.image_runtime = lambda elf, symtab, relocs: ([], [])
        gen_required_symbols.requirements = lambda mod_id, path, symgen: ([(mod_id, "import", "game", "f")], [])
        gen_required_symbols.service_imports = lambda path, symgen: []
        host_services.sdk_services = lambda include: {"x": (1, 0)}
        package_audit.describe = describe
        check_h37_candidate.git_identity = lambda root: {"commit": "0" * 40, "dirty": 0, "changed": []}
        elf, symdb = tmp / "g.elf", tmp / "g.symdb"
        elf.write_bytes(b"e")
        symdb.write_bytes(b"s")
        report = tmp / "report.json"
        args = ([] if static else ["--elf", str(elf), "--symdb", str(symdb), "--nro", str(nro)]) + [
                "--packages", str(packs), "--catalog", str(cat), "--symgen", "x", "--source-root", str(root), "--report", str(report)]
        if require_catalog:
            args.append("--require-catalog")
        with contextlib.redirect_stdout(io.StringIO()) as out:
            code = check_h37_candidate.main(args)
        return code, json.loads(report.read_text()), out.getvalue()
    finally:
        (elf_audit.Elf, check_symbol_manifest.elf_build_id, check_symbol_manifest.read_symdb,
         check_symbol_manifest.check_requirements, image_surface.exports, image_surface.symbols,
         image_surface.relative_relocations, image_surface.image_runtime, gen_required_symbols.requirements,
         gen_required_symbols.service_imports, host_services.sdk_services, package_audit.describe,
         check_h37_candidate.git_identity) = saved


with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    official = IDS["required"]
    cat = IDS["catalog"]
    both_bad = {"com.dusklight.twilit_essentials": "tls", "dev.bezide.dawnlight": "tls"}

    # catalog present and refused (TLS), --require-catalog off: reported in full, ready unaffected
    code, r, out = gate(tmp, official + cat, both_bad, False)
    twilit = r["packages"]["com.dusklight.twilit_essentials"]
    check(code == 0 and r["ready"] is True and r["problems"] == [], f"ready with refused catalog: {code} {r['problems']}")
    check(twilit["compatible"] is False and any(TLS in x for x in twilit["rejections"]) and twilit["loader_blockers"] == [TLS],
          "the TLS cause is kept")
    check(twilit["blocks_ready"] is False and "com.dusklight.twilit_essentials" in r["catalog_findings_not_blocking"],
          "marked as not blocking")
    check(r["packages"]["dev.albt.albw"]["compatible"] is True, "ALBW compatible in the same report")
    check(all(r["packages"][m]["compatible"] is True and not r["packages"][m]["blocks_ready"] for m in official),
          "the official verdicts are untouched")
    check("does NOT block ready" in out and "READY" in out, "the output says it does not block")
    # the same with --require-catalog: blocked
    code, r, _ = gate(tmp, official + cat, both_bad, True)
    check(code == 1 and r["ready"] is False and any("TLS" in p or "thread-local" in p for p in r["problems"]), "required catalog refused blocks")
    check(r["packages"]["com.dusklight.twilit_essentials"]["blocks_ready"] is True, "blocks_ready is set")
    # static only: never ready, whatever the tiers say, and nothing else blocks
    code, r, out = gate(tmp, official + cat, both_bad, False, static=True)
    check(code == 1 and not r["ready"] and len(r["problems"]) == 1 and "FINAL ELF VALIDATION PENDING" in r["problems"][0],
          f"static-only: {r['problems']}")
    check("PENDING" in out and r["packages"]["dev.bezide.dawnlight"]["compatible"] is False, "static-only keeps the catalog findings")
    code, r, _ = gate(tmp, official + cat, both_bad, True, static=True)
    check(code == 1 and not r["ready"], "static-only with --require-catalog is not ready either")
    # catalog absent
    code, r, _ = gate(tmp, official, {}, False)
    check(code == 0 and r["ready"] and all(r["packages"][m]["status"] == "not provided" for m in cat), "absent catalog does not block")
    code, r, _ = gate(tmp, official, {}, True)
    check(code == 1 and not r["ready"] and sum("no package with this id" in p for p in r["problems"]) == 3, "absent catalog blocks when required")
    # catalog all present and compatible, required on
    code, r, _ = gate(tmp, official + cat, {}, True)
    check(code == 0 and r["ready"], "all three present and compatible passes with --require-catalog")
    # required refused (imports and TLS), and missing
    code, r, _ = gate(tmp, official + cat, {"dev.twilitrealm.randomizer": "imports"}, False)
    check(code == 1 and not r["ready"] and any("randomizer" in p for p in r["problems"]), "required incompatible blocks")
    code, r, _ = gate(tmp, official + cat, {"dev.aleks2.loader_lab": "tls"}, False)
    check(code == 1 and not r["ready"], "required with a loader blocker blocks")
    code, r, _ = gate(tmp, [m for m in official if m != "dev.twilitrealm.cosmetics"] + cat, {}, False)
    check(code == 1 and any("cosmetics" in p and "missing" in p for p in r["problems"]), "required missing blocks")
    # optional: absent does not block; provided and refused does
    # H37-c6: the official Randomizer must come from lib/switch-arm64; a linux-aarch64 one is never compatible
    rando = "dev.twilitrealm.randomizer"
    code, r, out = gate(tmp, official + cat, {rando: "linux"}, False)
    check(code == 1 and not r["ready"] and r["packages"][rando]["compatible"] is False and
          any(rando in p and "lib/switch-arm64/mod.so" in p for p in r["problems"]), f"linux Randomizer: {r['problems']}")
    code, r, out = gate(tmp, official + cat, {}, False)
    check(code == 0 and r["packages"][rando]["platform"] == "switch-arm64" and r["packages"][rando]["unvalidated"]
          and "UNVALIDATED" in out, "switch-arm64 Randomizer passes and says its iostreams are not validated")
    # other required packages may still be linux-aarch64
    code, r, _ = gate(tmp, official + cat, {"dev.twilitrealm.cosmetics": "linux"}, False)
    check(code == 0 and r["ready"], "a linux-aarch64 Cosmetics keeps its behaviour")
    luau = "dev.twilitrealm.luau"
    code, r, _ = gate(tmp, official, {}, False)
    check(r["packages"][luau]["status"] == "not provided" and code == 0, "optional absent does not block")
    code, r, _ = gate(tmp, official + [luau], {luau: "imports"}, False)
    check(code == 1 and not r["ready"] and any(luau in p for p in r["problems"]), "optional provided and refused blocks")
    code, r, _ = gate(tmp, official + [luau], {}, False)
    check(code == 0 and r["ready"], "optional provided and fine passes")

# --- the real catalog packages ------------------------------------------------------------------------------
catalog, symgen = os.environ.get("H37_CATALOG_DIR"), os.environ.get("SYMGEN")
if not catalog or not symgen or not pathlib.Path(catalog).is_dir():
    skipped.append("the three catalog packages (set H37_CATALOG_DIR and SYMGEN)")
else:
    got = check_h37_candidate.find_packages(pathlib.Path(catalog), check_h37_candidate.CATALOG)
    check(set(got) == set(check_h37_candidate.CATALOG), f"catalog packages found: {sorted(got)}")
    d = {m: package_audit.describe(p, symgen) for m, p in got.items()}
    for m, e in d.items():
        check(e["platform"] == "linux-aarch64" and "compatibility" in e["mode"], f"{m}: library {e['library']}")
        check(e["estimated_pages"] >= e["hooks_declared"]["by_name_targets"], f"{m}: pages")
    tls = {m: any(k.startswith("TLSDESC") or k == "R_AARCH64_TLSDESC" for k in e["relocations"]) for m, e in d.items()}
    check(tls["com.dusklight.twilit_essentials"] and tls["dev.bezide.dawnlight"] and not tls["dev.albt.albw"],
          f"TLS: {tls}")
    for m in ("com.dusklight.twilit_essentials", "dev.bezide.dawnlight"):
        check(any("thread-local" in b or "PT_TLS" in b for b in d[m]["loader_blockers"]), f"{m}: TLS is a blocker")
    # H37-c6: all three use fstream/stringstream/ios_base, so as linux-aarch64 builds all three are refused for the
    # libstdc++ ABI; ALBW has no other blocker (no TLS)
    for m, e in d.items():
        check(any("libstdc++ ABI not safe" in b for b in e["loader_blockers"]), f"{m}: stream ABI blocker")
    check(all("libstdc++ ABI not safe" in b for b in d["dev.albt.albw"]["loader_blockers"]),
          f"albw blockers {d['dev.albt.albw']['loader_blockers']}")

print("package audit: " + ("ok" if not failures else f"{failures} failure(s)"))
for s in skipped:
    print(f"SKIPPED: {s}")
sys.exit(1 if failures else 0)
