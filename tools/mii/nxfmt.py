#!/usr/bin/env python3
"""Readers for the Switch file formats the Mii parts use (for the research in
tools/mii/): zstd (.zs), SARC (.pack/.sarc), BYML (.byml/.bgyml), BNTX
(textures: BC1-BC5 and BC7 decoded in numpy, after undoing the block-linear
swizzle) and BFRES models (shapes: positions, normals, UVs, indices).

Written from the public format notes on the Switch modding wikis; only what
the Mii parts need is read.
"""
import struct

import numpy as np

try:
    import zstandard
except ImportError:  # pragma: no cover
    zstandard = None


def unzs(data):
    if data[:4] == b"\x28\xb5\x2f\xfd":
        return zstandard.ZstdDecompressor().decompress(data, max_output_size=512 << 20)
    return data


def read(path):
    with open(path, "rb") as f:
        return unzs(f.read())


# ── SARC ─────────────────────────────────────────────────────────────────────
def sarc(data):
    """{name: bytes}"""
    assert data[:4] == b"SARC", "not a SARC"
    bom = data[6:8]
    e = "<" if bom == b"\xff\xfe" else ">"
    hdr_size, bom_, size, data_off = struct.unpack_from(e + "HHII", data, 4)
    sfat = 0x14
    assert data[sfat:sfat + 4] == b"SFAT"
    _, count, _ = struct.unpack_from(e + "HHI", data, sfat + 4)
    nodes = [struct.unpack_from(e + "IIII", data, sfat + 12 + i * 16) for i in range(count)]
    sfnt = sfat + 12 + count * 16
    assert data[sfnt:sfnt + 4] == b"SFNT"
    names_base = sfnt + 8
    out = {}
    for h, attr, start, end in nodes:
        name = ""
        if attr & 0x01000000:
            off = names_base + (attr & 0xFFFF) * 4
            name = data[off:data.index(b"\0", off)].decode("utf-8", "replace")
        out[name or "%08x" % h] = data[data_off + start:data_off + end]
    return out


# ── BYML ─────────────────────────────────────────────────────────────────────
def byml(data):
    """A BYML document as Python dicts, lists and values."""
    e = "<" if data[:2] == b"YB" else ">"
    version, keys_off, strs_off, root_off = struct.unpack_from(e + "HIII", data, 2)

    def table(off):
        if not off:
            return []
        n = int.from_bytes(data[off + 1:off + 4], "little" if e == "<" else "big")
        offs = struct.unpack_from(e + "%dI" % (n + 1), data, off + 4)
        return [data[off + offs[i]:data.index(b"\0", off + offs[i])].decode("utf-8", "replace") for i in range(n)]

    keys, strs = table(keys_off), table(strs_off)

    def node(t, v):
        if t == 0xA0:
            return strs[v]
        if t in (0xC0, 0xC1, 0x20):
            return parse(v)
        if t == 0xD0:
            return bool(v)
        if t == 0xD1:
            return struct.unpack(e + "i", struct.pack(e + "I", v))[0]
        if t == 0xD2:
            return struct.unpack(e + "f", struct.pack(e + "I", v))[0]
        if t == 0xD3:
            return v
        if t in (0xD4, 0xD5, 0xD6):        # 64-bit values: v is an offset
            raw = data[v:v + 8]
            return struct.unpack(e + {0xD4: "q", 0xD5: "Q", 0xD6: "d"}[t], raw)[0]
        if t == 0xFF:
            return None
        return ("?%02x" % t, v)

    def parse(off):
        t = data[off]
        n = int.from_bytes(data[off + 1:off + 4], "little" if e == "<" else "big")
        if t == 0xC0:                       # array
            types = data[off + 4:off + 4 + n]
            vals_off = (off + 4 + n + 3) & ~3
            return [node(types[i], struct.unpack_from(e + "I", data, vals_off + 4 * i)[0]) for i in range(n)]
        if t == 0xC1:                       # dictionary
            out = {}
            for i in range(n):
                k, = struct.unpack_from(e + "I", data, off + 4 + 8 * i)
                ki = (k >> 8) if e == ">" else (k & 0xFFFFFF)   # a 24-bit key index, then the type byte
                kt = data[off + 4 + 8 * i + 3]
                v, = struct.unpack_from(e + "I", data, off + 4 + 8 * i + 4)
                out[keys[ki]] = node(kt, v)
            return out
        if t == 0x20:                       # dictionary keyed by 32-bit hashes (v7): pairs, then the types
            out = {}
            types = data[off + 4 + 8 * n:off + 4 + 9 * n]
            for i in range(n):
                k, v = struct.unpack_from(e + "II", data, off + 4 + 8 * i)
                out[k] = node(types[i], v)
            return out
        return node(t, struct.unpack_from(e + "I", data, off + 4)[0])

    return parse(root_off) if root_off else None


