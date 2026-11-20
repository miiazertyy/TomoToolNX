#!/usr/bin/env python3
"""A small software rasterizer for the Mii parts research: front views of the
game's 3D Mii parts (orthographic, z-buffered, Lambert-lit), so the console
can later draw a Mii as flat layers."""
import numpy as np


class Canvas:
    def __init__(self, w, h, scale, cx, cy):
        self.w, self.h, self.scale, self.cx, self.cy = w, h, scale, cx, cy
        self.color = np.zeros((h, w, 4), np.float32)
        self.z = np.full((h, w), -1e9, np.float32)

    def project(self, p):
        return np.stack([self.cx + p[:, 0] * self.scale, self.cy - p[:, 1] * self.scale, p[:, 2]], 1)

    def draw(self, p, n, idx, rgb, light=(-0.35, 0.45, 0.82), uv=None, tex=None, alpha_from_tex=False, ambient=0.42):
        """Triangles `idx` of points `p` (model space) with normals `n`, in colour `rgb` (0..1),
        optionally multiplied by `tex` (RGBA float 0..1) sampled at `uv`."""
        p = np.asarray(p, np.float32)[:, :3]
        s = self.project(p)
        L = np.array(light, np.float32)
        L /= np.linalg.norm(L)
        shade = ambient + (1 - ambient) * np.clip(n @ L, 0, 1) if n is not None else np.ones(len(p), np.float32)
        for t in idx:
            a, b, c = s[t[0]], s[t[1]], s[t[2]]
            # back faces away
            cross = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
            if cross >= 0:
                continue
            x0 = max(int(np.floor(min(a[0], b[0], c[0]))), 0)
            x1 = min(int(np.ceil(max(a[0], b[0], c[0]))), self.w - 1)
            y0 = max(int(np.floor(min(a[1], b[1], c[1]))), 0)
            y1 = min(int(np.ceil(max(a[1], b[1], c[1]))), self.h - 1)
            if x1 < x0 or y1 < y0:
                continue
            ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1]
            px, py = xs + 0.5, ys + 0.5
            area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
            if area == 0:
                continue
            w0 = ((b[0] - px) * (c[1] - py) - (b[1] - py) * (c[0] - px)) / area
            w1 = ((c[0] - px) * (a[1] - py) - (c[1] - py) * (a[0] - px)) / area
            w2 = ((a[0] - px) * (b[1] - py) - (a[1] - py) * (b[0] - px)) / area
            inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            if not inside.any():
                continue
            z = w0 * a[2] + w1 * b[2] + w2 * c[2]
            zb = self.z[y0:y1 + 1, x0:x1 + 1]
            win = inside & (z > zb)
            if not win.any():
                continue
            sh = w0 * shade[t[0]] + w1 * shade[t[1]] + w2 * shade[t[2]]
            col = np.empty(win.shape + (4,), np.float32)
            col[..., 0] = rgb[0] * sh
            col[..., 1] = rgb[1] * sh
            col[..., 2] = rgb[2] * sh
            col[..., 3] = 1.0
            if tex is not None and uv is not None:
                u = w0 * uv[t[0], 0] + w1 * uv[t[1], 0] + w2 * uv[t[2], 0]
                v = w0 * uv[t[0], 1] + w1 * uv[t[1], 1] + w2 * uv[t[2], 1]
                th, tw = tex.shape[:2]
                tx = np.clip((u % 1.0) * tw, 0, tw - 1).astype(np.int32)
                ty = np.clip((v % 1.0) * th, 0, th - 1).astype(np.int32)
                tc = tex[ty, tx]
                if alpha_from_tex:
                    col = tc.copy()
                    col[..., :3] *= sh[..., None]
                else:
                    col[..., :3] *= tc[..., :3]
            zb[win] = z[win]
            region = self.color[y0:y1 + 1, x0:x1 + 1]
            a_ = col[..., 3:4]
            region[win] = region[win] * (1 - a_[win]) + np.concatenate([col[..., :3] * a_, a_], -1)[win]

    def image(self):
        from PIL import Image
        c = self.color.copy()
        a = np.clip(c[..., 3:4], 1e-6, 1)
        rgb = np.where(c[..., 3:4] > 0, c[..., :3] / a, 0)
        return Image.fromarray(np.clip(np.concatenate([rgb, c[..., 3:4]], -1) * 255, 0, 255).astype(np.uint8), "RGBA")
