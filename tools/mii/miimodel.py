#!/usr/bin/env python3
"""A Mii part's model where the game draws it: every shape's points and
normals in model space (Mii research, tools/mii/README.md).

BFRES shapes are bound to bones three ways (the FSHP's vertex skin count):
  0  rigid to the shape's own bone: its points are in that bone's space
  1  rigid per point: `_i0` names a matrix, the skeleton's matrix-to-bone
     list names the bone, the point is in that bone's space
  2+ smooth skinning: the points are already in model space (bind pose)
Bones keep a scale, an Euler XYZ rotation (radians) and a translation, each
relative to the parent."""
import os
import struct

import numpy as np

import nxfmt


def _euler(rx, ry, rz):
    cx, sx, cy, sy, cz, sz = np.cos(rx), np.sin(rx), np.cos(ry), np.sin(ry), np.cos(rz), np.sin(rz)
    X = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Y = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Z = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return Z @ Y @ X


def _local(b):
    m = np.eye(4)
    m[:3, :3] = _euler(*b["r"][:3]) @ np.diag(b["s"])
    m[:3, 3] = b["t"]
    return m


def world_matrices(bones):
    out = [None] * len(bones)

    def get(i):
        if out[i] is None:
            b = bones[i]
            out[i] = _local(b) if b["parent"] < 0 else get(b["parent"]) @ _local(b)
        return out[i]
    for i in range(len(bones)):
        get(i)
    return out


def _matrix_to_bone(d, model_off):
    """The skeleton's matrix-to-bone list (u16 per matrix: smooth ones, then rigid)."""
    skel = struct.unpack_from("<Q", d, model_off + 0x18)[0]
    if not skel or d[skel:skel + 4] != b"FSKL":
        return []
    lst = struct.unpack_from("<Q", d, skel + 0x18)[0]
    nb, nsm, nrg = struct.unpack_from("<HHH", d, skel + 0x38)
    if not lst:
        return []
    return list(struct.unpack_from("<%dH" % (nsm + nrg), d, lst))


def _attr_raw(d, model_off, vb_index, name):
    """A vertex attribute by name, as integers (for `_i0`)."""
    m = model_off
    vtx_arr = struct.unpack_from("<Q", d, m + 0x20)[0]
    buf_info = struct.unpack_from("<Q", d, 0xB0)[0]
    buf_base = struct.unpack_from("<Q", d, buf_info + 8)[0] if buf_info else 0
    v = vtx_arr + vb_index * 0x58
    attr_arr = struct.unpack_from("<Q", d, v + 8)[0]
    sizes_arr, strides_arr = struct.unpack_from("<QQ", d, v + 0x30)
    boff, n_attr, n_buf, _ = struct.unpack_from("<IBBH", d, v + 0x48)
    count = struct.unpack_from("<I", d, v + 0x50)[0]
    sizes = [struct.unpack_from("<I", d, sizes_arr + 0x10 * k)[0] for k in range(n_buf)]
    strides = [struct.unpack_from("<I", d, strides_arr + 0x10 * k)[0] for k in range(n_buf)]
    starts, pos = [], buf_base + boff
    for k in range(n_buf):
        starts.append(pos)
        pos += (sizes[k] + 0xFF) & ~0xFF
    for k in range(n_attr):
        a = attr_arr + k * 0x10
        anp, fmt, aoff, bidx = struct.unpack_from("<QIHH", d, a)
        if nxfmt._s(d, anp) != name:
            continue
        base, stride = starts[bidx] + aoff, strides[bidx]
        comp = fmt >> 8
        kind = {0x02: ("<u1", 1), 0x09: ("<u1", 2), 0x0B: ("<u1", 4), 0x03: ("<u2", 1), 0x0A: ("<u2", 2), 0x12: ("<u2", 2),
                0x0C: ("<u2", 4), 0x14: ("<u4", 1)}.get(comp)
        if kind is None:
            return None
        dt, n = kind
        out = np.empty((count, n), np.int64)
        sz = np.dtype(dt).itemsize
        for i in range(count):
            out[i] = struct.unpack_from("<%d%s" % (n, dt[-2:].replace("u1", "B").replace("u2", "H").replace("u4", "I")), d,
                                        base + i * stride)
        return out
    return None


def load(path, name=None):
    """{'name', 'bones', 'world': [4x4], 'shapes': [{'name', 'p' (N,3 model space), 'n' (N,3), 'uv0', 'uv1', 'idx'}]}
    The file's first model, or the one called name (None when it has none)."""
    d = nxfmt.read(path)
    res = nxfmt.bfres(d)
    if name is not None:
        found = [mm for mm in res["models"] if mm["name"] == name]
        if not found:
            return None
        m = found[0]
    else:
        m = res["models"][0]
    model_off = m["off"]
    world = world_matrices(m["bones"]) if m["bones"] else []
    m2b = _matrix_to_bone(d, model_off)
    shp_arr = struct.unpack_from("<Q", d, model_off + 0x28)[0]
    out = []
    for i, sh in enumerate(m["shapes"]):
        p = np.asarray(sh["p"], np.float64)[:, :3]
        n = np.asarray(sh["n"], np.float64)[:, :3] if sh["n"] is not None else None
        skin = sh.get("skin", 0)
        if skin == 0 and 0 <= sh["bone"] < len(world):
            M = world[sh["bone"]]
            p = p @ M[:3, :3].T + M[:3, 3]
            if n is not None:
                n = n @ M[:3, :3].T
        elif skin == 1 and world:
            s = shp_arr + i * 0x60
            vbi = struct.unpack_from("<H", d, s + 0x56)[0]
            idx = _attr_raw(d, model_off, vbi, "_i0")
            if idx is not None and m2b:
                q = np.empty_like(p)
                nn = np.empty_like(n) if n is not None else None
                for k in range(len(p)):
                    mi = int(idx[k, 0])
                    bi = m2b[mi] if mi < len(m2b) else 0
                    M = world[bi]
                    q[k] = M[:3, :3] @ p[k] + M[:3, 3]
                    if nn is not None:
                        nn[k] = M[:3, :3] @ n[k]
                p, n = q, nn
        if n is not None:
            ln = np.linalg.norm(n, axis=1, keepdims=True)
            n = n / np.maximum(ln, 1e-9)
        out.append({"name": sh["name"], "p": p.astype(np.float32), "n": None if n is None else n.astype(np.float32),
                    "uv0": sh.get("uv0"), "uv1": sh.get("uv1"), "idx": sh["idx"], "skin": skin})
    return {"name": m["name"], "bones": m["bones"], "world": world, "shapes": out}


def bone_world(model, name):
    """A bone's world position (model space)."""
    for i, b in enumerate(model["bones"]):
        if b["name"] == name:
            return np.asarray(model["world"][i][:3, 3], np.float32)
    return np.zeros(3, np.float32)


def bone_matrix(model, name):
    for i, b in enumerate(model["bones"]):
        if b["name"] == name:
            return model["world"][i]
    return np.eye(4)


if __name__ == "__main__":
    import sys
    for f in sys.argv[1:]:
        mm = load(f)
        print(os.path.basename(f))
        for s in mm["shapes"]:
            p = s["p"]
            print("  %-36s skin %d  x[%.3f %.3f] y[%.3f %.3f] z[%.3f %.3f]" % (s["name"], s["skin"], p[:, 0].min(), p[:, 0].max(),
                                                                              p[:, 1].min(), p[:, 1].max(), p[:, 2].min(), p[:, 2].max()))
