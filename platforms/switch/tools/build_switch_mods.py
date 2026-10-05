#!/usr/bin/env python3
"""aleks2 (H38): compile native mods for the console from pinned sources, audit them, and say what they need.

The general form of what build-h37-test-mods.sh did for the Randomizer. A native mod built for Linux (Ubuntu's GCC
13) runs against the game's libstdc++ (devkitA64, GCC 16) with a different iostream/locale layout and, when it uses
thread-local storage, with no TLS block at all; the loader supports neither. So every mod that goes to the console
is compiled from source with devkitA64 against the game's own SDK (SwitchArm64Mod.toolchain.cmake) and packed with
exactly one library, lib/switch-arm64/mod.so. No exception is made per mod: a package whose only build is for Linux
is refused by the gate, never loaded "because it happens to work".

What drives it is the manifest platforms/switch/mods/h38-sources.json: per mod a pinned URL, commit AND git tree
object, a directory of patches, and how to build the package. Adding a mod is adding an entry.

Per mod, in order:
  1. resolve a source checkout (--source NAME=DIR, the repository's own checkout, or a fetch of the pinned commit),
     and refuse it unless the commit exists and its tree is the manifest's;
  2. export a clean tree of that commit (git archive: no working-tree changes, no history) and apply the patches
     of the mod's patch directory in name order with git apply (deterministic: the tree hash is in the report);
  3. configure and build with SwitchArm64Mod.toolchain.cmake against this checkout's SDK, with the build path
     mapped away (-ffile-prefix-map) so two output directories give the same bytes;
  4. repack the SDK's package canonically (make_dusk.py) and require it to hold exactly lib/switch-arm64/mod.so;
  5. check_switch_module.py (no TLS/TLSDESC, no DT_NEEDED, PT_GNU_EH_FRAME, ...) and package_audit.describe
     (imports, hooks, services, an upper bound of CodeMemory pages);
  6. write the hashes and a report with no clock and no absolute path in it.

Two phases, so the NRO is built once: `packages` does all of the above; `exports` then reads the packages that
are in the output directory, regenerates their lines of mod_required_symbols.txt into OUT/mod_required_symbols.txt
and writes OUT/mod_required_symbols.diff. The committed file is never modified: applying the diff is a separate,
visible step.

  build_switch_mods.py --phase packages --out OUT [--mods randomizer twilit_essentials] [--symgen SYMGEN]
  build_switch_mods.py --phase exports  --out OUT --symgen SYMGEN
"""
import argparse
import difflib
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

def _abs(path) -> pathlib.Path:
    """An absolute path that is not resolved through links: on Windows resolve() turns a subst drive (X:) back into the
    long path with spaces that the switch-arm64 build refuses; abspath keeps the drive the caller chose."""
    return pathlib.Path(os.path.abspath(path))


HERE = _abs(__file__).parent
sys.path.insert(0, str(HERE))
import check_switch_module  # noqa: E402
import gen_required_symbols  # noqa: E402
import make_dusk  # noqa: E402
import package_audit  # noqa: E402

EXPECTED_LIBRARY = "lib/switch-arm64/mod.so"
REPORT_NAME = "h38-mods-report.json"
SCHEMA = "dusklight-aleks-switch-mod-sources"
HEX40 = set("0123456789abcdef")


class PipelineError(Exception):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# --- manifest ---------------------------------------------------------------------------------------------------
def load_manifest(path: pathlib.Path) -> dict:
    """The manifest, or PipelineError saying what is wrong with it."""
    try:
        manifest = json.loads(path.read_text())
    except (OSError, ValueError) as error:
        raise PipelineError(f"{path}: cannot read the manifest: {error}")
    if manifest.get("schema") != SCHEMA or manifest.get("version") != 1:
        raise PipelineError(f"{path}: not a {SCHEMA} version 1 manifest")
    mods = manifest.get("mods")
    if not isinstance(mods, dict) or not mods:
        raise PipelineError(f"{path}: no mods")
    for name, mod in mods.items():
        for key in ("mod_id", "tier", "source", "patches", "build"):
            if key not in mod:
                raise PipelineError(f"{path}: mod {name!r} has no {key!r}")
        src = mod["source"]
        for key in ("url", "commit", "tree"):
            if key not in src:
                raise PipelineError(f"{path}: mod {name!r} source has no {key!r}")
        for key in ("commit", "tree"):
            if len(src[key]) != 40 or set(src[key]) - HEX40:
                raise PipelineError(f"{path}: mod {name!r}: source {key} must be a full 40-digit lowercase hash "
                                    f"(a branch or a short hash does not pin anything), got {src[key]!r}")
        for key in ("package_target", "package_file"):
            if key not in mod["build"]:
                raise PipelineError(f"{path}: mod {name!r} build has no {key!r}")
    return manifest


