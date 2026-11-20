#!/usr/bin/env python3
"""Draw a Mii from its CharInfoEx with a parts pack (v3), the same way the
app does (source/mii_art.cpp): the reference for the C++ and the tool the
pictures are checked with.

    python tools/mii/compose.py PACK FILE.ltd [...]      writes FILE_new.png next to each

The 3D pieces (hairstyle, hair part, front piece, ears, and the head with
all on it: its own texture with make-up, wrinkles and stubble, the face paint
and the face texture, beard, nose) are laid down by depth, pixel by pixel,
farthest first; glasses go over all. Each pack picture is RGBA: R x 1.25
multiplies the part's colour, G lightens it, B darkens it (in hair pictures:
where the second hair colour goes), A covers."""
import math
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nxfmt  # noqa: E402

# CharInfoEx (tlmodding/charinfo-ex, MIT)
K = dict(faceFlags=0x2B, facelineType=0x2C, facelineColor=0x2D,
         wrinkleLowerType=0x2E, wrinkleLowerScale=0x2F, wrinkleLowerAspect=0x30, wrinkleLowerX=0x31, wrinkleLowerY=0x32,
         wrinkleUpperType=0x33, wrinkleUpperScale=0x34, wrinkleUpperAspect=0x35, wrinkleUpperX=0x36, wrinkleUpperY=0x37,
         makeup0=0x38, makeup0Color=0x39, makeup0Scale=0x3A, makeup0Aspect=0x3B, makeup0X=0x3C, makeup0Y=0x3D,
         makeup1=0x3E, makeup1Color=0x3F, makeup1Scale=0x40, makeup1Aspect=0x41, makeup1X=0x42, makeup1Y=0x43,
         hairType=0x44, hairColor0=0x46, hairColor1=0x47, hairTypeFront=0x48, hairTypeBack=0x49, hairStyle=0x4A,
         earType=0x4B, earScale=0x4C, earY=0x4D,
         eyeType=0x4E, eyeColor=0x4F, eyeScale=0x50, eyeAspect=0x51, eyeRotate=0x52, eyeX=0x53, eyeY=0x54, eyeShadowColor=0x55,
         hiType=0x56, hiScale=0x57, hiAspect=0x58, hiRotate=0x59, hiX=0x5A, hiY=0x5B,
         luType=0x5C, luScale=0x5D, luAspect=0x5E, luRotate=0x5F, luX=0x60, luY=0x61,
         llType=0x62, llScale=0x63, llAspect=0x64, llRotate=0x65, llX=0x66, llY=0x67,
         duType=0x68, duScale=0x69, duAspect=0x6A, duRotate=0x6B, duX=0x6C, duY=0x6D,
         dlType=0x6E, dlScale=0x6F, dlAspect=0x70, dlRotate=0x71, dlX=0x72, dlY=0x73,
         eyebrowType=0x74, eyebrowColor=0x75, eyebrowScale=0x76, eyebrowAspect=0x77, eyebrowRotate=0x78, eyebrowX=0x79,
         eyebrowY=0x7A, noseType=0x7B, noseScale=0x7C, noseY=0x7D, mouthType=0x7E, mouthColor=0x7F, mouthScale=0x80,
         mouthAspect=0x81, mouthRotate=0x82, mouthY=0x83, beardType=0x84, beardColor=0x85, beardShortType=0x86,
         beardShortColor=0x87, mustacheType=0x88, mustacheColor=0x89, mustacheScale=0x8A, mustacheAspect=0x8B,
         mustacheY=0x8C, glassType=0x8D, glassColor=0x8E, glassScale=0x8F, glassAspect=0x90, glassY=0x91,
         lensType=0x92, lensColor=0x93, moleScale=0x94, moleX=0x95, moleY=0x96)

PALETTE = [
    0x2d2828, 0x402010, 0x5c180a, 0x7c3a14, 0x787880, 0x4e3e10, 0x885818, 0xd0a04a, 0x000000, 0x6c7070, 0x663c2c, 0x605e30,
    0x4654a8, 0x387058, 0x603810, 0xa81008, 0x203068, 0xa86000, 0x787068, 0xd85208, 0xf00c08, 0xf54848, 0xf09a74, 0x8c5040,
    0x842626, 0xff7366, 0xffa6a6, 0xffc0ba, 0x732e3b, 0x991f3d, 0x8a173e, 0xb53e42, 0xc71e56, 0xb05381, 0xc7546e, 0xfa7597,
    0xfcacc9, 0xffc9d8, 0x311c40, 0x37283d, 0x4c184d, 0x6f42b3, 0x855cb8, 0xc083cc, 0xa893c9, 0xc5ace6, 0xeebefa, 0xd2c5ed,
    0x191f40, 0x123f66, 0x2a82d4, 0x57b4f2, 0x7ac5de, 0x89a6fa, 0x84bdfa, 0xa1e3ff, 0x0b2e36, 0x013d3b, 0x0d4f59, 0x236663,
    0x307e8c, 0x4faeb0, 0x7ac49e, 0x7fd4c0, 0x87e5b6, 0x0a4a35, 0x437a00, 0x027562, 0x369970, 0x4bad1a, 0x92bf0a, 0x63c788,
    0x9ee042, 0x96de7e, 0xbbf2aa, 0x99932b, 0xa69563, 0xccc039, 0xccb987, 0xd9cc82, 0xd5d96f, 0xd5e683, 0xd8fa9d, 0x7d4500,
    0xe6bb7a, 0xfee24a, 0xfade82, 0xf7ea9c, 0xfaf89b, 0xa64d1e, 0xff960d, 0xd19b69, 0xffb266, 0xffc28c, 0xe5cfb1, 0x414141,
    0x9b9b9b, 0xbebebe, 0xdcd7cd, 0xffffff,
]
# Skin tones 100-113 (sRGB): 100-109 are the system Mii library's ten
# (nn::mii FacelineColorTable), 110-113 the game's own four.
SKIN = [0xFFD3AD, 0xFFB66B, 0xDE7942, 0xFFAA8C, 0xAD5129, 0x632C18, 0xFFBEA5, 0xFFC58F,
        0x8C3C23, 0x3C2D23, 0xFCD9C0, 0xEE9167, 0xE29454, 0x583623]
