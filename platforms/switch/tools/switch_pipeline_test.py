#!/usr/bin/env python3
"""H38: the switch-arm64 mod pipeline (build_switch_mods.py), the H38 gate (check_h38_candidate.py, h38_policy.py) and
the pinned Twilit source, with fixtures and fake tools -- no devkitA64, no symgen, no network. What this proves is the
pipeline's logic; that devkitA64 really builds Twilit is for the local run (docs/switch-h38-mods.md).

  manifest and pins   the real manifest is valid and pins Twilit Essentials 2.5.0 at its commit and tree; a short
                      hash or a missing field is refused
  determinism         the same source, patches and tools give the same package bytes from two output directories,
                      patches apply in name order, the package holds exactly lib/switch-arm64/mod.so, the report
                      names no absolute path
  refusals            wrong tree, a patch that does not apply, a build that also made a Linux library
  selection           a package with both builds is read through its switch-arm64 library
  gate                a Linux package with TLS is refused; Twilit missing is a failure only with the option on;
                      a package pinned by version must match; the combined hook pages are checked against the
                      arena table; the final ELF still decides `ready`
  exports             a separate diff and proposed file; the committed list is untouched; no change when applied
"""
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

here = pathlib.Path(__file__).resolve().parent
root = here.parent.parent.parent
sys.path.insert(0, str(here))
import build_switch_mods as bsm  # noqa: E402
import check_h37_candidate  # noqa: E402
import check_switch_module  # noqa: E402
import elf_audit  # noqa: E402
import h38_policy  # noqa: E402
import pipeline_fixtures as fx  # noqa: E402

failures = 0


def check(cond, what):
    global failures
    if not cond:
        print(f"FAIL: {what}")
        failures += 1


