#!/usr/bin/env python3
"""Draw a Mii from its CharInfoEx with the game's own parts (research, see
tools/mii/README.md). The parts are read from a folder of files taken out of
your copy of the game (ltdfs.py extract); nothing of the game ships here.

How it is put together:
  - each part's number in the save is the PartsIndex in Mii/Parts/*.bgyml,
    which names its texture (eyes, eyebrows, mouth, ...) or model (head
    shape, hair, nose, ...);
  - the face (eyes, eyebrows, mouth, moustache, mole) is composed into one
    texture with the placement maths of Nintendo's earlier Mii library (FFL,
    as decompiled), since the save keeps the same meaning for these values;
  - that texture is laid on the head's "Mask" surface, the head is drawn in
    the skin colour, then the nose, the ears and the hair at the head's anchor
    bones (set_nose, set_ear, set_hair).
"""
import math
import os
import re
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nxfmt  # noqa: E402
import raster  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# CharInfoEx offsets (tlmodding/charinfo-ex, MIT)
O = dict(gender=0x27, faceFlags=0x2B, facelineType=0x2C, facelineColor=0x2D, hairType=0x44, hairColor0=0x46, hairColor1=0x47,
         hairTypeFront=0x48, hairTypeBack=0x49, earType=0x4B, earScale=0x4C, earY=0x4D, eyeType=0x4E, eyeColor=0x4F, eyeScale=0x50,
         eyeAspect=0x51, eyeRotate=0x52, eyeX=0x53, eyeY=0x54, eyeShadowColor=0x55, eyebrowType=0x74, eyebrowColor=0x75,
         eyebrowScale=0x76, eyebrowAspect=0x77, eyebrowRotate=0x78, eyebrowX=0x79, eyebrowY=0x7A, noseType=0x7B, noseScale=0x7C,
         noseY=0x7D, mouthType=0x7E, mouthColor=0x7F, mouthScale=0x80, mouthAspect=0x81, mouthRotate=0x82, mouthY=0x83,
         beardType=0x84, beardColor=0x85, mustacheType=0x88, mustacheColor=0x89, mustacheScale=0x8A, mustacheY=0x8C,
         glassType1=0x8D, glassColor1=0x8E, glassScale=0x8F, glassY=0x91, moleScale=0x94, moleX=0x95, moleY=0x96)

# Skin colours 100..113 are not in the 100-colour table; until they are read
# from the game, the earlier library's six skin tones stand in.
SKIN = {100: (255, 211, 173), 101: (255, 182, 107), 102: (222, 121, 66), 103: (255, 170, 140), 104: (173, 81, 41), 105: (99, 44, 24)}


def palette():
    html = open(os.path.join(ROOT, "data", "webui.html"), encoding="utf-8").read()
    m = re.search(r'const PALETTE = \("([0-9a-f,"+\s]+)\)\.split', html)
    hexes = re.sub(r'["+\s]', "", m.group(1)).split(",")
    return [tuple(int(h[i:i + 2], 16) for i in (0, 2, 4)) for h in hexes]


PAL = palette()


def colour(i, default=(40, 30, 25)):
    if i in SKIN:
        return SKIN[i]
    return PAL[i] if 0 <= i < len(PAL) else default


class Parts:
    def __init__(self, romfs):
        self.romfs = romfs
        self.by_cat = {}
        pack = nxfmt.sarc(nxfmt.read(os.path.join(romfs, "Pack", "MiiParts.pack.zs")))
        for name, data in pack.items():
            d = nxfmt.byml(data)
            self.by_cat.setdefault(d.get("Category"), {})[d.get("PartsIndex")] = d
        self.tex = {t[0]: t for t in nxfmt.bntx(nxfmt.read(os.path.join(romfs, "Tex", "Pack", "MiiParts.bntx.zs")))}
        self._models = {}

    def part(self, cat, index):
        return self.by_cat.get(cat, {}).get(index)

    def texture(self, name):
        t = self.tex.get(name)
        return nxfmt.decode(*t).astype(np.float32) / 255.0 if t else None

    def model(self, fmdb):
        name = os.path.basename(fmdb).replace(".fmdb", "")
        if name not in self._models:
            path = os.path.join(self.romfs, "Model", name + ".bfres.zs")
            self._models[name] = nxfmt.bfres(nxfmt.read(path))["models"][0] if os.path.exists(path) else None
        return self._models[name]


