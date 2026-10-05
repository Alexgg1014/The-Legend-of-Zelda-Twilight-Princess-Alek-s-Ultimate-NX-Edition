"""aleks2 (H38): what the H38 candidate gate adds to the H37 one, as pure functions the gate and its tests share.

  * Twilit Essentials is a required package when ALEKS2_H38_REQUIRE_TWILIT is ON (--require-twilit), a catalog
    package (reported, never blocking) when it is not. Ordinary project builds never ask for it.
  * Every package that carries the Switch library -- and Randomizer and Twilit whatever they carry -- is judged as a
    switch-arm64 module by check_switch_module.py: no TLS or TLSDESC, no DT_NEEDED, PT_GNU_EH_FRAME, one library.
    H38 supports no TLS in a module; a package whose only build is for Linux and uses TLS is refused, not special-cased.
  * The hook pages of the packages that would be loaded together must fit the loader's real limit, the arena table
    (CODE_ARENA_MAX objects of CODE_ARENA_PAGES pages): above it the combination cannot be hooked.
  * A package pinned by version in the sources manifest must be that version.
  * A hook target that is known to be inlined away in some builds gets a precise note.
  * H38-c6: a hook on a virtual member resolves the way the loader resolves it (by its overrider's name, else through
    its class's vtable), and the console findings for exact packages (hardware_findings.inc) are reported apart from
    the static results.
"""
import json
import os
import pathlib
import re

import check_switch_module
import package_audit

TWILIT_ID = "com.dusklight.twilit_essentials"
RANDOMIZER_ID = "dev.twilitrealm.randomizer"
ALBW_ID = "dev.albt.albw"
LOADER_LAB_ID = "dev.aleks2.loader_lab"
SWITCH_ONLY_IDS = (RANDOMIZER_ID, TWILIT_ID, ALBW_ID)

# Twilit Essentials and ALBW hook this function by name; H37-c8 showed -O3 inlines it away. H38 declares it
# DUSK_NOINLINE (src/d/actor/d_a_e_rdb.cpp), so a missing symbol now means the ELF is not this source.
KNOWN_HOOK_NOTES = {
    "src/d/actor/d_a_e_rdb.cpp#damage_check":
        "Twilit Essentials and ALBW hook daE_Rdb damage_check by file-local name. H38 declares it DUSK_NOINLINE in "
        "src/d/actor/d_a_e_rdb.cpp (H37-c8 had it inlined away); if this ELF still has no such function, the ELF "
        "was not built from this source or the attribute was dropped",
}


# H38-c6: the console findings for exact packages. The game compiles the same file (src/dusk/mods/hardware_findings.hpp)
# for its mod browser and update check, so the gate and the browser cannot disagree about a package.
HARDWARE_FINDINGS = pathlib.Path("src/dusk/mods/hardware_findings.inc")  # relative to the source root
HARDWARE_STATUSES = {"Tested": "tested", "Unverified": "unverified", "Inactive": "inactive", "Incompatible": "incompatible"}
UNVERIFIED_REASON = "Not shown to work on the console for this build yet."
_FINDING = re.compile(r'^DUSK_MOD_HARDWARE\("([^"\\]+)", "([^"\\]+)", "([0-9a-f]{64}|)", (\w+), "([^"\\]*)"\)$')
_VARIANT = re.compile(r'^DUSK_MOD_SWITCH_VARIANT\("([^"\\]+)", "([^"\\]*)"\)$')