# The upper lip's shade of each colour (nn::mii UpperLipColorTable, sRGB)
UPPER_LIP = [
    0x171414, 0x201008, 0x2e0c05, 0x4a230c, 0x54545a, 0x271f08, 0x52350e, 0xb18028, 0x000000, 0x4c4e4e, 0x331e16, 0x3a381d,
    0x2a3265, 0x274e3e, 0x301c08, 0x650a05, 0x101834, 0x764300, 0x544e49, 0x823018, 0x780c0c, 0x882028, 0xdc7850, 0x461e0a,
    0x4f1717, 0x99453d, 0xe68585, 0xe6a19b, 0x451c23, 0x5c1325, 0x530e25, 0x6d2528, 0x771234, 0x6a324d, 0x773242, 0xaf526a,
    0xe38cac, 0xe6abbb, 0x190e20, 0x1c141f, 0x260c27, 0x43286b, 0x50376e, 0x865c8f, 0x76678d, 0xab90cf, 0xd4a0e1, 0xb8aad5,
    0x0d1020, 0x092033, 0x1d5b94, 0x3297da, 0x5cadc8, 0x6786e1, 0x629fe1, 0x80c7e6, 0x06171b, 0x011f1e, 0x07282d, 0x184745,
    0x225862, 0x308b8d, 0x60b087, 0x63bfa9, 0x69ce9b, 0x05251b, 0x284900, 0x01463b, 0x266b4e, 0x2c8a00, 0x6e9900, 0x47b36f,
    0x82ca1f, 0x7ac860, 0x9eda8c, 0x6b671e, 0x746845, 0xa39816, 0xb8a36d, 0xc3b565, 0xbfc351, 0xbdcf64, 0xbce17d, 0x4b2900,
    0xcfa15a, 0xe5c622, 0xe1c35f, 0xded07c, 0xe1df7a, 0x642e12, 0xcc780a, 0xbc824c, 0xe69240, 0xe6a469, 0xceb696, 0x212121,
    0x7c7c7c, 0xababab, 0xc6c1b6, 0xd9d9d9,
]
MOLE = np.array([0x12, 0x0F, 0x0F], np.float32) / 255.0      # nn::mii MoleColor
SHAVE = np.array([1 / 3, 0.4, 0.4], np.float32)               # the shaved-stubble ink


def upper_lip(i):
    c = UPPER_LIP[i] if 0 <= i < 100 else 0x000000
    return np.array([(c >> 16) & 255, (c >> 8) & 255, c & 255], np.float32) / 255.0


def colour(i):
    c = 0x402010
    if 100 <= i < 114:
        c = SKIN[i - 100]
    elif 0 <= i < 100:
        c = PALETTE[i]
    return np.array([(c >> 16) & 255, (c >> 8) & 255, c & 255], np.float32) / 255.0