# ── The face texture (FFL's placement) ───────────────────────────────────────
def compose_mask(ci, parts, res=512):
    g = lambda k: ci[O[k]]
    mask = np.zeros((res, res, 4), np.float32)
    base = res / 64.0
    POS_X_ADD, POS_Y_ADD, SPACING_MUL, POS_X_MUL, POS_Y_MUL = 3.5323312, 4.629278, 0.88961464, 1.7792293, 1.0760943

    def place(tex_rgba, pos, scale, rot_deg, origin, mirror=False):
        """Draw a part quad (FFL's unit quad, scaled, rotated, squashed, moved) onto the mask."""
        if tex_rgba is None:
            return
        th, tw = tex_rgba.shape[:2]
        ys, xs = np.mgrid[0:res, 0:res]
        # mask pixel -> FFL space (its orthographic view runs top to bottom)
        X = xs + 0.5 - pos[0]
        Y = (ys + 0.5) - pos[1]
        X, Y = X / 0.88961464, Y / 0.9276675
        r = math.radians(rot_deg)
        c, s = math.cos(-r), math.sin(-r)
        qx, qy = (X * c - Y * s) / scale[0], (X * s + Y * c) / scale[1]
        add = {"left": -1.0, "right": 0.0, "center": -0.5}[origin]
        u = qx - add                      # 0..1 across the quad
        v = qy + 0.5                      # 0..1 top to bottom
        inside = (u >= 0) & (u < 1) & (v >= 0) & (v < 1)
        if not inside.any():
            return
        if mirror:
            u = 1.0 - u
        tx = np.clip((u * tw).astype(np.int32), 0, tw - 1)
        ty = np.clip((v * th).astype(np.int32), 0, th - 1)
        src = tex_rgba[ty, tx]
        a = src[..., 3:4] * inside[..., None]
        mask[..., :3] = mask[..., :3] * (1 - a) + src[..., :3] * a
        mask[..., 3:4] = mask[..., 3:4] * (1 - a) + a

    def tint_eye(t):
        iris, white = np.array(colour(g("eyeColor") if g("eyeColor") < 100 else 8)) / 255.0, np.ones(3)
        shadow = np.array(colour(g("eyeShadowColor"))) / 255.0
        shadow_on = (g("faceFlags") >> 4) & 1
        out = np.zeros_like(t)
        r, gg, b = t[..., 0:1], t[..., 1:2], t[..., 2:3]
        rgb = white * gg + iris * b + (shadow * r if shadow_on else 0)
        out[..., :3] = np.clip(rgb, 0, 1)
        out[..., 3] = np.clip(t[..., 3] * (1.0 if shadow_on else 1.0) - (0 if shadow_on else r[..., 0] * (1 - gg[..., 0] - b[..., 0]).clip(0)), 0, 1)
        return out

    def tint_mask(t, rgb, channel=0):
        out = np.zeros_like(t)
        out[..., :3] = np.array(rgb) / 255.0
        out[..., 3] = t[..., channel]
        return out

    def tint_mouth(t):
        lip = np.array(colour(g("mouthColor"))) / 255.0
        out = np.zeros_like(t)
        r, gg = t[..., 0:1], t[..., 1:2]          # lower lip, upper lip
        out[..., :3] = np.clip(lip * r + lip * 0.9 * gg, 0, 1)
        out[..., 3] = t[..., 3]
        return out

    # Eyebrows
    eb = parts.part("Eyebrow", g("eyebrowType"))
    if eb and eb.get("TextureName"):
        t = parts.texture(eb["TextureName"])
        if t is not None:
            t = tint_mask(t, colour(g("eyebrowColor")))
            sx = g("eyebrowX") * SPACING_MUL
            bs = 0.4 * g("eyebrowScale") + 1.0
            scale = (5.0625 * bs * base, 4.5 * bs * (0.12 * g("eyebrowAspect") + 0.64) * base)
            py = (g("eyebrowY") * POS_Y_MUL + POS_Y_ADD + 11.920528) * base
            rot = (g("eyebrowRotate") - 6 + eb.get("OffsetRotate", 0)) * (360.0 / 32)
            place(t, ((32 - sx) * base, py), scale, rot, "left")
            place(t, ((32 + sx) * base, py), scale, 360 - rot, "right", mirror=True)
    # Eyes
    ey = parts.part("Eye", g("eyeType"))
    if ey and ey.get("TextureName"):
        t = parts.texture(ey["TextureName"])
        if t is not None:
            t = tint_eye(t)
            sx = g("eyeX") * SPACING_MUL
            bs = 0.4 * g("eyeScale") + 1.0
            scale = (5.34375 * bs * base, 4.5 * bs * (0.12 * g("eyeAspect") + 0.64) * base)
            py = (g("eyeY") * POS_Y_MUL + POS_Y_ADD + 13.822246) * base
            rot = (g("eyeRotate") - 4 + ey.get("OffsetRotate", 0)) * (360.0 / 32)
            place(t, ((32 - sx) * base, py), scale, rot, "left")
            place(t, ((32 + sx) * base, py), scale, 360 - rot, "right", mirror=True)
    # Mouth
    mo = parts.part("Mouth", g("mouthType"))
    if mo and mo.get("TextureName"):
        t = parts.texture(mo["TextureName"])
        if t is not None:
            t = tint_mouth(t)
            bs = 0.4 * g("mouthScale") + 1.0
            scale = (6.1875 * bs * base, 4.5 * bs * (0.12 * g("mouthAspect") + 0.64) * base)
            py = (g("mouthY") * POS_Y_MUL + POS_Y_ADD + 24.629572) * base
            place(t, (32 * base, py), scale, 0.0, "center")
    # Moustache
    mu = parts.part("Mustache", g("mustacheType"))
    if mu and mu.get("TextureName"):
        t = parts.texture(mu["TextureName"])
        if t is not None:
            t = tint_mask(t, colour(g("mustacheColor")))
            bs = 0.4 * g("mustacheScale") + 1.0
            scale = (4.5 * bs * base, 9.0 * bs * base)
            py = (g("mustacheY") * POS_Y_MUL + POS_Y_ADD + 27.134275) * base
            place(t, (32 * base, py), scale, 0.0, "left")
            place(t, (32 * base, py), scale, 0.0, "right", mirror=True)
    # Mole
    if (g("faceFlags") >> 7) & 1:
        t = parts.texture("Mole00")
        if t is not None:
            t = tint_mask(t, (40, 28, 24))
            s = 0.4 * g("moleScale") + 1.0
            place(t, ((g("moleX") * POS_X_MUL + POS_X_ADD + 14.233834) * base, (g("moleY") * POS_Y_MUL + POS_Y_ADD + 11.178394 + 2 * POS_Y_MUL) * base),
                  (s * base, s * base), 0.0, "center")
    return mask


