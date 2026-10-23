#!/usr/bin/env python3
"""Turn shader caches taken off a console into the seeds the NRO ships in its romfs.

Two kinds (docs/history/switch-v1-roadmap.md, "Entregable B"):

  pipeline  The recipe list (pipeline_cache.db, or the Shader Tour's export
            master_pipeline_cache-*.db). Portable: every console compiles it locally
            at the "Preparing shaders" screen. This is what 1.0 ships.

              python make-shader-seed.py pipeline master_pipeline_cache-....db [more.db ...] \\
                  -o platforms/switch/romfs/initial_pipeline_cache.db

            --zstd also writes <output>.zst (what the NRO should carry: ~40x smaller;
            Aurora opens it in memory and prefers it over the plain file).

            Several inputs are merged (one row per (type, hash), the earliest
            first_frame_used). Rows whose config version is not the one this source
            tree builds are dropped (they would be ignored on the console anyway), as are
            rows that are not self-consistent.

  dawn      Dawn's compiled blobs (dawn_cache.db), keyed to the exact driver that made
            them; Aurora ignores a seed recorded for another driver. Not shipped in 1.0
            (size, driver coupling); kept for a measured experiment.

              python make-shader-seed.py dawn dawn_cache.db \\
                  --driver "NVIDIA Tegra X1 (NVK gm20b)|NVK: Mesa 26.2.3 (...)" \\
                  -o platforms/switch/romfs/initial_dawn_cache.db

The output is only ever read, so it carries nothing else and is VACUUMed. A damaged
input (the console's "database disk image is malformed") is refused: recover it first
with the sqlite3 shell (sqlite3 bad.db .recover | sqlite3 recovered.db) and pass that.
"""
import argparse
import pathlib
import re
import subprocess
import shutil
import sqlite3
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
AURORA = HERE.parent.parent.parent / "extern" / "aurora" / "lib"

PIPELINE_SCHEMA = 1
# ShaderType in extern/aurora/lib/gfx/pipeline_cache.hpp
TYPES = {0: "clear", 1: "gx", 2: "rml"}
VERSION_SOURCES = {
    0: (AURORA / "gfx" / "clear.hpp", r"ClearPipelineConfigVersion\s*=\s*(\d+)"),
    1: (AURORA / "gx" / "pipeline.hpp", r"GXPipelineConfigVersion\s*=\s*(\d+)"),
    2: (AURORA / "rmlui" / "pipeline.hpp", r"RmlPipelineConfigVersion\s*=\s*(\d+)"),
}

# Exactly what Aurora creates (prepare_pipeline_cache_db), so the seed reads like a cache.
PIPELINE_DDL = """
CREATE TABLE aurora_schema(value INTEGER);
CREATE TABLE pipeline_cache (
  type INTEGER NOT NULL,
  hash INTEGER NOT NULL,
  config_version INTEGER NOT NULL,
  config_size INTEGER NOT NULL,
  config BLOB NOT NULL,
  first_frame_used INTEGER NOT NULL,
  PRIMARY KEY (type, hash)
);
CREATE INDEX pipeline_cache_load_order_idx
  ON pipeline_cache(type, config_version, first_frame_used);
"""


def current_versions():
    versions = {}
    for kind, (path, pattern) in VERSION_SOURCES.items():
        m = re.search(pattern, path.read_text(encoding="utf-8")) if path.is_file() else None
        if m is None:
            sys.exit(f"cannot read the {TYPES[kind]} config version from {path}")
        versions[kind] = int(m.group(1))
    return versions


def open_checked(path):
    db = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        ok = db.execute("PRAGMA quick_check").fetchone()[0]
    except sqlite3.DatabaseError as exc:
        sys.exit(f"{path}: damaged ({exc}); recover it first (see --help)")
    if ok != "ok":
        sys.exit(f"{path}: damaged ({ok}); recover it first (see --help)")
    return db


