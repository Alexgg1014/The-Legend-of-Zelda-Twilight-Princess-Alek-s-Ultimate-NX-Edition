#!/usr/bin/env python3
"""aleks2 (H37-c2): may this build be handed over as an H37 candidate? A strict report, and exit 1
unless every answer is yes.

The post-link check (check_symbol_manifest.py) runs on every link; the H37 preset keeps it lenient
(ALEKS2_SYMBOL_CHECK_STRICT=OFF) so development builds go on. This is the step before delivery,
and it is never lenient:

  identity   the NRO is the ELF's (same GNU build id), the symbol manifest is that ELF's (not
             stale), and the sources are a commit with no tracked change (dirty=0).
  packages   each official package (.dusk) in --packages: loader_lab, Cosmetics, Roll and Sprint
             and Randomizer. What it requires is read from the package itself -- its library's
             imports and its by-name hooks, exactly as gen_required_symbols.py reads them -- not
             from the committed list, which could be stale. Each requirement is then resolved
             against the linked ELF the way the loader resolves it on the console
             (check_symbol_manifest.check_requirements): imports in .dynsym or the image runtime
             table, hooks in the manifest, unambiguous, at a function funchook can patch and
             something reaches. A package refused by name (elf_audit.DENIED) fails too.

  H37-c5: the same judgement, one package at a time, for eight packages:
    required   loader_lab, Cosmetics, Roll and Sprint, Randomizer          (--packages; missing = a problem)
    optional   luau_runtime                                                (--packages; checked when present)
               H38-c5 (profile h38 only), five official catalog packages: cowboy_link, deferred_fog, fairy_companion,
               kh2_camera, tp_classic_buttons                              (--packages; absent never blocks, a
               provided one must pass and then counts in the capacity profiles)
    catalog    Twilit Essentials, Dawnlight, ALBW  (any *.dusk in --catalog, default --packages, found by the
               id in its mod.json; checked when present. Absent or incompatible: reported, and only
               --require-catalog makes that block ready -- see package_audit.blocks)
  Each report entry says which library the loader takes (switch-arm64, else linux-aarch64 in compatibility
  mode), its size and relocations, imports found/total and the missing ones, refused imports, by-name hooks
  found/total with the missing, ambiguous and uninterceptable, the services it needs and may use, an upper
  bound of the hook pages and CodeMemory objects it needs (package_audit.py) and the reason for every
  rejection. The report also lists the pairs of packages whose hooks together exceed the planned budget or
  share a by-name target. Compatible one at a time never means compatible together.

  READY means the static checks passed -- symbols, by-name hooks, services, the loader's structural
  rules -- against this exact ELF. It does not certify behaviour on the console: Randomizer still has to
  generate, save and reload a game, and each catalog mod needs a short functional test.

  H38-c6 (profile h38): each package's report separates what the gate checked from what the console showed.
    static_compatible  the static checks above, plus the hooks on virtual members resolved the loader's way (by the
                       overrider's name, else the class's vtable) against the manifest
    hardware_status    tested / unverified / inactive / incompatible, from src/dusk/mods/hardware_findings.inc for that
                       exact id, version and SHA-256 (the file the game's mod browser compiles); anything unlisted is
                       unverified
    compatible         both: a package the console showed to be inactive or incompatible is never compatible, is not
                       counted in the capacity profiles and, provided as a required or optional package, blocks ready.
                       One of the five official packages found only in --catalog is reported there (tier catalog).

  Without --elf/--symdb (a "static only" run, before the final ELF exists) the packages are read and judged
  as far as they can be -- loader structure, refused imports, services, capacity -- and the verdict is
  "FINAL ELF VALIDATION PENDING": never READY.

  check_h37_candidate.py --elf dusklight.elf --symdb dusklight.symdb.raw --nro dusklight.nro \\
      --packages DIR --symgen SYMGEN --source-root REPO --tag H37-c2 --report h37-candidate.json

Writes the report whatever the outcome; exit 0 only when the candidate is ready.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_symbol_manifest  # noqa: E402
import elf_audit  # noqa: E402
import gen_required_symbols  # noqa: E402
import host_services  # noqa: E402
import image_surface  # noqa: E402
import package_audit  # noqa: E402
import h38_policy  # noqa: E402

OFFICIAL = {
    "dev.aleks2.loader_lab": "loader_lab.dusk",
    "dev.twilitrealm.cosmetics": "cosmetics.dusk",
    "com.melonspeedruns.rollandsprint": "rollandsprint.dusk",
    "dev.twilitrealm.randomizer": "randomizer.dusk",
}
# H37-c6: packages whose library must be the switch-arm64 one (compiled with the game's own devkitA64 libstdc++).
# Randomizer's iostreams died inside the game's libstdc++ when built with Ubuntu's GCC 13 headers (H37-c5).
REQUIRE_SWITCH_ARM64 = ("dev.twilitrealm.randomizer",)
OPTIONAL = {"dev.twilitrealm.luau": "luau_runtime.dusk"}
# H38-c5: five official catalog packages whose only gap was exports (docs/switch-h38-mods.md). Optional: absent never
# blocks, a provided one must pass (package_audit.blocks) and, being compatible, takes part in the capacity profiles.
OPTIONAL_H38 = {
    "alexandre.cooper_hat.texture_selector": "cowboy_link.dusk",         # Cowboy Link 1.1.0
    "dev.automata.deferred_fog": "deferred_fog.dusk",                    # Deferred Fog 2.0.0
    "com.agustinfuac.fairy_companion": "fairy_companion.dusk",           # Fairy Companion 1.0.0
    "com.kite.kh2camera": "kh2_camera.dusk",                             # KH2 Camera 1.0.1
    "org.dusklight.tp_classic_buttons": "tp_classic_buttons.dusk",       # TP Classic Modern Controller UI 1.3.1
}
CATALOG = ("com.dusklight.twilit_essentials", "dev.bezide.dawnlight", "dev.albt.albw")
NRO_BUILD_ID = 0x40  # NroStart (0x10) + NroHeader.BuildId (0x30), as elf2nro writes it


def git_identity(root: pathlib.Path) -> dict:
    def git(*args):
        return subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True, check=True).stdout
    try:
        commit = git("rev-parse", "HEAD").strip()
        status = git("status", "--porcelain", "--untracked-files=no").splitlines()
    except (OSError, subprocess.CalledProcessError) as error:
        return {"commit": None, "dirty": None, "error": str(error)}
    # As BuildIdentity.cmake: the hand-edited documentation does not make a build dirty.
    changed = [line for line in status if not line.endswith("platforms/switch/README.md") and " docs/" not in line]
    return {"commit": commit, "dirty": 1 if changed else 0, "changed": [line[3:] for line in changed]}


def package_id(path: pathlib.Path) -> str:
    with zipfile.ZipFile(path) as z:
        return json.loads(z.read("mod.json"))["id"]


def find_packages(directory: pathlib.Path, wanted) -> dict:
    """{mod id: path} of the .dusk files in `directory` whose mod.json id is in `wanted`."""
    found = {}
    if directory.is_dir():
        for path in sorted(directory.glob("*.dusk")):
            try:
                mod_id = package_id(path)
            except (KeyError, ValueError, zipfile.BadZipFile, OSError):
                continue
            if mod_id in wanted:
                found.setdefault(mod_id, path)
    return found


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", type=pathlib.Path, help="the linked ELF; without it (and --symdb) the run is static only")
    ap.add_argument("--symdb", type=pathlib.Path, help="the uncompressed manifest of --elf")
    ap.add_argument("--nro", type=pathlib.Path, help="the NRO made from --elf")
    ap.add_argument("--packages", required=True, type=pathlib.Path, help="directory with the official .dusk files")
    ap.add_argument("--catalog", type=pathlib.Path, help="directory with the catalog .dusk files (default: --packages)")
    ap.add_argument("--require-catalog", action="store_true",
                    help="all three catalog packages must be present AND compatible, or the candidate is not ready "
                         "(default: a catalog package that is absent or incompatible is reported in the JSON and "
                         "does not affect ready)")
    ap.add_argument("--symgen", required=True)
    ap.add_argument("--source-root", required=True, type=pathlib.Path)
    ap.add_argument("--sdk-include", type=pathlib.Path,
                    help="the SDK include directory with mods/svc/*.h (the services' versions); default: <source-root>/sdk/include")
    ap.add_argument("--profile", choices=("h37", "h38"), default="h37",
                    help="h38 (check_h38_candidate.py): Twilit Essentials required with --require-twilit, every "
                         "switch-arm64 package judged by check_switch_module.py, the combined hook pages checked "
                         "against the loader's arena table, the sources manifest's version pins enforced")
    ap.add_argument("--require-twilit", action="store_true", default=h38_policy.env_flag(),
                    help="h38 only: Twilit Essentials is a required package (default: $ALEKS2_H38_REQUIRE_TWILIT)")
    ap.add_argument("--manifest", type=pathlib.Path,
                    help="h38 only: the sources manifest with the version pins (default: <source-root>/platforms/"
                         "switch/mods/h38-sources.json)")
    ap.add_argument("--hardware-findings", type=pathlib.Path,
                    help="h38 only: the console findings (default: <source-root>/src/dusk/mods/hardware_findings.inc)")
    ap.add_argument("--tag", default="")
    ap.add_argument("--report", required=True, type=pathlib.Path)
    args = ap.parse_args(argv)
    if (args.elf is None) != (args.symdb is None):
        ap.error("--elf and --symdb go together (omit both for a static-only run)")
    static_only = args.elf is None
    h38 = args.profile == "h38"
    pins = h38_policy.pinned_versions(args.manifest or args.source_root / "platforms/switch/mods/h38-sources.json") if h38 else {}
    switch_only = h38_policy.SWITCH_ONLY_IDS if h38 else REQUIRE_SWITCH_ARM64
    hardware = {"findings": [], "switch_variants": {}}
    findings_problem = None
    if h38:
        findings_path = args.hardware_findings or args.source_root / h38_policy.HARDWARE_FINDINGS
        try:
            hardware = h38_policy.hardware_findings(findings_path)
        except (OSError, ValueError) as error:
            findings_problem = f"the console findings could not be read: {error}"

    problems = []
    report = {"schema": "dusklight-aleks-h38-candidate" if h38 else "dusklight-aleks-h37-candidate",
              "version": 2, "tag": args.tag, "validation": "static-only" if static_only else "final-elf"}
    if h38:
        report["require_twilit"] = bool(args.require_twilit)
        if findings_problem:
            problems.append(findings_problem)
    report.update(git_identity(args.source_root))
    if report.get("dirty") != 0:
        problems.append(f"the sources are not a clean commit (dirty={report.get('dirty')}): "
                        + ", ".join(report.get("changed", [])[:10]))

    elf = manifest = None
    exports, symtab, relocs, runtime = {}, [], {}, []
    elf_id = b""
    if static_only:
        problems.append("FINAL ELF VALIDATION PENDING: no --elf/--symdb, so no import, by-name hook or "
                        "interceptability was checked against the linked image")
        report["elf_build_id"] = report["manifest_build_id"] = None
    else:
        try:
            elf = elf_audit.Elf(args.elf.read_bytes(), args.elf.name)
            elf_id = check_symbol_manifest.elf_build_id(elf)
            symdb_id, manifest = check_symbol_manifest.read_symdb(args.symdb.read_bytes())
        except (check_symbol_manifest.CheckError, elf_audit.ElfError, OSError) as error:
            print(f"check_h37_candidate: {error}", file=sys.stderr)
            return 2
        report["elf_build_id"] = elf_id.hex()
        report["manifest_build_id"] = symdb_id.hex()
        if symdb_id != elf_id:
            problems.append(f"stale symbol manifest: made for {symdb_id.hex()}, the ELF is {elf_id.hex()}")
    if args.nro and not args.nro.is_file():
        problems.append(f"{args.nro} is missing: the delivered file was not checked against the ELF")
    elif args.nro and not static_only:
        nro = args.nro.read_bytes()
        nro_id = nro[NRO_BUILD_ID:NRO_BUILD_ID + 0x20]
        report["nro"] = {"file": args.nro.name, "build_id": nro_id.hex(), "sha256": hashlib.sha256(nro).hexdigest()}
        if nro[0x10:0x14] != b"NRO0" or nro_id[:len(elf_id)] != elf_id or any(nro_id[len(elf_id):]):
            problems.append(f"the NRO's build id {nro_id.hex()} is not the ELF's {elf_id.hex()}")
        if h38:
            # H38-c4: never UNKNOWN-VERSION, never a version of zero (the NACP and the string in the image).
            report["nro"]["nacp_version"] = h38_policy.nro_nacp_version(nro)
            for why in h38_policy.version_rejections(nro, args.elf.read_bytes()):
                problems.append(why)
    elif not static_only:
        problems.append("no --nro: the delivered file was not checked against the ELF")

    if not static_only:
        exports = image_surface.exports(elf)
        symtab = image_surface.symbols(elf)
        relocs = image_surface.relative_relocations(elf)
        runtime, runtime_problems = image_surface.image_runtime(elf, symtab, relocs)
        if runtime is None:
            runtime_problems.append("no image runtime table: this ELF was not built with the native mod loader")
            runtime = []
        problems += runtime_problems
        report["exported_symbols"] = len(exports)
        report["image_runtime"] = sorted(e["name"] for e in runtime)

    # --- the packages: required, optional, catalog ---------------------------------------------
    catalog_dir = args.catalog or args.packages
    # H38-c6: one of the five official packages that is only in --catalog is reported there, as a catalog package.
    catalog_found = find_packages(catalog_dir, CATALOG + (tuple(OPTIONAL_H38) if h38 else ()))
    plan = []  # (mod id, tier, path or None)
    for mod_id, file_name in OFFICIAL.items():
        plan.append((mod_id, "required", args.packages / file_name))
    for mod_id, file_name in {**OPTIONAL, **(OPTIONAL_H38 if h38 else {})}.items():
        path = args.packages / file_name
        if not path.is_file() and h38 and mod_id in OPTIONAL_H38 and catalog_found.get(mod_id):
            plan.append((mod_id, "catalog", catalog_found[mod_id]))
            continue
        plan.append((mod_id, "optional", path if path.is_file() else None))
    for mod_id in CATALOG:
        if h38 and mod_id == h38_policy.TWILIT_ID and args.require_twilit:
            # Required: twilit_essentials.dusk in --packages, else the package with its id in --catalog.
            default = args.packages / "twilit_essentials.dusk"
            plan.append((mod_id, "required", default if default.is_file() else catalog_found.get(mod_id) or default))
        elif h38 and mod_id == h38_policy.ALBW_ID and (args.packages / "albw.dusk").is_file():
            # H38: ALBW built for switch-arm64 and put in --packages is an optional package: absent never blocks, a
            # provided one must pass, and it is judged as a switch-arm64 module. The Linux package of the catalog is
            # never taken in its place (it is reported below as a rejected alternative).
            plan.append((mod_id, "optional", args.packages / "albw.dusk"))
        else:
            plan.append((mod_id, "catalog", catalog_found.get(mod_id)))

    required, packages, details = [], {}, {}
    sdk_include = args.sdk_include or args.source_root / "sdk" / "include"
    sdk_services = host_services.sdk_services(sdk_include)
    if not sdk_services:
        problems.append(f"{sdk_include}: no service definitions: the services were not checked")
    for mod_id, tier, path in plan:
        entry = packages[mod_id] = {"tier": tier, "file": path.name if path else None, "rejections": []}
        reject = entry["rejections"].append  # why this package is refused; whether that blocks ready: package_audit.blocks
        if path is None or not path.is_file():
            if package_audit.blocks(tier, args.require_catalog, present=False):
                reject(f"{path} is missing" if tier == "required" else f"no package with this id in {catalog_dir}")
                entry["status"] = "missing"
            else:
                entry["status"] = "not provided"
            continue
        data = path.read_bytes()
        entry["sha256"] = hashlib.sha256(data).hexdigest()
        try:
            if package_id(path) != mod_id:
                reject(f"{path.name}: its mod.json says {package_id(path)}, not {mod_id}")
            lines, denied = gen_required_symbols.requirements(mod_id, path, args.symgen)
            details[mod_id] = package_audit.describe(path, args.symgen)
        except (KeyError, ValueError, zipfile.BadZipFile, elf_audit.ElfError, subprocess.CalledProcessError,
                SystemExit) as error:
            reject(f"cannot read the package: {error}")
            entry["status"] = "unreadable"
            entry["compatible"] = False
            continue
        entry.update({k: v for k, v in details[mod_id].items() if k not in ("id", "file", "services")})
        entry["status"] = "read"
        if mod_id in switch_only and entry["platform"] != "switch-arm64":
            reject(f"the library the loader would take is {entry['library']}: this package must ship "
                   "lib/switch-arm64/mod.so (built with devkitA64's libstdc++, the game's); a linux-aarch64 build uses "
                   "the GCC 13 C++ library layout and crashed the game in std::locale")
        if h38:
            for r in h38_policy.module_rejections(path, entry["platform"]):
                reject(r)
            bad_version = h38_policy.version_rejection(mod_id, entry.get("version"), pins)
            if bad_version:
                reject(bad_version)
        required += lines
        try:
            imports = gen_required_symbols.service_imports(path, args.symgen)
        except (KeyError, ValueError, elf_audit.ElfError, subprocess.CalledProcessError) as error:
            reject(f"cannot read its service imports: {error}")
            imports = []
        service_problems = host_services.check_services(imports, sdk_services)
        entry["services"] = {"required": sum(1 for i in imports if i["service_id"].startswith(host_services.PREFIX)),
                             "problems": service_problems,
                             "required_ids": details[mod_id]["services"]["required"],
                             "optional_ids": details[mod_id]["services"]["optional"],
                             "optional_unavailable": [p["id"] for p in service_problems if not p["blocking"]]}
        if denied:
            reject(f"imports the loader refuses by name: {', '.join(sorted(denied))}")
            entry["denied"] = sorted(denied)
        if entry["loader_blockers"]:
            reject(f"the loader would refuse its library ({entry['library']}): " + "; ".join(entry["loader_blockers"]))
        blocking = host_services.first_blocking(service_problems)
        entry["compatible"] = None if static_only else True
        if blocking or entry["loader_blockers"] or denied:
            entry["compatible"] = False
        if blocking:
            reject(f"not compatible with this build: {blocking}")

    if not static_only:
        checked = check_symbol_manifest.check_requirements(
            elf, symtab, relocs, exports, {e["name"] for e in runtime}, manifest, required, {})
        for mod_id, result in checked["mods"].items():
            entry = packages[mod_id]
            mine = entry["compatible"]
            entry.update(result)
            entry["compatible"] = bool(result["compatible"] and mine)
            if not result["compatible"]:
                i, h = result["imports"], result["hooks"]
                what = i["missing"] + h["missing"] + h["ambiguous"] + h["not_interceptable"]
                entry["rejections"].append(f"not compatible with this build: {'; '.join(what)}")
            if h38:
                entry["hook_notes"] = h38_policy.hook_notes(result["hooks"])
                entry["rejections"] += entry["hook_notes"]
        if h38:
            # H38-c6: hooks on virtual members, resolved as the loader resolves them (H38-c5's Deferred Fog stayed
            # inactive on the console over one of these while this gate, which only read by-name hooks, said READY).
            for mod_id, tier, path in plan:
                entry = packages[mod_id]
                if entry.get("status") != "read":
                    continue
                try:
                    members = gen_required_symbols.member_hooks(path, args.symgen)
                except (KeyError, ValueError, elf_audit.ElfError, subprocess.CalledProcessError) as error:
                    entry["rejections"].append(f"cannot read its member hooks: {error}")
                    continue
                unresolved = h38_policy.member_hook_problems(members, manifest)
                entry["member_hooks"] = {"declared": len(members),
                                         "virtual": sum(1 for h in members if h.get("virtual_slot") is not None),
                                         "unresolved": unresolved}
                if unresolved:
                    entry["compatible"] = False
                    entry["rejections"].append("not compatible with this build: a hook on a virtual member does not "
                                               "resolve: " + "; ".join(unresolved))
        report["hook_targets"] = checked.get("hook_targets", {})

    # --- the tier policy: which rejections decide ready ---------------------------------------------
    for mod_id, tier, _path in plan:
        entry = packages[mod_id]
        entry["blocks_ready"] = bool(entry["rejections"]) and package_audit.blocks(tier, args.require_catalog)
        if entry["rejections"]:
            entry["compatible"] = False
        if entry["blocks_ready"]:
            problems += [f"{mod_id}: {r}" for r in entry["rejections"]]
        if h38 and entry.get("status") in ("read", "unreadable"):
            # H38-c6: the static result, then what the console showed for these exact bytes; `compatible` needs both.
            entry["static_compatible"] = entry.get("compatible")
            verdict = h38_policy.hardware_verdict(hardware, mod_id, entry.get("version"), entry.get("sha256", ""))
            entry["hardware_status"] = verdict["status"]
            entry["hardware_reason"] = verdict["reason"]
            if mod_id in hardware["switch_variants"]:
                entry["switch_variant"] = hardware["switch_variants"][mod_id]
            if verdict["status"] in ("inactive", "incompatible"):
                entry["compatible"] = False
                entry["hardware_rejection"] = f"{verdict['status']} on the console: {verdict['reason']}"
                if package_audit.blocks(tier, args.require_catalog):
                    entry["blocks_ready"] = True
                    problems.append(f"{mod_id}: {entry['hardware_rejection']}")
    report["require_catalog"] = bool(args.require_catalog)
    report["catalog_findings_not_blocking"] = {m: e["rejections"] for m, e in packages.items()
                                               if e["rejections"] and not e["blocks_ready"]}

    # --- capacity and combinations --------------------------------------------------------------
    read = {m: d for m, d in details.items()}
    report["capacity_notes"] = package_audit.capacity_notes(read)
    by_name = {}
    for mod_id, kind, _group, name in ((l[0], l[1], l[2], l[3]) for l in required):
        if kind == "hook":
            by_name.setdefault(mod_id, set()).add(name)
    shared = {}
    for a in sorted(by_name):
        for b in sorted(by_name):
            if a < b and by_name[a] & by_name[b]:
                shared[(a, b)] = sorted(by_name[a] & by_name[b])
    if h38:
        # A package the console showed to be incompatible is never part of what is loaded.
        loaded = {m: d["estimated_pages"] for m, d in read.items()
                  if (packages[m]["tier"] in ("required", "optional") or not packages[m]["rejections"])
                  and packages[m].get("hardware_status") != "incompatible"}
        profiles = h38_policy.capacity_profiles(loaded)
        # The required profile: everything but ALBW (base + Randomizer + Twilit) against the arena table.
        report["combined_capacity"] = profiles["primary"]
        report["capacity_profiles"] = profiles
        problems += report["combined_capacity"]["problems"]
        report["capacity_notes"] += report["combined_capacity"]["notes"]
        albw_alone = profiles.get("albw")
        if albw_alone and albw_alone["problems"] and packages[h38_policy.ALBW_ID]["tier"] == "optional":
            # ALBW that cannot be hooked even with the base alone would not load: not a package to deliver.
            reason = "base + ALBW: " + albw_alone["problems"][0]
            packages[h38_policy.ALBW_ID]["rejections"].append(reason)
            packages[h38_policy.ALBW_ID]["compatible"] = False
            packages[h38_policy.ALBW_ID]["blocks_ready"] = True
            problems.append(f"{h38_policy.ALBW_ID}: {reason}")
        for key in ("albw_randomizer", "twilit_albw"):
            profile = profiles.get(key)
            if profile is None:
                continue
            what = {"albw_randomizer": "base + Randomizer + ALBW", "twilit_albw": "base + Twilit + ALBW"}[key]
            verdict = ("NOT SUPPORTED, never enable Twilit and ALBW together" if key == "twilit_albw" else
                       "alternative profile, only after ALBW alone ran on the console" if profile["within_arena_table"]
                       else "does not fit the arena table: not to be enabled")
            report["capacity_notes"].append(
                f"{what}: up to {profile['pages']} hook pages = {profile['arena_objects']} CodeMemory objects "
                f"(table {package_audit.CODE_ARENA_MAX}): {verdict}")
        # The catalog (Linux) copies that are not what the console would run: the ones in --catalog whose package of
        # the same id was taken from --packages. Reported with the reasons; never blocking and never installed.
        alternatives = {}
        for mod_id, found in catalog_found.items():
            if mod_id not in CATALOG:
                continue
            chosen = next((p for m, _t, p in plan if m == mod_id), None)
            if found is None or chosen is None or not chosen.is_file() or found.resolve() == chosen.resolve():
                continue
            try:
                d = package_audit.describe(found, args.symgen)
                reasons = list(d["loader_blockers"])
                if d["platform"] != "switch-arm64":
                    reasons.insert(0, f"its library is {d['library']}: only a switch-arm64 build is accepted")
                reasons += h38_policy.module_rejections(found, d["platform"])
                alternatives[mod_id] = {"file": found.name, "sha256": hashlib.sha256(found.read_bytes()).hexdigest(),
                                        "version": d.get("version"), "platform": d["platform"],
                                        "accepted": False, "chosen_instead": chosen.name, "reasons": reasons}
            except (KeyError, ValueError, zipfile.BadZipFile, elf_audit.ElfError, subprocess.CalledProcessError,
                    SystemExit) as error:
                alternatives[mod_id] = {"file": found.name, "accepted": False, "chosen_instead": chosen.name,
                                        "reasons": [f"cannot read the package: {error}"]}
        report["catalog_alternatives_rejected"] = alternatives
    report["combinations"] = package_audit.combinations(read, shared)
    report["not_recommended_together"] = [c for c in report["combinations"]
                                          if not c["within_planned_budget"] or c["shared_by_name_targets"]]
    report["notes"] = [
        "ready means the static checks passed against this ELF; it does not certify behaviour on the console",
        "compatible one at a time does not mean compatible together: see not_recommended_together",
    ]
    if h38:
        report["notes"].append("static_compatible is what this gate checked; hardware_status is what the console showed "
                               "for that exact package (src/dusk/mods/hardware_findings.inc); compatible needs both")
    report["packages"] = packages
    report["problems"] = problems
    report["ready"] = not problems
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=1, sort_keys=True) + "\n")

    print(f"{'H38' if h38 else 'H37'} candidate {args.tag or '(untagged)'}: commit {(report.get('commit') or '?')[:10]}, "
          f"dirty={report.get('dirty')}, {'STATIC ONLY' if static_only else 'build ' + elf_id.hex()}")
    for mod_id, tier, _path in plan:
        p = packages[mod_id]
        if p.get("status") != "read":
            print(f"  [{tier}] {mod_id}: {p.get('status', 'NOT CHECKED').upper()}")
            continue
        blocking = sum(1 for sp in p["services"]["problems"] if sp["blocking"])
        head = f"  [{tier}] {mod_id} {p.get('version', '')}: {p['mode']}, {p['library_bytes']} bytes"
        if "imports" in p:
            i, h = p["imports"], p["hooks"]
            print(f"{head}: {'compatible' if p['compatible'] else 'INCOMPATIBLE'}: imports {i['found']}/{i['required']}"
                  f" ({len(i['image_runtime'])} through the image runtime table), by-name hooks {h['found']}/{h['required']},"
                  f" services {p['services']['required'] - blocking}/{p['services']['required']}")
        else:
            print(f"{head}: static only (symbols and hooks not checked), services "
                  f"{p['services']['required'] - blocking}/{p['services']['required']}")
        if "hardware_status" in p:
            static = {True: "compatible", False: "NOT compatible"}.get(p["static_compatible"], "pending")
            print(f"      static {static}; console: {p['hardware_status'].upper()}"
                  + (f" ({p['hardware_reason']})" if p["hardware_status"] != "unverified" else ""))
        if p["rejections"] and not p["blocks_ready"]:
            print(f"      does NOT block ready ({tier} package, --require-catalog is off): "
                  + " | ".join(p["rejections"])[:600])
        for note in p.get("unvalidated", []):
            print(f"      UNVALIDATED: {note}")
        print(f"      hooks declared up to {p['estimated_pages']} pages = {p['estimated_arena_objects']} CodeMemory "
              f"object(s); relocations {sum(p['relocations'].values())}; optional services unavailable: "
              f"{', '.join(p['services']['optional_unavailable']) or 'none'}")
    for note in report["capacity_notes"]:
        print(f"  capacity: {note}")
    for c in report["not_recommended_together"]:
        why = []
        if not c["within_planned_budget"]:
            why.append(f"up to {c['pages']} pages = {c['arena_objects']} objects")
        if c["shared_by_name_targets"]:
            why.append(f"both hook {len(c['shared_by_name_targets'])} target(s) by name")
        print(f"  together: {c['mods'][0]} + {c['mods'][1]}: {'; '.join(why)}")
    for mod_id, alt in report.get("catalog_alternatives_rejected", {}).items():
        print(f"  catalog copy {alt['file']} of {mod_id} REJECTED as an alternative (the console runs "
              f"{alt['chosen_instead']}): {' | '.join(alt['reasons'])[:400]}")
    for problem in problems:
        print(f"  problem: {problem}")
    if static_only:
        print(f"  FINAL ELF VALIDATION PENDING (static checks only; never READY): {args.report}")
    else:
        print(f"  {'READY' if report['ready'] else 'NOT READY'} (static compatibility and loadability against this "
              f"ELF; not a statement about gameplay): {args.report}")
    return 0 if report["ready"] else 1


if __name__ == "__main__":
    sys.exit(main())