# ── The head ─────────────────────────────────────────────────────────────────
def bone_world(model, name):
    """A bone's position from the model root (translations only: enough for the anchors)."""
    bones = model["bones"]
    by = {b["name"]: (i, b) for i, b in enumerate(bones)}
    if name not in by:
        return np.zeros(3, np.float32)
    i, b = by[name]
    p = np.array(b["t"], np.float32)
    while b["parent"] >= 0:
        b = bones[b["parent"]]
        p = p + np.array(b["t"], np.float32)
    return p


def render(ci, parts, size=256, out=None):
    g = lambda k: ci[O[k]]
    skin = np.array(colour(g("facelineColor"), (255, 211, 173))) / 255.0
    hair_rgb = np.array(colour(g("hairColor0"))) / 255.0
    cv = raster.Canvas(size, size, size * 1.02, size / 2, size * 0.80)
    face = parts.part("Faceline", g("facelineType")) or parts.part("Faceline", 0)
    head = parts.model(face["ModelUnit"]["Fmdb"])
    head_at = bone_world(head, "Head")
    mask = compose_mask(ci, parts)
    for sh in head["shapes"]:
        if sh["name"].startswith("Head"):
            cv.draw(sh["p"][:, :3] + head_at, sh["n"], sh["idx"], skin)
    for sh in head["shapes"]:
        if sh["name"].startswith("Mask") and sh["uv0"] is not None:
            # the face texture over the skin, a hair in front of the head
            m = mask.copy()
            cv.draw(sh["p"][:, :3] + np.array([0, 0, 0.002], np.float32), sh["n"], sh["idx"], (1, 1, 1),
                    uv=sh["uv0"], tex=m, alpha_from_tex=True)
    nose = parts.part("Nose", g("noseType"))
    if nose and nose.get("ModelUnit"):
        nm = parts.model(nose["ModelUnit"]["Fmdb"])
        if nm:
            at = head_at + np.array(next((b["t"] for b in head["bones"] if b["name"] == "set_nose"), (0, 0, 0)), np.float32)
            for sh in nm["shapes"]:
                if sh["name"].startswith("Nose__"):
                    cv.draw(sh["p"][:, :3] + at, sh["n"], sh["idx"], skin * 0.98)
    hair = parts.part("Hair", g("hairType"))
    if hair and hair.get("ModelUnit"):
        hm = parts.model(hair["ModelUnit"]["Fmdb"])
        if hm:
            at = head_at + np.array(next((b["t"] for b in head["bones"] if b["name"] == "set_hair"), (0, 0, 0)), np.float32)
            for sh in hm["shapes"]:
                cv.draw(sh["p"][:, :3] + at, sh["n"], sh["idx"], hair_rgb)
    img = cv.image()
    if out:
        img.save(out)
    return img, mask


