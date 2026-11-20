// game_data.cpp — TomoToolNX. See game_data.h.
// Ports of tools/mii/nxfmt.py (SARC, BYML, BNTX, BFRES).

#include "game_data.h"
#include "save_mount.h"
#include "texture_processor.h"
#include "errors.h"
#include "../libs/astcdec/astc.h"

#include <switch.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace GameData {
namespace {

Mutex sRootLock;
bool sTried = false;
std::string sRoot, sWhy;
const char* kSrcDir = "sdmc:/switch/TomoToolNX/miiparts_src/";

// ── Bytes ──
inline uint16_t U16(const Buf& d, size_t o) { return o + 2 <= d.size() ? (uint16_t)(d[o] | (d[o + 1] << 8)) : 0; }
inline uint32_t U32(const Buf& d, size_t o) { return o + 4 <= d.size() ? (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) | ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24) : 0; }
inline uint64_t U64(const Buf& d, size_t o) { return (uint64_t)U32(d, o) | ((uint64_t)U32(d, o + 4) << 32); }
inline int16_t S16(const Buf& d, size_t o) { return (int16_t)U16(d, o); }
inline float F32(const Buf& d, size_t o) { const uint32_t u = U32(d, o); float f; memcpy(&f, &u, 4); return f; }
float Half(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = m * (1.f / 16777216.f);                 // subnormal: m * 2^-24
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = ldexpf(1.f + m / 1024.f, (int)e - 15);
    return s ? -v : v;
}
std::string Str16(const Buf& d, uint64_t off) {
    if (!off || off + 2 > d.size()) return {};
    const uint16_t n = U16(d, off);
    if (off + 2 + n > d.size()) return {};
    return std::string((const char*)&d[off + 2], n);
}

}  // namespace

std::string Root() {
    mutexLock(&sRootLock);
    if (!sTried) {
        sTried = true;
        struct stat st;
        // The game's files the app carries (romfs/game: the item tables and
        // their models, tools/assets/build_game_files.py); for tests, a folder
        // of them on the SD card.
        if (stat("romfs:/game/RSDB", &st) == 0) sRoot = "romfs:/game/";
        else if (stat((std::string(kSrcDir) + "RSDB").c_str(), &st) == 0) sRoot = kSrcDir;
        else sWhy = "nofile";
    }
    const std::string r = sRoot;
    mutexUnlock(&sRootLock);
    return r;
}

std::string Why() {
    mutexLock(&sRootLock);
    const std::string w = sWhy;
    mutexUnlock(&sRootLock);
    return w;
}

void Close() {
    mutexLock(&sRootLock);
    sTried = false;
    sRoot.clear();
    sWhy.clear();
    mutexUnlock(&sRootLock);
}

std::string FindFile(const std::string& dir, const std::string& prefix) {
    std::string found;
    DIR* d = opendir(dir.c_str());
    if (!d) return found;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.compare(0, prefix.size(), prefix) == 0) { found = dir + (dir.empty() || dir.back() == '/' ? "" : "/") + n; break; }
    }
    closedir(d);
    return found;
}

Buf ReadFile(const std::string& path) {
    Buf b;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return b;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && n < (256L << 20)) {
        b.resize((size_t)n);
        if (fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
    }
    fclose(f);
    if (b.size() >= 4 && b[0] == 0x28 && b[1] == 0xB5 && b[2] == 0x2F && b[3] == 0xFD)
        return TextureProcessor::ZstdDecompressBuffer(b.data(), b.size());
    return b;
}

// ── SARC: {name: (offset, size)} ──
std::vector<std::pair<std::string, std::pair<size_t, size_t>>> Sarc(const Buf& d) {
    std::vector<std::pair<std::string, std::pair<size_t, size_t>>> out;
    if (d.size() < 0x20 || memcmp(d.data(), "SARC", 4) != 0) return out;
    const uint32_t dataOff = U32(d, 0x0C);
    const size_t sfat = 0x14;
    if (memcmp(&d[sfat], "SFAT", 4) != 0) return out;
    const uint16_t count = U16(d, sfat + 6);
    const size_t sfnt = sfat + 12 + (size_t)count * 16;
    if (sfnt + 8 > d.size() || memcmp(&d[sfnt], "SFNT", 4) != 0) return out;
    const size_t names = sfnt + 8;
    for (int i = 0; i < count; i++) {
        const size_t n = sfat + 12 + (size_t)i * 16;
        const uint32_t hash = U32(d, n), attr = U32(d, n + 4), start = U32(d, n + 8), end = U32(d, n + 12);
        std::string name;
        if (attr & 0x01000000) {
            const size_t o = names + (attr & 0xFFFF) * 4;
            for (size_t k = o; k < d.size() && d[k]; k++) name += (char)d[k];
        }
        if (name.empty()) { char h[12]; snprintf(h, sizeof h, "%08x", (unsigned)hash); name = h; }
        if (end >= start && dataOff + (size_t)end <= d.size()) out.push_back({ name, { dataOff + (size_t)start, (size_t)(end - start) } });
    }
    return out;
}

