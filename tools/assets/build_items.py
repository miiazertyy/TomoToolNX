#!/usr/bin/env python3
"""Pictures and colours for the Mii items (clothes, outfit sets, pocket goods).

The WebUI shows each item with its render from the ltdimages/images repository
on GitHub (one PNG per colour: <asset>_00.png, _01.png, ...). The console has
no network to count on, so this makes, once:

  romfs/items/<asset>.png   a small picture of each item (its first colour),
  include/item_art_data.h   for each item: its picture, and the real colour of
                            each of its colour variants, for the colour dots.

A variant's colour is measured where the variants differ (the same render in
each colour, so the pixels that change are the fabric that is dyed); a
one-colour item takes its largest area of colour.

    python tools/assets/build_items.py [--cache DIR] [--jobs 16]

The downloads (about 240 MB) are kept in the cache folder, which defaults to
assetsfromthegametouseinthetomotoolnxapp/ltdimages (ignored by git).
"""
import argparse
import concurrent.futures as cf
import io
import json
import os
import re
import sys
import urllib.request

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RAW = "https://raw.githubusercontent.com/ltdimages/images/main/"
TREE = "https://api.github.com/repos/ltdimages/images/git/trees/main?recursive=1"
THUMB = 56          # px, the longest side of a picture in the app
UA = {"User-Agent": "tomotoolnx-build"}


def webui_map(html, name):
    m = re.search(r"const " + name + r"=(\{.*?\});", html)
    if not m:
        sys.exit("no %s in data/webui.html" % name)
    return json.loads(m.group(1))


def c_items(text, table, pattern):
    start = text.index(table)
    end = text.index("};", start)
    return re.findall(pattern, text[start:end])


def ssl_context():
    """Python's own certificate store is empty on some Windows setups: use
    certifi's when it is installed."""
    import ssl
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        return ssl.create_default_context()


SSL = ssl_context()


def fetch(url, path):
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return path
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=60, context=SSL) as r:
        data = r.read()
    tmp = path + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)
    return path


def opaque_pixels(im):
    im = im.convert("RGBA")
    im.thumbnail((96, 96), Image.LANCZOS)
    return im