class Pack:
    def __init__(self, d):
        self.d = d
        self.tex, self.heads, self.hairs, self.fronts = {}, {}, {}, {}
        self.noses, self.ears, self.beards, self.glasses = {}, {}, {}, {}
        self.eye_acc, self.eye_axis = {}, {}
        self.hparts = {}
        self.size = 128
        self.z_far, self.z_near = -0.6, 0.5
        self.pics = {}
        for line in open(os.path.join(d, "index.txt"), encoding="utf-8"):
            f = line.split()
            if not f or f[0].startswith("#"):
                continue
            k = f[0]
            if k == "size":
                self.size = int(f[1])
            elif k == "depth":
                self.z_far, self.z_near = float(f[1]), float(f[2])
            elif k == "tex":
                self.tex[(f[1], int(f[2]))] = (f[3], int(f[4]), f[5] if len(f) > 5 and f[5] != "-" else "")
            elif k == "head":
                h = dict(shade=f[2], uv=f[3])
                for kv in f[4:]:
                    kk, v = kv.split("=")
                    h[kk] = v if kk in ("fl", "z") else np.array([float(x) for x in v.split(",")], np.float32)
                self.heads[int(f[1])] = h
            elif k == "hair":
                # attach: 0 alone, 1 takes a front piece, 2 also hair parts
                self.hairs[int(f[1])] = (f[2], f[3], int(f[4]))
            elif k == "front":
                self.fronts[int(f[1])] = (f[2], f[3])
            elif k == "hairpart":
                self.hparts[(int(f[1]), int(f[2]))] = (f[3], f[4])
            elif k == "nose":
                self.noses[int(f[1])] = f[2]
            elif k == "ear":
                self.ears[int(f[1])] = (f[2], f[3])
            elif k == "beard":
                self.beards[int(f[1])] = (f[2], f[3])
            elif k == "glass":
                self.glasses[int(f[1])] = (f[2], f[3])
            elif k == "eyeacc":
                # eyeacc <eye> axis=ax,ay <Cat>=x,y,scale,aspect,rotate ...
                e = {}
                for kv in f[2:]:
                    kk, v = kv.split("=")
                    vals = [float(x) for x in v.split(",")]
                    if kk == "axis":
                        self.eye_axis[int(f[1])] = tuple(vals)
                    else:
                        e[kk] = dict(x=vals[0], y=vals[1], s=vals[2], a=vals[3], r=vals[4])
                self.eye_acc[int(f[1])] = e
        self.head0 = self.heads[min(self.heads)]

    def depth(self, name):
        """A depth picture as model z (nearer is larger), -inf where empty."""
        q = self.pic(name)
        if q is None:
            return None
        v = q[..., 0] * 255.0
        return np.where(v > 0.5, self.z_far + (v - 1) / 254.0 * (self.z_near - self.z_far), -np.inf).astype(np.float32)

    def pic(self, name):
        if name not in self.pics:
            p = os.path.join(self.d, name) if name and name != "-" else ""
            self.pics[name] = np.asarray(Image.open(p).convert("RGBA"), np.float32) / 255.0 if p and os.path.exists(p) else None
        return self.pics[name]


def over(cv, rgb, a):
    k = 1.0 - a
    cv[..., :3] = rgb * a[..., None] + cv[..., :3] * k[..., None]
    cv[..., 3] = a + cv[..., 3] * k


def lit(p, tint):
    """A pack picture (R shade, G light, B dark) in a colour."""
    c = tint * (p[..., 0:1] * 1.25)
    c = c + (1.0 - c) * p[..., 1:2]
    c = c * (1.0 - p[..., 2:3])
    return np.clip(c, 0, 1)


def warp(p, N, ax, ay, s=1.0, dx=0.0, dy=0.0, mirror=False):
    """Picture p (N x N) scaled by s about its anchor (ax, ay), moved by (dx, dy),
    optionally mirrored about the canvas centre; bilinear."""
    ys, xs = np.mgrid[0:N, 0:N].astype(np.float32)
    X, Y = xs + 0.5, ys + 0.5
    if mirror:
        X = N - X
    sx = (X - ax - dx) / s + ax - 0.5
    sy = (Y - ay - dy) / s + ay - 0.5
    x0, y0 = np.floor(sx).astype(np.int32), np.floor(sy).astype(np.int32)
    fx, fy = sx - x0, sy - y0
    h, w = p.shape[:2]

    def at(yy, xx):
        ok = (xx >= 0) & (yy >= 0) & (xx < w) & (yy < h)
        out = p[np.clip(yy, 0, h - 1), np.clip(xx, 0, w - 1)].copy()
        out[~ok] = 0
        return out
    # premultiplied for the blend
    def pm(q):
        r = q.copy()
        r[..., :3] *= r[..., 3:4]
        return r
    v = (pm(at(y0, x0)) * ((1 - fx) * (1 - fy))[..., None] + pm(at(y0, x0 + 1)) * (fx * (1 - fy))[..., None] +
         pm(at(y0 + 1, x0)) * ((1 - fx) * fy)[..., None] + pm(at(y0 + 1, x0 + 1)) * (fx * fy)[..., None])
    a = v[..., 3:4]
    v[..., :3] = np.where(a > 1e-5, v[..., :3] / np.maximum(a, 1e-5), 0)
    return v


def hair_colour(p, c0, c1, back_on, bangs_on):
    """A hair picture in its colours, lit: the second colour where its masks
    say (B: two-tone back hair in the low 4 bits, bangs in the high 4).
    Straight RGBA, ready to move and lay down."""
    if p is None:
        return None
    b = np.round(p[..., 2] * 255).astype(np.int32)
    w = np.zeros(p.shape[:2], np.float32)
    if back_on:
        w += (b & 15) / 15.0
    if bangs_on:
        w += (b >> 4) / 15.0
    w = np.clip(w, 0, 1)[..., None]
    c = (c0 * (1 - w) + c1 * w) * (p[..., 0:1] * 1.25)
    c = c + (1.0 - c) * p[..., 1:2]
    return np.concatenate([np.clip(c, 0, 1), p[..., 3:4]], -1)


def put(cv, q):
    if q is not None:
        over(cv, q[..., :3], q[..., 3])