// ── BYML ──
struct Byml {
    const uint8_t* d = nullptr;
    size_t n = 0;
    std::vector<std::string> keys, strs;
    uint32_t U(size_t o) const { return o + 4 <= n ? (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) | ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24) : 0; }
    uint32_t U24(size_t o) const { return o + 4 <= n ? (uint32_t)d[o + 1] | ((uint32_t)d[o + 2] << 8) | ((uint32_t)d[o + 3] << 16) : 0; }
    std::vector<std::string> Table(size_t off) const {
        std::vector<std::string> out;
        if (!off || off >= n) return out;
        const uint32_t cnt = U24(off);
        for (uint32_t i = 0; i < cnt; i++) {
            size_t o = off + U(off + 4 + 4 * (size_t)i);
            std::string s;
            while (o < n && d[o]) s += (char)d[o++];
            out.push_back(std::move(s));
        }
        return out;
    }
    Val Node(uint8_t t, uint32_t v, int depth) const {
        Val r;
        switch (t) {
            case 0xA0: r.t = Val::Str; if (v < strs.size()) r.s = strs[v]; break;
            case 0xC0: case 0xC1: return Parse(v, depth + 1);
            case 0xD0: r.t = Val::Bool; r.i = v != 0; break;
            case 0xD1: r.t = Val::Int; r.i = (int32_t)v; break;
            case 0xD2: { r.t = Val::Float; float f; memcpy(&f, &v, 4); r.f = f; break; }
            case 0xD3: r.t = Val::Int; r.i = v; break;
            case 0xD4: case 0xD5: { r.t = Val::Int; if (v + 8 <= n) { uint64_t q; memcpy(&q, d + v, 8); r.i = (int64_t)q; } break; }
            case 0xD6: { r.t = Val::Float; if (v + 8 <= n) { double q; memcpy(&q, d + v, 8); r.f = q; } break; }
            default: break;
        }
        return r;
    }
    Val Parse(size_t off, int depth) const {
        Val r;
        if (off + 4 > n || depth > 64) return r;
        const uint8_t t = d[off];
        const uint32_t cnt = U24(off);
        if (t == 0xC0) {
            r.t = Val::Arr;
            const size_t vals = (off + 4 + cnt + 3) & ~(size_t)3;
            for (uint32_t i = 0; i < cnt && off + 4 + i < n; i++) r.a.push_back(Node(d[off + 4 + i], U(vals + 4 * (size_t)i), depth));
        } else if (t == 0xC1) {
            r.t = Val::Dict;
            for (uint32_t i = 0; i < cnt; i++) {
                const size_t e = off + 4 + 8 * (size_t)i;
                const uint32_t k = U(e) & 0xFFFFFF;
                const uint8_t kt = e + 3 < n ? d[e + 3] : 0;
                r.m.push_back({ k < keys.size() ? keys[k] : std::string(), Node(kt, U(e + 4), depth) });
            }
        } else {
            return Node(t, U(off + 4), depth);
        }
        return r;
    }
};
bool ParseByml(const uint8_t* d, size_t n, Val& out) {
    if (n < 16 || d[0] != 'Y' || d[1] != 'B') return false;          // little-endian only (the Switch's)
    Byml b;
    b.d = d; b.n = n;
    b.keys = b.Table(b.U(4));
    b.strs = b.Table(b.U(8));
    const uint32_t root = b.U(12);
    if (!root) return false;
    out = b.Parse(root, 0);
    return true;
}

