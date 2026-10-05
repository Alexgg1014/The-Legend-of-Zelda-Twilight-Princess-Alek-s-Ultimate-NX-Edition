#!/usr/bin/env bash
# H38: the real pinned ALBW source (albt-dusks v0.3.0, 5cf4a36, mod.json dev.albt.albw 0.3.0) through the pipeline's own
# resolve_source/export_tree: the tree is the manifest's, it is built unpatched, it has no thread_local, and a commit,
# tree, id or version that is not the pinned one is refused. ALBW_SRC is a checkout holding the pinned commit (else the
# commit is fetched from the manifest's URL, which is the modular repository, never the full fork).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
py=python3; command -v "$py" >/dev/null 2>&1 || py=python
out="${1:-${TMPDIR:-/tmp}/dusk-albw-source-test}"
rm -rf "$out"
mkdir -p "$out"
"$py" - "$root" "$out" <<'PYEOF'
import copy, json, os, pathlib, re, sys
root, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
sys.path.insert(0, str(root / "platforms/switch/tools"))
import build_switch_mods as b
import h38_policy
manifest = b.load_manifest(root / "platforms/switch/mods/h38-sources.json")
mod = manifest["mods"]["albw"]
assert mod["source"]["url"] == "https://github.com/WadeWinningWilson/albt-dusks", mod["source"]
assert mod["mod_id"] == h38_policy.ALBW_ID == "dev.albt.albw" and mod["mod_json_version"] == "0.3.0"
overrides = {"albw": os.environ["ALBW_SRC"]} if os.environ.get("ALBW_SRC") else {}
checkout = b.resolve_source("albw", mod, root, out, overrides)
tree = out / "tree"
b.export_tree("albw", mod, checkout, tree)
mod_json = json.loads((tree / "mod.json").read_text())
assert mod_json["id"] == mod["mod_id"] and mod_json["version"] == "0.3.0", mod_json
# built unpatched: no patch directory with patches, so nothing is applied and the tree stays the pinned one
patches = b.apply_patches("albw", tree, root / mod["patches"])
assert patches == [], patches
cmake = (tree / "CMakeLists.txt").read_text()
assert "add_mod(albt_full_plugin" in cmake and "FetchDusklight.cmake" in cmake, "the modular repository's CMake"
assert mod["build"]["package_target"] == "albt_full_plugin_package"
# no thread-local storage anywhere in the sources (the loader supports none)
tls = [str(p.relative_to(tree)) for p in tree.glob("src/**/*") if p.is_file()
       and re.search(r"\bthread_local\b|__thread\b", p.read_text(errors="replace"))]
assert not tls, f"thread-local storage in {tls}"
# the full game fork is not the source: no copy of the game's own tree in the export
assert not (tree / "src/d").exists() and not (tree / "libs/JSystem").exists(), "this is the full fork, not the module"
# a source that is not the pinned one is refused: commit, tree, id and version
def broken(**changes):
    m = copy.deepcopy(mod)
    for key, value in changes.items():
        m["source"][key] = value
    return m
try:
    b.resolve_source("albw", broken(commit="0" * 40), root, out, {"albw": str(checkout)})
    raise SystemExit("FAIL: a commit that the source does not have was accepted")
except b.PipelineError:
    pass
try:
    b.export_tree("albw", broken(tree="0" * 40), checkout, out / "tree-bad")
    raise SystemExit("FAIL: a commit whose tree is not the pinned one was accepted")
except b.PipelineError:
    pass
wrong = copy.deepcopy(mod)
wrong["mod_id"] = "dev.albt.other"
wrong["mod_json_version"] = "0.4.0"
import zipfile
pkg = out / "wrong.dusk"
with zipfile.ZipFile(pkg, "w") as z:
    z.writestr("mod.json", json.dumps(mod_json))
audit = b.audit("albw", wrong, pkg, None)
assert any("mod.json id is dev.albt.albw" in p for p in audit["module_problems"]), audit["module_problems"]
assert any("pins 0.4.0" in p for p in audit["module_problems"]), audit["module_problems"]
print(f"albw source: albt-dusks tree {mod['source']['tree'][:10]} 0.3.0, unpatched, no thread_local, not the fork; "
      "wrong commit, tree, id and version refused: ok")
PYEOF