# ── BNTX ─────────────────────────────────────────────────────────────────────
FMT = {  # BNTX format high byte → (name, block bytes, block size)
    0x1A: ("BC1", 8), 0x1B: ("BC2", 16), 0x1C: ("BC3", 16), 0x1D: ("BC4", 8), 0x1E: ("BC5", 16),
    0x1F: ("BC6", 16), 0x20: ("BC7", 16), 0x2D: ("ASTC4x4", 16), 0x2F: ("ASTC5x5", 16), 0x34: ("ASTC8x8", 16), 0x0B: ("RGBA8", 4), 0x02: ("R8", 1), 0x09: ("RG8", 2),
}


def bntx(data):
    """[(name, fmt name, width, height, data bytes, info)] — data is the first mip, still swizzled."""
    assert data[:4] == b"BNTX"
    nx = 0x20
    assert data[nx:nx + 4] == b"NX  "
    count, = struct.unpack_from("<I", data, nx + 4)
    info_ptr, = struct.unpack_from("<Q", data, nx + 8)
    out = []
    for i in range(count):
        brti_off, = struct.unpack_from("<Q", data, info_ptr + 8 * i)
        b = brti_off
        assert data[b:b + 4] == b"BRTI"
        flags, dim, tile_mode, swizzle, mips, samples = struct.unpack_from("<BBHHHI", data, b + 0x10)
        fmt, = struct.unpack_from("<I", data, b + 0x1C)
        w, h, d = struct.unpack_from("<III", data, b + 0x24)
        array_len, = struct.unpack_from("<I", data, b + 0x30)
        block_height_log2, = struct.unpack_from("<I", data, b + 0x34)
        data_size, alignment = struct.unpack_from("<II", data, b + 0x50)
        name_off, = struct.unpack_from("<Q", data, b + 0x60)
        mip_ptrs_off, = struct.unpack_from("<Q", data, b + 0x70)
        name_len, = struct.unpack_from("<H", data, name_off)
        name = data[name_off + 2:name_off + 2 + name_len].decode("utf-8", "replace")
        mip0, = struct.unpack_from("<Q", data, mip_ptrs_off)
        kind = fmt >> 8
        out.append((name, FMT.get(kind, ("fmt%02x" % kind, 0))[0], w, h, data[mip0:mip0 + data_size],
                    {"fmt": fmt, "tile": tile_mode, "bh": block_height_log2, "mips": mips, "array": array_len}))
    return out