def make_pipeline(args):
    versions = current_versions()
    rows = {}
    stats = {"read": 0, "old_version": 0, "inconsistent": 0, "duplicate": 0}
    for src in args.sources:
        if not src.is_file():
            sys.exit(f"no such file: {src}")
        db = open_checked(src)
        try:
            schema = [r[0] for r in db.execute("SELECT value FROM aurora_schema")]
        except sqlite3.DatabaseError:
            schema = []
        if schema != [PIPELINE_SCHEMA]:
            sys.exit(f"{src}: pipeline cache schema {schema}, this tree uses [{PIPELINE_SCHEMA}]")
        for t, h, ver, size, config, first in db.execute(
                "SELECT type, hash, config_version, config_size, config, first_frame_used FROM pipeline_cache"):
            stats["read"] += 1
            if t not in TYPES or size != len(config) or first < 0 or first > 0xFFFFFFFF:
                stats["inconsistent"] += 1
                continue
            if ver != versions[t]:
                stats["old_version"] += 1
                continue
            key = (t, h)
            if key in rows:
                stats["duplicate"] += 1
                if first < rows[key][4]:
                    rows[key] = (t, h, ver, config, first)
                continue
            rows[key] = (t, h, ver, config, first)
        db.close()
    if not rows:
        sys.exit("no usable rows: play (or run the Shader Tour) with this build first")

    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp) / "seed.db"
        db = sqlite3.connect(work)
        db.executescript(PIPELINE_DDL)
        db.execute("INSERT INTO aurora_schema VALUES (?)", (PIPELINE_SCHEMA,))
        db.executemany(
            "INSERT INTO pipeline_cache (type, hash, config_version, config_size, config, first_frame_used) "
            "VALUES (?, ?, ?, ?, ?, ?)",
            [(t, h, v, len(c), c, f) for (t, h, v, c, f) in sorted(rows.values(), key=lambda r: (r[4], r[0], r[1]))])
        db.commit()
        db.execute("VACUUM")
        db.close()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(work, args.output)

    by_type = {}
    payload = 0
    for (t, _h, _v, c, _f) in rows.values():
        by_type[TYPES[t]] = by_type.get(TYPES[t], 0) + 1
        payload += len(c)
    size = args.output.stat().st_size
    print(f"{len(rows)} recipes ({', '.join(f'{k} {v}' for k, v in sorted(by_type.items()))}); "
          f"{payload} bytes of configs -> {args.output} ({size} bytes)")
    print(f"read {stats['read']}, dropped: {stats['old_version']} other config version, "
          f"{stats['inconsistent']} inconsistent, {stats['duplicate']} duplicates merged")
    print("versions: " + ", ".join(f"{TYPES[k]} {v}" for k, v in sorted(versions.items())))
    if args.zstd:
        packed = args.output.with_name(args.output.name + ".zst")
        subprocess.run(["zstd", "-q", "-f", "-19", str(args.output), "-o", str(packed)], check=True)
        back = subprocess.run(["zstd", "-q", "-d", "-c", str(packed)], check=True, capture_output=True).stdout
        if back != args.output.read_bytes():
            sys.exit(f"{packed} does not decompress to {args.output}")
        print(f"zstd -19: {packed} ({packed.stat().st_size} bytes, round trip checked)")
        if not args.keep_plain:
            args.output.unlink()
            print(f"removed {args.output} (the NRO carries the .zst; --keep-plain keeps both)")
    print("Rebuild with a fresh configure: romfs is assembled at configure time "
          "(platforms/switch/CMakeLists.txt).")
    return 0


def make_dawn(args):
    if "|" not in args.driver:
        sys.exit('--driver must be "<device>|<driver>" as the session log prints them')
    src = args.sources[0]
    open_checked(src).close()
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp) / "seed.db"
        shutil.copyfile(src, work)
        db = sqlite3.connect(work)
        db.execute("PRAGMA journal_mode=delete")
        rows = db.execute("SELECT COUNT(*), COALESCE(SUM(size), 0), COALESCE(SUM(length(value)), 0) FROM cache").fetchone()
        if rows[0] == 0:
            sys.exit("that cache is empty: play with the build first")
        db.execute("CREATE TABLE IF NOT EXISTS aurora_meta(key TEXT PRIMARY KEY, value TEXT)")
        db.execute("REPLACE INTO aurora_meta(key, value) VALUES ('driver', ?)", (args.driver,))
        for table in ("used_keys", "prune_keys"):
            db.execute(f"DROP TABLE IF EXISTS {table}")
        db.commit()
        db.execute("VACUUM")
        db.close()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(work, args.output)
    # V1-rc2: the console reads the driver from this text file, not through SQLite (romfs paths).
    args.output.with_name(args.output.name + ".driver").write_text(args.driver + "\n", encoding="utf-8")
    print(f"{rows[0]} blobs, {rows[1]} bytes uncompressed ({rows[2]} stored) -> {args.output} "
          f"({args.output.stat().st_size} bytes); driver: {args.driver}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kind", choices=["pipeline", "dawn"])
    ap.add_argument("sources", nargs="+", type=pathlib.Path)
    ap.add_argument("-o", "--output", type=pathlib.Path, required=True)
    ap.add_argument("--driver", help='dawn only: "<device>|<driver>"')
    ap.add_argument("--zstd", action="store_true", help="pipeline: also write <output>.zst")
    ap.add_argument("--keep-plain", action="store_true", help="with --zstd: keep the uncompressed file too")
    args = ap.parse_args()
    if args.kind == "dawn":
        if not args.driver or len(args.sources) != 1:
            sys.exit("dawn: one source and --driver")
        return make_dawn(args)
    return make_pipeline(args)


if __name__ == "__main__":
    sys.exit(main())