// ── BNTX ──
std::vector<BntxTex> Bntx(const Buf& d) {
    std::vector<BntxTex> out;
    if (d.size() < 0x40 || memcmp(d.data(), "BNTX", 4) != 0 || memcmp(&d[0x20], "NX  ", 4) != 0) return out;
    const uint32_t count = U32(d, 0x24);
    const uint64_t infoPtr = U64(d, 0x28);
    for (uint32_t i = 0; i < count; i++) {
        const uint64_t b = U64(d, infoPtr + 8 * (size_t)i);
        if (b + 0x80 > d.size() || memcmp(&d[b], "BRTI", 4) != 0) continue;
        BntxTex t;
        t.tile = U16(d, b + 0x12);
        t.fmt = U32(d, b + 0x1C);
        t.w = (int)U32(d, b + 0x24);
        t.h = (int)U32(d, b + 0x28);
        t.bhLog2 = (int)U32(d, b + 0x34);
        t.size = U32(d, b + 0x50);
        t.name = Str16(d, U64(d, b + 0x60));
        t.off = (size_t)U64(d, U64(d, b + 0x70));
        if (t.off + t.size <= d.size() && t.w > 0 && t.h > 0) out.push_back(std::move(t));
    }
    return out;
}
// The first mip as RGBA, the way texture2ddecoder gives it (BC4: the value in red).
bool DecodeTex(const Buf& d, const BntxTex& t, std::vector<uint8_t>& rgba) {
    const int kind = (int)(t.fmt >> 8);
    static const int kAstc[][3] = { {0x2D,4,4},{0x2E,5,4},{0x2F,5,5},{0x30,6,5},{0x31,6,6},{0x32,8,5},{0x33,8,6},{0x34,8,8},
                                    {0x35,10,5},{0x36,10,6},{0x37,10,8},{0x38,10,10},{0x39,12,10},{0x3A,12,12} };
    int bw = 4, bh = 4;
    bool astc = false;
    for (auto& a : kAstc) if (a[0] == kind) { bw = a[1]; bh = a[2]; astc = true; }
    const bool bc = kind == 0x1A || kind == 0x1C || kind == 0x1D;
    const std::vector<uint8_t> raw(d.begin() + (long)t.off, d.begin() + (long)(t.off + t.size));
    rgba.assign((size_t)t.w * t.h * 4, 0);
    if (astc || bc) {
        const int nbx = (t.w + bw - 1) / bw, nby = (t.h + bh - 1) / bh;
        const int bpb = (kind == 0x1A || kind == 0x1D) ? 8 : 16;
        const std::vector<uint8_t> lin = t.tile == 0 ? TextureProcessor::DeswizzleBlockLinear(raw, nbx, nby, bpb, 1 << t.bhLog2) : raw;
        if (lin.size() < (size_t)nbx * nby * bpb) return false;
        const int W = nbx * bw, H = nby * bh;
        if (astc) {
            std::vector<uint32_t> px((size_t)W * H, 0);
            decode_astc(lin.data(), W, H, bw, bh, px.data());
            for (int y = 0; y < t.h; y++)
                for (int x = 0; x < t.w; x++) {
                    const uint32_t c = px[(size_t)y * W + x];
                    uint8_t* o = &rgba[((size_t)y * t.w + x) * 4];
                    o[0] = (c >> 16) & 255; o[1] = (c >> 8) & 255; o[2] = c & 255; o[3] = c >> 24;
                }
            return true;
        }
        if (kind == 0x1D) {
            for (int by = 0; by < nby; by++)
                for (int bx = 0; bx < nbx; bx++) {
                    const uint8_t* b = &lin[((size_t)by * nbx + bx) * 8];
                    const int a0 = b[0], a1 = b[1];
                    int pal[8] = { a0, a1 };
                    if (a0 > a1) for (int k = 1; k < 7; k++) pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
                    else { for (int k = 1; k < 5; k++) pal[k + 1] = ((5 - k) * a0 + k * a1) / 5; pal[6] = 0; pal[7] = 255; }
                    uint64_t bits = 0;
                    for (int k = 0; k < 6; k++) bits |= (uint64_t)b[2 + k] << (8 * k);
                    for (int k = 0; k < 16; k++) {
                        const int x = bx * 4 + (k & 3), y = by * 4 + (k >> 2);
                        if (x >= t.w || y >= t.h) continue;
                        uint8_t* o = &rgba[((size_t)y * t.w + x) * 4];
                        o[0] = (uint8_t)pal[(bits >> (3 * k)) & 7]; o[1] = 0; o[2] = 0; o[3] = 255;
                    }
                }
            return true;
        }
        const std::vector<uint8_t> out = kind == 0x1A ? TextureProcessor::Bc1Decode(lin, W, H) : TextureProcessor::Bc3Decode(lin, W, H);
        if (out.size() < (size_t)W * H * 4) return false;
        for (int y = 0; y < t.h; y++) memcpy(&rgba[(size_t)y * t.w * 4], &out[(size_t)y * W * 4], (size_t)t.w * 4);
        return true;
    }
    if (kind == 0x0B || kind == 0x02) {
        const int bpe = kind == 0x0B ? 4 : 1;
        const std::vector<uint8_t> lin = t.tile == 0 ? TextureProcessor::DeswizzleBlockLinear(raw, t.w, t.h, bpe, 1 << t.bhLog2) : raw;
        if (lin.size() < (size_t)t.w * t.h * bpe) return false;
        for (size_t i = 0; i < (size_t)t.w * t.h; i++) {
            if (bpe == 4) memcpy(&rgba[i * 4], &lin[i * 4], 4);
            else { rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = lin[i]; rgba[i * 4 + 3] = 255; }
        }
        return true;
    }
    return false;
}