def deswizzle(data, width_blocks, height_blocks, bpb, block_height_log2):
    """Undo the Tegra block-linear layout (GOBs of 64 bytes x 8 rows)."""
    bh = 1 << block_height_log2
    pitch = width_blocks * bpb
    gob_w = 64
    width_gobs = (pitch + gob_w - 1) // gob_w
    out = bytearray(pitch * height_blocks)
    src = memoryview(data)
    for y in range(height_blocks):
        for xb in range(pitch):
            # The address of byte (xb, y) in block-linear memory.
            gob_addr = ((y // (8 * bh)) * width_gobs * bh + (xb // 64) * bh + (y % (8 * bh)) // 8) * 512
            x = xb % 64
            yy = y % 8
            addr = gob_addr + ((x % 64) // 32) * 256 + ((yy % 8) // 2) * 64 + ((x % 32) // 16) * 32 + (yy % 2) * 16 + (x % 16)
            if addr < len(src):
                out[y * pitch + xb] = src[addr]
    return bytes(out)


def _bc1_block(b, alpha=True):
    c0, c1, idx = struct.unpack("<HHI", b)
    def rgb(c):
        return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31
    p = [rgb(c0) + (255,), rgb(c1) + (255,)]
    if c0 > c1 or not alpha:
        p.append(tuple((2 * p[0][i] + p[1][i]) // 3 for i in range(3)) + (255,))
        p.append(tuple((p[0][i] + 2 * p[1][i]) // 3 for i in range(3)) + (255,))
    else:
        p.append(tuple((p[0][i] + p[1][i]) // 2 for i in range(3)) + (255,))
        p.append((0, 0, 0, 0))
    return [p[(idx >> (2 * k)) & 3] for k in range(16)]


def _bc4_block(b):
    a0, a1 = b[0], b[1]
    bits = int.from_bytes(b[2:8], "little")
    if a0 > a1:
        pal = [a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)]
    else:
        pal = [a0, a1] + [((5 - k) * a0 + k * a1) // 5 for k in range(1, 5)] + [0, 255]
    return [pal[(bits >> (3 * k)) & 7] for k in range(16)]


ASTC = {0x2D: (4, 4), 0x2E: (5, 4), 0x2F: (5, 5), 0x30: (6, 5), 0x31: (6, 6), 0x32: (8, 5), 0x33: (8, 6), 0x34: (8, 8),
        0x35: (10, 5), 0x36: (10, 6), 0x37: (10, 8), 0x38: (10, 10), 0x39: (12, 10), 0x3A: (12, 12)}


def decode(name, fmt, w, h, raw, info):
    """RGBA uint8 array (h, w, 4) of the first mip (texture2ddecoder for the
    block formats, after the block-linear swizzle is undone)."""
    import texture2ddecoder as t2d
    kind = info["fmt"] >> 8
    if kind in ASTC or kind in (0x1A, 0x1C, 0x1D, 0x1E, 0x20):
        bw_, bh_ = ASTC.get(kind, (4, 4))
        nbx, nby = (w + bw_ - 1) // bw_, (h + bh_ - 1) // bh_
        bpb = 8 if kind in (0x1A, 0x1D) else 16
        lin = deswizzle(raw, nbx, nby, bpb, info["bh"]) if info["tile"] == 0 else raw
        if kind in ASTC:
            out = t2d.decode_astc(lin, nbx * bw_, nby * bh_, bw_, bh_)
        else:
            fn = {0x1A: t2d.decode_bc1, 0x1C: t2d.decode_bc3, 0x1D: t2d.decode_bc4, 0x1E: t2d.decode_bc5, 0x20: t2d.decode_bc7}[kind]
            out = fn(lin, nbx * 4, nby * 4)
        img = np.frombuffer(out, np.uint8).reshape(nby * bh_, nbx * bw_, 4)[:h, :w]
        return img[..., [2, 1, 0, 3]].copy()          # BGRA -> RGBA
    if kind == 0x0B:
        lin = deswizzle(raw, w, h, 4, info["bh"]) if info["tile"] == 0 else raw
        return np.frombuffer(lin[:w * h * 4], np.uint8).reshape(h, w, 4).copy()
    if kind == 0x02:
        lin = deswizzle(raw, w, h, 1, info["bh"]) if info["tile"] == 0 else raw
        g = np.frombuffer(lin[:w * h], np.uint8).reshape(h, w)
        return np.dstack([g, g, g, np.full_like(g, 255)])
    raise ValueError("format %x not decoded" % info["fmt"])


# BC7: the full mode table (from the Khronos BPTC description).
_BC7_MODES = [  # ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2
    (3, 4, 0, 0, 4, 0, 1, 0, 3, 0), (2, 6, 0, 0, 6, 0, 0, 1, 3, 0), (3, 6, 0, 0, 5, 0, 0, 0, 2, 0),
    (2, 6, 0, 0, 7, 0, 1, 0, 2, 0), (1, 0, 2, 1, 5, 6, 0, 0, 2, 3), (1, 0, 2, 0, 7, 8, 0, 0, 2, 2),
    (1, 0, 0, 0, 7, 7, 1, 0, 4, 0), (2, 6, 0, 0, 5, 5, 1, 0, 2, 0)]
_P2 = [0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80, 0xc800, 0xffec, 0xfe80, 0xe800, 0xffe8, 0xff00, 0xfff0, 0xf000,
       0xf710, 0x008e, 0x7100, 0x08ce, 0x008c, 0x7310, 0x3100, 0x8cce, 0x088c, 0x3110, 0x6666, 0x366c, 0x17e8, 0x0ff0, 0x718e, 0x399c,
       0xaaaa, 0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696, 0xa55a, 0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660,
       0x0272, 0x04e4, 0x4e40, 0x2720, 0xc936, 0x936c, 0x39c6, 0x639c, 0x9336, 0x9cc6, 0x817e, 0xe718, 0xccf0, 0x0fcc, 0x7744, 0xee22]
_P3 = ["0011001102210222", "0001001122112221", "0000200122112211", "0222002200110111", "0000000011221122", "0011001100220022",
       "0022002211111111", "0011001122112211", "0000000011112222", "0000111111112222", "0000111122222222", "0012001200120012",
       "0112011201120112", "0122012201220122", "0011011211221222", "0011200122002220", "0001001101120122", "0000100111221222",
       "0000000222110022", "0012001200120012", "0122012201220122", "0011220011221122", "0001002201220122", "0000112211221122",
       "0222001200110122", "0011001101220222", "0122012200120012", "0001101200121112", "0111001200120111", "0002000201220122",
       "0000110112211222", "0000111122221111", "0000222200001111", "0112011201120112", "0111012201220122", "0011000101110222",
       "0000000011121222", "0110110011001100", "0112011221222122", "0010202001021202", "0000210022102221", "0110211022100110",
       "0000000022221111", "0002000200020222", "0222202220002000", "0012021200120212", "0010222002200100", "0111022202220111",
       "0000000002020202", "0000210000212100", "0000000012120000", "0220122102201221", "0010121010101010", "0102010222220000",
       "0012001200122222", "0100010001000111", "0000112211001122", "0000002200110011", "0222022202220000", "0110011022222222",
       "0010222220100000", "0120011202212000", "0101110011101011", "0000000001111111"]
_A2 = [15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
       15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15]
_A3a = [3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
        8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3]
_A3b = [15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
        15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8]
_W = {2: [0, 21, 43, 64], 3: [0, 9, 18, 27, 37, 46, 55, 64], 4: [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64]}


def bc7_block(b):
    bits = int.from_bytes(b, "little")
    pos = [0]

    def take(n):
        v = (bits >> pos[0]) & ((1 << n) - 1)
        pos[0] += n
        return v

    mode = 0
    while mode < 8 and not take(1):
        mode += 1
    if mode >= 8:
        return [(0, 0, 0, 0)] * 16
    ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2 = _BC7_MODES[mode]
    part = take(pb)
    rot = take(rb)
    idxsel = take(isb)
    ends = [[[0] * 4 for _ in range(2)] for _ in range(ns)]
    for c in range(3):
        for s in range(ns):
            for e in range(2):
                ends[s][e][c] = take(cb)
    for s in range(ns):
        for e in range(2):
            ends[s][e][3] = take(ab) if ab else 255
    pbits = []
    if epb:
        pbits = [[take(1), take(1)] for _ in range(ns)]
    elif spb:
        for s in range(ns):
            v = take(1)
            pbits.append([v, v])
    for s in range(ns):
        for e in range(2):
            for c in range(4):
                bitsn = cb if c < 3 else ab
                if c == 3 and not ab:
                    continue
                v = ends[s][e][c]
                n = bitsn
                if pbits:
                    v = (v << 1) | pbits[s][e]
                    n += 1
                v = (v << (8 - n)) | (v >> (2 * n - 8)) if n < 8 else v
                ends[s][e][c] = v & 0xFF

    def subset(i):
        if ns == 1:
            return 0
        if ns == 2:
            return (_P2[part] >> i) & 1
        return int(_P3[part][i])

    def anchor(i, s):
        if s == 0:
            return i == 0
        if ns == 2:
            return i == _A2[part]
        return i == (_A3a[part] if s == 1 else _A3b[part])

    idx = []
    for i in range(16):
        s = subset(i)
        n = ib - (1 if (i == 0 or anchor(i, s)) else 0)
        idx.append(take(n))
    idx2 = []
    if ib2:
        for i in range(16):
            n = ib2 - (1 if i == 0 else 0)
            idx2.append(take(n))
    out = []
    for i in range(16):
        s = subset(i)
        e0, e1 = ends[s]
        if ib2:
            ci, ai = (idx2[i], idx[i]) if idxsel else (idx[i], idx2[i])
            cw, aw = (_W[ib2], _W[ib]) if idxsel else (_W[ib], _W[ib2])
            w_c, w_a = cw[ci], aw[ai]
        else:
            w_c = w_a = _W[ib][idx[i]]
        px = [((64 - w_c) * e0[c] + w_c * e1[c] + 32) >> 6 for c in range(3)]
        a = ((64 - w_a) * e0[3] + w_a * e1[3] + 32) >> 6
        px.append(a)
        if rot:
            px[rot - 1], px[3] = px[3], px[rot - 1]
        out.append(tuple(px))
    return out


# ── BFRES (v10 models) ───────────────────────────────────────────────────────
def _s(d, off):
    if not off:
        return ""
    n = struct.unpack_from("<H", d, off)[0]
    return d[off + 2:off + 2 + n].decode("utf-8", "replace")


def _attr(d, base, stride, count, fmt):
    raw = np.frombuffer(d, np.uint8, count * stride, base).reshape(count, stride)
    if fmt == 0x1805:                         # 32_32_32 float
        return raw[:, :12].copy().view("<f4").reshape(count, 3)
    if fmt == 0x1705:                         # 32_32 float
        return raw[:, :8].copy().view("<f4").reshape(count, 2)
    if fmt == 0x1205:                         # 16_16 float
        return raw[:, :4].copy().view("<f2").reshape(count, 2).astype(np.float32)
    if fmt == 0x1505:                         # 16_16_16_16 float
        return raw[:, :8].copy().view("<f2").reshape(count, 4).astype(np.float32)
    if fmt == 0x0E02:                         # 10_10_10_2 snorm
        v = raw[:, :4].copy().view("<u4").reshape(count)
        out = np.zeros((count, 3), np.float32)
        for k in range(3):
            c = ((v >> (10 * k)) & 0x3FF).astype(np.int32)
            c = np.where(c >= 512, c - 1024, c)
            out[:, k] = np.maximum(c / 511.0, -1.0)
        return out
    if fmt == 0x0B01:                         # 8_8_8_8 unorm
        return raw[:, :4].astype(np.float32) / 255.0
    if fmt == 0x0B02:
        return raw[:, :4].view(np.int8).astype(np.float32) / 127.0
    if fmt == 0x0901:                         # 8_8 unorm
        return raw[:, :2].astype(np.float32) / 255.0
    if fmt == 0x1201:                         # 16_16 unorm
        return raw[:, :4].copy().view("<u2").reshape(count, 2).astype(np.float32) / 65535.0
    return None


def bfres(d):
    """{'models': [{'name', 'bones': [...], 'shapes': [{'name', 'material', 'p', 'n', 'uv0', 'uv1', 'idx', 'bone'}]}]}"""
    assert d[:8] == b"FRES    "
    buf_info = struct.unpack_from("<Q", d, 0xB0)[0]
    buf_base = struct.unpack_from("<Q", d, buf_info + 8)[0] if buf_info else 0
    model_arr = struct.unpack_from("<Q", d, 0x28)[0]
    out = {"models": []}
    # the models sit one after another: the stride is the distance to the next
    # FMDL header (a file of several models holds, say, each place of a hair part)
    nxt = d.find(b"FMDL", model_arr + 4) if model_arr else -1
    stride = nxt - model_arr if nxt > 0 and nxt - model_arr <= 0x100 else 0
    m = model_arr
    while m and d[m:m + 4] == b"FMDL":
        name = _s(d, struct.unpack_from("<Q", d, m + 8)[0])
        skel, vtx_arr, shp_arr, _, mat_arr = struct.unpack_from("<QQQQQ", d, m + 0x18)
        n_vtx, n_shp, n_mat = struct.unpack_from("<HHH", d, m + 0x68)
        mats = []
        for i in range(n_mat):
            mo = mat_arr + i * 0xB0 if False else None
        # vertex buffers
        vbs = []
        for i in range(n_vtx):
            v = vtx_arr + i * 0x58
            assert d[v:v + 4] == b"FVTX"
            attr_arr = struct.unpack_from("<Q", d, v + 8)[0]
            sizes_arr, strides_arr = struct.unpack_from("<QQ", d, v + 0x30)
            boff, n_attr, n_buf, _ = struct.unpack_from("<IBBH", d, v + 0x48)
            count = struct.unpack_from("<I", d, v + 0x50)[0]
            sizes = [struct.unpack_from("<I", d, sizes_arr + 0x10 * k)[0] for k in range(n_buf)]
            strides = [struct.unpack_from("<I", d, strides_arr + 0x10 * k)[0] for k in range(n_buf)]
            starts, pos = [], buf_base + boff
            for k in range(n_buf):
                starts.append(pos)
                pos += (sizes[k] + 0xFF) & ~0xFF          # buffers sit on 256-byte boundaries
            attrs = {}
            for k in range(n_attr):
                a = attr_arr + k * 0x10
                anp, fmt, aoff, bidx = struct.unpack_from("<QIHH", d, a)
                val = _attr(d, starts[bidx] + aoff, strides[bidx], count, fmt)
                if val is not None:
                    attrs[_s(d, anp)] = val
            vbs.append(attrs)
        shapes = []
        for i in range(n_shp):
            s = shp_arr + i * 0x60
            assert d[s:s + 4] == b"FSHP"
            sname = _s(d, struct.unpack_from("<Q", d, s + 8)[0])
            vb_ptr, mesh_arr = struct.unpack_from("<QQ", d, s + 0x10)
            vb_index = (vb_ptr - vtx_arr) // 0x58
            # index, material, bone (the shape's own, for rigid shapes), vertex
            # buffer, skin bone count, vertex skin count (0: the points are in
            # the bone's space; 1: in each point's bone; 2+: in model space)
            _, mat_index, bone_index, _, _, skin_count = struct.unpack_from("<HHHHHB", d, s + 0x50)
            mm = mesh_arr                                  # the first (most detailed) level
            fbo, prim, ifmt, icount, firstv = struct.unpack_from("<IIIII", d, mm + 0x20)
            ib = buf_base + fbo
            if ifmt == 1:
                idx = np.frombuffer(d, "<u2", icount, ib).astype(np.int64)
            else:
                idx = np.frombuffer(d, "<u4", icount, ib).astype(np.int64)
            idx = idx + firstv
            a = vbs[vb_index]
            shapes.append({"name": sname, "mat": mat_index, "bone": bone_index, "skin": skin_count, "p": a.get("_p0"), "n": a.get("_n0"),
                           "uv0": a.get("_u0"), "uv1": a.get("_u1"), "uv2": a.get("_u2"),
                           "idx": idx.reshape(-1, 3) if prim == 3 else idx})
        # bones
        bones = []
        if skel and d[skel:skel + 4] == b"FSKL":
            bone_arr = struct.unpack_from("<Q", d, skel + 0x10)[0]
            nb = struct.unpack_from("<H", d, skel + 0x38)[0]
            for k in range(nb):
                b = bone_arr + k * 0x58
                bname = _s(d, struct.unpack_from("<Q", d, b)[0])
                idx_, parent = struct.unpack_from("<hh", d, b + 0x20)
                sc = struct.unpack_from("<3f", d, b + 0x30)
                rot = struct.unpack_from("<4f", d, b + 0x3C)
                tr = struct.unpack_from("<3f", d, b + 0x4C)
                bones.append({"name": bname, "parent": parent, "s": sc, "r": rot, "t": tr})
        out["models"].append({"name": name, "shapes": shapes, "bones": bones, "off": m})
        if not stride:
            break
        m += stride
    return out
