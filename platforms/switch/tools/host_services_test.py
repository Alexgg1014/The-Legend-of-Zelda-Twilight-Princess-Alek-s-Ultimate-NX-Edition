#!/usr/bin/env python3
"""H37-c4: the candidate gate's view of a package -- which library the loader takes from a
multi-platform package (elf_audit.select_library), and whether the services it imports exist on the
Switch build (host_services.py). test-host-services.sh runs it.

With H37_PACKAGES_DIR (the official .dusk packages) and SYMGEN (symgen), Randomizer's real service
imports are read too and must be satisfied: the case H37-c3 got wrong.
"""
import io
import os
import pathlib
import re
import sys
import unittest
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
import elf_audit  # noqa: E402
import host_services as hs  # noqa: E402

P = hs.PREFIX


def imp(name, major=1, min_minor=0, optional=False):
    return {"service_id": P + name if "." not in name else name, "major": major, "min_minor": min_minor,
            "optional": optional}


def package(tmp: pathlib.Path, name: str, libs: dict) -> pathlib.Path:
    """A .dusk with mod.json and lib/<platform>/<file> entries whose bytes are the platform's name."""
    path = tmp / name
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("mod.json", '{"id":"x"}')
        for entry, data in libs.items():
            z.writestr(entry, data)
    return path


class SelectLibrary(unittest.TestCase):
    def setUp(self):
        import tempfile
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = pathlib.Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def pick(self, libs):
        label, data = elf_audit.select_library(package(self.tmp, "p.dusk", libs))
        return label.split(":", 1)[1], data

    def test_catalog_package_takes_linux_aarch64(self):
        libs = {f"lib/{p}/mod.so": p.encode() for p in
                ("android-aarch64", "ios-arm64", "linux-aarch64", "linux-x86_64", "macos-arm64", "macos-x86_64")}
        libs.update({"lib/windows-amd64/mod.dll": b"win", "lib/windows-arm64/mod.dll": b"winarm"})
        name, data = self.pick(libs)
        self.assertEqual((name, data), ("lib/linux-aarch64/mod.so", b"linux-aarch64"))

    def test_switch_build_wins_in_any_order(self):
        a = {"lib/linux-aarch64/mod.so": b"linux", "lib/switch-arm64/mod.so": b"switch"}
        b = {"lib/switch-arm64/mod.so": b"switch", "lib/linux-aarch64/mod.so": b"linux"}
        self.assertEqual(self.pick(a)[1], b"switch")
        self.assertEqual(self.pick(b)[1], b"switch")

    def test_only_switch_or_only_linux(self):
        self.assertEqual(self.pick({"lib/switch-arm64/mod.so": b"s"})[0], "lib/switch-arm64/mod.so")
        self.assertEqual(self.pick({"lib/linux-aarch64/mod.so": b"l"})[0], "lib/linux-aarch64/mod.so")

    def test_other_platforms_only_is_the_loaders_refusal(self):
        with self.assertRaises(elf_audit.ElfError) as raised:
            self.pick({"lib/windows-amd64/mod.dll": b"w", "lib/android-aarch64/mod.so": b"a"})
        text = str(raised.exception)
        self.assertIn("android-aarch64", text)
        self.assertIn("windows-amd64", text)
        self.assertIn("no switch-arm64 or linux-aarch64", text)

    def test_no_library_is_an_error(self):
        with self.assertRaises(elf_audit.ElfError):
            self.pick({})

    def test_a_loose_so_is_itself(self):
        loose = self.tmp / "mod.so"
        loose.write_bytes(b"\x7fELF-ish")
        self.assertEqual(elf_audit.select_library(loose), ("mod.so", b"\x7fELF-ish"))

    def test_a_deeper_path_is_not_a_library(self):
        # only lib/<platform>/mod.so counts, as in natives.cpp
        with self.assertRaises(elf_audit.ElfError):
            self.pick({"res/lib/linux-aarch64/mod.so": b"x"})

    def test_platform_names_are_the_loaders(self):
        natives = (ROOT / "src/dusk/mods/loader/natives.cpp").read_text(encoding="utf-8")
        loader = (ROOT / "src/dusk/switch_native_loader.cpp").read_text(encoding="utf-8")
        for platform in elf_audit.LOADER_PLATFORMS:
            self.assertIn(f'"{platform}"', natives)
            self.assertIn(f'"{platform}"', loader)
        self.assertEqual(elf_audit.LOADER_PLATFORMS, ("switch-arm64", "linux-aarch64"))


