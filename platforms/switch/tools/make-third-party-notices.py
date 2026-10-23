#!/usr/bin/env python3
"""aleks2 (V1-rc5): res/licenses/THIRD_PARTY_NOTICES.txt for the Switch NRO (it ships in romfs:/res/licenses) and
the release zip. Every component linked into the NRO or shipped with it, with its license text copied from the
source the build used when it is on disk, or its license and where to read it when it is not (prebuilt libraries
without a license file). Run after a build (it reads build/<preset>/_deps):

  make-third-party-notices.py --root <worktree> --build <build dir> --devkitpro <devkitPro> [--out FILE]
"""
import argparse
import pathlib

HEADER = """Third-party software in Dusklight for Nintendo Switch
=====================================================

Dusklight for Switch contains, or is linked with, the software below. Each is the property of its authors and is
used under the license shown. Where the license text is not reproduced, the license named applies and its text is at
the address given.

funchook is under the GNU General Public License version 2 or later (with a linking exception, see its text below),
and this build modifies it (the Switch platform layer: platforms/switch/source/funchook_os_switch.c, code_arena.c,
code_patch_switch.c, funchook_arena_glue.c and cmake/PatchFunchook.cmake). Its complete corresponding source, with
those changes, is distributed next to each release (funchook-source-*.tar.gz), and is available on request from the
Dusklight Switch maintainers for three years from the release.

"""

