#!/usr/bin/env python3
"""H37-c6: library selection, the libstdc++ ABI refusal and the switch-arm64 module checks, on real AArch64 shared
objects made with clang + ld.lld (SKIPPED, not passed, when they are missing)."""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

here = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(here))
import check_switch_module  # noqa: E402
import elf_audit  # noqa: E402

failures = 0


def check(cond, what):
    global failures
    if not cond:
        print(f"FAIL: {what}")
        failures += 1


if not (shutil.which("clang") and shutil.which("ld.lld")):
    print("SKIPPED: abi policy fixtures need clang and ld.lld")
    sys.exit(0)

MODS = "int mod_meta, mod_ctx; int mod_initialize(void){return 0;} int mod_update(void){return 0;} int mod_shutdown(void){return 0;}\n"
IOS = ["_ZNSt8ios_baseC2Ev", "_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1Ev", "_ZTVSt14basic_ofstreamIcSt11char_traitsIcEE",
       "_ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E",
       "_ZNSt7__cxx1119basic_stringstreamIcSt11char_traitsIcESaIcEEC1Ev"]
# Cosmetics' six locale imports: allowed for a Linux module
LOCALE = ["_ZNKSt6locale2id5_M_idEv", "_ZNSt6locale5facetD2Ev", "_ZNSt6localeC1ERKS_", "_ZNSt6localeC1Ev",
          "_ZNSt6localeD1Ev", "_ZTINSt6locale5facetE"]
PLAIN = ["_Znwm", "_ZdlPv", "memcpy"]
INIT = ["_ZNSt8ios_base4InitC1Ev"]


def source(imports, extra=""):
    decls = "".join(f'extern char f{i}(void) __asm__("{n}");\n' for i, n in enumerate(imports))
    calls = "".join(f"f{i}();" for i in range(len(imports)))
    return decls + MODS + f"int use(void){{{calls}return 0;}}\n" + extra


def build(tmp, name, src, link=()):
    c = tmp / f"{name}.c"
    c.write_text(src)
    o = tmp / f"{name}.o"
    subprocess.run(["clang", "--target=aarch64-linux-gnu", "-O1", "-fPIC", "-ffreestanding", "-c", str(c), "-o", str(o)],
                   check=True, capture_output=True)
    so = tmp / f"{name}.so"
    subprocess.run(["ld.lld", "-shared", "-o", str(so), str(o), *link], check=True, capture_output=True)
    return so


def package(path, libs):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("mod.json", '{"id":"dev.test.abi","version":"1"}')
        for name, data in libs.items():
            z.writestr(name, data)
    return path