def warp_z(z, N, ax, ay, s=1.0, dx=0.0, dy=0.0, mirror=False):
    """A depth picture moved like warp() moves its picture (nearest)."""
    ys, xs = np.mgrid[0:N, 0:N].astype(np.float32)
    X, Y = xs + 0.5, ys + 0.5
    if mirror:
        X = N - X
    sx = np.floor((X - ax - dx) / s + ax).astype(np.int32)
    sy = np.floor((Y - ay - dy) / s + ay).astype(np.int32)
    h, w = z.shape
    ok = (sx >= 0) & (sy >= 0) & (sx < w) & (sy < h)
    out = z[np.clip(sy, 0, h - 1), np.clip(sx, 0, w - 1)]
    return np.where(ok, out, -np.inf).astype(np.float32)


def lit_rgba(p, tint):
    """A pack picture lit in a colour, as straight RGBA."""
    return np.concatenate([lit(p, tint), p[..., 3:4]], -1)


def layer(cv, p, tint):
    if p is None:
        return
    over(cv, lit(p, tint), p[..., 3])


# ── The head's own texture: skin, make-up, wrinkles, stubble ─────────────────
# The game paints these into the head's texture (half the face, mirrored down
# the middle), then lights the head. A part is a quad anchored on its side
# toward the middle: X steps it out, Y steps it down, both by 1.29/32 of the
# texture's width (Y by half that of its height, which is twice the width).
# Kinds in the game's drawing order: category, charinfo offset of the type,
# of the colour (None: black), base size, aspect factor.
FL_PARTS = (("MakeLower", 0x3E, 0x3F, 0.16, 0.9),
            ("MakeUpper", 0x38, 0x39, 0.18, 0.8),
            ("WrinkleLower", 0x33, None, 0.16, 0.9),
            ("WrinkleUpper", 0x2E, None, 0.16, 0.8))
FL_STEP = 1.29 / 32


def bilinear(img, x, y):
    """img (H, W, C) sampled at texel coordinates x, y (clamped)."""
    H, W = img.shape[:2]
    x = np.clip(x - 0.5, 0, W - 1)
    y = np.clip(y - 0.5, 0, H - 1)
    x0 = np.minimum(np.floor(x).astype(np.int32), W - 2 if W > 1 else 0)
    y0 = np.minimum(np.floor(y).astype(np.int32), H - 2 if H > 1 else 0)
    fx = (x - x0)[..., None]
    fy = (y - y0)[..., None]
    x1 = np.minimum(x0 + 1, W - 1)
    y1 = np.minimum(y0 + 1, H - 1)
    return ((img[y0, x0] * (1 - fx) + img[y0, x1] * fx) * (1 - fy) +
            (img[y1, x0] * (1 - fx) + img[y1, x1] * fx) * fy)


def faceline(pack, ci, fl, skin):
    """The head's colour per pixel: the skin with make-up, wrinkles and
    stubble painted on. fl holds each pixel's u, v on the head's texture."""
    N = fl.shape[0]
    u, v = fl[..., 0], fl[..., 1]
    on = fl[..., 3] > 0
    alb = np.empty((N, N, 3), np.float32)
    alb[:] = skin
    black = np.zeros(3, np.float32)

    def paint(t, u0, v0, w, h, inks):
        s = (u - u0) / w
        tt = (v - v0) / h
        inside = on & (s >= 0) & (s < 1) & (tt >= 0) & (tt < 1)
        if not inside.any():
            return
        q = bilinear(t, s * t.shape[1], tt * t.shape[0])
        for ch, ink in inks:
            a = (q[..., ch] * inside)[..., None]
            alb[:] = alb * (1 - a) + ink * a

    for cat, at, col_at, s0, aspect_k in FL_PARTS:
        typ = ci[at]
        t = pack.tex.get((cat, typ))
        p = pack.pic("tex/" + t[0] + ".png") if typ and t else None
        if p is None:
            continue
        o = at + (2 if col_at else 1)                 # scale, aspect, x, y follow
        S, A, X, Y = ci[o], ci[o + 1], ci[o + 2], ci[o + 3]
        w = s0 * (0.4 * S + 1)
        h = w * (0.12 * A + 0.64) * aspect_k * 0.5
        right = 1 - X * FL_STEP
        mid = 0.14 + Y * FL_STEP / 2
        ink = colour(ci[col_at]) if col_at else black
        inks = [(0, ink), (1, black)] if cat == "MakeLower" else [(0, ink)]   # blush, freckles
        paint(p, right - w, mid - h / 2, w, h, inks)
    # stubble covers the whole texture: the shaved shade, then its colour
    typ = ci[0x86]
    t = pack.tex.get(("BeardShort", typ))
    p = pack.pic("tex/" + t[0] + ".png") if typ and t else None
    if p is not None:
        paint(p, 0.0, 0.0, 1.0, 1.0, [(0, SHAVE), (1, colour(ci[0x87]))])
    return alb