# (name, license, files relative to a root key, or None, and a URL when the text is not on disk)
COMPONENTS = [
    ("Aurora", "MIT", [("root", "extern/aurora/LICENSE")], None),
    ("borealis", "MIT", [("root", "extern/borealis/LICENSE")], None),
    ("Dawn and Tint", "BSD-3-Clause", [("deps", "dawn-src/LICENSE")], None),
    ("Abseil", "Apache-2.0", [("deps", "dawn-src/third_party/abseil-cpp/LICENSE")], None),
    ("SDL 3", "zlib", [("deps", "sdl-src/LICENSE.txt")], None),
    ("RmlUi", "MIT", [("deps", "rmlui-src/LICENSE.txt")], None),
    ("Dear ImGui", "MIT", [("deps", "imgui-src/LICENSE.txt")], None),
    ("{fmt}", "MIT", [("deps", "fmt-src/LICENSE")], None),
    ("miniz", "MIT", [("deps", "miniz-src/LICENSE")], None),
    ("PicoSHA2", "MIT", [("deps", "picosha2-src/LICENSE")], None),
    ("nlohmann/json", "MIT", [("deps", "nlohmann_json-src/LICENSE.MIT")], None),
    ("cxxopts", "MIT", [("deps", "cxxopts-src/LICENSE")], None),
    ("Tracy", "BSD-3-Clause", [("deps", "tracy-src/LICENSE")], None),
    ("xxHash", "BSD-2-Clause", [("deps", "xxhash-src/LICENSE")], None),
    ("Luau", "MIT", [("deps", "luau-src/LICENSE.txt")], None),
    ("LuaBridge3", "MIT", [("deps", "luabridge3-src/LICENSE.txt")], None),
    ("funchook", "GPL-2.0-or-later with a linking exception", [("deps", "funchook-src/LICENSE")], None),
    ("Capstone", "BSD-3-Clause", [("deps", "funchook-build/capstone-src/LICENSE.TXT")], None),
    ("FreeType", "FreeType License (FTL)", [("dkp", "portlibs/switch/licenses/switch-freetype/FTL.TXT")], None),
    ("Zstandard", "BSD-3-Clause", [("dkp", "portlibs/switch/licenses/switch-libzstd/LICENSE")], None),
    ("zlib", "zlib", [("dkp", "portlibs/switch/licenses/switch-zlib/LICENSE")], None),
    ("libpng", "libpng License", [("dkp", "portlibs/switch/licenses/switch-libpng/LICENSE")], None),
    ("bzip2", "bzip2 License", [("dkp", "portlibs/switch/licenses/switch-bzip2/LICENSE")], None),
    ("Mbed TLS", "Apache-2.0", [("dkp", "portlibs/switch/licenses/switch-mbedtls/LICENSE")], None),
    # 1.0.0: prebuilt libraries without a license file on disk: the upstream text, kept in platforms/switch/licenses.
    ("libcurl", "curl License (MIT-style)", [("root", "platforms/switch/licenses/curl-COPYING")], None),
    ("Mesa for Horizon (danfromtico/mesa-switch, Mesa 26.2.3, d4a00ea0ab): NVK, NIR, with parts of the Rust "
     "standard library", "MIT", [("root", "platforms/switch/licenses/mesa-license.rst")],
     "https://docs.mesa3d.org/license.html"),
    ("libnx", "ISC", [("root", "platforms/switch/licenses/libnx-LICENSE.md")], None),
    ("HarfBuzz", "Old MIT", [("root", "platforms/switch/licenses/harfbuzz-COPYING")], None),
    ("Expat", "MIT", [("root", "platforms/switch/licenses/expat-COPYING")], None),
    ("liblzma (XZ Utils)", "0BSD", [("root", "platforms/switch/licenses/xz-COPYING.0BSD")], None),
    ("SPIRV-Tools (in Dawn)", "Apache-2.0", [("deps", "dawn-src/third_party/spirv-tools/src/LICENSE")], None),
    ("PartitionAlloc (Chromium, in Dawn)", "BSD-3-Clause (text as for Dawn above)", None,
     "https://chromium.googlesource.com/chromium/src/+/main/LICENSE"),
    ("stb_image (Sean Barrett)", "MIT or public domain (Unlicense), see the end of stb_image.h", None,
     "https://github.com/nothings/stb/blob/master/LICENSE"),
    ("Freeverb (Jezar at Dreampoint)", "Public domain", None, None),
    ("yaml-cpp (in the bundled Randomizer)", "MIT", [("root", "platforms/switch/licenses/yaml-cpp-LICENSE")], None),
    ("SQLite", "Public domain", None, "https://www.sqlite.org/copyright.html"),
    ("Fira Sans, Fira Sans Condensed (Mozilla), Inter (Rasmus Andersson), Alegreya SC (Juan Pablo del Peral), "
     "Noto Mono (Google)", "SIL Open Font License 1.1", None, "https://openfontlicense.org/open-font-license-official-text/"),
    ("Material Symbols (Google)", "Apache-2.0 (text as for Abseil above)", None,
     "https://www.apache.org/licenses/LICENSE-2.0"),
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True, type=pathlib.Path)
    ap.add_argument("--build", required=True, type=pathlib.Path)
    ap.add_argument("--devkitpro", required=True, type=pathlib.Path)
    ap.add_argument("--out", type=pathlib.Path)
    a = ap.parse_args()
    roots = {"root": a.root, "deps": a.build / "_deps", "dkp": a.devkitpro}
    out = [HEADER]
    missing = []
    for name, license_name, files, url in COMPONENTS:
        out.append("-" * 78 + f"\n{name}\nLicense: {license_name}\n")
        if url:
            out.append(f"Text: {url}\n")
        for key, rel in files or []:
            path = roots[key] / rel
            if not path.is_file():
                missing.append(str(path))
                continue
            out.append("\n" + path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n").strip() + "\n")
        out.append("\n")
    # 1.0.0: the hand-written entries (forks and renderer sources credited with their own notices) are kept in
    # platforms/switch/licenses/extra-notices.txt and appended as they are.
    extra = a.root / "platforms" / "switch" / "licenses" / "extra-notices.txt"
    if extra.is_file():
        out.append(extra.read_text(encoding="utf-8").replace(chr(13) + chr(10), chr(10)))
    else:
        missing.append(str(extra))
    target = a.out or a.root / "res" / "licenses" / "THIRD_PARTY_NOTICES.txt"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("".join(out), encoding="utf-8", newline="\n")
    print(f"{target}: {len(COMPONENTS)} components, {target.stat().st_size} bytes")
    for m in missing:
        print(f"missing: {m}")
    return 1 if missing else 0


if __name__ == "__main__":
    raise SystemExit(main())
