#!/usr/bin/env bash
# H38: the real pinned Twilit Essentials source (7682060, mod.json 2.5.0) with the real patch series, through the
# pipeline's own export_tree/apply_patches: the tree is the manifest's, the patches apply, stb_image loses its TLS on
# the switch-arm64 toolchain only, the hermetic-CMake guards are in, and the game-thread assertion is there.
# TWILIT_SRC is a checkout holding the pinned commit (else the commit is fetched from the manifest's URL).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
py=python3; command -v "$py" >/dev/null 2>&1 || py=python
out="${1:-${TMPDIR:-/tmp}/dusk-twilit-patches-test}"
rm -rf "$out"
mkdir -p "$out"
"$py" - "$root" "$out" <<'PYEOF'
import json, os, pathlib, sys
root, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
sys.path.insert(0, str(root / "platforms/switch/tools"))
import build_switch_mods as b
manifest = b.load_manifest(root / "platforms/switch/mods/h38-sources.json")
mod = manifest["mods"]["twilit_essentials"]
overrides = {"twilit_essentials": os.environ["TWILIT_SRC"]} if os.environ.get("TWILIT_SRC") else {}
checkout = b.resolve_source("twilit_essentials", mod, root, out, overrides)
tree = out / "tree"
b.export_tree("twilit_essentials", mod, checkout, tree)
mod_json = json.loads((tree / "mod.json").read_text())
assert mod_json["version"] == "2.5.0" and mod_json["id"] == mod["mod_id"], mod_json
patches = b.apply_patches("twilit_essentials", tree, root / mod["patches"])
assert [p["name"] for p in patches] == ["0001-stb-image-no-thread-locals.patch", "0002-hermetic-cmake-for-switch-arm64.patch"], patches
lib = (tree / "collection-lib/CMakeLists.txt").read_text()
assert "if (DUSK_SWITCH_ARM64_MODULE_TOOLCHAIN)\n    target_compile_definitions(collection_lib PRIVATE STBI_NO_THREAD_LOCALS)" in lib
src = (tree / "collection-lib/src/tex_replacements.cpp").read_text()
assert "tex_rep_on_owner_thread()" in src and src.count("stbi_load_from_memory") == 1
# the only stb_image user is that one function: nothing else in the mod or the library calls it or starts a thread
for path in list(tree.glob("src/**/*.cpp")) + list(tree.glob("collection-lib/src/*.cpp")):
    text = path.read_text(errors="replace")
    if path.name != "tex_replacements.cpp":
        assert "stbi_" not in text, f"{path} uses stb_image"
    assert "std::thread" not in text.replace("std::thread::id", "").replace("<thread>", "") or path.name == "tex_replacements.cpp", f"{path} starts a thread"
top = (tree / "CMakeLists.txt").read_text()
assert top.count("NOT DUSK_SWITCH_ARM64_MODULE_TOOLCHAIN") == 3, "three hermetic guards"
try:
    b.apply_patches("twilit_essentials", tree, root / mod["patches"])
    raise SystemExit("FAIL: the patches applied twice")
except b.PipelineError:
    pass
print(f"twilit patches: pinned tree {mod['source']['tree'][:10]}, 2 patches apply, stb_image TLS-free on switch-arm64 only, "
      "single user on the game thread, hermetic CMake: ok")
PYEOF