# ── The face texture: FFL's placement on a 64-unit grid ──────────────────────
def place(mask, t, px, py, sx, sy, rot, origin, mirror, tint):
    if t is None or sx <= 0 or sy <= 0:
        return
    H, W = mask.shape[:2]
    r = math.radians(rot)
    c, s = math.cos(-r), math.sin(-r)
    add = {"left": -1.0, "right": 0.0, "center": -0.5}[origin]
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float32)
    X = (xs + 0.5 - px) / 0.88961464
    Y = (ys + 0.5 - py) / 0.9276675
    u = (X * c - Y * s) / sx - add
    v = (X * s + Y * c) / sy + 0.5
    inside = (u >= 0) & (u < 1) & (v >= 0) & (v < 1)
    if mirror:
        u = 1.0 - u
    th, tw = t.shape[:2]
    tx = np.clip((u * tw).astype(np.int32), 0, tw - 1)
    ty = np.clip((v * th).astype(np.int32), 0, th - 1)
    q = t[ty, tx]
    rgb, a = tint(q)
    over(mask, rgb, a * inside)


SPX, SQY = 0.88961464, 0.9276675          # the face texture's squash


def quad(mask, t, cx, cy, sx, sy, rot, mirror, tint):
    """A part centred at (cx, cy) on the mask, sx x sy before the squash,
    turned rot degrees."""
    if t is None or sx <= 0 or sy <= 0:
        return
    H, W = mask.shape[:2]
    r = math.radians(rot)
    c, s = math.cos(-r), math.sin(-r)
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float32)
    X = (xs + 0.5 - cx) / SPX
    Y = (ys + 0.5 - cy) / SQY
    u = (X * c - Y * s) / sx + 0.5
    v = (X * s + Y * c) / sy + 0.5
    inside = (u >= 0) & (u < 1) & (v >= 0) & (v < 1)
    if mirror:
        u = 1.0 - u
    th, tw = t.shape[:2]
    q = t[np.clip((v * th).astype(np.int32), 0, th - 1), np.clip((u * tw).astype(np.int32), 0, tw - 1)]
    rgb, a = tint(q)
    over(mask, rgb, a * inside)


def wrapped(default, stored, top):
    """A newer part's value: the eye's default moved by the stored delta, kept
    modulo the field's range (0..top)."""
    return (default + stored) % (top + 1)


def eye_extras(mask, pack, ci, base):
    g = lambda k: ci[K[k]]
    acc = pack.eye_acc.get(g("eyeType"))
    if not acc:
        return
    axis = pack.eye_axis.get(g("eyeType"), (0.0, 0.0))
    bs = 0.4 * g("eyeScale") + 1
    w, h = 5.34375 * bs, 4.5 * bs * (0.12 * g("eyeAspect") + 0.64)
    t_eye = pack.tex.get(("Eye", g("eyeType")))
    off = t_eye[1] if t_eye else 0
    rot_l = (g("eyeRotate") - 4 + off) * (360 / 32)
    black = np.zeros(3, np.float32)
    white = np.ones(3, np.float32)
    flat = lambda col: (lambda q: (np.broadcast_to(col, q.shape[:2] + (3,)), q[..., 0]))
    for side in (-1, 1):                          # -1: the eye on the left of the picture
        ax = 32 + side * g("eyeX") * SPX
        ay = g("eyeY") * 1.0760943 + 18.451524
        rot = rot_l if side < 0 else -rot_l
        rr = math.radians(rot)
        # the eye's pivot: its axis point, inside the quad from the inner corner
        px_ = ax + side * axis[0] * w * SPX
        py_ = ay - axis[1] * h * SQY

        def at(dx, dy):
            ox, oy = side * dx * SPX, dy * SQY
            return (px_ + ox * math.cos(rr) - oy * math.sin(rr), py_ + ox * math.sin(rr) + oy * math.cos(rr))

        for cat, k0, top_y, lash in (("EyelidUpper", "du", 36, False), ("EyelidLower", "dl", 36, False),
                                     ("EyelashUpper", "lu", 31, True), ("EyelashLower", "ll", 31, True)):
            typ = g(k0 + "Type")
            t = pack.tex.get((cat, typ))
            if not typ or not t:
                continue
            d = acc[cat]
            X = wrapped(d["x"], g(k0 + "X"), 31)
            Y = wrapped(d["y"], g(k0 + "Y"), top_y)
            S = wrapped(d["s"], g(k0 + "Scale"), 14)
            A = wrapped(d["a"], g(k0 + "Aspect"), 6)
            R = wrapped(d["r"], g(k0 + "Rotate"), 31)
            k = (S + 5) / 28.0 if lash else (S + 8) * 0.04
            sx = k * w
            sy = k * w * (0.12 * A + 0.64) * 0.8421053
            cx, cy = at(w * (X / 31 - axis[0]), h * (axis[1] + Y / 31 - 0.5))
            ang = rot - side * ((R if R < 16 else R - 32) * 11.25)
            quad(mask, pack.pic("tex/" + t[0] + ".png"), cx * base, cy * base, sx * base, sy * base, ang, side > 0, flat(black))
        typ = g("hiType")
        t = pack.tex.get(("Highlight", typ))
        if typ and t:
            d = acc["Highlight"]
            X = wrapped(d["x"], g("hiX"), 32)
            Y = wrapped(d["y"], g("hiY"), 32)
            S = wrapped(d["s"], g("hiScale"), 14)
            A = wrapped(d["a"], g("hiAspect"), 6)
            hs = 0.0345479 * S + 0.1554656
            sx = hs * min(w, h / 0.8421053)
            sy = sx * (0.12 * A + 0.64)
            cx = px_ + (0.5 - X / 32) * w * SPX
            cy = py_ + (Y / 32 - 0.5) * h * SQY
            quad(mask, pack.pic("tex/" + t[0] + ".png"), cx * base, cy * base, sx * base, sy * base, 0, False, flat(white))


