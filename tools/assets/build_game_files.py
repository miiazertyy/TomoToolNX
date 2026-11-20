#!/usr/bin/env python3
"""The few files of the game the 3D item view reads, carried by the app:
the item tables and every model they name, as the game stores them
(zstd-compressed, under 1 MB together).

    python tools/assets/build_game_files.py <the game's RomFS folder>

Copies RSDB/UgcClothParam, UgcObj and UgcTypeParam, then each model those
tables name (Model/<name>.bfres.zs), into romfs/game/ with the same layout,
which is where the app looks for them (source/game_data.cpp). Dumped from
your own copy; tools/mii/ltdfs.py can extract them.
"""
import glob
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "mii"))
import nxfmt  # noqa: E402

OUT = os.path.join(HERE, "..", "..", "romfs", "game")
TABLES = ("UgcClothParam.Product.", "UgcObj.Product.", "UgcTypeParam.Product.")


def model_file(path):
    """'Work/Model/UgcEditor/X/output/Y.fmdb' -> 'X' (the folder before output); else the name."""
    name = path.rsplit("/", 1)[-1].replace(".fmdb", "")
    if "/output/" in path:
        return path[: path.index("/output/")].rsplit("/", 1)[-1]
    return name


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    game = sys.argv[1]
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(os.path.join(OUT, "RSDB"))
    os.makedirs(os.path.join(OUT, "Model"))
    models, total = set(), 0
    for prefix in TABLES:
        found = glob.glob(os.path.join(game, "RSDB", prefix + "*"))
        if not found:
            sys.exit("table not found: " + prefix)
        shutil.copy2(found[0], os.path.join(OUT, "RSDB"))
        total += os.path.getsize(found[0])
        rows = nxfmt.byml(nxfmt.read(found[0]))
        for r in rows:
            if prefix.startswith("UgcClothParam") and isinstance(r.get("ClothID"), str):
                models.add(r["ClothID"].rsplit("/", 1)[-1].split(".")[0])
            if prefix.startswith("UgcObj"):
                det = r.get("UvDetail") if isinstance(r.get("UvDetail"), dict) else {}
                path = det.get("Model") or ((r.get("ModelList") or [None])[0])
                if path:
                    models.add(model_file(path))
    missing = []
    for m in sorted(models):
        src = os.path.join(game, "Model", m + ".bfres.zs")
        if not os.path.exists(src):
            missing.append(m)
            continue
        shutil.copy2(src, os.path.join(OUT, "Model"))
        total += os.path.getsize(src)
    print("%d models, %.2f MB -> %s" % (len(models) - len(missing), total / 1048576, os.path.relpath(OUT)))
    if missing:
        print("not in the game (the 3D view skips them):", ", ".join(missing))


if __name__ == "__main__":
    main()