# --- processes --------------------------------------------------------------------------------------------------
def run(cmd, cwd=None, env=None, log=None, check=True):
    """Runs `cmd`; its output goes to `log` (a path) or is returned. Raises PipelineError on failure when check."""
    if log is not None:
        with open(log, "ab") as fh:
            fh.write(("$ " + " ".join(str(c) for c in cmd) + "\n").encode())
            fh.flush()
            proc = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, stdout=fh, stderr=subprocess.STDOUT)
        out = ""
    else:
        proc = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, capture_output=True, text=True)
        out = proc.stdout
    if check and proc.returncode != 0:
        tail = ""
        if log is not None:
            tail = "\n".join(pathlib.Path(log).read_text(errors="replace").splitlines()[-25:])
        else:
            tail = (proc.stderr or "")[-1500:]
        raise PipelineError(f"{' '.join(str(c) for c in cmd[:3])}... failed ({proc.returncode})\n{tail}")
    return out


def rmtree(path) -> None:
    """shutil.rmtree that also removes what git leaves read-only (pack files) on Windows."""
    def writable(func, target, _exc):
        os.chmod(target, 0o700)
        func(target)
    shutil.rmtree(path, onerror=writable) if os.path.exists(path) else None


def git(*args, cwd=None):
    return run(["git", *args], cwd=cwd)


# --- source ---------------------------------------------------------------------------------------------------
def resolve_source(name: str, mod: dict, root: pathlib.Path, out: pathlib.Path, overrides: dict) -> pathlib.Path:
    """A git checkout that holds the pinned commit: --source, the repository's own checkout, else a fetch of it."""
    src = mod["source"]
    candidates = []
    if name in overrides:
        candidates.append(("--source", pathlib.Path(overrides[name])))
    if src.get("checkout"):
        candidates.append(("the repository's checkout", root / src["checkout"]))
    for label, path in candidates:
        if path.is_dir():
            have = subprocess.run(["git", "-C", str(path), "cat-file", "-e", src["commit"] + "^{commit}"],
                                  capture_output=True)
            if have.returncode == 0:
                return path
            if label == "--source":
                raise PipelineError(f"{name}: --source {path} does not contain the pinned commit {src['commit']}")
    # Fetch exactly the pinned commit. The one network use of the pipeline.
    fetched = out / f"fetch-{name}"
    rmtree(fetched)
    fetched.mkdir(parents=True)
    git("init", "-q", cwd=fetched)
    git("remote", "add", "origin", src["url"], cwd=fetched)
    try:
        git("fetch", "--depth", "1", "origin", src["commit"], cwd=fetched)
    except PipelineError as error:
        raise PipelineError(f"{name}: no checkout of {src['commit']} found ({'; '.join(l for l, _ in candidates) or 'none given'}) "
                            f"and fetching it from {src['url']} failed:\n{error}")
    return fetched


def export_tree(name: str, mod: dict, checkout: pathlib.Path, dest: pathlib.Path) -> None:
    """Verifies the pin (commit exists, its tree is the manifest's) and exports a clean tree of it into `dest`."""
    src = mod["source"]
    tree = git("rev-parse", src["commit"] + "^{tree}", cwd=checkout).strip()
    if tree != src["tree"]:
        raise PipelineError(f"{name}: commit {src['commit']} has tree {tree}, the manifest pins {src['tree']}: "
                            "this is not the pinned source")
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    archive = subprocess.run(["git", "-C", str(checkout), "archive", "--format=tar", src["commit"]],
                             capture_output=True)
    if archive.returncode != 0:
        raise PipelineError(f"{name}: git archive failed: {archive.stderr.decode(errors='replace')}")
    subprocess.run(["tar", "-x", "-C", str(dest)], input=archive.stdout, check=True)
    # git apply anchors to a repository even if OUT_DIR sits inside another checkout.
    git("init", "-q", cwd=dest)


