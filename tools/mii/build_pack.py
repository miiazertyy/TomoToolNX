#!/usr/bin/env python3
"""Make the Mii parts pack the app draws real Mii pictures with (pack v2).

    python tools/mii/build_pack.py --romfs DIR --out DIR      # files already taken out of the game
    python tools/mii/build_pack.py --nsp GAME.nsp --keys prod.keys [--titlekeys title.keys] --out DIR

Every picture is a front view drawn with one camera, so they stack. The 3D
pieces (heads, ears, hair, front pieces, hair parts, beards) also have a depth
picture (NAME_z.png, grey): how near each pixel is, 1 (far, Z_FAR) to 255
(near, Z_NEAR), 0 where the piece is not; the app lays the pieces down nearest
last, pixel by pixel, as the game's depth test would. A part's picture holds,
per pixel (RGBA, 8 bits):
  R  how lit it is: the colour is multiplied by R x 1.25 (flat light is 0.82,
     the game's 1.03)
  G  light added over it: highlights on hair, the rim of light along an edge,
     the nose's shine (c + (1 - c) x G)
  B  a dark line drawn over it: the nose's line, outlines (c x (1 - B))
  A  where the part is
The app colours each part with the Mii's colour (source/mii_art.cpp).

Files:
  head_NN.png, headuv_NN.png  a head shape, and where the face texture lies on
                              it (R,G = its u,v; A = the texture's sheet, which
                              reaches past the head; B = the part on the head)
  headfl_NN.png               where the head's own texture lies on it (R,G =
                              u,v; half the face, u = 1 down the middle):
                              wrinkles, make-up and stubble are drawn there
  hair_NNN.png                a hairstyle (hairs are drawn at head 0's
                              anchor; the app moves them to the head's)
  front_NNN.png               a front piece for the back hairstyles
  part_NN_PP.png              a hair part (bun, tail) at its place PP (upper,
                              middle, lower x 4 + centre, left, right, both)
  (hair pictures: B = where the second hair colour goes, low 4 bits for
   two-tone back hair, high 4 bits for two-tone bangs)
  ear_N.png                   the right ear at head 0's anchor (the left one is
                              its mirror)
  nose_NN.png                 a nose (shape, shine, line) at head 0's anchor
  beard_NN.png                a beard at head 0's anchor
  glass_NN.png, lens_NN.png   glasses frame and lenses, centred on the canvas
  tex/NAME.png                the face parts (eyes, eyebrows, ...): masks
  index.txt                   one line per part, the anchors per head
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gbuf  # noqa: E402
import miimodel  # noqa: E402
import nxfmt  # noqa: E402

PORTRAIT = 128          # pixels, the pack's pictures
SS = 4                  # drawn this much bigger, then reduced
SCALE = 0.98            # canvas widths per model unit
ROOT_Y = 0.86           # where the model's origin sits, from the top
LIGHT = np.array([-0.25, 0.55, 0.80], np.float32)
LIGHT /= np.linalg.norm(LIGHT)
FLAT = 1.03             # the game's flat-lit colour, as a multiple of the base

# What the head-0 anchors are (set_*), filled in by the heads.
ANCHOR = {}
# The depth range a depth picture holds (model units, nearer is larger)
Z_FAR, Z_NEAR = -0.6, 0.5


def project(p, size=PORTRAIT):
    return (size / 2 + p[0] * size * SCALE, size * ROOT_Y - p[1] * size * SCALE)


def new_buf():
    s = PORTRAIT * SS
    return gbuf.GBuf(s, s, s * SCALE, s / 2, s * ROOT_Y)


def load_tex(romfs, name, with_format=False):
    p = os.path.join(romfs, "Tex", name + ".bntx.zs")
    img, fmt = None, ""
    if os.path.exists(p):
        for t in nxfmt.bntx(nxfmt.read(p)):
            img, fmt = nxfmt.decode(*t).astype(np.float32) / 255.0, t[1]
            break
    return (img, fmt) if with_format else img


def encode(r, g, b, a):
    """Per-pixel channels at SS size to the pack's RGBA picture."""
    cov = a
    stack = np.stack([r * cov, g * cov, b * cov, cov], -1)
    small = gbuf.downsample(stack, SS)
    al = small[..., 3:4]
    rgb = np.where(al > 1e-4, small[..., :3] / np.maximum(al, 1e-4), 0)
    out = np.concatenate([rgb, al], -1)
    return Image.fromarray((np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA")


def shade_skin(buf, tagmask):
    N = buf.normals()
    nl = N @ LIGHT
    f = 1.0 - np.clip(N[..., 2], 0, 1)
    mult = FLAT * (0.86 + 0.14 * gbuf.smoothstep(-0.2, 0.35, nl))
    rim = 0.42 * gbuf.smoothstep(0.62, 0.92, f)
    return mult / 1.25, rim


def shade_hair(buf, tagmask, mim):
    N = buf.normals()
    nl = N @ LIGHT
    f = 1.0 - np.clip(N[..., 2], 0, 1)
    detail = mim[..., 0] if mim is not None else np.ones(nl.shape, np.float32)
    shine = mim[..., 1] if mim is not None else np.zeros(nl.shape, np.float32)
    mult = 1.04 * (0.60 + 0.40 * gbuf.smoothstep(-0.05, 0.4, nl)) * (0.52 + 0.48 * detail)
    # the shine streaks, and the pale rim the game lights along the edges
    light = 0.30 * shine * gbuf.smoothstep(0.1, 0.6, nl) + 0.45 * gbuf.smoothstep(0.78, 0.96, f)
    return mult / 1.25, np.clip(light, 0, 1)


def encode_z(z, cov):
    """Each pixel's depth (the mean over its covered samples): 1..255 from
    Z_FAR to Z_NEAR, 0 where the piece is not."""
    c = cov.astype(np.float32)
    zz = np.where(c > 0, z, 0).astype(np.float32)
    sm = gbuf.downsample(np.stack([zz * c, c], -1), SS)
    on = sm[..., 1] > 1e-6
    mean = np.where(on, sm[..., 0] / np.maximum(sm[..., 1], 1e-6), 0)
    q = np.clip((mean - Z_FAR) / (Z_NEAR - Z_FAR), 0, 1) * 254 + 1
    return Image.fromarray(np.where(on, q + 0.5, 0).astype(np.uint8), "L")


def save(img, out, name):
    top = img.getextrema()[1] if img.mode == "L" else img.getextrema()[3][1]
    if top == 0:
        return "-"
    img.save(os.path.join(out, name), optimize=True)
    return name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--romfs", help="a folder of files already taken out of the game (else --nsp)")
    ap.add_argument("--nsp")
    ap.add_argument("--keys")
    ap.add_argument("--titlekeys")
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="", help="comma list: tex,head,hair,front,hairpart,ear,nose,beard,glass (default all)")
    ap.add_argument("--hairs", default="", help="only these hair numbers (testing)")
    a = ap.parse_args()
    only = set(x for x in a.only.split(",") if x)
    want = lambda k: not only or k in only
    romfs = a.romfs
    if not romfs:
        import ltdfs
        romfs = os.path.join(a.out, "_romfs")
        fs = ltdfs.open_game(a.nsp, a.keys, a.titlekeys)
        pick = ("/Model/Mii", "/Pack/MiiParts", "/Tex/Pack/MiiParts", "/Tex/Mii")
        for p, off, size in fs.walk():
            if any(p.startswith(w) for w in pick):
                dst = os.path.join(romfs, p.lstrip("/"))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, "wb") as o:
                    o.write(fs.read(off, size))
    out = a.out
    os.makedirs(os.path.join(out, "tex"), exist_ok=True)
    pack = nxfmt.sarc(nxfmt.read(os.path.join(romfs, "Pack", "MiiParts.pack.zs")))
    parts = {}
    for data in pack.values():
        d = nxfmt.byml(data)
        if d.get("PartsIndex") is not None:
            parts.setdefault(d["Category"], {})[d["PartsIndex"]] = d
    lines = ["# TomoToolNX Mii parts pack v3 (made by tools/mii/build_pack.py from your game)",
             "size %d" % PORTRAIT, "version 3", "depth %.3f %.3f" % (Z_FAR, Z_NEAR)]

    def model(d):
        mu = (d or {}).get("ModelUnit") or {}
        if not mu.get("Fmdb"):
            return None
        name = os.path.basename(mu["Fmdb"]).replace(".fmdb", "")
        path = os.path.join(romfs, "Model", name + ".bfres.zs")
        return (name, miimodel.load(path)) if os.path.exists(path) else None

    # ── The face parts, as the game keeps them ──
    texnames = set()
    for t in nxfmt.bntx(nxfmt.read(os.path.join(romfs, "Tex", "Pack", "MiiParts.bntx.zs"))):
        if want("tex"):
            img = nxfmt.decode(*t)
            Image.fromarray(img, "RGBA").save(os.path.join(out, "tex", t[0] + ".png"), optimize=True)
        texnames.add(t[0])
    for cat in ("Eye", "Eyebrow", "Mouth", "Mustache", "Mole", "BeardShort", "MakeUpper", "MakeLower",
                "WrinkleUpper", "WrinkleLower", "Highlight", "EyelashUpper", "EyelashLower", "EyelidUpper", "EyelidLower"):
        for idx, d in sorted(parts.get(cat, {}).items()):
            if d.get("TextureName") in texnames:
                lines.append("tex %s %d %s %d %s" % (cat, idx, d["TextureName"], int(d.get("OffsetRotate") or 0),
                                                       d.get("NoLipTextureName") or "-"))

    # ── Each eye's frame: its axis point and the default place, size, shape
    #    and turn of its lids, lashes and highlight (RSDB MiiEyeAccessoryParam,
    #    found by the murmur3 of the eye's reference) ──
    rsdb = os.path.join(romfs, "RSDB", "MiiEyeAccessoryParam.Product.100.rstbl.byml.zs")
    if os.path.exists(rsdb):
        rows = {r["__RowId"]: r for r in nxfmt.byml(nxfmt.read(rsdb))}
        for idx, d in sorted(parts.get("Eye", {}).items()):
            row = rows.get((d.get("ComponentsHash") or {}).get("EyeAccessoryRef"))
            if not row:
                continue
            ax = d.get("AxisForExpression") or {}
            fields = ["axis=%.4f,%.4f" % (ax.get("X", 0.0), ax.get("Y", 0.0))]
            for cat in ("EyelidUpper", "EyelidLower", "EyelashUpper", "EyelashLower", "Highlight"):
                pos = row.get(cat + "Pos") or {}
                fields.append("%s=%d,%d,%d,%d,%d" % (cat, pos.get("X", 0), pos.get("Y", 0), row.get(cat + "Scale", 0),
                                                     row.get(cat + "Aspect", 0), row.get(cat + "Rotate", 0)))
            lines.append("eyeacc %d %s" % (idx, " ".join(fields)))

    # ── Heads: shading, where the face texture lies, the anchors ──
    uvtex = np.zeros((256, 256, 4), np.float32)
    uvtex[..., 0] = (np.arange(256)[None, :] + 0.5) / 256
    uvtex[..., 1] = (np.arange(256)[:, None] + 0.5) / 256
    ref_depth = None
    heads = sorted(parts.get("Faceline", {}).items())
    for idx, d in heads:
        mm = model(d)
        if not mm:
            continue
        name, m = mm
        buf = new_buf()
        for sh in m["shapes"]:
            if sh["name"].startswith("Head"):
                buf.draw(sh["p"], sh["n"], sh["idx"], 1)
        cov = (buf.tag == 1).astype(np.float32)
        if ref_depth is None:
            ref_depth = buf.z.copy()
        r, g = shade_skin(buf, cov > 0)
        if want("head"):
            save(encode(r, g, np.zeros_like(r), cov), out, "head_%02d.png" % idx)
            save(encode_z(buf.z, cov > 0), out, "head_%02d_z.png" % idx)
            # the face texture's u,v over the head
            uv = new_buf()
            for sh in m["shapes"]:
                if sh["name"].startswith("Head"):
                    uv.draw(sh["p"], sh["n"], sh["idx"], 1)
            head_cov = uv.tag == 1
            for sh in m["shapes"]:
                if sh["name"].startswith("Mask") and sh["uv0"] is not None:
                    uv.draw(sh["p"], sh["n"], sh["idx"], 2, uv=sh["uv0"], zbias=0.004)
            # A: where the face texture's sheet is (it reaches past the head:
            # face paint shows there), B: where the head is (the face's parts
            # stay on it)
            mask_on = uv.tag == 2
            u = np.clip(uv.uv[..., 0], 0, 1)
            v = np.clip(uv.uv[..., 1], 0, 1)
            img = np.stack([u, v, np.zeros_like(u), mask_on.astype(np.float32)], -1)
            # nearest reduction for coordinates (they must not blend across the edge)
            img_small = img[SS // 2::SS, SS // 2::SS]
            img_small[..., 2] = gbuf.downsample((mask_on & head_cov).astype(np.float32)[..., None], SS)[..., 0]
            img_small[..., 3] = gbuf.downsample(mask_on.astype(np.float32)[..., None], SS)[..., 0]
            Image.fromarray((np.clip(img_small, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA").save(
                os.path.join(out, "headuv_%02d.png" % idx), optimize=True)
            # the head's own texture (skin, wrinkles, make-up, stubble) over
            # the head: half the face, mirrored down the middle, so u runs
            # from the side (0) to the middle (1); v runs down
            fl = new_buf()
            for sh in m["shapes"]:
                if sh["name"].startswith("Head") and sh["uv0"] is not None:
                    fl.draw(sh["p"], sh["n"], sh["idx"], 1, uv=sh["uv0"])
            fl_on = fl.tag == 1
            fu = fl.uv[..., 0]
            fu = np.where(fu > 1, 2 - fu, fu)
            img = np.stack([np.clip(fu, 0, 1), np.clip(fl.uv[..., 1], 0, 1), np.zeros_like(fu),
                            fl_on.astype(np.float32)], -1)
            img_small = img[SS // 2::SS, SS // 2::SS]
            img_small[..., 3] = gbuf.downsample(fl_on.astype(np.float32)[..., None], SS)[..., 0]
            Image.fromarray((np.clip(img_small, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA").save(
                os.path.join(out, "headfl_%02d.png" % idx), optimize=True)
        anchors = []
        for bn in ("set_nose", "set_hair", "set_beard", "set_ear"):
            p = miimodel.bone_world(m, bn)
            x, y = project(p)
            anchors.append("%s=%.2f,%.2f" % (bn, x, y))
            if idx == heads[0][0]:
                ANCHOR[bn] = miimodel.bone_matrix(m, bn)
        lines.append("head %d head_%02d.png headuv_%02d.png fl=headfl_%02d.png z=head_%02d_z.png %s" % (idx, idx, idx, idx, idx, " ".join(anchors)))
        print("head", idx, name, flush=True)

    # ── Hair: in front of the head, behind it ──
    # B holds where the second hair colour goes (the game's _Mgh masks): the
    # low 4 bits for two-tone back hair (towards the tips), the high 4 bits for
    # two-tone bangs (streaks). A two-channel mask has the bangs in R, the back
    # in G; a one-channel one is the bangs' on a front piece, else the back's.
    def hair_pictures(m, mdl_name, tag, bangs_piece=False):
        buf = new_buf()
        for k, sh in enumerate(m["shapes"]):
            buf.draw(sh["p"], sh["n"], sh["idx"], k, uv=sh["uv0"])
        mim = load_tex(romfs, mdl_name + "_Mim")
        mgh, mgh_fmt = load_tex(romfs, mdl_name + "_Mgh", with_format=True)
        cov_all = buf.tag >= 0
        samp = np.zeros(buf.tag.shape + (4,), np.float32)
        samp[..., 0] = 1.0
        back = np.zeros(buf.tag.shape, np.float32)
        bangs = np.zeros(buf.tag.shape, np.float32)
        for k, sh in enumerate(m["shapes"]):
            if sh["uv0"] is None or "Decoration" in sh["name"]:
                continue
            msk = buf.tag == k
            if not msk.any():
                continue
            if mim is not None:
                samp[msk] = gbuf.sample(mim, buf.uv, msk)[msk]
            if mgh is not None:
                q = gbuf.sample(mgh, buf.uv, msk)
                if mgh_fmt == "BC5":
                    bangs[msk], back[msk] = q[msk, 0], q[msk, 1]
                elif bangs_piece:
                    bangs[msk] = q[msk, 0]
                else:
                    back[msk] = q[msk, 0]
        r, g = shade_hair(buf, cov_all, samp)
        cov = cov_all.astype(np.float32)
        px = np.asarray(encode(r, g, back, cov)).copy()
        bg = np.asarray(encode(r, g, bangs, cov))[..., 2]
        px[..., 2] = ((px[..., 2].astype(np.int32) * 15 + 127) // 255) | (((bg.astype(np.int32) * 15 + 127) // 255) << 4)
        pic = save(Image.fromarray(px, "RGBA"), out, tag + ".png")
        return pic, (save(encode_z(buf.z, cov_all), out, tag + "_z.png") if pic != "-" else "-")

    only_hairs = set(int(x) for x in a.hairs.split(",") if x)
    if want("hair"):
        for idx, d in sorted(parts.get("Hair", {}).items()):
            if only_hairs and idx not in only_hairs:
                continue
            # a hairstyle built of parts (HairBack000) has no model: its front
            # piece and parts are drawn on their own
            built = bool(d.get("HairPartsAttachInfo"))
            mm = model(d)
            if not mm:
                if built:
                    lines.append("hair %d - - 2" % idx)
                continue
            name, m = mm
            f, z = hair_pictures(m, name, "hair_%03d" % idx)
            # attach: 0 alone, 1 takes a front piece, 2 a front piece and hair parts
            lines.append("hair %d %s %s %d" % (idx, f, z, 2 if built else 1 if d.get("IsAttachableHairFront") else 0))
            print("hair", idx, name, flush=True)
    if want("front"):
        for idx, d in sorted(parts.get("HairFront", {}).items()):
            mm = model(d)
            if not mm:
                continue
            name, m = mm
            f, z = hair_pictures(m, name, "front_%03d" % idx, bangs_piece=True)
            lines.append("front %d %s %s" % (idx, f, z))
            print("front", idx, name, flush=True)
    # Hair parts (buns, tails): one model per place, already where it goes.
    # The place is upper / middle / lower (x4) + centre / left / right / both.
    if want("hairpart"):
        for idx, d in sorted(parts.get("HairParts", {}).items()):
            units = d.get("HairPartsModelUnit")
            if not isinstance(units, dict):
                continue
            for v, vn in enumerate(("Upper", "Middle", "Lower")):
                for h, hn in enumerate(("Center", "Left", "Right", "LeftRight")):
                    fm = next((u.get("Fmdb") for u in units.values() if isinstance(u, dict) and
                               (u.get("Fmdb") or "").endswith("_%s%s.fmdb" % (vn, hn))), None)
                    if not fm:
                        continue
                    mdl = os.path.basename(fm).replace(".fmdb", "")
                    fname = mdl[:mdl.rindex("_")]
                    path = os.path.join(romfs, "Model", fname + ".bfres.zs")
                    m = miimodel.load(path, mdl) if os.path.exists(path) else None
                    if not m:
                        continue
                    pos = v * 4 + h
                    f, z = hair_pictures(m, fname, "part_%02d_%02d" % (idx, pos))
                    lines.append("hairpart %d %d %s %s" % (idx, pos, f, z))
            print("hairpart", idx, d.get("FileName"), flush=True)

    # ── Ears: the right one at head 0's anchor ──
    if want("ear"):
        M = ANCHOR.get("set_ear", np.eye(4))
        for idx, d in sorted(parts.get("Ear", {}).items()):
            mm = model(d)
            if not mm:
                continue
            name, m = mm
            buf = new_buf()
            for k, sh in enumerate(m["shapes"]):
                p = sh["p"] @ M[:3, :3].T + M[:3, 3]
                n = sh["n"] @ M[:3, :3].T
                buf.draw(p, n, sh["idx"], k)
            cov = (buf.tag >= 0).astype(np.float32)
            r, g = shade_skin(buf, cov > 0)
            lines.append("ear %d %s %s" % (idx, save(encode(r, g, np.zeros_like(r), cov), out, "ear_%d.png" % idx),
                                           save(encode_z(buf.z, cov > 0), out, "ear_%d_z.png" % idx)))
            print("ear", idx, name, flush=True)

    # ── Noses: the shape, its shine and its line, at head 0's anchor ──
    if want("nose"):
        M = ANCHOR.get("set_nose", np.eye(4))
        for idx, d in sorted(parts.get("Nose", {}).items()):
            mm = model(d)
            if not mm:
                continue
            name, m = mm
            buf = new_buf()
            line = new_buf()
            for k, sh in enumerate(m["shapes"]):
                p = sh["p"] @ M[:3, :3].T + M[:3, 3]
                n = sh["n"] @ M[:3, :3].T
                if sh["name"].startswith("NoseLine"):
                    line.draw(p, n, sh["idx"], 1, uv=sh["uv0"], cull=False)
                else:
                    buf.draw(p, n, sh["idx"], 1, uv=sh["uv0"])
            cov = (buf.tag == 1)
            r, g = shade_skin(buf, cov)
            mim = load_tex(romfs, name + "_Mim")
            if mim is not None and cov.any():
                dot = gbuf.sample(mim, buf.uv, cov)[..., 1]
                g = np.clip(g + 0.85 * dot, 0, 1)
            lmask = load_tex(romfs, name + "_NoseLine_Msk")
            lcov = line.tag == 1
            lalpha = gbuf.sample(lmask, line.uv, lcov, wrap=False)[..., 0] if lmask is not None and lcov.any() else np.zeros(cov.shape, np.float32)
            # the line over the shape: dark, wherever either is
            alpha = np.maximum(cov.astype(np.float32), lalpha)
            rr = np.where(cov, r, FLAT / 1.25)
            bb = lalpha
            lines.append("nose %d %s" % (idx, save(encode(rr, g, bb, alpha), out, "nose_%02d.png" % idx)))
            print("nose", idx, name, flush=True)

    # ── Beards at head 0's anchor ──
    if want("beard"):
        M = ANCHOR.get("set_beard", np.eye(4))
        for idx, d in sorted(parts.get("Beard", {}).items()):
            mm = model(d)
            if not mm:
                continue
            name, m = mm
            buf = new_buf()
            for k, sh in enumerate(m["shapes"]):
                p = sh["p"] @ M[:3, :3].T + M[:3, 3]
                n = sh["n"] @ M[:3, :3].T
                buf.draw(p, n, sh["idx"], k, uv=sh["uv0"])
            cov = buf.tag >= 0
            mim = load_tex(romfs, name + "_Mim")
            samp = gbuf.sample(mim, buf.uv, cov) if mim is not None else None
            r, g = shade_hair(buf, cov, samp)
            lines.append("beard %d %s %s" % (idx, save(encode(r, g, np.zeros_like(r), cov.astype(np.float32)), out, "beard_%02d.png" % idx),
                                             save(encode_z(buf.z, cov), out, "beard_%02d_z.png" % idx)))
            print("beard", idx, name, flush=True)

    # ── Glasses, centred: the frame and the lenses ──
    if want("glass"):
        for idx, d in sorted(parts.get("Glass", {}).items()):
            mm = model(d)
            if not mm:
                continue
            name, m = mm
            s = PORTRAIT * SS
            frame = gbuf.GBuf(s, s, s * SCALE, s / 2, s / 2)
            lens = gbuf.GBuf(s, s, s * SCALE, s / 2, s / 2)
            for k, sh in enumerate(m["shapes"]):
                (lens if sh["name"].startswith(("Opa", "Trs")) else frame).draw(sh["p"], sh["n"], sh["idx"], k, cull=False)
            fc = frame.tag >= 0
            N = frame.normals()
            nl = N @ LIGHT
            fr = (0.78 + 0.30 * gbuf.smoothstep(-0.3, 0.6, nl)) / 1.25
            fg = 0.25 * gbuf.smoothstep(0.65, 0.95, 1 - np.clip(N[..., 2], 0, 1))
            lc = (lens.tag >= 0) & ~fc
            lines.append("glass %d %s %s" % (idx, save(encode(fr, fg, np.zeros_like(fr), fc.astype(np.float32)), out, "glass_%02d.png" % idx),
                                             save(encode(np.full(fr.shape, 0.8, np.float32), np.zeros_like(fr), np.zeros_like(fr),
                                                         lc.astype(np.float32)), out, "lens_%02d.png" % idx)))
            print("glass", idx, name, flush=True)

    with open(os.path.join(out, "index.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("pack:", out)


if __name__ == "__main__":
    main()
