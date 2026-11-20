#!/usr/bin/env python3
"""Draw the little flags next to each language (romfs/gfx/flag_<code>.png).

They are drawn here with Pillow, not taken from anywhere: 3:2, supersampled,
with rounded corners and a faint rim so the white ones still show on a light
theme. The console and the WebUI (/gfx/flag_<code>.png) use the same files.

    python tools/assets/make_flags.py
"""
import math
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "romfs", "gfx")
W, H, SS = 66, 44, 8          # final size, and how much bigger it is drawn first
w, h = W * SS, H * SS


def stripes(d, colours, vertical):
    n = len(colours)
    for i, c in enumerate(colours):
        if vertical:
            d.rectangle((i * w // n, 0, (i + 1) * w // n, h), fill=c)
        else:
            d.rectangle((0, i * h // n, w, (i + 1) * h // n), fill=c)


def star(d, cx, cy, r, fill, toward=None):
    """A five-pointed star; `toward` turns its top point to face (x, y)."""
    rot = 0.0 if toward is None else math.atan2(toward[0] - cx, -(toward[1] - cy))
    pts = []
    for i in range(10):
        a = rot + i * math.pi / 5
        rr = r if i % 2 == 0 else r * 0.382
        pts.append((cx + rr * math.sin(a), cy - rr * math.cos(a)))
    d.polygon(pts, fill=fill)


def band(d, x0, y0, x1, y1, width, fill):
    """A straight band of `width` from (x0, y0) to (x1, y1), overshooting the edges."""
    dx, dy = x1 - x0, y1 - y0
    n = math.hypot(dx, dy)
    ox, oy = -dy / n * width / 2, dx / n * width / 2
    ex, ey = dx / n * w, dy / n * w
    d.polygon([(x0 - ex + ox, y0 - ey + oy), (x1 + ex + ox, y1 + ey + oy),
               (x1 + ex - ox, y1 + ey - oy), (x0 - ex - ox, y0 - ey - oy)], fill=fill)


def gb(d):
    blue, red, white = (1, 33, 105), (200, 16, 46), (255, 255, 255)
    d.rectangle((0, 0, w, h), fill=blue)
    band(d, 0, 0, w, h, h * 0.20, white)
    band(d, 0, h, w, 0, h * 0.20, white)
    # The red diagonals sit off-centre, turning round the middle.
    t = h * 0.067
    o = h * 0.033
    for (x0, y0, x1, y1, sx, sy) in ((0, 0, w / 2, h / 2, 1, -1), (w / 2, h / 2, w, h, -1, 1),
                                     (0, h, w / 2, h / 2, -1, -1), (w / 2, h / 2, w, 0, 1, 1)):
        dx, dy = x1 - x0, y1 - y0
        n = math.hypot(dx, dy)
        px, py = -dy / n * o * sx, dx / n * o * sx
        poly = [(x0 + px - dy / n * t / 2, y0 + py + dx / n * t / 2), (x1 + px - dy / n * t / 2, y1 + py + dx / n * t / 2),
                (x1 + px + dy / n * t / 2, y1 + py - dx / n * t / 2), (x0 + px + dy / n * t / 2, y0 + py - dx / n * t / 2)]
        d.polygon(poly, fill=red)
    d.rectangle((w / 2 - h / 6, 0, w / 2 + h / 6, h), fill=white)
    d.rectangle((0, h / 2 - h / 6, w, h / 2 + h / 6), fill=white)
    d.rectangle((w / 2 - h / 10, 0, w / 2 + h / 10, h), fill=red)
    d.rectangle((0, h / 2 - h / 10, w, h / 2 + h / 10), fill=red)


def es(d):
    red, yellow = (170, 21, 27), (241, 191, 0)
    d.rectangle((0, 0, w, h), fill=red)
    d.rectangle((0, h / 4, w, h * 3 / 4), fill=yellow)
    # The coat of arms, simplified for a little flag: centred a third of the
    # way along, two fifths of the height tall (as on the real flag): the
    # crowned shield (castle, lion, the bars, the chains, the blue centre)
    # between the two pillars with their red bands.
    u = h
    cx, top = w / 3, h * 0.30
    gold, silver, crimson, blue, purple = (170, 135, 20), (190, 190, 190), (173, 21, 25), (28, 70, 160), (190, 70, 150)
    dark = (60, 40, 10)
    # pillars
    for sx in (-1, 1):
        px = cx + sx * u * 0.165
        d.rectangle((px - u * 0.022, top + u * 0.08, px + u * 0.022, top + u * 0.36), fill=silver, outline=dark, width=max(1, int(u * 0.004)))
        d.rectangle((px - u * 0.032, top + u * 0.065, px + u * 0.032, top + u * 0.085), fill=gold)
        d.rectangle((px - u * 0.032, top + u * 0.345, px + u * 0.032, top + u * 0.37), fill=gold)
        d.ellipse((px - u * 0.026, top + u * 0.035, px + u * 0.026, top + u * 0.07), fill=crimson, outline=gold, width=max(1, int(u * 0.006)))
        d.rectangle((px - u * 0.045, top + u * 0.17, px + u * 0.045, top + u * 0.2), fill=crimson)
    # the shield, quartered
    sw, s_top, s_bot = u * 0.13, top + u * 0.085, top + u * 0.36
    mid_x, mid_y = cx, (s_top + s_bot) / 2 - u * 0.01
    d.rounded_rectangle((cx - sw, s_top, cx + sw, s_bot), radius=u * 0.07, fill=crimson, outline=dark, width=max(1, int(u * 0.005)))
    d.rectangle((cx - sw, s_top, cx + sw, s_top + u * 0.02), fill=crimson)
    d.rectangle((mid_x, s_top + 1, cx + sw - 1, mid_y), fill=silver)                       # León
    d.ellipse((mid_x + sw * 0.25, s_top + u * 0.02, mid_x + sw * 0.8, mid_y - u * 0.012), fill=purple)
    d.rectangle((cx - sw * 0.78, s_top + u * 0.03, cx - sw * 0.22, mid_y - u * 0.01), fill=gold)  # Castilla
    for k in range(4):                                                                    # Aragón
        x0 = cx - sw + 1 + k * sw / 4
        d.rectangle((x0, mid_y, x0 + sw / 8, s_bot - u * 0.04), fill=gold)
    d.rectangle((mid_x + sw * 0.2, mid_y + u * 0.02, cx + sw * 0.8, s_bot - u * 0.05), outline=gold, width=max(1, int(u * 0.012)))  # Navarra
    d.ellipse((cx - u * 0.04, mid_y - u * 0.04, cx + u * 0.04, mid_y + u * 0.04), fill=blue, outline=crimson, width=max(1, int(u * 0.01)))
    # the crown
    d.rectangle((cx - sw * 0.95, s_top - u * 0.035, cx + sw * 0.95, s_top - u * 0.005), fill=gold)
    d.chord((cx - sw * 0.85, s_top - u * 0.14, cx + sw * 0.85, s_top + u * 0.02), 180, 360, fill=crimson, outline=gold, width=max(1, int(u * 0.012)))
    d.rectangle((cx - u * 0.006, s_top - u * 0.17, cx + u * 0.006, s_top - u * 0.12), fill=gold)


def jp(d):
    d.rectangle((0, 0, w, h), fill=(255, 255, 255))
    r = h * 0.3
    d.ellipse((w / 2 - r, h / 2 - r, w / 2 + r, h / 2 + r), fill=(188, 0, 45))


def cn(d):
    d.rectangle((0, 0, w, h), fill=(238, 28, 37))
    u = w / 30.0
    yellow = (255, 222, 0)
    big = (5 * u, 5 * u)
    star(d, big[0], big[1], 3 * u, yellow)
    for (x, y) in ((10, 2), (12, 4), (12, 7), (10, 9)):
        star(d, x * u, y * u, 1 * u, yellow, toward=big)


FLAGS = {
    "en": gb,
    "fr": lambda d: stripes(d, [(0, 85, 164), (255, 255, 255), (239, 65, 53)], True),
    "es": es,
    "de": lambda d: stripes(d, [(0, 0, 0), (221, 0, 0), (255, 206, 0)], False),
    "ru": lambda d: stripes(d, [(255, 255, 255), (0, 57, 166), (213, 43, 30)], False),
    "ja": jp,
    "zh": cn,
}


def main():
    os.makedirs(OUT, exist_ok=True)
    radius = 5 * SS
    for code, draw in FLAGS.items():
        flag = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        draw(ImageDraw.Draw(flag))
        mask = Image.new("L", (w, h), 0)
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, w - 1, h - 1), radius=radius, fill=255)
        out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        out.paste(flag, (0, 0), mask)
        # A faint rim, and a soft light along the top.
        rim = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        rd = ImageDraw.Draw(rim)
        rd.rounded_rectangle((0, 0, w - 1, h - 1), radius=radius, outline=(0, 0, 0, 70), width=SS)
        rd.rounded_rectangle((SS, SS, w - 1 - SS, h // 2), radius=radius - SS, fill=(255, 255, 255, 22))
        out = Image.alpha_composite(out, Image.composite(rim, Image.new("RGBA", (w, h), (0, 0, 0, 0)), mask))
        out = out.resize((W, H), Image.LANCZOS)
        path = os.path.join(OUT, "flag_%s.png" % code)
        out.save(path, "PNG", optimize=True)
        print(path, os.path.getsize(path))


if __name__ == "__main__":
    main()
