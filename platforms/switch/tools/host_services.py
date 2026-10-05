#!/usr/bin/env python3
"""aleks2 (H37-c4): the game's own services on the Switch build, and whether a mod's imports of them
can be satisfied -- the same verdict src/dusk/mods/loader/mod_services.cpp gives Dry Run on the
console, for the candidate gate, from the package alone.

A native mod lists the services it imports (id, major, minimum minor, optional) in its library's
metadata records; `symgen modmeta` prints them. The versions the build offers are the SDK's own
macros (sdk/include/mods/svc/*.h -- each service module registers with them). What this platform
cannot offer is a short, explicit list that a test keeps honest against the sources it comes from.
Only dev.twilitrealm.dusklight.* services are judged: another mod's service exists once that mod
has loaded, which the loader decides.

H37-c1..c3 never asked: the candidate gate said Randomizer was compatible (881/881 imports, 17/17
hooks) and the console refused it at activation, "missing: dev.twilitrealm.dusklight.file".
"""
import pathlib
import re

PREFIX = "dev.twilitrealm.dusklight."

# Services registered with available() false on a Switch build, and the source that says so
# (test-host-services.sh checks each line still says it). The file service is NOT here since H37-c4:
# file_availability.hpp makes it exist without a picker.
UNAVAILABLE_ON_SWITCH = {
    "net": "platforms/switch/source/borealis_net_switch.cpp",
    "websocket": "platforms/switch/source/borealis_ws_switch.cpp",
}

_ID = re.compile(r'#define\s+(\w+)_SERVICE_ID\s+(?:DUSKLIGHT_SERVICE_ID_PREFIX\s+"([a-z_0-9]+)"|"([a-z_0-9.]+)")')
_VERSION = re.compile(r"#define\s+(\w+)_SERVICE_(MAJOR|MINOR)\s+(\d+)u?")


def sdk_services(sdk_include: pathlib.Path) -> dict:
    """{service id: (major, minor)} from sdk/include/mods/svc/*.h."""
    ids, versions = {}, {}
    for header in sorted((pathlib.Path(sdk_include) / "mods" / "svc").glob("*.h")):
        for line in header.read_text(encoding="utf-8", errors="replace").splitlines():
            m = _ID.search(line)
            if m:
                ids[m.group(1)] = PREFIX + m.group(2) if m.group(2) else m.group(3)
                continue
            m = _VERSION.search(line)
            if m:
                versions.setdefault(m.group(1), {})[m.group(2)] = int(m.group(3))
    return {ids[name]: (v["MAJOR"], v["MINOR"]) for name, v in versions.items()
            if name in ids and "MAJOR" in v and "MINOR" in v}


def check_services(imports: list, services: dict, unavailable=frozenset(UNAVAILABLE_ON_SWITCH)) -> list:
    """Verdicts for `imports` ({service_id, major, min_minor, optional}) -- one dict per problem:
    {id, major, status, blocking, message}. Same rules, statuses and wording as mod_services.cpp."""
    problems = []
    for im in imports:
        sid = im["service_id"]
        if not sid.startswith(PREFIX):
            continue
        optional = bool(im.get("optional", False))
        offered = {m: n for (i, (m, n)) in services.items() if i == sid}  # major -> minor (one each)
        suffix = sid[len(PREFIX):]
        wording, status = "Mod requires a service that is unavailable", "unavailable"
        if im["major"] in offered:
            if suffix in unavailable:
                pass
            elif offered[im["major"]] < im["min_minor"]:
                wording, status = "Mod requires a newer Dusklight version", "needs_newer_dusklight"
            else:
                continue
        elif not offered:
            status = "unknown"
        elif im["major"] > max(offered):
            wording, status = "Mod requires a newer Dusklight version", "needs_newer_dusklight"
        else:
            wording, status = "Mod must be updated for the current Dusklight version", "mod_needs_update"
        problems.append({"id": sid, "major": im["major"], "status": status, "blocking": not optional,
                         "message": f"{wording} (missing: {sid})"})
    return problems


def first_blocking(problems: list) -> str:
    blocking = [p for p in problems if p["blocking"]]
    if not blocking:
        return ""
    line = blocking[0]["message"]
    if len(blocking) > 1:
        line += f" (and {len(blocking) - 1} more service problem(s))"
    return line
