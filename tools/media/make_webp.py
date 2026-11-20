"""Turn a folder of PNG frames into an animated WebP for the README.

The frames come from the app's tour recorder (include/ui_tour.h: one frame per
1/30 s of the app's clock, in switch/TomoToolNX/rec/<clip>/ on the SD card) or
from a browser screencast. Frames are scaled, thinned to the asked frame rate,
and the quality is lowered step by step until the file fits the size limit.

    python tools/media/make_webp.py <frames dir> <out.webp> [--width 960] [--fps 30]
           [--src-fps 30] [--quality 80] [--max-mb 4] [--hold-last 1.2] [--trim-start 0]
"""
import argparse
import glob
import os
import sys

from PIL import Image


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("frames", help="folder of numbered PNG frames")
    ap.add_argument("out", help="the .webp to write")
    ap.add_argument("--width", type=int, default=960, help="width of the clip (default 960)")
    ap.add_argument("--fps", type=float, default=30, help="frame rate of the clip (default 30)")
    ap.add_argument("--src-fps", type=float, default=30, help="frame rate the frames were recorded at (default 30)")
    ap.add_argument("--quality", type=int, default=80, help="starting quality, 0-100 (default 80)")
    ap.add_argument("--max-mb", type=float, default=4.0, help="size limit in MB (default 4)")
    ap.add_argument("--hold-last", type=float, default=1.2, help="seconds the last frame stays (default 1.2)")
    ap.add_argument("--trim-start", type=int, default=0, help="frames to skip at the start")
    a = ap.parse_args()

    files = sorted(glob.glob(os.path.join(a.frames, "*.png")))[a.trim_start:]
    if not files:
        sys.exit("no frames in " + a.frames)
    step = a.src_fps / a.fps
    picks, t = [], 0.0
    while int(round(t)) < len(files):
        picks.append(files[int(round(t))])
        t += step
    frames = []
    for f in picks:
        im = Image.open(f).convert("RGB")
        if im.width != a.width:
            im = im.resize((a.width, round(im.height * a.width / im.width)), Image.LANCZOS)
        frames.append(im)
    each = int(round(1000 / a.fps))
    durations = [each] * len(frames)
    durations[-1] += int(a.hold_last * 1000)

    q = a.quality
    while True:
        frames[0].save(a.out, save_all=True, append_images=frames[1:], duration=durations, loop=0,
                       quality=q, method=6, allow_mixed=True)
        mb = os.path.getsize(a.out) / 1048576
        if mb <= a.max_mb or q <= 40:
            break
        q -= 8
    print("%s: %d frames, %dx%d, %.1f s, quality %d, %.2f MB" % (
        a.out, len(frames), frames[0].width, frames[0].height, sum(durations) / 1000, q, mb))


if __name__ == "__main__":
    main()