# Face paint: the player's drawing (256 x 256), centred on the face texture a
# little low, 0.75 of its size (the game's FacePaintOffset / FacePaintSize)
FP_SIZE, FP_DY = 0.75, -0.03


def facepaint_from_canvas(raw):
    """A face paint's canvas (256 x 256 RGBA, block-linear, linear light) as
    sRGB RGBA floats; None when it is not one."""
    if len(raw) != 256 * 256 * 4:
        return None
    px = np.frombuffer(nxfmt.deswizzle(raw, 256, 256, 4, 4), np.uint8).reshape(256, 256, 4).astype(np.float32) / 255
    rgb = px[..., :3]
    rgb = np.where(rgb <= 0.0031308, rgb * 12.92, 1.055 * np.power(rgb, 1 / 2.4) - 0.055)
    # kept at half size, as the app keeps it (it is read at about that)
    return np.concatenate([rgb, px[..., 3:4]], -1).reshape(128, 2, 128, 2, 4).mean(axis=(1, 3))


def ltd_facepaint(path):
    """The face paint inside a .ltd (after its A3A3A3A3 mark), or None."""
    b = open(path, "rb").read()
    if len(b) < 4 or not b[1]:
        return None
    i3 = b.find(bytes([0xA3] * 4), 400)
    i4 = b.find(bytes([0xA4] * 4), i3 + 4) if i3 >= 0 else -1
    if i4 < 0:
        return None
    try:
        return facepaint_from_canvas(nxfmt.unzs(b[i3 + 4:i4]))
    except Exception:
        return None


def compose_face(pack, ci, res=256):
    mask = np.zeros((res, res, 4), np.float32)
    base = res / 64.0
    PXA, PYA, SP, PXM, PYM = 3.5323312, 4.629278, 0.88961464, 1.7792293, 1.0760943
    g = lambda k: ci[K[k]]

    def tex(cat, i):
        t = pack.tex.get((cat, i))
        return (pack.pic("tex/" + t[0] + ".png"), t[1]) if t else (None, 0)

    flat = lambda col: (lambda q: (np.broadcast_to(col, q.shape[:2] + (3,)), q[..., 0]))
    PYM, SP = 1.0760943, 0.88961464
    # Eyebrows
    p, off = tex("Eyebrow", g("eyebrowType"))
    if p is not None:
        sx = g("eyebrowX") * SP
        bs = 0.4 * g("eyebrowScale") + 1
        w, h = 5.0625 * bs * base, 4.5 * bs * (0.12 * g("eyebrowAspect") + 0.64) * base
        y = (g("eyebrowY") * PYM + PYA + 11.920528) * base
        rot = (g("eyebrowRotate") - 6 + off) * (360 / 32)
        tint = flat(colour(g("eyebrowColor")))
        place(mask, p, (32 - sx) * base, y, w, h, rot, "left", False, tint)
        place(mask, p, (32 + sx) * base, y, w, h, 360 - rot, "right", True, tint)
    # Eyes
    p, off = tex("Eye", g("eyeType"))
    if p is not None:
        iris, shadow = colour(g("eyeColor")), colour(g("eyeShadowColor"))
        on = (g("faceFlags") >> 4) & 1

        def tint(q):
            R, G, B, A = q[..., 0:1], q[..., 1:2], q[..., 2:3], q[..., 3]
            rgb = np.clip(G + iris * B + (shadow * R if on else 0), 0, 1)
            a = A if on else np.maximum(0, A - R[..., 0] * np.maximum(0, 1 - G[..., 0] - B[..., 0]))
            return rgb, a
        sx = g("eyeX") * SP
        bs = 0.4 * g("eyeScale") + 1
        w, h = 5.34375 * bs * base, 4.5 * bs * (0.12 * g("eyeAspect") + 0.64) * base
        y = (g("eyeY") * PYM + PYA + 13.822246) * base
        rot = (g("eyeRotate") - 4 + off) * (360 / 32)
        place(mask, p, (32 - sx) * base, y, w, h, rot, "left", False, tint)
        place(mask, p, (32 + sx) * base, y, w, h, 360 - rot, "right", True, tint)
    # Eye extras: lids, lashes, highlights, placed in each eye's own frame
    eye_extras(mask, pack, ci, base)
    # Mouth: the lower lip (R) in the mouth colour, the upper lip (G) in its
    # darker shade, the rest of it black (the line). Without lipstick (flag
    # bit 5) the game draws the mouth's no-lip version, else only its line.
    t = pack.tex.get(("Mouth", g("mouthType")))
    lips = (g("faceFlags") >> 5) & 1
    p = pack.pic("tex/" + (t[2] if t and t[2] and not lips else t[0]) + ".png") if t else None
    if p is not None:
        lower, upper = colour(g("mouthColor")), upper_lip(g("mouthColor"))

        def tint(q):
            R, G, A = q[..., 0:1], q[..., 1:2], q[..., 3]
            if lips:
                return np.clip(lower * R + upper * G, 0, 1), A
            return np.zeros(q.shape[:2] + (3,), np.float32), A * np.clip(1 - R[..., 0] - G[..., 0], 0, 1)
        bs = 0.4 * g("mouthScale") + 1
        w, h = 6.1875 * bs * base, 4.5 * bs * (0.12 * g("mouthAspect") + 0.64) * base
        y = (g("mouthY") * PYM + PYA + 24.629572) * base
        place(mask, p, 32 * base, y, w, h, (g("mouthRotate") & 31) * 11.25, "center", False, tint)
    # Moustache
    p, off = tex("Mustache", g("mustacheType"))
    if p is not None:
        tint = flat(colour(g("mustacheColor")))
        bs = 0.4 * g("mustacheScale") + 1
        y = (g("mustacheY") * PYM + PYA + 27.134275) * base
        place(mask, p, 32 * base, y, 4.5 * bs * base, 9 * bs * base, 0, "left", False, tint)
        place(mask, p, 32 * base, y, 4.5 * bs * base, 9 * bs * base, 0, "right", True, tint)
    # Mole
    if (g("faceFlags") >> 7) & 1:
        p, off = tex("Mole", 1)
        if p is not None:
            s = (0.4 * g("moleScale") + 1) * base
            place(mask, p, (g("moleX") * PXM + PXA + 14.233834) * base, (g("moleY") * PYM + PYA + 11.178394 + 2 * PYM) * base,
                  s, s, 0, "center", False, flat(MOLE))
    return mask


