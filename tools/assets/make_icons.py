#!/usr/bin/env python3
"""The WebUI's tab icon and home-screen icon, from the logo (romfs/gfx/logo.png):
the yellow mark in the middle, cropped square so it fills the tab instead of
shrinking to a sliver.

    python tools/assets/make_icons.py
"""
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GFX = os.path.join(ROOT, "romfs", "gfx")


def main():
    logo = Image.open(os.path.join(GFX, "logo.png")).convert("RGBA")
    w, h = logo.size
    # The mark sits between the two arms: about the middle 55%.
    side = h
    x0 = (w - int(w * 0.55)) // 2
    crop = logo.crop((x0, 0, x0 + int(w * 0.55), h))
    sq = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    sq.alpha_composite(crop, ((side - crop.width) // 2, 0))
    fav = sq.resize((64, 64), Image.LANCZOS)
    fav.save(os.path.join(GFX, "favicon.png"), optimize=True)
    # Home screen (iPhone draws it on its own rounded square): on a dark tile.
    t = 180
    tile = Image.new("RGBA", (t, t), (20, 22, 30, 255))
    mark = sq.resize((150, 150), Image.LANCZOS)
    tile.alpha_composite(mark, ((t - 150) // 2, (t - 150) // 2 + 4))
    tile.convert("RGB").save(os.path.join(GFX, "touch_icon.png"), optimize=True)
    print("favicon.png, touch_icon.png")


if __name__ == "__main__":
    main()
