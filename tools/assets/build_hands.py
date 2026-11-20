#!/usr/bin/env python3
"""The player's hands, big, for the hand preview (Player > Skin tone, A):
the game's photos in the six skin tones, from "all hands.zip" in the game
assets folder, cropped and scaled down to what the screen shows.

  romfs/gfx/hands/pick_open_<t>.png   the pinch, open (arm from above)
  romfs/gfx/hands/pick_close_<t>.png  the pinch, closed
  romfs/gfx/hands/point_<t>.png       pointing left (arm from the right)
  romfs/gfx/hands/hold_<t>.png        holding a profile card (from below)
  romfs/gfx/hands/thumb_<t>.png       the thumb drawn over the card

    python tools/assets/build_hands.py [assets folder]
"""
import io
import os
import sys
import zipfile

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "assetsfromthegametouseinthetomotoolnxapp")
OUT = os.path.join(ROOT, "romfs", "gfx", "hands")

# (zip name pattern, out name, longest side). The hold hand and its thumb keep
# one scale (the thumb sits on the hand), so they are not cropped.
POSES = [
    ("Hand_Pick_Open%02d^w.png", "pick_open", 500, True),
    ("Hand_Pick_Close%02d^w.png", "pick_close", 500, True),
    ("Hand_PushPointing_%02d^w.png", "point", 560, True),
    ("Hand_MiiTouchProfileHand_%02d^w.png", "hold", 300, False),
    ("Hand_MiiTouchProfileHandThumb_%02d^w.png", "thumb", 117, False),
]


def main():
    os.makedirs(OUT, exist_ok=True)
    z = zipfile.ZipFile(os.path.join(SRC, "all hands.zip"))
    total = 0
    for pattern, name, side, crop in POSES:
        for t in range(6):
            im = Image.open(io.BytesIO(z.read(pattern % t))).convert("RGBA")
            if crop:
                bbox = im.getchannel("A").point(lambda v: 255 if v > 8 else 0).getbbox()
                if bbox:
                    im = im.crop(bbox)
            k = side / max(im.size) if name not in ("hold", "thumb") else side / 512.0 if name == "hold" else side / 200.0
            im = im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS)
            path = os.path.join(OUT, "%s_%d.png" % (name, t))
            im.save(path, "PNG", optimize=True)
            total += os.path.getsize(path)
    print("hands: %d pictures, %.2f MB" % (len(POSES) * 6, total / 1e6))


if __name__ == "__main__":
    main()
