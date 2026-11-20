#!/usr/bin/env python3
"""Pictures of the big facilities seen from straight above, for the island
map: the plot is covered the way the game builds it, instead of a small
front view floating in a large plot (the fountain park is 6 x 10 tiles).

    python tools/assets/build_map_tops.py <the game's RomFS folder>

Reads Model/<name>.bfres.zs and Tex/<name>_<material>_Alb.bntx.zs from the
game's files (dumped from your own copy; tools/mii/ltdfs.py can extract them)
and writes romfs/items/Top_<actor>.png at 32 pixels per tile. The console
draws them on the map, turned with the object.
"""
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "mii"))
import nxfmt  # noqa: E402
from raster import Canvas  # noqa: E402

PX = 32           # pixels per tile
UNIT = 2.0        # world units per tile (the park's floor: 11.7 x 19.7 units on 6 x 10 tiles)
OUT = os.path.join(HERE, "..", "..", "romfs", "items")

# actor -> (tiles across, tiles down, [(model, (x, y, z) offset)])
# The fountain park is the park with the fountain in its northern circle: the
# park's balloon stands at z -5.7, the fountain's own at z -0.65.
FACILITIES = {
    "FacilityPark": (6, 10, [("Park", (0, 0, 0))]),
    "FacilityFountainPark": (6, 10, [("Park", (0, 0, 0)), ("Fountain", (0, 0, -5.05))]),
}
# Colours for materials without a picture of their own.
PLAIN = {"Window": (0.90, 0.95, 1.0)}


def texture(game, name):
    p = os.path.join(game, "Tex", name + ".bntx.zs")
    if not os.path.exists(p):
        return None
    for name_, fmt, w, h, raw, info in nxfmt.bntx(nxfmt.read(p)):
        return nxfmt.decode(name_, fmt, w, h, raw, info).astype(np.float32) / 255.0
    return None


def draw_model(cv, game, name, offset):
    mod = nxfmt.bfres(nxfmt.read(os.path.join(game, "Model", name + ".bfres.zs")))["models"][0]
    for s in mod["shapes"]:
        mat = s["name"].split("__mt_")[-1]
        if mat in ("Shadow", "GpuPick", "GpuClick2") or mat.startswith("Gpu"):
            continue
        tex = texture(game, "%s_%s_Alb" % (name, mat))
        p = np.asarray(s["p"], np.float32)[:, :3] + np.asarray(offset, np.float32)
        n = np.asarray(s["n"], np.float32)[:, :3] if s.get("n") is not None else None
        # From above: x to the right, z down the picture (the map's rows), height towards us.
        pp = np.stack([p[:, 0], -p[:, 2], p[:, 1]], 1)
        nn = np.stack([n[:, 0], -n[:, 2], n[:, 1]], 1) if n is not None else None
        idx = np.asarray(s["idx"], np.int64).reshape(-1, 3)   # (faces up as stored)
        uv = np.asarray(s["uv0"], np.float32)[:, :2] if s.get("uv0") is not None else None
        rgb = (1.0, 1.0, 1.0) if tex is not None else PLAIN.get(mat, (0.92, 0.94, 0.97))
        cv.draw(pp, nn, idx, rgb, light=(-0.3, 0.35, 0.89), uv=uv if tex is not None else None, tex=tex, ambient=0.62)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    game = sys.argv[1]
    for actor, (w, h, parts) in FACILITIES.items():
        cv = Canvas(w * PX, h * PX, PX / UNIT, w * PX / 2, h * PX / 2)
        for model, off in parts:
            draw_model(cv, game, model, off)
        out = os.path.join(OUT, "Top_%s.png" % actor)
        cv.image().save(out, optimize=True)
        print("wrote", os.path.relpath(out), "%dx%d" % (w * PX, h * PX))


if __name__ == "__main__":
    main()