def charinfos(mii_sav):
    b = open(mii_sav, "rb").read()
    off = struct.unpack_from("<I", b, 8)[0]

    def murmur3(s):
        data = s.encode(); n = len(data); h = 0; c1, c2 = 0xcc9e2d51, 0x1b873593
        rot = lambda x, r: ((x << r) | (x >> (32 - r))) & 0xffffffff
        for i in range(n // 4):
            k = struct.unpack_from("<I", data, i * 4)[0]
            k = (k * c1) & 0xffffffff; k = rot(k, 15); k = (k * c2) & 0xffffffff
            h ^= k; h = rot(h, 13); h = (h * 5 + 0xe6546b64) & 0xffffffff
        tail = data[(n // 4) * 4:]; k = 0
        if len(tail) >= 3: k ^= tail[2] << 16
        if len(tail) >= 2: k ^= tail[1] << 8
        if len(tail) >= 1:
            k ^= tail[0]; k = (k * c1) & 0xffffffff; k = rot(k, 15); k = (k * c2) & 0xffffffff; h ^= k
        h ^= n; h ^= h >> 16; h = (h * 0x85ebca6b) & 0xffffffff; h ^= h >> 13; h = (h * 0xc2b2ae35) & 0xffffffff; h ^= h >> 16
        return h

    want = murmur3("Mii.CharInfoEx")
    p = 0x20
    while p < off:
        h, v = struct.unpack_from("<II", b, p)
        p += 8
        if h == want:
            cnt = struct.unpack_from("<I", b, v)[0]
            o, out = v + 4, []
            for _ in range(cnt):
                sz = struct.unpack_from("<I", b, o)[0]
                out.append(bytearray(b[o + 4:o + 4 + sz]))
                o += 4 + sz
            return out
    return []


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("romfs")
    ap.add_argument("miisav")
    ap.add_argument("--slots", default="0")
    ap.add_argument("--out", default="mii_%d.png")
    a = ap.parse_args()
    P = Parts(a.romfs)
    infos = charinfos(a.miisav)
    for s in [int(x) for x in a.slots.split(",")]:
        render(infos[s], P, out=a.out % s)
