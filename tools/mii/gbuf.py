#!/usr/bin/env python3
"""A small deferred rasterizer for the Mii parts pack: front views
(orthographic, z-buffered) that keep, per pixel, the depth, the normal, the
texture coordinates and which shape covers it, so the shading is worked out
afterwards for the whole picture at once."""
import numpy as np


class GBuf:
    def __init__(self, w, h, scale, cx, cy):
        self.w, self.h, self.scale, self.cx, self.cy = w, h, scale, cx, cy
        self.z = np.full((h, w), -1e9, np.float32)
        self.n = np.zeros((h, w, 3), np.float32)
        self.uv = np.zeros((h, w, 2), np.float32)
        self.uv2 = np.zeros((h, w, 2), np.float32)
        self.tag = np.full((h, w), -1, np.int32)

    def project(self, p):
        return np.stack([self.cx + p[:, 0] * self.scale, self.cy - p[:, 1] * self.scale, p[:, 2]], 1)

    def draw(self, p, n, idx, tag, uv=None, uv2=None, cull=True, zbias=0.0):
        p = np.asarray(p, np.float32)[:, :3]
        s = self.project(p)
        s[:, 2] += zbias
        nn = np.asarray(n, np.float32) if n is not None else np.tile(np.array([0, 0, 1], np.float32), (len(p), 1))
        uvs = np.asarray(uv, np.float32)[:, :2] if uv is not None else None
        uvs2 = np.asarray(uv2, np.float32)[:, :2] if uv2 is not None else None
        tri = np.asarray(idx).reshape(-1, 3)
        A, B, C = s[tri[:, 0]], s[tri[:, 1]], s[tri[:, 2]]
        area = (B[:, 0] - A[:, 0]) * (C[:, 1] - A[:, 1]) - (B[:, 1] - A[:, 1]) * (C[:, 0] - A[:, 0])
        keep = (area < 0) if cull else (area != 0)
        for t in np.nonzero(keep)[0]:
            a, b, c = A[t], B[t], C[t]
            x0 = max(int(np.floor(min(a[0], b[0], c[0]))), 0)
            x1 = min(int(np.ceil(max(a[0], b[0], c[0]))), self.w - 1)
            y0 = max(int(np.floor(min(a[1], b[1], c[1]))), 0)
            y1 = min(int(np.ceil(max(a[1], b[1], c[1]))), self.h - 1)
            if x1 < x0 or y1 < y0:
                continue
            ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1]
            px, py = xs + 0.5, ys + 0.5
            ar = area[t]
            w0 = ((b[0] - px) * (c[1] - py) - (b[1] - py) * (c[0] - px)) / ar
            w1 = ((c[0] - px) * (a[1] - py) - (c[1] - py) * (a[0] - px)) / ar
            w2 = 1.0 - w0 - w1
            inside = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
            if not inside.any():
                continue
            z = w0 * a[2] + w1 * b[2] + w2 * c[2]
            zb = self.z[y0:y1 + 1, x0:x1 + 1]
            win = inside & (z > zb)
            if not win.any():
                continue
            i0, i1, i2 = tri[t]
            zb[win] = z[win]
            nreg = self.n[y0:y1 + 1, x0:x1 + 1]
            for k in range(3):
                nreg[..., k][win] = (w0 * nn[i0, k] + w1 * nn[i1, k] + w2 * nn[i2, k])[win]
            if uvs is not None:
                ureg = self.uv[y0:y1 + 1, x0:x1 + 1]
                for k in range(2):
                    ureg[..., k][win] = (w0 * uvs[i0, k] + w1 * uvs[i1, k] + w2 * uvs[i2, k])[win]
            if uvs2 is not None:
                ureg = self.uv2[y0:y1 + 1, x0:x1 + 1]
                for k in range(2):
                    ureg[..., k][win] = (w0 * uvs2[i0, k] + w1 * uvs2[i1, k] + w2 * uvs2[i2, k])[win]
            self.tag[y0:y1 + 1, x0:x1 + 1][win] = tag

    def normals(self):
        n = self.n.copy()
        ln = np.linalg.norm(n, axis=2, keepdims=True)
        return n / np.maximum(ln, 1e-6)


def sample(tex, uv, mask, wrap=True):
    """Texture (H,W,C float) at uv (H,W,2) where mask, nearest; zeros elsewhere."""
    th, tw = tex.shape[:2]
    u, v = uv[..., 0], uv[..., 1]
    if wrap:
        u, v = u % 1.0, v % 1.0
    tx = np.clip((u * tw).astype(np.int32), 0, tw - 1)
    ty = np.clip((v * th).astype(np.int32), 0, th - 1)
    out = tex[ty, tx]
    out[~mask] = 0
    return out


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def downsample(img, ss):
    """Box-filter an (H,W,C) float picture by ss (premultiplied colours stay right)."""
    h, w = img.shape[:2]
    return img.reshape(h // ss, ss, w // ss, ss, -1).mean(axis=(1, 3))