def sha(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def git(*args, cwd):
    return subprocess.run(["git", "-C", str(cwd), *args], check=True, capture_output=True, text=True,
                          env={**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
                               "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}).stdout.strip()


# --- the real manifest ---------------------------------------------------------------------------------------------
manifest = bsm.load_manifest(root / "platforms/switch/mods/h38-sources.json")
tw = manifest["mods"]["twilit_essentials"]
check(tw["source"]["url"] == "https://github.com/F1mmel/dusklight-twilit-essentials", "twilit url")
check(tw["source"]["commit"] == "7682060c20e2e3757b2888a27bbd958506ccb4f7", "twilit commit")
check(tw["mod_json_version"] == "2.5.0" and tw["mod_id"] == h38_policy.TWILIT_ID, "twilit version and id")
check(h38_policy.pinned_versions(root / "platforms/switch/mods/h38-sources.json") ==
      {h38_policy.TWILIT_ID: "2.5.0", h38_policy.ALBW_ID: "0.3.0"}, "version pins")
al = manifest["mods"]["albw"]
check(al["source"]["url"] == "https://github.com/WadeWinningWilson/albt-dusks", "albw comes from the modular repository")
check("A-Link-Between-Twilight" not in json.dumps(al["source"]), "albw never from the full fork")
check(al["source"]["commit"] == "5cf4a362b128834199baaaecf8ab56aca9130769" and
      al["source"]["tree"] == "77fb2d415d59df9cf19da61ffa5b7508393907fc", "albw commit and tree")
check(al["mod_id"] == h38_policy.ALBW_ID == "dev.albt.albw" and al["mod_json_version"] == "0.3.0", "albw id and version")
check(al["build"]["package_target"] == "albt_full_plugin_package", "albw package target")
check(manifest["mods"]["randomizer"]["source"]["commit"] == "1f24e81303536db4b77f49d057a8ec1f11162000", "randomizer pin")
for name in ("randomizer", "twilit_essentials"):
    mod = manifest["mods"][name]
    check((root / mod["patches"]).is_dir() and list((root / mod["patches"]).glob("*.patch")), f"{name} has patches")
check(not list((root / al["patches"]).glob("*.patch")) if (root / al["patches"]).is_dir() else True,
      "albw is built unpatched")

with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    # a manifest that pins nothing is refused
    for mutate, why in ((lambda m: m["mods"]["randomizer"]["source"].__setitem__("commit", "main"), "branch name"),
                        (lambda m: m["mods"]["randomizer"]["source"].__setitem__("tree", "abc123"), "short tree"),
                        (lambda m: m["mods"]["randomizer"].pop("patches"), "no patches"),
                        (lambda m: m["mods"]["twilit_essentials"]["source"].pop("url"), "no url")):
        bad = json.loads((root / "platforms/switch/mods/h38-sources.json").read_text())
        mutate(bad)
        (tmp / "bad.json").write_text(json.dumps(bad))
        try:
            bsm.load_manifest(tmp / "bad.json")
            check(False, f"manifest with {why} accepted")
        except bsm.PipelineError:
            pass

    # --- a fixture repository standing in for a pinned source ---------------------------------------------------------
    srcrepo = tmp / "srcrepo"
    srcrepo.mkdir()
    git("init", "-q", cwd=srcrepo)
    (srcrepo / "CMakeLists.txt").write_text("project(fixture)\n")
    (srcrepo / "lib.cpp").write_text("line one\n")
    git("add", "-A", cwd=srcrepo)
    git("commit", "-qm", "pinned", cwd=srcrepo)
    commit = git("rev-parse", "HEAD", cwd=srcrepo)
    tree = git("rev-parse", "HEAD^{tree}", cwd=srcrepo)
    # two patches: the second only applies after the first, so name order matters
    (srcrepo / "lib.cpp").write_text("line one\nline two\n")
    git("diff", cwd=srcrepo)
    p1 = git("diff", cwd=srcrepo) + "\n"
    git("checkout", "--", "lib.cpp", cwd=srcrepo)
    (srcrepo / "lib.cpp").write_text("line one\nline two\nline three\n")
    both = git("diff", cwd=srcrepo) + "\n"
    git("checkout", "--", "lib.cpp", cwd=srcrepo)
    # p2 = difference between "one,two" and "one,two,three"
    two = tmp / "two"
    two.mkdir()
    git("init", "-q", cwd=two)
    (two / "lib.cpp").write_text("line one\nline two\n")
    git("add", "-A", cwd=two)
    git("commit", "-qm", "x", cwd=two)
    (two / "lib.cpp").write_text("line one\nline two\nline three\n")
    p2 = git("diff", cwd=two) + "\n"

    fake_root = tmp / "root"
    (fake_root / "platforms/switch/cmake").mkdir(parents=True)
    (fake_root / "sdk").mkdir()
    (fake_root / "sdk/CMakeLists.txt").write_text("# the SDK of this checkout\n")
    (fake_root / "platforms/switch/cmake/SwitchArm64Mod.toolchain.cmake").write_text("# toolchain\n")
    (fake_root / "platforms/switch/patches/fixture").mkdir(parents=True)
    (fake_root / "platforms/switch/patches/fixture/0002-second.patch").write_text(p2)
    (fake_root / "platforms/switch/patches/fixture/0001-first.patch").write_text(p1)
    shutil.copyfile(root / "platforms/switch/mod_required_symbols.txt",
                    fake_root / "platforms/switch/mod_required_symbols.txt")

    def fixture_manifest(path, tree_hash=tree, patches="platforms/switch/patches/fixture"):
        path.write_text(json.dumps({
            "schema": bsm.SCHEMA, "version": 1,
            "mods": {"fixture": {
                "mod_id": "dev.fixture.mod", "tier": "required", "mod_json_version": "1.2.3",
                "source": {"url": "file://unused", "commit": commit, "tree": tree_hash},
                "patches": patches,
                "build": {"package_target": "fixture_package", "package_file": "mods/fixture.dusk",
                          "cmake_args": ["-DFIXTURE_PREFIX={tree}"], "dependency_sources": {"FMT_SRC": "FETCHCONTENT_SOURCE_DIR_FMT"}}}}}))
        return path

    manifest_path = fixture_manifest(tmp / "fixture.json")
    fake_cmake = tmp / "cmake"
    fake_cmake.write_text(fx.FAKE_CMAKE)
    fake_cmake.chmod(0o755)
    fake_symgen = tmp / "symgen"
    fake_symgen.write_text(fx.FAKE_SYMGEN)
    fake_symgen.chmod(0o755)
    good_lib = fx.make_elf(imports=("game_function", "memcpy"), seed=1)
    table = {hashlib.sha256(good_lib).hexdigest(): {
        "hooks": [{"kind": "name", "name": "daAlink_c::create"}, {"kind": "fn"}],
        "imports": []}}
    (tmp / "symgen-table.json").write_text(json.dumps(table))
    packages = {"fixture_package": {"mod_id": "dev.fixture.mod", "version": "1.2.3",
                                    "libs": {"switch-arm64": {"imports": ["game_function", "memcpy"], "seed": 1}}}}
    (tmp / "packages.json").write_text(json.dumps(packages))
    os.environ.update({"FAKE_CMAKE_FIXTURES": str(here), "FAKE_CMAKE_PACKAGES": str(tmp / "packages.json"),
                       "FAKE_SYMGEN_TABLE": str(tmp / "symgen-table.json")})

    def run_pipeline(out, extra=(), phase="packages", mf=None, quiet=True):
        argv = ["--phase", phase, "--out", str(out), "--root", str(fake_root), "--manifest", str(mf or manifest_path),
                "--cmake", str(fake_cmake), "--symgen", str(fake_symgen), "--source", f"fixture={srcrepo}",
                "--allow-missing-toolchain", *extra]
        if quiet:
            import contextlib
            import io
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                return bsm.main(argv)
        return bsm.main(argv)

    # --- determinism, canonical package, patch order -----------------------------------------------------------------
    out1, out2 = tmp / "out1", tmp / "out2"
    rc1 = run_pipeline(out1)
    rc2 = run_pipeline(out2)
    check(rc1 == 0 and rc2 == 0, f"pipeline ran ({rc1}, {rc2})")
    r1 = json.loads((out1 / bsm.REPORT_NAME).read_text())
    r2 = json.loads((out2 / bsm.REPORT_NAME).read_text())
    e1, e2 = r1["mods"]["fixture"], r2["mods"]["fixture"]
    check(e1["package_sha256"] == e2["package_sha256"] and sha(out1 / "fixture.dusk") == sha(out2 / "fixture.dusk"),
          "the same package bytes from two output directories")
    check(e1["patched_tree_sha256"] == e2["patched_tree_sha256"], "patched trees are identical")
    check((out1 / "src-fixture/lib.cpp").read_text() == "line one\nline two\nline three\n", "patches applied in order")
    check([p["name"] for p in e1["patches"]] == ["0001-first.patch", "0002-second.patch"], "patch list in name order")
    with zipfile.ZipFile(out1 / "fixture.dusk") as z:
        libs = [n for n in z.namelist() if n.startswith("lib/")]
        check(libs == ["lib/switch-arm64/mod.so"], f"canonical package holds exactly the switch-arm64 library: {libs}")
        check(all(i.date_time == (1980, 1, 1, 0, 0, 0) and i.compress_type == zipfile.ZIP_STORED for i in z.infolist()),
              "entries stored with fixed times")
        check(z.read("res/readme.txt") == b"hello\n", "text files packed with LF")
    check(str(out1) not in (out1 / bsm.REPORT_NAME).read_text() and str(tmp) not in (out1 / bsm.REPORT_NAME).read_text(),
          "the report holds no absolute path")
    check(e1["source"] == {"url": "file://unused", "commit": commit, "tree": tree}, "report records the pin")
    check(e1["audit"]["module_problems"] == [] and e1["audit"]["estimated_pages"] == 2, f"audit {e1['audit']}")
    check(r1["combined_capacity"]["pages"] == 2, "combined capacity in the report")
    check("fixture.dusk" in (out1 / "SHA256SUMS").read_text(), "SHA256SUMS")
    conf = json.loads((out1 / "build-fixture/configure.json").read_text())
    check(conf["ci"] == "1" and any(a.startswith("-DCMAKE_TOOLCHAIN_FILE=") and a.endswith("SwitchArm64Mod.toolchain.cmake")
                                    for a in conf["args"]), "configured with the switch-arm64 toolchain and CI=1")
    check(f"-DDUSKLIGHT_DIR={fake_root}" in conf["args"] and "-DFIXTURE_PREFIX=/h38-mods/src-fixture" in conf["args"],
          "this checkout's SDK and the mapped source prefix are passed")
    check(any(a.startswith("-DDUSK_SWITCH_ARM64_EXTRA_FLAGS=") and "-ffile-prefix-map=" in a and "=/h38-mods" in a
              for a in conf["args"]), "build paths are mapped away")

    # --- refusals ----------------------------------------------------------------------------------------------------
    bad_tree = fixture_manifest(tmp / "badtree.json", tree_hash="0" * 40)
    check(run_pipeline(tmp / "o3", mf=bad_tree) == 2, "a commit whose tree is not the pinned one is refused")
    (fake_root / "platforms/switch/patches/broken").mkdir()
    (fake_root / "platforms/switch/patches/broken/0001-nope.patch").write_text(p2)  # needs 'line two' first
    bad_patch = fixture_manifest(tmp / "badpatch.json", patches="platforms/switch/patches/broken")
    check(run_pipeline(tmp / "o4", mf=bad_patch) == 2, "a patch that does not apply stops the build")
    both_libs = dict(packages)
    both_libs["fixture_package"] = {**packages["fixture_package"], "libs": {
        "switch-arm64": {"imports": ["game_function"], "seed": 1}, "linux-aarch64": {"imports": ["game_function"], "seed": 2}}}
    (tmp / "packages.json").write_text(json.dumps(both_libs))
    check(run_pipeline(tmp / "o5") == 2, "a build that also holds a Linux library is refused")
    (tmp / "packages.json").write_text(json.dumps(packages))
    # a mod's own FetchDusklight.cmake would fetch another SDK from a checkout that is not plain: refused, nothing built
    (fake_root / ".stamp").write_text("422d7bb\n")
    check(run_pipeline(tmp / "o5b") == 2 and not (tmp / "o5b/build-fixture").exists(), "a managed .stamp checkout is refused")
    (fake_root / ".stamp").unlink()
    # id and version of a package must be the manifest's: the audit says so and the pipeline exits 1
    for what, spec in (("version", {"version": "9.9.9"}), ("id", {"mod_id": "dev.someone.else"})):
        wrong = {"fixture_package": {**packages["fixture_package"], **spec}}
        (tmp / "packages.json").write_text(json.dumps(wrong))
        check(run_pipeline(tmp / f"o6{what}") == 1, f"a package with another {what} than the manifest's fails the audit")
        problems = json.loads((tmp / f"o6{what}" / bsm.REPORT_NAME).read_text())["mods"]["fixture"]["audit"]["module_problems"]
        check(any(("mod.json " + what) in p for p in problems), f"the {what} problem is named: {problems}")
    (tmp / "packages.json").write_text(json.dumps(packages))

    # --- exports: a separate diff, the committed list untouched ---------------------------------------------------------
    committed = fake_root / "platforms/switch/mod_required_symbols.txt"
    before = sha(committed)
    check(run_pipeline(out1, phase="exports") == 0, "exports phase")
    check(sha(committed) == before, "the committed mod_required_symbols.txt is never modified")
    diff = (out1 / "mod_required_symbols.diff").read_text()
    check("+dev.fixture.mod import game game_function" in diff, f"the diff adds the fixture's import: {diff[:300]}")
    check("+dev.fixture.mod hook name daAlink_c::create" in diff, "hook line in the diff")
    r = json.loads((out1 / bsm.REPORT_NAME).read_text())["exports"]
    check(r["changed"] and r["diff"] == "mod_required_symbols.diff" and "cp " in r["apply"], f"exports report {r}")
    shutil.copyfile(out1 / "mod_required_symbols.txt", committed)  # "applied"
    check(run_pipeline(out1, phase="exports") == 0 and not (out1 / "mod_required_symbols.diff").exists(),
          "no diff once applied")
    check(json.loads((out1 / bsm.REPORT_NAME).read_text())["exports"]["changed"] is False, "exports unchanged after apply")
    check(run_pipeline(tmp / "empty", phase="exports") == 2, "exports without packages says to run packages first")

    # --- exports take a package only when it passed its audit (ALBW joins the exports only if its package passed) -------
    two = json.loads(manifest_path.read_text())
    two["mods"]["fixture_b"] = json.loads(json.dumps(two["mods"]["fixture"]))
    two["mods"]["fixture_b"].update({"mod_id": "dev.fixture.b", "mod_json_version": "2.0.0"})
    two["mods"]["fixture_b"]["build"]["package_target"] = "fixture_b_package"
    two["mods"]["fixture_b"]["build"]["package_file"] = "mods/fixture_b.dusk"
    (tmp / "two.json").write_text(json.dumps(two))
    both_mods = {"fixture_package": packages["fixture_package"],
                 "fixture_b_package": {"mod_id": "dev.fixture.b", "version": "9.9.9",  # not the pinned 2.0.0
                                       "libs": {"switch-arm64": {"imports": ["game_function", "memcpy"], "seed": 1}}}}
    (tmp / "packages.json").write_text(json.dumps(both_mods))
    committed_before = sha(committed)
    extra = ("--mods", "fixture", "fixture_b", "--source", f"fixture_b={srcrepo}")
    out_b = tmp / "out-b"
    check(run_pipeline(out_b, extra=extra, mf=tmp / "two.json") == 1, "the failing second package fails the packages phase")
    check(run_pipeline(out_b, extra=extra, phase="exports", mf=tmp / "two.json") == 1,
          "exports of the one that passed are written, and the failing package still makes the run exit 1")
    rep_b = json.loads((out_b / bsm.REPORT_NAME).read_text())["exports"]
    proposed = (out_b / "mod_required_symbols.txt").read_text()
    check(rep_b["left_out_failed_audit"] == ["fixture_b"] and "dev.fixture.b" not in proposed,
          f"a package that failed its audit adds nothing to the exports: {rep_b}")
    both_mods["fixture_b_package"]["version"] = "2.0.0"
    (tmp / "packages.json").write_text(json.dumps(both_mods))
    out_c = tmp / "out-c"
    check(run_pipeline(out_c, extra=extra, mf=tmp / "two.json") == 0, "both packages pass")
    check(run_pipeline(out_c, extra=extra, phase="exports", mf=tmp / "two.json") == 0, "exports of both")
    check("dev.fixture.b import game game_function" in (out_c / "mod_required_symbols.txt").read_text()
          and json.loads((out_c / bsm.REPORT_NAME).read_text())["exports"]["left_out_failed_audit"] == [],
          "a package that passed is in the exports")
    check(sha(committed) == committed_before, "the committed list is still untouched")
    # a mod that does not build (ALBW if it ever fails to compile) is recorded; the other packages are still built, the
    # run exits 1, nothing of it reaches the exports, and a package an earlier run left is removed
    (tmp / "packages.json").write_text(json.dumps({"fixture_package": packages["fixture_package"]}))
    out_d = tmp / "out-d"
    out_d.mkdir()
    (out_d / "fixture_b.dusk").write_bytes(b"stale")
    check(run_pipeline(out_d, extra=extra, mf=tmp / "two.json") == 1, "a mod that does not build makes the run exit 1")
    rep_d = json.loads((out_d / bsm.REPORT_NAME).read_text())
    check(rep_d["mods"]["fixture_b"].get("build_failed") and "audit" not in rep_d["mods"]["fixture_b"]
          and rep_d["mods"]["fixture_b"]["build_log"] == "build-fixture_b.log", "the failure is in the report")
    check("audit" in rep_d["mods"]["fixture"] and (out_d / "fixture.dusk").is_file(), "the other mod was still built")
    check(not (out_d / "fixture_b.dusk").exists(), "no stale package of the failed mod is left")
    check(run_pipeline(out_d, extra=extra, phase="exports", mf=tmp / "two.json") == 1
          and "dev.fixture.b" not in (out_d / "mod_required_symbols.txt").read_text(), "nothing of it reaches the exports")
    (tmp / "packages.json").write_text(json.dumps(packages))

    # --- selection: switch-arm64 is preferred ---------------------------------------------------------------------------
    sel = tmp / "sel.dusk"
    fx.write_package(sel, "x.y", "1", {"linux-aarch64": fx.make_elf(seed=5), "switch-arm64": fx.make_elf(seed=6),
                                       "android-aarch64": fx.make_elf(seed=7)})
    label, data = elf_audit.select_library(sel)
    check(label.endswith("lib/switch-arm64/mod.so") and data == fx.make_elf(seed=6), "switch-arm64 library preferred")
    check(any("exactly lib/switch-arm64/mod.so" in p for p in check_switch_module.check(sel)),
          "check_switch_module wants exactly one library")

    # --- the gate ---------------------------------------------------------------------------------------------------------
    pk = tmp / "pk"
    pk.mkdir()
    ids = {"loader_lab.dusk": "dev.aleks2.loader_lab", "cosmetics.dusk": "dev.twilitrealm.cosmetics",
           "rollandsprint.dusk": "com.melonspeedruns.rollandsprint", "randomizer.dusk": "dev.twilitrealm.randomizer"}
    gate_table = {}
    for n, (file_name, mod_id) in enumerate(sorted(ids.items())):
        lib = fx.make_elf(imports=(f"game_{n}",), seed=100 + n)
        platform = "switch-arm64" if mod_id == "dev.twilitrealm.randomizer" else "linux-aarch64"
        fx.write_package(pk / file_name, mod_id, "1.0.0", {platform: lib})
        gate_table[hashlib.sha256(lib).hexdigest()] = {"hooks": [{"kind": "name", "name": f"hook_{n}"}] * 3, "imports": []}

    def twilit_package(platform="switch-arm64", tls=False, version="2.5.0", hooks=269):
        lib = fx.make_elf(imports=("game_twilit",), tls=tls, seed=200 + hooks)
        fx.write_package(pk / "twilit_essentials.dusk", h38_policy.TWILIT_ID, version, {platform: lib})
        gate_table[hashlib.sha256(lib).hexdigest()] = {
            "hooks": [{"kind": "name", "name": f"twilit_hook_{i}"} for i in range(hooks)], "imports": []}
        (tmp / "symgen-table.json").write_text(json.dumps(gate_table))

    (tmp / "symgen-table.json").write_text(json.dumps(gate_table))

    def gate(extra=(), twilit=True):
        report = tmp / "gate.json"
        import contextlib
        import io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = check_h37_candidate.main(["--profile", "h38", "--packages", str(pk), "--symgen", str(fake_symgen),
                                           "--source-root", str(root), "--tag", "H38-test", "--report", str(report),
                                           *extra])
        return rc, json.loads(report.read_text()), buf.getvalue()

    # Twilit missing: only a failure when the option is on
    rc, rep, _ = gate()
    check(rep["schema"] == "dusklight-aleks-h38-candidate" and rep["ready"] is False and rc == 1, "never ready without the final ELF")
    check(rep["packages"][h38_policy.TWILIT_ID]["tier"] == "catalog" and not rep["packages"][h38_policy.TWILIT_ID]["blocks_ready"],
          "twilit not required by default: missing does not block")
    rc, rep, _ = gate(["--require-twilit"])
    tw_entry = rep["packages"][h38_policy.TWILIT_ID]
    check(tw_entry["tier"] == "required" and tw_entry["blocks_ready"] and tw_entry["status"] == "missing", "twilit required: missing blocks")
    check(any("twilit_essentials.dusk" in p and "missing" in p for p in rep["problems"]), "a clear message names the missing Twilit package")
    os.environ["ALEKS2_H38_REQUIRE_TWILIT"] = "ON"
    rc, rep, _ = gate()
    check(rep["require_twilit"] is True and rep["packages"][h38_policy.TWILIT_ID]["tier"] == "required", "ALEKS2_H38_REQUIRE_TWILIT=ON requires it")
    del os.environ["ALEKS2_H38_REQUIRE_TWILIT"]

    # the real thing: switch-arm64 Twilit 2.5.0 (269 pages) with the others: fits, within the arena table
    twilit_package()
    rc, rep, out = gate(["--require-twilit"])
    t = rep["packages"][h38_policy.TWILIT_ID]
    check(t["platform"] == "switch-arm64" and t["rejections"] == [] and t["estimated_pages"] == 269, f"twilit read: {t['rejections']}")
    cap = rep["combined_capacity"]
    check(cap["pages"] == 269 + 4 and cap["arena_objects"] == 5 and cap["within_arena_table"] and not cap["problems"],
          f"combined capacity {cap}")
    check(rep["ready"] is False and any("FINAL ELF VALIDATION PENDING" in p for p in rep["problems"]), "ready stays false until the ELF")
    check("H38 candidate" in out, "the gate says H38")

    # a Linux package with TLS is refused (not special-cased by id)
    twilit_package(platform="linux-aarch64", tls=True)
    rc, rep, _ = gate(["--require-twilit"])
    t = rep["packages"][h38_policy.TWILIT_ID]
    reasons = " | ".join(t["rejections"])
    check(t["blocks_ready"] and "must ship lib/switch-arm64/mod.so" in reasons and "thread-local" in reasons,
          f"Linux TLS package refused: {reasons}")
    check(t["compatible"] is False, "not compatible")

    # a TLS module that claims to be switch-arm64 is refused too
    twilit_package(platform="switch-arm64", tls=True)
    rc, rep, _ = gate(["--require-twilit"])
    reasons = " | ".join(rep["packages"][h38_policy.TWILIT_ID]["rejections"])
    check("switch-arm64 module check" in reasons and "thread-local" in reasons, f"switch-arm64 TLS refused: {reasons}")

    # version pin
    twilit_package(version="2.4.0")
    rc, rep, _ = gate(["--require-twilit"])
    check(any("pins 2.5.0" in r for r in rep["packages"][h38_policy.TWILIT_ID]["rejections"]), "version pin enforced")

    # the combined limit: pages that cannot fit the arena table block, as the loader's table is the real limit
    check(h38_policy.combined_capacity({"a": 300, "b": 212})["within_arena_table"], "512 pages = 8 objects fit")
    over = h38_policy.combined_capacity({"a": 300, "b": 213})
    check(not over["within_arena_table"] and "cannot all be hooked" in over["problems"][0], "513 pages do not")
    twilit_package(hooks=600)
    rc, rep, _ = gate(["--require-twilit"])
    check(any("arena table" in p for p in rep["problems"]) and rep["combined_capacity"]["problems"], "gate reports the combined overflow")

    # the hook that was inlined away in H37-c8 gets a precise note (H38 declares it DUSK_NOINLINE)
    notes = h38_policy.hook_notes({"missing": ["src/d/actor/d_a_e_rdb.cpp#damage_check", "other"], "ambiguous": [],
                                   "not_interceptable": []})
    check(len(notes) == 1 and "DUSK_NOINLINE" in notes[0] and "ALBW" in notes[0], "damage_check note")
    src_rdb = (root / "src/d/actor/d_a_e_rdb.cpp").read_text()
    check("static DUSK_NOINLINE void damage_check(e_rdb_class* i_this)" in src_rdb, "damage_check is DUSK_NOINLINE")
    check(h38_policy.hook_notes({"missing": [], "ambiguous": [], "not_interceptable": []}) == [], "no note when nothing is missing")

    # --- ALBW: an optional switch-arm64 package, its Linux catalog copy a rejected alternative ---------------------------
    twilit_package()
    albw_id = h38_policy.ALBW_ID

    def albw_package(path, platform="switch-arm64", tls=False, version="0.3.0", hooks=331, mod_id=albw_id, seed=300):
        lib = fx.make_elf(imports=("game_albw",), tls=tls, seed=seed + hooks)
        fx.write_package(path, mod_id, version, {platform: lib})
        gate_table[hashlib.sha256(lib).hexdigest()] = {
            "hooks": [{"kind": "name", "name": f"albw_hook_{i}"} for i in range(hooks)], "imports": []}
        (tmp / "symgen-table.json").write_text(json.dumps(gate_table))

    rc, rep, _ = gate(["--require-twilit"])
    check(rep["packages"][albw_id]["status"] == "not provided" and not rep["packages"][albw_id]["blocks_ready"],
          "ALBW absent does not block")
    check("twilit_albw" not in rep["capacity_profiles"] and "albw" not in rep["capacity_profiles"], "no ALBW profiles without ALBW")
    albw_package(pk / "albw.dusk")
    rc, rep, out = gate(["--require-twilit"])
    a = rep["packages"][albw_id]
    check(a["tier"] == "optional" and a["platform"] == "switch-arm64" and a["rejections"] == [] and not a["blocks_ready"]
          and a["estimated_pages"] == 331, f"ALBW read as an optional switch-arm64 package: {a['rejections']}")
    prof = rep["capacity_profiles"]
    check(rep["combined_capacity"] == prof["primary"] and albw_id not in prof["primary"]["mods"] and prof["primary"]["pages"] == 273,
          "the required profile is everything but ALBW: base + Randomizer + Twilit")
    check(prof["albw"]["pages"] == 3 + 331 and prof["albw"]["within_arena_table"] and not prof["albw"]["problems"], "base + ALBW fits")
    check(prof["albw_randomizer"]["pages"] == 4 + 331 and prof["albw_randomizer"]["within_arena_table"]
          and prof["albw_randomizer"]["only_if_albw_alone_ran_on_the_console"], "base + Randomizer + ALBW is the alternative")
    check(prof["twilit_albw"]["supported"] is False and prof["twilit_albw"]["pages"] == 3 + 269 + 331, "Twilit + ALBW never supported")
    check(any("NOT SUPPORTED, never enable Twilit and ALBW together" in n for n in rep["capacity_notes"]), "the note says it")
    check(not any(albw_id in p for p in rep["problems"]) and all("ALBW" not in p for p in rep["combined_capacity"]["problems"]),
          "a good ALBW adds no problem")
    check("[optional] " + albw_id in out, "the gate prints ALBW as an optional package")

    # the Linux copy of the catalog is never taken in its place: it is reported as a rejected alternative
    cat = tmp / "catalog"
    cat.mkdir()
    albw_package(cat / "A.Link.Between.Twilight.dusk", platform="linux-aarch64", tls=False, hooks=331, seed=400)
    rc, rep, out = gate(["--require-twilit", "--catalog", str(cat)])
    alt = rep.get("catalog_alternatives_rejected", {}).get(albw_id)
    check(rep["packages"][albw_id]["file"] == "albw.dusk" and rep["packages"][albw_id]["platform"] == "switch-arm64",
          "the Switch package is the one judged")
    check(alt and alt["accepted"] is False and alt["platform"] == "linux-aarch64" and alt["chosen_instead"] == "albw.dusk"
          and any("only a switch-arm64 build is accepted" in r for r in alt["reasons"]), f"Linux ALBW rejected: {alt}")
    check("REJECTED as an alternative" in out, "the gate prints it")
    check(not any("A.Link.Between" in p for p in rep["problems"]), "the Linux copy does not block")
    # without a Switch package the Linux copy is the catalog entry, incompatible and not blocking
    (pk / "albw.dusk").rename(tmp / "albw-switch.dusk")
    rc, rep, _ = gate(["--require-twilit", "--catalog", str(cat)])
    a = rep["packages"][albw_id]
    check(a["tier"] == "catalog" and a["platform"] == "linux-aarch64" and not a["blocks_ready"]
          and any("must ship lib/switch-arm64/mod.so" in r for r in a["rejections"]), f"Linux ALBW is not accepted: {a['rejections']}")
    (tmp / "albw-switch.dusk").rename(pk / "albw.dusk")

    # a switch-arm64 ALBW with TLS, with another version or id, or too big for the arena table, is refused and blocks
    albw_package(pk / "albw.dusk", tls=True)
    rc, rep, _ = gate(["--require-twilit"])
    a = rep["packages"][albw_id]
    check(a["blocks_ready"] and "thread-local" in " | ".join(a["rejections"]), "ALBW with TLS refused")
    albw_package(pk / "albw.dusk", version="0.2.9")
    rc, rep, _ = gate(["--require-twilit"])
    check(any("pins 0.3.0" in r for r in rep["packages"][albw_id]["rejections"]), "ALBW version pin enforced")
    albw_package(pk / "albw.dusk", mod_id="dev.someone.else")
    rc, rep, _ = gate(["--require-twilit"])
    check(any("mod.json says dev.someone.else" in r for r in rep["packages"][albw_id]["rejections"]), "ALBW id enforced")
    albw_package(pk / "albw.dusk", hooks=600)
    rc, rep, _ = gate(["--require-twilit"])
    a = rep["packages"][albw_id]
    check(a["blocks_ready"] and any("base + ALBW" in r for r in a["rejections"]) and rep["capacity_profiles"]["albw"]["problems"],
          "ALBW that does not fit the table even alone is not deliverable")
    (pk / "albw.dusk").unlink()

    import contextlib
    import io
    # --- H38-c5: five official catalog packages, optional ----------------------------------------------------------------
    twilit_package()
    five = check_h37_candidate.OPTIONAL_H38
    check(sorted(five.values()) == ["cowboy_link.dusk", "deferred_fog.dusk", "fairy_companion.dusk", "kh2_camera.dusk",
                                    "tp_classic_buttons.dusk"], "the five optional files have stable names")
    pages5 = {"alexandre.cooper_hat.texture_selector": 0, "dev.automata.deferred_fog": 11, "com.agustinfuac.fairy_companion": 0,
              "com.kite.kh2camera": 9, "org.dusklight.tp_classic_buttons": 13}  # the real hook pages of the catalog packages

    def opt_package(mod_id, platform="linux-aarch64", tls=False, mod_json_id=None, version="1.0.0"):
        n = pages5[mod_id]
        lib = fx.make_elf(imports=("game_opt",), tls=tls, seed=600 + n + len(mod_id))
        fx.write_package(pk / five[mod_id], mod_json_id or mod_id, version, {platform: lib})
        gate_table[hashlib.sha256(lib).hexdigest()] = {
            "hooks": [{"kind": "name", "name": f"{five[mod_id]}_hook_{i}"} for i in range(n)], "imports": []}
        (tmp / "symgen-table.json").write_text(json.dumps(gate_table))

    rc, rep, _ = gate(["--require-twilit"])
    base_pages = rep["combined_capacity"]["pages"]
    check(all(rep["packages"][m]["tier"] == "optional" and rep["packages"][m]["status"] == "not provided"
              and not rep["packages"][m]["blocks_ready"] for m in five), "the five absent: optional, not provided, never blocking")
    check(not any(m in p for m in five for p in rep["problems"]), "absent adds no problem")
    for m in five:
        opt_package(m)
    rc, rep, out = gate(["--require-twilit"])
    check(all(rep["packages"][m]["tier"] == "optional" and rep["packages"][m]["rejections"] == []
              and not rep["packages"][m]["blocks_ready"] and rep["packages"][m]["estimated_pages"] == pages5[m] for m in five),
          "the five present and good: optional, no rejection, estimated pages as declared")
    check(rep["combined_capacity"]["pages"] == base_pages + 33 and all(m in rep["combined_capacity"]["mods"] for m in five),
          "present and compatible, they take part in the capacity profile (+33 pages)")
    check(all(f"[optional] {m}" in out for m in five), "the gate prints them as optional packages")
    check(not any(m in p for m in five for p in rep["problems"]), "good ones add no problem")
    # one at a time the profile grows by that package's pages only
    for m in five:
        for other in five:
            if other != m:
                (pk / five[other]).rename(tmp / five[other])
        rc, rep, _ = gate(["--require-twilit"])
        check(rep["combined_capacity"]["pages"] == base_pages + pages5[m], f"{m} alone adds {pages5[m]} pages")
        for other in five:
            if other != m:
                (tmp / five[other]).rename(pk / five[other])
    # a provided one that is not acceptable blocks: TLS, another id, another pin of nothing but its own mod.json id
    opt_package("com.kite.kh2camera", tls=True)
    rc, rep, _ = gate(["--require-twilit"])
    e = rep["packages"]["com.kite.kh2camera"]
    check(e["blocks_ready"] and "thread-local" in " | ".join(e["rejections"]) and any("com.kite.kh2camera" in p for p in rep["problems"]),
          "a provided optional package with TLS blocks ready")
    opt_package("com.kite.kh2camera")
    opt_package("dev.automata.deferred_fog", mod_json_id="dev.someone.else")
    rc, rep, _ = gate(["--require-twilit"])
    check(rep["packages"]["dev.automata.deferred_fog"]["blocks_ready"]
          and any("mod.json says dev.someone.else" in r for r in rep["packages"]["dev.automata.deferred_fog"]["rejections"]),
          "a provided optional package with another id blocks ready")
    opt_package("dev.automata.deferred_fog")
    # the capacity wording at the real sizes: the main set of c4 (467) plus the five (33) is 500 of 512 pages, 8 objects, 12 left
    real = h38_policy.combined_capacity({"main": 467, "optional": 33})
    check(real["pages"] == 500 and real["arena_objects"] == 8 and real["within_arena_table"] and not real["problems"]
          and real["remaining_pages"] == 12 and real["table_pages"] == 512, f"500 of 512: {real}")
    check(any("only 12 remain" in n for n in real["notes"]), "the note says only 12 pages remain")
    check(h38_policy.combined_capacity({"main": 467})["remaining_pages"] == 45, "c4's main set leaves 45 pages")
    check(h38_policy.combined_capacity({"main": 10})["notes"] == [], "a small set says nothing about remaining pages")
    check(not h38_policy.combined_capacity({"main": 513 - 0})["within_arena_table"]
          and h38_policy.combined_capacity({"main": 513})["remaining_pages"] < 0, "over the table stays a problem")
    # the H37 profile does not know them
    with contextlib.redirect_stdout(io.StringIO()):
        check_h37_candidate.main(["--packages", str(pk), "--symgen", str(fake_symgen), "--source-root", str(root),
                                  "--report", str(tmp / "h37-opt.json")])
    check(not any(m in json.loads((tmp / "h37-opt.json").read_text())["packages"] for m in five), "H37 ignores the five")
    for m in five:
        (pk / five[m]).unlink()

    # --- H38-c6: the console findings, apart from the static result ----------------------------------------------------
    real = h38_policy.hardware_findings(root / h38_policy.HARDWARE_FINDINGS)
    want = {("alexandre.cooper_hat.texture_selector", "1.1.0"): "tested", ("dev.automata.deferred_fog", "2.0.0"): "incompatible",
            ("com.agustinfuac.fairy_companion", "1.0.0"): "unverified", ("com.kite.kh2camera", "1.0.1"): "unverified",
            ("org.dusklight.tp_classic_buttons", "1.3.1"): "incompatible"}
    for (mod_id, version), status in want.items():
        check(h38_policy.hardware_verdict(real, mod_id, version)["status"] == status, f"findings: {mod_id} {version} is {status}")
    tp = next(f for f in real["findings"] if f["id"] == "org.dusklight.tp_classic_buttons")
    check(tp["sha256"].startswith("be891f39") and tp["sha256"].endswith("22fd"), "TP Classic is pinned to the bytes that crashed")
    check(all(f["status"] != "tested" for f in real["findings"] if f["id"] == "dev.automata.deferred_fog"),
          "Deferred Fog is never tested by host tests alone")
    # H38-c7: Deferred Fog 2.0.0 crashed on the console (Dawn/Tint worker stack); only those exact bytes are blocked
    fog = next(f for f in real["findings"] if f["id"] == "dev.automata.deferred_fog")
    check(fog["sha256"] == "da70c3c2bf37bcc721423d5de3c0dbb740e6877820481358b9959119579a4e8b" and "fog.wgsl" in fog["reason"],
          "Deferred Fog 2.0.0 is pinned to the bytes that crashed, with its cause")
    check(h38_policy.hardware_verdict(real, "dev.automata.deferred_fog", "2.0.0", fog["sha256"])["status"] == "incompatible",
          "the exact Deferred Fog 2.0.0 is incompatible")
    check(h38_policy.hardware_verdict(real, "dev.automata.deferred_fog", "2.0.1")["status"] == "unverified",
          "a future Deferred Fog is unverified, not blocked")
    check(set(real["switch_variants"]) == {h38_policy.RANDOMIZER_ID, h38_policy.TWILIT_ID}
          and set(real["switch_variants"]) <= set(h38_policy.SWITCH_ONLY_IDS), "Randomizer and Twilit are the Switch variants")
    check(len(real["findings"]) == sum(1 for line in (root / h38_policy.HARDWARE_FINDINGS).read_text().splitlines()
                                       if line.startswith("DUSK_MOD_HARDWARE(")), "every entry of the file is read")
    # the rule: exact id and version (and bytes when both sides know them); anything else is unverified
    v = h38_policy.hardware_verdict(real, "org.dusklight.tp_classic_buttons", "1.3.2")
    check(v["status"] == "unverified" and v["listed"], "another version of TP Classic is unverified, not blocked")
    check(h38_policy.hardware_verdict(real, "org.dusklight.tp_classic_buttons", "1.3.1", "0" * 64)["status"] == "unverified",
          "other bytes under the same version are unverified")
    check(h38_policy.hardware_verdict(real, "org.dusklight.tp_classic_buttons", "1.3.1", tp["sha256"])["status"] == "incompatible",
          "the exact bytes are incompatible")
    v = h38_policy.hardware_verdict(real, "dev.nobody.unlisted", "1.0.0")
    check(v["status"] == "unverified" and not v["listed"], "an unlisted package is unverified")
    bad = tmp / "bad-findings.inc"
    bad.write_text('DUSK_MOD_HARDWARE("a", "1.0.0", "", Broken, "x")\n')
    try:
        h38_policy.hardware_findings(bad)
        check(False, "an unknown status is an error")
    except ValueError:
        pass

    # the gate: fixture findings for fixture packages (the real ones pin the real bytes)
    def findings_file(entries):
        path = tmp / "findings.inc"
        path.write_text("".join(f'DUSK_MOD_HARDWARE("{i}", "{ver}", "{s}", {st}, "{why}")\n' for i, ver, s, st, why in entries))
        return path

    tp_id, cl_id, kh_id, fog_id = ("org.dusklight.tp_classic_buttons", "alexandre.cooper_hat.texture_selector",
                                   "com.kite.kh2camera", "dev.automata.deferred_fog")
    opt_package(tp_id, version="1.3.1")
    opt_package(cl_id, version="1.1.0")
    opt_package(kh_id, version="1.0.1")
    opt_package(fog_id, version="2.0.0")
    fpath = findings_file([(tp_id, "1.3.1", sha(pk / five[tp_id]), "Incompatible", "crashes"),
                           (cl_id, "1.1.0", "", "Tested", "works"), (kh_id, "9.9.9", "", "Incompatible", "other version")])
    rc, rep, out = gate(["--require-twilit", "--hardware-findings", str(fpath)])
    t, c, k, f = (rep["packages"][m] for m in (tp_id, cl_id, kh_id, fog_id))
    check(t["hardware_status"] == "incompatible" and t["compatible"] is False and t["static_compatible"] is not False
          and t["rejections"] == [] and "crashes" in t["hardware_rejection"],
          f"statically valid, incompatible on the console, the two kept apart: {t}")
    check(t["blocks_ready"] and any(tp_id in p and "incompatible on the console" in p for p in rep["problems"]),
          "an incompatible package provided as optional blocks ready")
    check(tp_id not in rep["combined_capacity"]["mods"], "an incompatible package is not counted in the capacity")
    check(c["hardware_status"] == "tested" and "hardware_rejection" not in c and cl_id in rep["combined_capacity"]["mods"],
          "a tested package stays tested and counted")
    check(k["hardware_status"] == "unverified" and not k["blocks_ready"] and kh_id in rep["combined_capacity"]["mods"],
          "an entry for 9.9.9 leaves 1.0.1 unverified and usable")
    check(f["hardware_status"] == "unverified" and not f["blocks_ready"], "a package without an entry is unverified")
    check("console: INCOMPATIBLE (crashes)" in out and "console: TESTED" in out, "the gate prints both statuses")
    # only in --catalog: reported as a catalog package, not blocking, not in the capacity
    cat6 = tmp / "catalog-c6"
    cat6.mkdir()
    (pk / five[tp_id]).rename(cat6 / "org.dusklight.tp_classic_buttons-1.3.1.dusk")
    rc, rep, _ = gate(["--require-twilit", "--hardware-findings", str(fpath), "--catalog", str(cat6)])
    t = rep["packages"][tp_id]
    check(t["tier"] == "catalog" and t["hardware_status"] == "incompatible" and t["compatible"] is False and not t["blocks_ready"]
          and not any(tp_id in p for p in rep["problems"]) and tp_id not in rep["combined_capacity"]["mods"],
          f"TP Classic only in the catalog: reported incompatible, never blocking, never counted: {t}")
    check(tp_id not in rep.get("catalog_alternatives_rejected", {}), "it is not mistaken for a rejected Switch alternative")
    # H38-c7: Deferred Fog marked incompatible keeps its static results; provided it blocks, only in the catalog it does not
    fpath7 = findings_file([(tp_id, "1.3.1", "", "Incompatible", "crashes"),
                            (fog_id, "2.0.0", sha(pk / five[fog_id]), "Incompatible", "shader stack")])
    rc, rep, _ = gate(["--require-twilit", "--hardware-findings", str(fpath7), "--catalog", str(cat6)])
    f = rep["packages"][fog_id]
    check(f["tier"] == "optional" and f["hardware_status"] == "incompatible" and f["compatible"] is False
          and f["static_compatible"] is not False and f["rejections"] == [] and f["blocks_ready"]
          and fog_id not in rep["combined_capacity"]["mods"], f"Deferred Fog provided and incompatible: {f}")
    (pk / five[fog_id]).rename(cat6 / "dev.automata.deferred_fog-2.0.0.dusk")
    rc, rep, _ = gate(["--require-twilit", "--hardware-findings", str(fpath7), "--catalog", str(cat6)])
    f = rep["packages"][fog_id]
    check(f["tier"] == "catalog" and f["hardware_status"] == "incompatible" and f["compatible"] is False and not f["blocks_ready"]
          and not any(fog_id in p for p in rep["problems"]) and fog_id not in rep["combined_capacity"]["mods"],
          f"Deferred Fog only in the catalog: incompatible, never blocking, never counted: {f}")
    (cat6 / "dev.automata.deferred_fog-2.0.0.dusk").rename(pk / five[fog_id])
    # a findings file the gate cannot read is a problem, never silently empty
    rc, rep, _ = gate(["--require-twilit", "--hardware-findings", str(bad)])
    check(any("console findings could not be read" in p for p in rep["problems"]), "unreadable findings are a problem")
    for m in (cl_id, kh_id, fog_id):
        (pk / five[m]).unlink()

    # hooks on virtual members, resolved the loader's way against a manifest {name: (flags, rva)}
    code, dup = h38_policy.SYMBOL_CODE, h38_policy.SYMBOL_DUP_NAME
    manifest = {"Outer_c::Inner_c::draw": (code, 0x10), "_ZTVN7Outer_c7Inner_cE": (2, 0x20), "Dup_c::f": (code | dup, 0),
                "_ZTV5Dup_c": (2, 0x30), "Data_c::x": (2, 0x40)}
    member = lambda display, vtable="", slot=8: {"kind": "member", "display": display, "vtable": vtable, "virtual_slot": slot}
    check(h38_policy.member_hook_problems([member("Outer_c::Inner_c::draw")], manifest) == [],
          "a nested class's virtual member resolves by its full name")
    check(h38_policy.member_hook_problems([member("Outer_c::Inner_c::other", "_ZTVN7Outer_c7Inner_cE")], manifest) == [],
          "else through a nested class's Itanium vtable symbol")
    check(h38_policy.member_hook_problems([member("Gone_c::Inner_c::f", slot=None)], manifest) == [],
          "a non-virtual member is a relocation, not resolved here")
    check(h38_policy.member_hook_problems([member("Dup_c::f", "_ZTV5Dup_c")], manifest) == [],
          "an overloaded name still resolves through the vtable slot")
    p = h38_policy.member_hook_problems([member("Dup_c::f")], manifest)
    check(len(p) == 1 and "more than one address" in p[0], f"an ambiguous name without a vtable is refused: {p}")
    p = h38_policy.member_hook_problems([member("dBgp_c::modelMaterial_c::drawSimple", slot=0)], manifest)
    check(len(p) == 1 and "not found" in p[0] and "not representable as a vtable symbol" in p[0],
          f"H38-c5's Deferred Fog case says both reasons: {p}")
    p = h38_policy.member_hook_problems([member("Data_c::x", "_ZTV6Data_c")], manifest)
    check(len(p) == 1 and "not a code symbol" in p[0] and "not found" in p[0], f"a data name and a missing vtable: {p}")

    # the H37 gate is unchanged: Twilit stays a catalog package and the schema name is H37's
    import contextlib
    import io
    with contextlib.redirect_stdout(io.StringIO()):
        check_h37_candidate.main(["--packages", str(pk), "--symgen", str(fake_symgen), "--source-root", str(root),
                                  "--report", str(tmp / "h37.json")])
    r37 = json.loads((tmp / "h37.json").read_text())
    check(r37["schema"] == "dusklight-aleks-h37-candidate" and r37["packages"][h38_policy.TWILIT_ID]["tier"] == "catalog",
          "the H37 profile is unchanged")
    check(not any("hardware_status" in e or "static_compatible" in e or "member_hooks" in e for e in r37["packages"].values()),
          "the H37 profile has no console findings or member-hook check")

if failures == 0:
    print("switch mod pipeline: manifest pins, deterministic canonical packages, refusals, exports diff, H38 gate: ok")
sys.exit(1 if failures else 0)