def tree_hash(tree: pathlib.Path) -> str:
    """One hash of the files of a tree (names and bytes, sorted; .git ignored)."""
    h = hashlib.sha256()
    for path in sorted(p for p in tree.rglob("*") if p.is_file() and ".git" not in p.relative_to(tree).parts):
        rel = path.relative_to(tree).as_posix()
        h.update(rel.encode() + b"\0" + sha256(path.read_bytes()).encode() + b"\n")
    return h.hexdigest()


def apply_patches(name: str, tree: pathlib.Path, patch_dir: pathlib.Path) -> list:
    """Applies patch_dir/*.patch in name order; [{name, sha256}]. A patch that does not apply stops the build."""
    applied = []
    for patch in sorted(patch_dir.glob("*.patch")) if patch_dir.is_dir() else []:
        try:
            git("apply", "--check", str(patch), cwd=tree)
            git("apply", str(patch), cwd=tree)
        except PipelineError as error:
            raise PipelineError(f"{name}: patch {patch.name} does not apply to the pinned source:\n{error}")
        applied.append({"name": patch.name, "sha256": sha256(patch.read_bytes())})
    return applied


# --- toolchain ----------------------------------------------------------------------------------------------------
def find_devkit(explicit):
    """The directory with aarch64-none-elf-g++ (explicit, PATH, $DEVKITPRO), or None."""
    if explicit:
        return pathlib.Path(explicit)
    found = shutil.which("aarch64-none-elf-g++")
    if found:
        return pathlib.Path(found).parent
    candidate = pathlib.Path(os.environ.get("DEVKITPRO", "/opt/devkitpro")) / "devkitA64" / "bin"
    return candidate if (candidate / "aarch64-none-elf-g++").exists() or (candidate / "aarch64-none-elf-g++.exe").exists() else None


def no_spaces(label: str, path) -> None:
    if " " in str(path):
        raise PipelineError(f"the switch-arm64 build does not support a space in {label}: '{path}'. Map the "
                            "repository to a drive letter without spaces first (Windows: subst X: \"<path>\"), and "
                            "use an OUT_DIR that is spaceless too")


def prefix_maps(root, out, deps: dict) -> list:
    # Most general first: GCC applies the last map that matches, and OUT_DIR may be inside the repository.
    maps = [f"-ffile-prefix-map={root}=/dusklight"]
    for name, path in sorted(deps.items()):
        maps.append(f"-ffile-prefix-map={path}=/deps/{name.lower()}")
    maps.append(f"-ffile-prefix-map={out}=/h38-mods")
    return maps


def build_package(name, mod, tree, out, root, cmake, devkit_bin, maps, deps, env_extra) -> pathlib.Path:
    """Configures and builds the mod's package target; returns the SDK's .dusk."""
    build = out / f"build-{name}"
    log = out / f"build-{name}.log"
    log.write_bytes(b"")
    shutil.rmtree(build, ignore_errors=True)
    mapped_tree = f"/h38-mods/src-{name}"
    toolchain = root / "platforms/switch/cmake/SwitchArm64Mod.toolchain.cmake"
    args = [cmake, "-S", tree, "-B", build, "-G", "Ninja", f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
            f"-DDUSK_SWITCH_ARM64_EXTRA_FLAGS={' '.join(maps)}", f"-DDUSKLIGHT_DIR={root}"]
    for env_name, cmake_var in sorted(mod["build"].get("dependency_sources", {}).items()):
        if env_name in deps:
            args.append(f"-D{cmake_var}={deps[env_name]}")
    webgpu = env_extra.get("DUSK_WEBGPU_INCLUDE_DIR")
    if webgpu:
        args += [f"-DCMAKE_PROJECT_INCLUDE={root / 'platforms/switch/cmake/SwitchModWebGPUHeaders.cmake'}",
                 f"-DDUSK_SWITCH_ARM64_WEBGPU_INCLUDE_DIR={webgpu}"]
    args += [a.replace("{tree}", mapped_tree).replace("{root}", str(root)) for a in mod["build"].get("cmake_args", [])]
    env = dict(os.environ)
    env.update(env_extra)
    env["CI"] = "1"  # a mod's own CMake must not run local-only steps (logo generation, copying into the user's mods)
    if devkit_bin:
        env["PATH"] = f"{devkit_bin}{os.pathsep}{env['PATH']}"
    run(args, env=env, log=log)
    run([cmake, "--build", build, "--target", mod["build"]["package_target"]], env=env, log=log)
    package = build / mod["build"]["package_file"]
    if not package.is_file():
        raise PipelineError(f"{name}: the build finished but {package} does not exist")
    return package


