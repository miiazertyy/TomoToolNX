#!/usr/bin/env python3
"""Pictures for the island map: each ground (and its path version) and each
object, shop and house, from the ltdimages/images renders on GitHub.

  romfs/items/Floor_<code>.png    each ground as a little block (the game's landscaping menu)
  romfs/items/<asset>.png         each object's render (its own colour variant)
  include/map_art_data.h          map actor / ground hash -> picture name

The console draws them in the map's lists, its panel and on the map itself;
its WebUI gets them from /mapart/<key>.png (no internet needed).

    python tools/assets/build_map_art.py [--cache DIR] [--jobs 16]

Downloads are kept in the same cache as build_items.py
(assetsfromthegametouseinthetomotoolnxapp/ltdimages, ignored by git).
"""
import argparse
import concurrent.futures as cf
import json
import os
import re
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_items import RAW, ROOT, TREE, fetch  # noqa: E402

OBJ = 64      # px, an object's longest side
FLOOR = 56    # px, a ground block's longest side


def table(src, start, pattern):
    a = src.index(start)
    b = src.index("};", a)
    return re.findall(pattern, src[a:b])


def asset_for(key, files):
    """The render for an actor key: the key itself, its colour variant, or
    the base object (the first colour)."""
    if key + ".png" in files:
        return key
    m = re.match(r"^(.*?)_(\d\d)$", key)
    base = m.group(1) if m else key
    if m and "%s_%s.png" % (base, m.group(2)) in files:
        return "%s_%s" % (base, m.group(2))
    for cand in (base, base + "_00"):
        if cand + ".png" in files:
            return cand
    # UGC shapes: ObjUgc13_02 -> ObjUgc13
    m = re.match(r"^(ObjUgc\d\d)_\d\d$", key)
    if m and m.group(1) + ".png" in files:
        return m.group(1)
    if key == "HouseOneRoom" and "HouseOneRoom_Nikoniko.png" in files:
        return "HouseOneRoom_Nikoniko"
    # No render of its own: the closest picture there is, rather than a
    # coloured block (the fountains and the park share the fountain park's;
    # the apartments and the houses players design, the standard house).
    stand_in = {"FacilityFountain": "FacilityFountainPark_Large", "FacilityFountainPark": "FacilityFountainPark_Large",
                "FacilityPark": "FacilityFountainPark_Large", "HouseDollHouse": "HouseOneRoom_Nikoniko"}.get(key)
    if not stand_in and re.match(r"^HouseUgc\d\d_\d\d$", key):
        stand_in = "HouseOneRoom_Nikoniko"
    if not stand_in and key.startswith("ObjSignboardTutorial"):
        stand_in = "ObjRoadSign"
    m2 = re.match(r"^(ObjUgc\d\d)_\d\d$", key)
    if not stand_in and m2:
        for v in ("_01", "_00", "_02"):
            if m2.group(1) + v + ".png" in files:
                stand_in = m2.group(1) + v
                break
    if stand_in and stand_in + ".png" in files:
        return stand_in
    return None