def facepaint_layer(facepaint, res=256):
    """The face paint on the face texture (res x res, straight RGBA)."""
    layer_ = np.zeros((res, res, 4), np.float32)
    size = FP_SIZE * res
    x0, y0 = (res - size) / 2, (res - size) / 2 + FP_DY * res
    ys, xs = np.mgrid[0:res, 0:res].astype(np.float32)
    u = (xs + 0.5 - x0) / size
    v = (ys + 0.5 - y0) / size
    inside = (u >= 0) & (u < 1) & (v >= 0) & (v < 1)
    q = bilinear(facepaint, u * facepaint.shape[1], v * facepaint.shape[0])
    over(layer_, np.clip(q[..., :3], 0, 1), np.clip(q[..., 3], 0, 1) * inside)
    return layer_


def portrait(pack, ci, facepaint=None):
    """The Mii's picture. The 3D pieces (hair, front piece, hair part, ears,
    and the head with everything on it) are laid down by depth, pixel by
    pixel, farthest first; glasses go over all."""
    N = pack.size
    g = lambda k: ci[K[k]]
    skin = colour(g("facelineColor"))
    flags = g("faceFlags")
    hc0, hc1 = colour(g("hairColor0")), colour(g("hairColor1"))
    back_on, bangs_on = (flags >> 2) & 1, (flags >> 3) & 1       # two-tone back hair, bangs
    head = pack.heads.get(g("facelineType")) or pack.head0
    h0 = pack.head0
    d_hair = head["set_hair"] - h0["set_hair"]
    ht = ci[K["hairType"]] | (ci[K["hairType"] + 1] << 8)
    hair = pack.hairs.get(ht)
    front = pack.fronts.get(g("hairTypeFront")) if hair and hair[2] >= 1 else None
    hpart = pack.hparts.get((g("hairTypeBack"), g("hairStyle"))) if hair and hair[2] >= 2 else None
    pieces = []                                                    # (straight RGBA, z)

    # hair: the style, its part, its front piece, moved to this head
    for piece in (hair, hpart, front):
        q = hair_colour(pack.pic(piece[0]), hc0, hc1, back_on, bangs_on) if piece else None
        z = pack.depth(piece[1]) if piece else None
        if q is None or z is None:
            continue
        if d_hair.any():
            q, z = warp(q, N, 0, 0, 1.0, *d_hair), warp_z(z, N, 0, 0, 1.0, *d_hair)
        pieces.append((q, z))
    # ears, both sides
    ear = pack.ears.get(g("earType"))
    if ear and pack.pic(ear[0]) is not None and pack.depth(ear[1]) is not None:
        ax, ay = h0["set_ear"]
        d = head["set_ear"] - h0["set_ear"]
        es = 1.0 + 0.1 * (g("earScale") - 2)
        edy = (g("earY") - 4) * 1.0
        e_rgba = lit_rgba(pack.pic(ear[0]), skin)
        for mirror in (False, True):
            pieces.append((warp(e_rgba, N, ax, ay, es, d[0], d[1] + edy, mirror),
                           warp_z(pack.depth(ear[1]), N, ax, ay, es, d[0], d[1] + edy, mirror)))
    # the head and all on it: its texture (skin, make-up, wrinkles, stubble),
    # face paint and the face's parts, the beard, the nose
    hcv = np.zeros((N, N, 4), np.float32)
    fl = pack.pic(head["fl"]) if "fl" in head else None
    layer(hcv, pack.pic(head["shade"]), faceline(pack, ci, fl, skin) if fl is not None else skin)
    uv = pack.pic(head["uv"])
    if uv is not None:
        mx = np.clip((uv[..., 0] * 255).astype(np.int32), 0, 255)
        my = np.clip((uv[..., 1] * 255).astype(np.int32), 0, 255)
        # face paint: on the face texture's whole sheet, under the face's parts
        layers = [(facepaint_layer(facepaint), uv[..., 3])] if facepaint is not None else []
        layers.append((compose_face(pack, ci), uv[..., 2]))
        for mask, cover in layers:
            # drawn at 256, read at half that: about one texel a pixel, so
            # fine lines (face paint) do not break up
            half = mask.reshape(128, 2, 128, 2, 4).mean(axis=(1, 3))
            m = half[my // 2, mx // 2]
            rgb = np.where(m[..., 3:4] > 0, m[..., :3] / np.maximum(m[..., 3:4], 1e-6), 0)     # it holds premultiplied colour
            over(hcv, rgb, m[..., 3] * cover)
    zh = pack.depth(head["z"]) if "z" in head else None
    if zh is None:
        zh = np.where(pack.pic(head["shade"])[..., 3] > 0, 0.0, -np.inf).astype(np.float32)
    beard = pack.beards.get(g("beardType"))
    if beard and pack.pic(beard[0]) is not None:
        d = head["set_beard"] - h0["set_beard"]
        b_rgba = lit_rgba(pack.pic(beard[0]), colour(g("beardColor")))
        if d.any():
            b_rgba = warp(b_rgba, N, 0, 0, 1.0, *d)
        put(hcv, b_rgba)
        zb = pack.depth(beard[1])
        if zb is not None:
            zh = np.maximum(zh, warp_z(zb, N, 0, 0, 1.0, *d) if d.any() else zb)
    nose = pack.noses.get(g("noseType"))
    if nose and pack.pic(nose) is not None:
        ax, ay = h0["set_nose"]
        d = head["set_nose"] - h0["set_nose"]
        ns = 1.0 + 0.1 * (g("noseScale") - 4)
        ndy = (g("noseY") - 9) * 1.1
        layer(hcv, warp(pack.pic(nose), N, ax, ay, ns, d[0], d[1] + ndy), skin)
    # what lies past the head (face paint, the nose's tip): just before the face
    zh = np.where(np.isinf(zh) & (hcv[..., 3] > 0), np.float32(0.25), zh)
    a = hcv[..., 3:4]
    pieces.append((np.concatenate([np.where(a > 0, hcv[..., :3] / np.maximum(a, 1e-6), 0), a], -1), zh))

    # farthest first, pixel by pixel
    Z = np.stack([z for _, z in pieces])
    C = np.stack([q for q, _ in pieces])
    order = np.argsort(Z, axis=0, kind="stable")
    cv = np.zeros((N, N, 4), np.float32)
    for k in range(len(pieces)):
        idx = order[k]
        c = np.take_along_axis(C, idx[None, ..., None], 0)[0]
        zk = np.take_along_axis(Z, idx[None], 0)[0]
        over(cv, c[..., :3], np.where(np.isinf(zk), 0, c[..., 3]))
    # glasses, over everything
    gl = pack.glasses.get(g("glassType"))
    if gl:
        fr, ln = pack.pic(gl[0]), pack.pic(gl[1])
        gs = 1.0 + 0.1 * (g("glassScale") - 4)
        gy = head["set_nose"][1] - 9.0 + (g("glassY") - 11) * 1.1
        dy = gy - N / 2
        if ln is not None and g("lensType"):
            lp = warp(ln, N, N / 2, N / 2, gs, 0, dy)
            over(cv, np.broadcast_to(colour(g("lensColor")), lp.shape[:2] + (3,)), lp[..., 3] * 0.7)
        if fr is not None:
            layer(cv, warp(fr, N, N / 2, N / 2, gs, 0, dy), colour(g("glassColor")))
    a = cv[..., 3:4]
    rgb = np.where(a > 0, cv[..., :3] / np.maximum(a, 1e-6), 0)
    return Image.fromarray((np.concatenate([rgb, a], -1) * 255 + 0.5).clip(0, 255).astype(np.uint8), "RGBA")


def ltd_charinfo(path):
    b = open(path, "rb").read()
    assert b[0] == 3, "a v3 .ltd"
    n = struct.unpack_from("<I", b, 4)[0]
    return bytearray(b[8:8 + n])


if __name__ == "__main__":
    pk = Pack(sys.argv[1])
    for p in sys.argv[2:]:
        portrait(pk, ltd_charinfo(p), ltd_facepaint(p)).save(os.path.splitext(p)[0] + "_new.png")