with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    ios = build(tmp, "ios", source(IOS)).read_bytes()
    plain = build(tmp, "plain", source(PLAIN)).read_bytes()
    init_only = build(tmp, "init", source(INIT)).read_bytes()
    locale_only = build(tmp, "locale", source(LOCALE)).read_bytes()
    LX, SW = "lib/linux-aarch64/mod.so", "lib/switch-arm64/mod.so"

    # --- library selection: the loader's own order ------------------------------------------------------------
    both = package(tmp / "both.dusk", {LX: plain, SW: ios, "lib/android-arm64/mod.so": b"x", "lib/windows-amd64/mod.dll": b"x"})
    label, data = elf_audit.select_library(both)
    check(elf_audit.library_platform(label) == "switch-arm64" and data == ios, f"switch-arm64 wins over linux: {label}")
    only_linux = package(tmp / "linux.dusk", {LX: ios})
    check(elf_audit.library_platform(elf_audit.select_library(only_linux)[0]) == "linux-aarch64", "linux when alone")
    try:
        elf_audit.select_library(package(tmp / "other.dusk", {"lib/android-arm64/mod.so": b"x", "lib/macos-arm64/mod.so": b"x"}))
        check(False, "android/macos only must be refused")
    except elf_audit.ElfError as error:
        check("switch-arm64 or linux-aarch64" in str(error), str(error))
    check(elf_audit.library_platform("randomizer.so") is None, "a loose library has no platform")

    # --- the refusal, by platform ---------------------------------------------------------------------------------
    def verdict(data):
        return elf_audit.audit_elf(elf_audit.Elf(data, "x"))["verdict"]
    v = verdict(ios)
    lx = " | ".join(v["linux_compat"]["blockers"])
    check("libstdc++ ABI not safe" in lx and "switch-arm64 library is required" in lx, f"linux_compat refuses: {lx}")
    check(not v["linux_compat"]["loadable_if_game_defines_imports"], "linux iostream module is not loadable")
    check(set(v["linux_compat"]["resolution"]["denied"]) == {n for n in IOS}, "all family imports are denied")
    check(not any("libstdc++" in b for b in v["switch_package"]["blockers"]) and v["switch_package"]["loadable"],
          f"switch_package not refused for the same imports: {v['switch_package']}")
    vp = verdict(plain)
    check(vp["linux_compat"]["loadable_if_game_defines_imports"] and not vp["linux_compat"]["blockers"],
          f"a linux module without the family is unchanged: {vp['linux_compat']}")
    vi = verdict(init_only)
    check(vi["linux_compat"]["loadable_if_game_defines_imports"], "ios_base::Init alone is allowed")
    vl = verdict(locale_only)
    check(vl["linux_compat"]["loadable_if_game_defines_imports"] and not vl["linux_compat"]["blockers"],
          f"Cosmetics' locale imports are allowed in a linux module: {vl['linux_compat']}")
    check(not any(elf_audit.unsafe_libstdcxx_abi(n) for n in LOCALE) and elf_audit.unsafe_libstdcxx_abi("_ZNSt8ios_baseC2Ev")
          and elf_audit.unsafe_libstdcxx_abi("_ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E")
          and not elf_audit.unsafe_libstdcxx_abi("localeconv") and not elf_audit.unsafe_libstdcxx_abi("_ZNSolsEi"),
          "name matcher")
    # the table the loader's test reads too: the same verdict for every name
    rows = [l.split(" ", 1) for l in (here / "abi_policy_names.txt").read_text().splitlines() if l and not l.startswith("#")]
    check(len(rows) >= 25, "table")
    for verdict_, name in rows:
        check(elf_audit.unsafe_libstdcxx_abi(name) == (verdict_ == "refused"), f"{name}: expected {verdict_}")
    # the committed requirement lists: the official linux packages pass, every stream user is refused
    req = (here.parent / "mod_required_symbols.txt").read_text().splitlines()
    users = {}
    for line in req:
        f = line.split(" ", 3)
        if len(f) == 4 and f[1] == "import" and elf_audit.unsafe_libstdcxx_abi(f[3]):
            users.setdefault(f[0], []).append(f[3])
    for mod in ("dev.twilitrealm.randomizer", "com.dusklight.twilit_essentials", "dev.bezide.dawnlight", "dev.albt.albw"):
        check(users.get(mod), f"{mod} still has stream imports that are refused in a linux build")
    for mod in ("dev.twilitrealm.cosmetics", "com.melonspeedruns.rollandsprint", "dev.aleks2.loader_lab"):
        check(not users.get(mod), f"{mod} must not be refused: {users.get(mod)}")

    # --- the switch-arm64 module checks ------------------------------------------------------------------------------
    good = package(tmp / "good.dusk", {SW: ios})
    check(check_switch_module.check(good) == [], f"good: {check_switch_module.check(good)}")
    check(check_switch_module.check(tmp / "plain.so") == [], "a loose good module")
    cases = {
        "two libs": (package(tmp / "two.dusk", {SW: ios, LX: ios}), "exactly lib/switch-arm64/mod.so"),
        "linux only": (package(tmp / "lonly.dusk", {LX: ios}), "exactly lib/switch-arm64/mod.so"),
    }
    libfoo = build(tmp, "foo", "int foo(void){return 1;}\n")
    needed = build(tmp, "needed", source(PLAIN), link=("--no-as-needed", str(libfoo)))
    cases["DT_NEEDED"] = (package(tmp / "n.dusk", {SW: needed.read_bytes()}), "DT_NEEDED")
    tls = build(tmp, "tls", source(PLAIN, "__thread int t; int get(void){return t++;}\n"))
    cases["TLS"] = (package(tmp / "t.dusk", {SW: tls.read_bytes()}), "thread-local")
    priv = build(tmp, "priv", source(PLAIN, "int _ZNSt6localeC1Ev(void){return 1;}\n"))
    cases["private runtime"] = (package(tmp / "p.dusk", {SW: priv.read_bytes()}), "private copy")
    none = build(tmp, "none", MODS)
    cases["no imports"] = (package(tmp / "i.dusk", {SW: none.read_bytes()}), "no undefined imports")
    for name, (path, text) in cases.items():
        problems = check_switch_module.check(path)
        check(any(text in p for p in problems), f"{name}: expected {text!r}, got {problems}")
    check(not check_switch_module.check(good) and subprocess.run(
        [sys.executable, str(here / "check_switch_module.py"), str(good)], capture_output=True).returncode == 0, "cli ok")
    check(subprocess.run([sys.executable, str(here / "check_switch_module.py"), str(tmp / "t.dusk")],
                         capture_output=True).returncode == 1, "cli fails on a bad module")

print("abi policy: " + ("ok" if not failures else f"{failures} failure(s)"))
sys.exit(1 if failures else 0)