def canonicalize(name: str, sdk_package: pathlib.Path, dest: pathlib.Path) -> None:
    """Unpacks the SDK's package and packs it again canonically; refuses anything but lib/switch-arm64/mod.so."""
    with zipfile.ZipFile(sdk_package) as z:
        libs = sorted(n for n in z.namelist() if n.startswith("lib/") and not n.endswith("/"))
        if libs != [EXPECTED_LIBRARY]:
            raise PipelineError(f"{name}: the built package must hold exactly {EXPECTED_LIBRARY}, it holds: "
                                f"{', '.join(libs) or 'no library'}")
        with tempfile.TemporaryDirectory() as tmp:
            stage = pathlib.Path(tmp) / "stage"
            for info in z.infolist():
                if info.is_dir():
                    continue
                target = stage / info.filename
                if stage.resolve() not in target.resolve().parents:
                    raise PipelineError(f"{name}: {info.filename} would land outside the package")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(z.read(info))
            make_dusk.pack(stage, dest)


# --- audit --------------------------------------------------------------------------------------------------------
def audit(name: str, mod: dict, package: pathlib.Path, symgen) -> dict:
    """The static findings for one canonical package."""
    result = {"module_problems": check_switch_module.check(package)}
    with zipfile.ZipFile(package) as z:
        meta = json.loads(z.read("mod.json"))
        result["library_sha256"] = sha256(z.read(EXPECTED_LIBRARY)) if EXPECTED_LIBRARY in z.namelist() else None
    result["mod_id"], result["version"] = meta.get("id"), meta.get("version")
    if meta.get("id") != mod["mod_id"]:
        result["module_problems"].append(f"mod.json id is {meta.get('id')}, the manifest says {mod['mod_id']}")
    if mod.get("mod_json_version") and meta.get("version") != mod["mod_json_version"]:
        result["module_problems"].append(f"mod.json version is {meta.get('version')}, the manifest pins "
                                         f"{mod['mod_json_version']}")
    if symgen:
        d = package_audit.describe(package, symgen)
        result.update({k: d[k] for k in ("library_bytes", "hooks_declared", "estimated_pages",
                                         "estimated_arena_objects", "iostream_imports", "unvalidated")})
        result["loader_blockers"] = d["loader_blockers"]
        result["services"] = d["services"]
    else:
        result["note"] = "SYMGEN not set: hooks, services and hook pages were not read"
    return result


def package_failed(entry) -> bool:
    """Did this report entry's audit find module problems or loader blockers? An entry with no audit did not pass."""
    audit_entry = (entry or {}).get("audit")
    if audit_entry is None:
        return True
    return bool(audit_entry.get("module_problems") or audit_entry.get("loader_blockers"))


def capacity_of(entries: dict) -> dict:
    pages = [e["estimated_pages"] for e in entries.values() if "estimated_pages" in e]
    if len(pages) != len(entries) or not entries:
        return {"pages": None}
    cap = package_audit.capacity(sum(pages))
    return {"pages": cap["pages"], "arena_objects": cap["arena_objects"],
            "within_planned_budget": cap["within_planned_budget"], "within_arena_table": cap["within_arena_table"]}


# --- exports ------------------------------------------------------------------------------------------------------
def exports_phase(root: pathlib.Path, out: pathlib.Path, manifest: dict, names, symgen) -> dict:
    """OUT/mod_required_symbols.txt and .diff for the packages in OUT; the committed file is left alone."""
    committed = root / "platforms/switch/mod_required_symbols.txt"
    proposed = out / "mod_required_symbols.txt"
    diff_path = out / "mod_required_symbols.diff"
    if not symgen:
        raise PipelineError("the exports phase needs --symgen (or $SYMGEN): it reads each package's imports and hooks")
    shutil.copyfile(committed, proposed)
    specs = []
    for name in names:
        package = out / f"{name}.dusk"
        if not package.is_file():
            raise PipelineError(f"{package} is missing: run --phase packages first")
        specs.append(f"{manifest['mods'][name]['mod_id']}={package}")
    rc = gen_required_symbols.main(["--symgen", symgen, "--merge", "--out", str(proposed), *specs])
    if rc != 0:
        raise PipelineError("gen_required_symbols.py refused a package (an import the loader refuses by name)")
    old = committed.read_text().splitlines(keepends=True)
    new = proposed.read_text().splitlines(keepends=True)
    diff = list(difflib.unified_diff(old, new, "a/platforms/switch/mod_required_symbols.txt",
                                     "b/platforms/switch/mod_required_symbols.txt"))
    diff_path.write_text("".join(diff))
    if not diff:
        diff_path.unlink()
    added = sum(1 for l in diff if l.startswith("+") and not l.startswith("+++"))
    removed = sum(1 for l in diff if l.startswith("-") and not l.startswith("---"))
    return {"changed": bool(diff), "lines_added": added, "lines_removed": removed,
            "diff": diff_path.name if diff else None, "proposed": proposed.name,
            "apply": ("cp %s platforms/switch/mod_required_symbols.txt (then commit only that)" % proposed
                      if diff else "nothing to apply: the committed list already has everything these packages need")}