def object_thumb(src, dst):
    im = Image.open(src).convert("RGBA")
    bbox = im.getchannel("A").point(lambda v: 255 if v > 12 else 0).getbbox()
    if bbox:
        im = im.crop(bbox)
    im.thumbnail((OBJ, OBJ), Image.LANCZOS)
    q = im.quantize(colors=128, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
    q.save(dst, "PNG", optimize=True)
    return os.path.getsize(dst)


def floor_thumb(src, dst):
    """A ground is a little block (the game's landscaping menu): kept whole."""
    im = Image.open(src).convert("RGBA")
    bbox = im.getchannel("A").point(lambda v: 255 if v > 12 else 0).getbbox()
    if bbox:
        im = im.crop(bbox)
    im.thumbnail((FLOOR, FLOOR), Image.LANCZOS)
    q = im.quantize(colors=128, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
    q.save(dst, "PNG", optimize=True)
    return os.path.getsize(dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", default=os.path.join(ROOT, "assetsfromthegametouseinthetomotoolnxapp", "ltdimages"))
    ap.add_argument("--jobs", type=int, default=16)
    a = ap.parse_args()
    os.makedirs(a.cache, exist_ok=True)
    out_dir = os.path.join(ROOT, "romfs", "items")
    os.makedirs(out_dir, exist_ok=True)

    src = open(os.path.join(ROOT, "source", "map_data.cpp"), encoding="utf-8").read()
    tiles = table(src, "const TileInfo TILE_INFO[]", r'\{ 0x([0-9a-f]+)u, "([^"]+)",')
    actors = table(src, "const ActorInfo ACTOR_INFO[]", r'\{ 0x([0-9a-f]+)u, "([^"]+)",')

    tree_path = os.path.join(a.cache, "_tree.json")
    if not os.path.exists(tree_path):
        fetch(TREE, tree_path)
    files = {e["path"] for e in json.load(open(tree_path))["tree"] if e["path"].endswith(".png")}

    tile_rows = [(int(h, 16), "Floor_" + code) for h, code in tiles if "Floor_" + code + ".png" in files]
    # The game's own English names (ltdimages_mapping.json): one per object,
    # or one per variant where the variants are different things (hedges, trees).
    mapping = json.load(open(os.path.join(ROOT, "ltdimages_mapping.json"), encoding="utf-8"))["Objects"]
    names = {}
    for label, base in mapping.items():
        label = label.replace("[Player Name]'s ", "")
        names.setdefault(base, []).append(label[:1].upper() + label[1:])

    def name_for(key):
        m = re.match(r"^(.*?)_(\d\d)$", key)
        base, var = (m.group(1), int(m.group(2))) if m else (key, 0)
        ns = names.get(base)
        if not ns:
            return None
        return ns[var] if len(ns) > 1 and var < len(ns) else ns[0]

    actor_rows = []
    for h, key in actors:
        asset = asset_for(key, files)
        if asset:
            actor_rows.append((int(h, 16), asset, name_for(key)))
    need = sorted({n for _, n in tile_rows} | {r[1] for r in actor_rows})
    print("grounds: %d/%d  objects: %d/%d  pictures: %d" % (len(tile_rows), len(tiles), len(actor_rows), len(actors), len(need)))

    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(fetch, RAW + n + ".png", os.path.join(a.cache, n + ".png")): n for n in need}
        for f in cf.as_completed(futs):
            try:
                f.result()
            except Exception as e:  # skipped below
                print("  failed:", futs[f], e)

    total = 0
    made = set()
    for n in need:
        p = os.path.join(a.cache, n + ".png")
        if not os.path.exists(p):
            continue
        dst = os.path.join(out_dir, n + ".png")
        total += floor_thumb(p, dst) if n.startswith("Floor_") else object_thumb(p, dst)
        made.add(n)
    print("pictures: %d, %.2f MB" % (len(made), total / 1e6))

    tile_rows = sorted(r for r in tile_rows if r[1] in made)
    actor_rows = sorted(r for r in actor_rows if r[1] in made)
    out = ["// map_art_data.h: made by tools/assets/build_map_art.py, do not edit.",
           "// The picture (romfs:/items/<asset>.png, from the ltdimages renders) of each",
           "// map ground and object, by hash (TILE_INFO / ACTOR_INFO), sorted by hash.",
           "#pragma once",
           "#include <cstdint>",
           "",
           "// name: the game's English name for the object (nullptr: none known).",
           "struct MapArtInfo { uint32_t hash; const char* asset; const char* name; };",
           "static const MapArtInfo MAP_TILE_ART[] = {"]
    out += ['    { 0x%08xu, "%s", nullptr },' % r for r in tile_rows]
    out += ["};", "static const MapArtInfo MAP_ACTOR_ART[] = {"]
    out += ['    { 0x%08xu, "%s", %s },' % (h, a, json.dumps(n) if n else "nullptr") for h, a, n in actor_rows]
    out += ["};",
            "static const int MAP_TILE_ART_COUNT = (int)(sizeof(MAP_TILE_ART) / sizeof(MAP_TILE_ART[0]));",
            "static const int MAP_ACTOR_ART_COUNT = (int)(sizeof(MAP_ACTOR_ART) / sizeof(MAP_ACTOR_ART[0]));",
            ""]
    with open(os.path.join(ROOT, "include", "map_art_data.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    print("include/map_art_data.h: %d grounds, %d objects" % (len(tile_rows), len(actor_rows)))


if __name__ == "__main__":
    main()