def largest_colour(im):
    """The colour covering most of the item (8 levels a channel, then averaged)."""
    px = [p for p in opaque_pixels(im).getdata() if p[3] > 200]
    if not px:
        return (128, 128, 128)
    bins = {}
    for r, g, b, a in px:
        k = (r >> 5, g >> 5, b >> 5)
        e = bins.setdefault(k, [0, 0, 0, 0])
        e[0] += r; e[1] += g; e[2] += b; e[3] += 1
    e = max(bins.values(), key=lambda v: v[3])
    return (e[0] // e[3], e[1] // e[3], e[2] // e[3])


def variant_colours(images):
    """Each variant's colour, measured where the variants differ from each other."""
    if len(images) == 1:
        return [largest_colour(images[0])]
    ims = [opaque_pixels(im) for im in images]
    w, h = ims[0].size
    if any(im.size != (w, h) for im in ims):
        return [largest_colour(im) for im in images]
    data = [list(im.getdata()) for im in ims]
    n = len(data)
    varying = []
    for i in range(w * h):
        if any(d[i][3] <= 200 for d in data):
            continue
        mr = sum(d[i][0] for d in data) / n
        mg = sum(d[i][1] for d in data) / n
        mb = sum(d[i][2] for d in data) / n
        spread = sum(abs(d[i][0] - mr) + abs(d[i][1] - mg) + abs(d[i][2] - mb) for d in data) / n
        varying.append((spread, i))
    if len(varying) < 12:
        return [largest_colour(im) for im in images]
    varying.sort(reverse=True)
    keep = [i for s, i in varying[:max(12, len(varying) // 3)] if s > 18]
    if len(keep) < 12:
        return [largest_colour(im) for im in images]
    out = []
    for d in data:
        # The most common colour among the changing pixels (8 levels a channel).
        bins = {}
        for i in keep:
            r, g, b, _ = d[i]
            k = (r >> 5, g >> 5, b >> 5)
            e = bins.setdefault(k, [0, 0, 0, 0])
            e[0] += r; e[1] += g; e[2] += b; e[3] += 1
        e = max(bins.values(), key=lambda v: v[3])
        out.append((e[0] // e[3], e[1] // e[3], e[2] // e[3]))
    return out


def thumbnail(im, path):
    im = im.convert("RGBA")
    bbox = im.getchannel("A").point(lambda v: 255 if v > 12 else 0).getbbox()
    if bbox:
        im = im.crop(bbox)
    im.thumbnail((THUMB, THUMB), Image.LANCZOS)
    q = im.quantize(colors=96, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
    q.save(path, "PNG", optimize=True)
    return os.path.getsize(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", default=os.path.join(ROOT, "assetsfromthegametouseinthetomotoolnxapp", "ltdimages"))
    ap.add_argument("--jobs", type=int, default=16)
    a = ap.parse_args()
    os.makedirs(a.cache, exist_ok=True)
    out_dir = os.path.join(ROOT, "romfs", "items")
    os.makedirs(out_dir, exist_ok=True)

    html = open(os.path.join(ROOT, "data", "webui.html"), encoding="utf-8").read()
    cloth_img = webui_map(html, "LTD_CLOTH_IMG")
    coord_img = webui_map(html, "LTD_COORD_IMG")
    goods_img = webui_map(html, "LTD_GOODS_IMG")
    treasure_img = webui_map(html, "LTD_TREASURE_IMG")
    hdr = open(os.path.join(ROOT, "include", "belongings_data.h"), encoding="utf-8").read()
    cloths = c_items(hdr, "BL_CLOTH_ITEMS[]", r'\{(\d+),(\d+),(\d+)u,"((?:[^"\\]|\\.)*)","([^"]*)"\}')
    coords = c_items(hdr, "BL_COORD_ITEMS[]", r'\{(\d+),(\d+)u,(\d+),"((?:[^"\\]|\\.)*)"\}')
    goods = c_items(hdr, "BL_GOODS_ITEMS[]", r'\{(\d+)u,"((?:[^"\\]|\\.)*)"\}')

    tree_path = os.path.join(a.cache, "_tree.json")
    if not os.path.exists(tree_path):
        fetch(TREE, tree_path)
    files = {e["path"] for e in json.load(open(tree_path))["tree"] if e["path"].endswith(".png")}

    # What to fetch: every colour of each item that has a picture.
    jobs = []   # (kind, hash, label, asset, [file names])
    for _, cc, h, label, _cat in cloths:
        asset = cloth_img.get(label)
        if asset:
            names = [n for n in ("%s_%02d.png" % (asset, k) for k in range(int(cc))) if n in files]
            if names:
                jobs.append((0, int(h), label, asset, names))
    for _, h, cc, label in coords:
        asset = coord_img.get(label)
        if asset:
            names = [n for n in ("%s_%02d.png" % (asset, k) for k in range(int(cc))) if n in files]
            if names:
                jobs.append((1, int(h), label, asset, names))
    for h, label in goods:
        asset = treasure_img.get(label) or goods_img.get(label)
        if asset and asset + ".png" in files:
            jobs.append((2, int(h), label, asset, [asset + ".png"]))
    need = sorted({n for j in jobs for n in j[4]})
    print("items: %d  files: %d" % (len(jobs), len(need)))
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = {ex.submit(fetch, RAW + n, os.path.join(a.cache, n)): n for n in need}
        done = 0
        for f in cf.as_completed(futs):
            try:
                f.result()
            except Exception as e:  # a missing file is skipped below
                print("  failed:", futs[f], e)
            done += 1
            if done % 500 == 0:
                print("  downloaded %d / %d" % (done, len(need)))

    rows = []
    total = 0
    seen = set()
    for kind, h, label, asset, names in jobs:
        paths = [os.path.join(a.cache, n) for n in names]
        if not all(os.path.exists(p) for p in paths):
            continue
        images = [Image.open(p) for p in paths]
        cols = variant_colours(images) if kind != 2 else []
        if asset not in seen:
            total += thumbnail(images[0], os.path.join(out_dir, asset + ".png"))
            seen.add(asset)
        rows.append((kind, h, asset, cols))
    print("pictures: %d, %.1f MB" % (len(seen), total / 1e6))

    rows.sort(key=lambda r: (r[0], r[1]))
    out = ["// item_art_data.h: made by tools/assets/build_items.py, do not edit.",
           "// Each Mii item's picture (romfs:/items/<asset>.png) and the real colour",
           "// of each of its colour variants (0xRRGGBB), from the ltdimages renders.",
           "#pragma once",
           "#include <cstdint>",
           "",
           "struct ItemArtInfo { uint8_t kind; uint32_t hash; const char* asset; uint8_t colours; uint32_t rgb[8]; };",
           "// kind: 0 a piece of clothing (nameHash), 1 an outfit set (keyHash), 2 a pocket good (nameHash)",
           "static const ItemArtInfo ITEM_ART[] = {"]
    for kind, h, asset, cols in rows:
        rgb = ", ".join("0x%02x%02x%02xu" % c for c in cols[:8]) or "0u"
        out.append('    { %d, %du, "%s", %d, { %s } },' % (kind, h, asset, len(cols[:8]), rgb))
    out += ["};", "static const int ITEM_ART_COUNT = (int)(sizeof(ITEM_ART) / sizeof(ITEM_ART[0]));", ""]
    with open(os.path.join(ROOT, "include", "item_art_data.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    print("include/item_art_data.h: %d items" % len(rows))


if __name__ == "__main__":
    main()