def hardware_findings(path) -> dict:
    """{"findings": [{id, version, sha256, status, reason}], "switch_variants": {id: note}} of hardware_findings.inc.
    Every DUSK_MOD_ line must parse: a line this reader does not understand is an error, never skipped."""
    findings, variants = [], {}
    for number, line in enumerate(pathlib.Path(path).read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line.startswith("DUSK_MOD_"):
            continue
        m = _FINDING.match(line)
        if m and m.group(4) in HARDWARE_STATUSES:
            findings.append({"id": m.group(1), "version": m.group(2), "sha256": m.group(3),
                             "status": HARDWARE_STATUSES[m.group(4)], "reason": m.group(5)})
            continue
        m = _VARIANT.match(line)
        if m:
            variants[m.group(1)] = m.group(2)
            continue
        raise ValueError(f"{path}:{number}: not a DUSK_MOD_HARDWARE/DUSK_MOD_SWITCH_VARIANT entry this reader knows")
    return {"findings": findings, "switch_variants": variants}


def hardware_verdict(table: dict, mod_id: str, version, sha256="") -> dict:
    """What the console showed for this exact package -- the same rule as hardware::verdict() in the game: an entry
    applies to its id and version and, when both sides know it, its SHA-256; anything else is unverified."""
    listed = False
    for f in table["findings"]:
        if f["id"] != mod_id:
            continue
        listed = True
        if f["version"] != version or (f["sha256"] and sha256 and f["sha256"] != sha256):
            continue
        return {"status": f["status"], "reason": f["reason"], "listed": True}
    return {"status": "unverified", "reason": UNVERIFIED_REASON, "listed": listed}


SYMBOL_CODE = 1 << 0     # HOOK_SYMBOL_CODE / manifest kFlagCode
SYMBOL_DUP_NAME = 1 << 4  # manifest kFlagDupName: the name maps to more than one address


def member_hook_problems(hooks, manifest: dict) -> list:
    """The virtual-member hooks of one package (symgen modmeta "member" records with a virtual_slot) that the loader
    could not resolve against this manifest ({name: (flags, rva)}), as "display: why" -- the loader's own order
    (src/dusk/mods/svc/hook.cpp, resolve_member_record): the overrider by its qualified name, else the slot of the
    class's vtable symbol. Non-virtual members are relocations to the game's symbols (the imports check covers them).
    The this-adjustment of a member pointer is not in modmeta's output: the loader still refuses a non-zero one."""
    out = []
    for hook in hooks:
        if hook.get("kind") != "member" or hook.get("virtual_slot") is None:
            continue
        display, vtable = hook.get("display") or "", hook.get("vtable") or ""
        flags = manifest.get(display, (None, None))[0] if display else None
        if flags is not None and flags & SYMBOL_CODE and not flags & SYMBOL_DUP_NAME:
            continue
        if flags is None:
            by_name = f"symbol '{display}' not found" if display else "no display name"
        elif flags & SYMBOL_DUP_NAME:
            by_name = f"'{display}' maps to more than one address"
        else:
            by_name = f"'{display}' is not a code symbol"
        vt_flags = manifest.get(vtable, (None, None))[0] if vtable else None
        if vtable and vt_flags is not None and not vt_flags & SYMBOL_DUP_NAME:
            continue
        if not vtable:
            via_vtable = "the class name is not representable as a vtable symbol"
        elif vt_flags is None:
            via_vtable = f"vtable '{vtable}' not found"
        else:
            via_vtable = f"vtable '{vtable}' maps to more than one address"
        out.append(f"{display or '<member>'}: did not resolve by name ({by_name}) and {via_vtable}")
    return out


def env_flag(name="ALEKS2_H38_REQUIRE_TWILIT") -> bool:
    return os.environ.get(name, "").strip().upper() in ("1", "ON", "TRUE", "YES")


def twilit_tier(require_twilit: bool) -> str:
    return "required" if require_twilit else "catalog"


def pinned_versions(manifest_path) -> dict:
    """{mod id: mod.json version} the sources manifest pins."""
    if manifest_path is None or not pathlib.Path(manifest_path).is_file():
        return {}
    mods = json.loads(pathlib.Path(manifest_path).read_text()).get("mods", {})
    return {m["mod_id"]: m["mod_json_version"] for m in mods.values() if m.get("mod_json_version")}


def module_rejections(path, platform) -> list:
    """Why `path`, a package whose loader library is `platform`, is not an acceptable switch-arm64 module (the reasons
    are check_switch_module's). A package built only for Linux is refused by the gate's own "must ship
    lib/switch-arm64/mod.so" rule; nothing here makes an exception for it."""
    if platform != "switch-arm64":
        return []
    return [f"switch-arm64 module check: {p}" for p in check_switch_module.check(pathlib.Path(path))]


def version_rejection(mod_id: str, version, pins: dict):
    want = pins.get(mod_id)
    if want and version != want:
        return f"mod.json version is {version}, the sources manifest pins {want}"
    return None


def combined_capacity(pages_by_mod: dict) -> dict:
    """The hook pages of the packages loaded together against the arena table. `pages_by_mod`: {mod id: pages}."""
    total = sum(pages_by_mod.values())
    cap = package_audit.capacity(total)
    out = {"mods": dict(sorted(pages_by_mod.items())), "pages": cap["pages"], "arena_objects": cap["arena_objects"],
           "arena_table": package_audit.CODE_ARENA_MAX, "within_planned_budget": cap["within_planned_budget"],
           "within_arena_table": cap["within_arena_table"], "problems": [], "notes": []}
    table_pages = package_audit.CODE_ARENA_MAX * package_audit.CODE_ARENA_PAGES
    out["table_pages"] = table_pages
    out["remaining_pages"] = table_pages - cap["pages"]
    if cap["within_arena_table"] and out["remaining_pages"] < package_audit.CODE_ARENA_PAGES:
        out["notes"].append(
            f"{cap['pages']} of {table_pages} hook pages in use: only {out['remaining_pages']} remain, so another "
            "package with hooks may not fit next to all of these")
    if not cap["within_arena_table"]:
        out["problems"].append(
            f"the packages loaded together declare up to {cap['pages']} hook pages = {cap['arena_objects']} arena "
            f"objects, more than the loader's arena table holds ({package_audit.CODE_ARENA_MAX}): they cannot all be "
            "hooked")
    elif not cap["within_planned_budget"]:
        out["notes"].append(
            f"together up to {cap['pages']} hook pages = {cap['arena_objects']} CodeMemory objects (planned "
            f"{package_audit.PLANNED_ARENA_OBJECTS}, table {package_audit.CODE_ARENA_MAX}); the kernel's pool is "
            "shared by the whole system, so this has to be shown on the console")
    return out


def capacity_profiles(pages_by_mod: dict) -> dict:
    """The combinations H38 supports, each against the arena table. `pages_by_mod`: {mod id: pages} of every package
    that would be loaded (a package that is not there simply is not in a profile).

      primary          everything but ALBW: base + Randomizer + Twilit. The required profile.
      albw             base + ALBW. ALBW's own profile: a package that cannot be hooked alone is not delivered.
      albw_randomizer  base + Randomizer + ALBW. Only after ALBW alone was measured; a note, never a problem.
      twilit_albw      never supported (the table cannot hold both): reported with its size, never a profile to run.

    `base` is every other package (loader_lab, Cosmetics, Roll and Sprint, the optional Luau runtime)."""
    albw = pages_by_mod.get(ALBW_ID)
    twilit = pages_by_mod.get(TWILIT_ID)
    randomizer = pages_by_mod.get(RANDOMIZER_ID)
    base = {m: p for m, p in pages_by_mod.items() if m not in (ALBW_ID, TWILIT_ID, RANDOMIZER_ID)}
    profiles = {"primary": combined_capacity({m: p for m, p in pages_by_mod.items() if m != ALBW_ID})}
    if albw is not None:
        profiles["albw"] = combined_capacity({**base, ALBW_ID: albw})
        if randomizer is not None:
            both = combined_capacity({**base, RANDOMIZER_ID: randomizer, ALBW_ID: albw})
            both["only_if_albw_alone_ran_on_the_console"] = True
            profiles["albw_randomizer"] = both
        if twilit is not None:
            profiles["twilit_albw"] = {**combined_capacity({**base, TWILIT_ID: twilit, ALBW_ID: albw}),
                                       "supported": False}
    return profiles


def hook_notes(hooks: dict) -> list:
    """Notes for the known hook targets among the missing, ambiguous or not interceptable ones of one package."""
    bad = list(hooks.get("missing", [])) + list(hooks.get("ambiguous", [])) + [
        h.split(":", 1)[0] for h in hooks.get("not_interceptable", [])]
    return [f"{name}: {KNOWN_HOOK_NOTES[name]}" for name in dict.fromkeys(bad) if name in KNOWN_HOOK_NOTES]


# H38-c4: the application version. A build from a checkout without version tags used to carry UNKNOWN-VERSION in its menu
# and 0.0.0.0 in its NACP (H34..H38-c3). cmake/DuskAppVersion.cmake settles it at configure; the gate refuses the NRO if it
# did not.
NACP_DISPLAY_VERSION = (0x3060, 0x3070)  # SwitchNacp display_version, 16 bytes
BAD_VERSION_MARKER = b"UNKNOWN-VERSION"


def nro_nacp_version(nro: bytes):
    """The NACP display version of an NRO (from its ASET block), or None when the NRO has no NACP."""
    import struct
    if len(nro) < 0x20 or nro[0x10:0x14] != b"NRO0":
        return None
    size = struct.unpack_from("<I", nro, 0x18)[0]
    if nro[size:size + 4] != b"ASET" or len(nro) < size + 0x38:
        return None
    _, _, nacp_off, nacp_size, _, _ = struct.unpack_from("<QQQQQQ", nro, size + 8)
    nacp = nro[size + nacp_off:size + nacp_off + nacp_size]
    if len(nacp) < NACP_DISPLAY_VERSION[1]:
        return None
    return nacp[NACP_DISPLAY_VERSION[0]:NACP_DISPLAY_VERSION[1]].split(b"\0")[0].decode("utf-8", "replace")


def version_text_rejection(text):
    """Why a version label is not acceptable (empty, UNKNOWN-VERSION, or the number 0 / 0.0.0 / 0.0.0.0), else None."""
    import re
    t = (text or "").strip()
    if not t:
        return "is empty"
    if BAD_VERSION_MARKER.decode() in t:
        return "is UNKNOWN-VERSION"
    if re.fullmatch(r"v?0(\.0)*([-+].*)?", t):
        return f"is {t}, a version of zero"
    return None


def version_rejections(nro: bytes, elf: bytes) -> list:
    """What is wrong with the version of this NRO and the ELF it was made from."""
    out = []
    nacp = nro_nacp_version(nro)
    if nacp is None:
        out.append("the NRO has no NACP (ASET block): its version could not be checked")
    else:
        why = version_text_rejection(nacp)
        if why:
            out.append(f"the NACP version {nacp!r} {why}")
    if BAD_VERSION_MARKER in elf:
        out.append("the ELF carries UNKNOWN-VERSION: the application version was not settled at configure "
                   "(cmake/DuskAppVersion.cmake)")
    return out