class Services(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.services = hs.sdk_services(ROOT / "sdk/include")

    def check(self, imports, **kw):
        return hs.check_services(imports, self.services, **kw)

    def test_the_sdk_defines_the_games_services(self):
        self.assertGreaterEqual(len(self.services), 25)
        self.assertEqual(self.services[P + "file"], (1, 0))
        self.assertIn(P + "gamemode", self.services)   # the id is not the macro name lower-cased
        self.assertIn(P + "audio_res", self.services)

    def test_file_service_is_offered_on_switch(self):
        self.assertNotIn("file", hs.UNAVAILABLE_ON_SWITCH)
        self.assertEqual(self.check([imp("file")]), [])

    def test_required_unavailable_blocks(self):
        problems = self.check([imp("websocket")])
        self.assertEqual(len(problems), 1)
        self.assertTrue(problems[0]["blocking"])
        self.assertEqual(problems[0]["status"], "unavailable")
        self.assertEqual(hs.first_blocking(problems),
                         f"Mod requires a service that is unavailable (missing: {P}websocket)")

    def test_optional_unavailable_does_not_block(self):
        problems = self.check([imp("websocket", optional=True), imp("net", optional=True)])
        self.assertEqual(len(problems), 2)
        self.assertFalse(any(p["blocking"] for p in problems))
        self.assertEqual(hs.first_blocking(problems), "")

    def test_c3_behaviour_is_reproduced_when_file_is_unavailable(self):
        problems = self.check([imp("file")], unavailable=frozenset({"net", "websocket", "file"}))
        self.assertEqual(hs.first_blocking(problems),
                         f"Mod requires a service that is unavailable (missing: {P}file)")

    def test_versions(self):
        major, minor = self.services[P + "gfx"]
        self.assertEqual(self.check([imp("gfx", major, minor)]), [])
        self.assertEqual(self.check([imp("gfx", major, 0)]), [])
        newer = self.check([imp("gfx", major, minor + 1)])
        self.assertEqual((newer[0]["status"], newer[0]["blocking"]), ("needs_newer_dusklight", True))
        self.assertTrue(newer[0]["message"].startswith("Mod requires a newer Dusklight version"))
        self.assertEqual(self.check([imp("gfx", major + 1, 0)])[0]["status"], "needs_newer_dusklight")
        game_major = self.services[P + "game"][0]
        older = self.check([imp("game", game_major - 1, 0)])
        self.assertEqual((older[0]["status"], older[0]["blocking"]), ("mod_needs_update", True))
        self.assertTrue(older[0]["message"].startswith("Mod must be updated for the current Dusklight version"))
        soft = self.check([imp("gfx", major, minor + 1, optional=True)])
        self.assertFalse(soft[0]["blocking"])

    def test_unknown_game_service_blocks(self):
        self.assertEqual(self.check([imp("does_not_exist")])[0]["status"], "unknown")

    def test_other_mods_services_are_left_to_the_loader(self):
        self.assertEqual(self.check([imp("com.kite.kh2hud.drive"), imp("org.example.thing", 2, 3, True)]), [])

    def test_several_problems_count_the_rest(self):
        line = hs.first_blocking(self.check([imp("net"), imp("websocket")]))
        self.assertTrue(line.endswith("(and 1 more service problem(s))"), line)

    def test_unavailable_list_matches_the_platform_sources(self):
        for suffix, source in hs.UNAVAILABLE_ON_SWITCH.items():
            text = (ROOT / source).read_text(encoding="utf-8")
            self.assertRegex(text, r"bool\s+available\(\)\s+noexcept\s*\{\s*return\s+false;\s*\}",
                             f"{source} no longer says the {suffix} service is unavailable")
            module = (ROOT / "src/dusk/mods/svc" / f"{suffix}.cpp").read_text(encoding="utf-8")
            self.assertRegex(module, r"\.available\s*=\s*\w+_available", f"{suffix}.cpp has no available()")
        # the services with an available() hook are exactly file, http, net, websocket: any other
        # service that grows one must be judged here too
        hooked = sorted(p.stem for p in (ROOT / "src/dusk/mods/svc").glob("*.cpp")
                        if re.search(r"\.available\s*=", p.read_text(encoding="utf-8")))
        self.assertEqual(hooked, ["file", "http", "net", "websocket"])

    def test_file_service_rule_is_in_the_sources(self):
        header = (ROOT / "src/dusk/mods/svc/file_availability.hpp").read_text(encoding="utf-8")
        self.assertRegex(header, r"#if defined\(__SWITCH__\)\s*inline constexpr bool kFileServiceWithoutPicker = true;")
        picker = (ROOT / "platforms/switch/source/borealis_file_select_switch.cpp").read_text(encoding="utf-8")
        self.assertIn("Capabilities capabilities() noexcept { return {}; }", picker)
        self.assertEqual(len(re.findall(r"answer_canceled\(done\)", picker)), 3)

    @unittest.skipUnless(os.environ.get("H37_PACKAGES_DIR") and os.environ.get("SYMGEN"),
                         "H37_PACKAGES_DIR and SYMGEN not set: Randomizer's real imports not read")
    def test_randomizer_satisfies_its_services(self):
        import gen_required_symbols
        path = pathlib.Path(os.environ["H37_PACKAGES_DIR"]) / "randomizer.dusk"
        imports = gen_required_symbols.service_imports(path, os.environ["SYMGEN"])
        self.assertTrue(any(i["service_id"] == P + "file" and not i["optional"] for i in imports))
        problems = self.check(imports)
        self.assertEqual(hs.first_blocking(problems), "", problems)
        stale = self.check(imports, unavailable=frozenset({"net", "websocket", "file"}))
        self.assertIn("dusklight.file", hs.first_blocking(stale))


class GateReadsTheLoadersLibrary(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("H37_PACKAGES_DIR") and os.environ.get("SYMGEN"),
                         "H37_PACKAGES_DIR and SYMGEN not set")
    def test_multi_platform_package_gives_the_same_requirements(self):
        import tempfile
        import gen_required_symbols as g
        source = pathlib.Path(os.environ["H37_PACKAGES_DIR"]) / "randomizer.dusk"
        with zipfile.ZipFile(source) as z:
            linux = z.read("lib/linux-aarch64/mod.so")
            manifest = z.read("mod.json")
        with tempfile.TemporaryDirectory() as tmp:
            multi = pathlib.Path(tmp) / "multi.dusk"
            with zipfile.ZipFile(multi, "w") as z:   # the order a catalog package has: Linux is not first
                z.writestr("mod.json", manifest)
                z.writestr("lib/android-aarch64/mod.so", b"ELF not a library")
                z.writestr("lib/ios-arm64/mod.so", b"ELF not a library")
                z.writestr("lib/linux-aarch64/mod.so", linux)
                z.writestr("lib/macos-arm64/mod.so", b"ELF not a library")
                z.writestr("lib/windows-amd64/mod.dll", b"MZ not a library")
            symgen = os.environ["SYMGEN"]
            self.assertEqual(g.requirements("dev.twilitrealm.randomizer", multi, symgen),
                             g.requirements("dev.twilitrealm.randomizer", source, symgen))
            self.assertEqual(g.service_imports(multi, symgen), g.service_imports(source, symgen))


if __name__ == "__main__":
    unittest.main(verbosity=1)