// ── BFRES (v10): the first model's shapes and bones ──

// One attribute as `comps` floats per vertex (missing components 0).
bool Attr(const Buf& d, size_t base, uint32_t stride, uint32_t count, uint32_t fmt, int comps, std::vector<float>& out) {
    out.assign((size_t)count * comps, 0.f);
    for (uint32_t v = 0; v < count; v++) {
        const size_t o = base + (size_t)v * stride;
        float c[4] = { 0, 0, 0, 0 };
        switch (fmt) {
            case 0x1805: if (o + 12 > d.size()) return false; for (int k = 0; k < 3; k++) c[k] = F32(d, o + 4 * k); break;
            case 0x1705: if (o + 8 > d.size()) return false; for (int k = 0; k < 2; k++) c[k] = F32(d, o + 4 * k); break;
            case 0x1205: if (o + 4 > d.size()) return false; for (int k = 0; k < 2; k++) c[k] = Half(U16(d, o + 2 * k)); break;
            case 0x1505: if (o + 8 > d.size()) return false; for (int k = 0; k < 4; k++) c[k] = Half(U16(d, o + 2 * k)); break;
            case 0x0E02: {
                if (o + 4 > d.size()) return false;
                const uint32_t q = U32(d, o);
                for (int k = 0; k < 3; k++) { int x = (int)((q >> (10 * k)) & 0x3FF); if (x >= 512) x -= 1024; c[k] = std::max(x / 511.f, -1.f); }
                break;
            }
            case 0x0B01: if (o + 4 > d.size()) return false; for (int k = 0; k < 4; k++) c[k] = d[o + k] / 255.f; break;
            case 0x0B02: if (o + 4 > d.size()) return false; for (int k = 0; k < 4; k++) c[k] = (int8_t)d[o + k] / 127.f; break;
            case 0x0901: if (o + 2 > d.size()) return false; for (int k = 0; k < 2; k++) c[k] = d[o + k] / 255.f; break;
            case 0x1201: if (o + 4 > d.size()) return false; for (int k = 0; k < 2; k++) c[k] = U16(d, o + 2 * k) / 65535.f; break;
            default: return false;
        }
        for (int k = 0; k < comps; k++) out[(size_t)v * comps + k] = c[k];
    }
    return true;
}