# --- main ---------------------------------------------------------------------------------------------------------
def write_sums(out: pathlib.Path) -> None:
    lines = [f"{sha256(p.read_bytes())}  {p.name}" for p in sorted(out.glob("*.dusk")) + sorted(out.glob("*.so"))]
    (out / "SHA256SUMS").write_text("\n".join(lines) + ("\n" if lines else ""))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--phase", choices=("packages", "exports", "all"), default="packages")
    ap.add_argument("--out", required=True, type=pathlib.Path)
    ap.add_argument("--manifest", type=pathlib.Path)
    ap.add_argument("--root", type=pathlib.Path, default=HERE.parent.parent.parent)
    ap.add_argument("--mods", nargs="*", help="manifest names to build (default: all)")
    ap.add_argument("--symgen", default=os.environ.get("SYMGEN"))
    ap.add_argument("--cmake", default=os.environ.get("CMAKE", "cmake"))
    ap.add_argument("--devkit-bin", default=None, help="devkitA64's bin directory (default: PATH, then $DEVKITPRO)")
    ap.add_argument("--source", action="append", default=[], metavar="NAME=DIR",
                    help="a checkout holding the pinned commit of NAME (also $<NAME>_SRC, e.g. RANDOMIZER_SRC)")
    ap.add_argument("--allow-missing-toolchain", action="store_true",
                    help="tests only: do not require devkitA64 on PATH (a fake cmake stands in)")
    args = ap.parse_args(argv)

    root = _abs(args.root)
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    out = _abs(out)
    try:
        manifest = load_manifest(args.manifest or root / "platforms/switch/mods/h38-sources.json")
        names = args.mods if args.mods else sorted(manifest["mods"])
        unknown = [n for n in names if n not in manifest["mods"]]
        if unknown:
            raise PipelineError(f"not in the manifest: {', '.join(unknown)}")
        overrides = {}
        for spec in args.source:
            name, _, path = spec.partition("=")
            if not path:
                raise PipelineError(f"--source {spec}: expected NAME=DIR")
            overrides[name] = path
        for name in names:
            env_dir = os.environ.get(f"{name.upper()}_SRC")
            alias = manifest["mods"][name]["source"].get("env")  # e.g. TWILIT_SRC for twilit_essentials
            if not env_dir and alias:
                env_dir = os.environ.get(alias)
            if env_dir and name not in overrides:
                overrides[name] = env_dir
        report_path = out / REPORT_NAME
        report = json.loads(report_path.read_text()) if report_path.is_file() else {}
        report.update({"schema": "dusklight-aleks-h38-mods", "version": 1})
        report.setdefault("mods", {})

        if args.phase in ("packages", "all"):
            devkit = find_devkit(args.devkit_bin)
            if devkit is None and not args.allow_missing_toolchain:
                raise PipelineError(
                    "devkitA64 not found (aarch64-none-elf-g++ on PATH, --devkit-bin, or $DEVKITPRO/devkitA64/bin). "
                    "A switch-arm64 package is compiled with the game's own compiler; nothing else is accepted, so "
                    "nothing was built.")
            no_spaces("the repository path", root)
            no_spaces("OUT_DIR", out)
            # A mod whose CMake brings its own FetchDusklight.cmake (ALBW) fetches a Dusklight SDK of its own unless
            # DUSKLIGHT_DIR is a checkout without the module's .stamp. DUSKLIGHT_DIR is this checkout, so it must be one.
            if not (root / "sdk/CMakeLists.txt").is_file() or (root / ".stamp").exists():
                raise PipelineError(f"{root} is not a plain Dusklight checkout (sdk/CMakeLists.txt missing or a "
                                    ".stamp present): a mod's FetchDusklight.cmake would fetch another SDK")
            deps = {}
            for mod in manifest["mods"].values():
                for env_name in mod["build"].get("dependency_sources", {}):
                    if os.environ.get(env_name):
                        deps[env_name] = str(_abs(os.environ[env_name]))
                        no_spaces(env_name, deps[env_name])
            maps = prefix_maps(root, out, {k[:-4] if k.endswith("_SRC") else k: v for k, v in deps.items()})
            env_extra = {k: os.environ[k] for k in ("DUSK_WEBGPU_INCLUDE_DIR",) if os.environ.get(k)}
            for name in names:
                mod = manifest["mods"][name]
                print(f"[{name}] source {mod['source']['commit'][:10]}")
                checkout = resolve_source(name, mod, root, out, overrides)
                tree = out / f"src-{name}"
                export_tree(name, mod, checkout, tree)
                patches = apply_patches(name, tree, root / mod["patches"])
                entry = {"mod_id": mod["mod_id"], "tier": mod["tier"],
                         "source": {k: mod["source"][k] for k in ("url", "commit", "tree")},
                         "patches": patches, "patched_tree_sha256": tree_hash(tree)}
                try:
                    sdk_package = build_package(name, mod, tree, out, root, args.cmake, devkit, maps, deps, env_extra)
                except PipelineError as error:
                    # The source is the pinned one but it does not compile or link for the console: this mod stops, the
                    # others go on. The entry has no audit, so it counts as failed everywhere (exports, gate); a package
                    # left by an earlier run is removed so it cannot be taken for this build's.
                    for stale in (out / f"{name}.dusk", out / f"{name}.so"):
                        stale.unlink(missing_ok=True)
                    entry["build_failed"] = str(error)[-3000:]
                    entry["build_log"] = f"build-{name}.log"
                    report["mods"][name] = entry
                    print(f"[{name}] BUILD FAILED (see {entry['build_log']}): {str(error).splitlines()[0]}")
                    continue
                package = out / f"{name}.dusk"
                canonicalize(name, sdk_package, package)
                with zipfile.ZipFile(package) as z:
                    so = z.read(EXPECTED_LIBRARY)
                (out / f"{name}.so").write_bytes(so)
                entry.update({"package": package.name, "package_sha256": sha256(package.read_bytes()),
                              "package_bytes": package.stat().st_size, "library_sha256": sha256(so)})
                entry["audit"] = audit(name, mod, package, args.symgen)
                report["mods"][name] = entry
                problems = entry["audit"]["module_problems"] + entry["audit"].get("loader_blockers", [])
                print(f"[{name}] {package.name} {entry['package_sha256'][:12]}: "
                      + ("ok" if not problems else "PROBLEMS: " + "; ".join(problems)))
            built = {n: e["audit"] for n, e in report["mods"].items() if "audit" in e}
            report["combined_capacity"] = capacity_of(built)
            write_sums(out)

        if args.phase in ("exports", "all"):
            have = [n for n in names if (out / f"{n}.dusk").is_file()]
            # Only a package that passed its audit contributes: a failed one (ALBW, say) is left out of the exports
            # and says so; exports are never generated from a package that was refused.
            failed = [n for n in have if package_failed(report["mods"].get(n))]
            for n in failed:
                print(f"[exports] {n}: its package did not pass the audit, left out of the exports")
            have = [n for n in have if n not in failed]
            if failed and not have:
                raise PipelineError(f"no package to export: {', '.join(failed)} did not pass the audit")
            report["exports"] = exports_phase(root, out, manifest, have or names, args.symgen)
            report["exports"]["left_out_failed_audit"] = failed
            print(f"[exports] {'changed: ' + str(report['exports']['diff']) if report['exports']['changed'] else 'no change'}"
                  f" ({report['exports']['apply']})")

        report_path.write_text(json.dumps(report, indent=1, sort_keys=True) + "\n")
        bad = [n for n, e in report["mods"].items()
               if e.get("build_failed") or e.get("audit", {}).get("module_problems")
               or e.get("audit", {}).get("loader_blockers")]
        return 1 if bad else 0
    except PipelineError as error:
        print(f"build_switch_mods: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