Model Bfres(const Buf& d, const std::string& modelName) {
    Model M;
    if (d.size() < 0x100 || memcmp(d.data(), "FRES    ", 8) != 0) return M;
    const uint64_t bufInfo = U64(d, 0xB0);
    const uint64_t bufBase = bufInfo ? U64(d, bufInfo + 8) : 0;
    uint64_t m = U64(d, 0x28);
    if (!modelName.empty()) {
        // Several models in one file: the one with that name.
        m = 0;
        for (size_t i = 0x20; i + 0x70 <= d.size(); i += 8) {
            if (memcmp(&d[i], "FMDL", 4) != 0) continue;
            if (Str16(d, U64(d, i + 8)) == modelName) { m = i; break; }
        }
    }
    if (!m || m + 0x70 > d.size() || memcmp(&d[m], "FMDL", 4) != 0) return M;
    M.name = Str16(d, U64(d, m + 8));
    const uint64_t skel = U64(d, m + 0x18), vtxArr = U64(d, m + 0x20), shpArr = U64(d, m + 0x28);
    const uint16_t nVtx = U16(d, m + 0x68), nShp = U16(d, m + 0x6A);
    struct Vb { std::vector<float> p, n, uv; };
    std::vector<Vb> vbs(nVtx);
    for (int i = 0; i < nVtx; i++) {
        const uint64_t v = vtxArr + (uint64_t)i * 0x58;
        if (v + 0x58 > d.size() || memcmp(&d[v], "FVTX", 4) != 0) return M;
        const uint64_t attrArr = U64(d, v + 8), sizesArr = U64(d, v + 0x30), stridesArr = U64(d, v + 0x38);
        const uint32_t boff = U32(d, v + 0x48);
        const uint8_t nAttr = d[v + 0x4C], nBuf = d[v + 0x4D];
        const uint32_t count = U32(d, v + 0x50);
        std::vector<uint32_t> strides(nBuf);
        std::vector<uint64_t> starts(nBuf);
        uint64_t pos = bufBase + boff;
        for (int k = 0; k < nBuf; k++) {
            const uint32_t sz = U32(d, sizesArr + 0x10 * (uint64_t)k);
            strides[k] = U32(d, stridesArr + 0x10 * (uint64_t)k);
            starts[k] = pos;
            pos += (sz + 0xFF) & ~0xFFu;                 // buffers sit on 256-byte boundaries
        }
        for (int k = 0; k < nAttr; k++) {
            const uint64_t a = attrArr + (uint64_t)k * 0x10;
            const std::string an = Str16(d, U64(d, a));
            const uint32_t fmt = U32(d, a + 8);
            const uint16_t aoff = U16(d, a + 12), bidx = U16(d, a + 14);
            if (bidx >= nBuf) continue;
            std::vector<float>* dst = an == "_p0" ? &vbs[i].p : an == "_n0" ? &vbs[i].n : an == "_u0" ? &vbs[i].uv : nullptr;
            if (!dst) continue;
            if (!Attr(d, starts[bidx] + aoff, strides[bidx], count, fmt, an == "_u0" ? 2 : 3, *dst)) dst->clear();
        }
    }
    for (int i = 0; i < nShp; i++) {
        const uint64_t s = shpArr + (uint64_t)i * 0x60;
        if (s + 0x60 > d.size() || memcmp(&d[s], "FSHP", 4) != 0) return M;
        Shape sh;
        sh.name = Str16(d, U64(d, s + 8));
        const uint64_t vbPtr = U64(d, s + 0x10), meshArr = U64(d, s + 0x18);
        const uint64_t vbIndex = vtxArr && vbPtr >= vtxArr ? (vbPtr - vtxArr) / 0x58 : 0;
        if (vbIndex >= vbs.size()) continue;
        const uint32_t fbo = U32(d, meshArr + 0x20), prim = U32(d, meshArr + 0x24), ifmt = U32(d, meshArr + 0x28);
        const uint32_t icount = U32(d, meshArr + 0x2C), firstv = U32(d, meshArr + 0x30);
        if (prim != 3) continue;                          // triangles
        const uint64_t ib = bufBase + fbo;
        const Vb& vb = vbs[vbIndex];
        const uint32_t nv = (uint32_t)(vb.p.size() / 3);
        sh.idx.reserve(icount);
        for (uint32_t k = 0; k < icount; k++) {
            const uint32_t x = (ifmt == 1 ? U16(d, ib + 2 * (uint64_t)k) : U32(d, ib + 4 * (uint64_t)k)) + firstv;
            sh.idx.push_back(x < nv ? x : 0);
        }
        sh.idx.resize(sh.idx.size() / 3 * 3);
        sh.p = vb.p; sh.n = vb.n; sh.uv = vb.uv;
        if (sh.p.empty()) continue;
        M.shapes.push_back(std::move(sh));
    }
    if (skel && skel + 0x40 <= d.size() && memcmp(&d[skel], "FSKL", 4) == 0) {
        const uint64_t boneArr = U64(d, skel + 0x10);
        const uint16_t nb = U16(d, skel + 0x38);
        for (int k = 0; k < nb; k++) {
            const uint64_t b = boneArr + (uint64_t)k * 0x58;
            if (b + 0x58 > d.size()) break;
            Bone bo;
            bo.name = Str16(d, U64(d, b));
            bo.parent = S16(d, b + 0x22);
            for (int j = 0; j < 3; j++) bo.t[j] = F32(d, b + 0x4C + 4 * j);
            M.bones.push_back(bo);
        }
    }
    M.ok = true;
    return M;
}
void BoneWorld(const Model& m, const char* name, float out[3]) {
    out[0] = out[1] = out[2] = 0;
    for (size_t i = 0; i < m.bones.size(); i++) {
        if (m.bones[i].name != name) continue;
        int b = (int)i, guard = 0;
        while (b >= 0 && b < (int)m.bones.size() && guard++ < 64) {
            for (int k = 0; k < 3; k++) out[k] += m.bones[b].t[k];
            b = m.bones[b].parent;
        }
        return;
    }
}


}  // namespace GameData
