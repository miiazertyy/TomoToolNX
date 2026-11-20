// item_preview.cpp — TomoToolNX. See item_preview.h.

#include "item_preview.h"
#include "game_data.h"
#include "texture_processor.h"

#include <switch.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

namespace ItemPreview {
namespace {

using namespace GameData;

struct Target { std::string file, model, material; };

struct Tables {
    bool loaded = false;
    std::map<uint32_t, std::string> cloth;      // UgcClothType -> model file
    std::map<uint32_t, Target> obj;             // goods and object types -> model
    std::map<uint32_t, uint32_t> mapType;       // UgcMapObjectType -> type
    std::vector<std::string> tableFiles;        // the tables read (paths inside the RomFS)
} sT;

// "Work/Model/UgcEditor/UgcObject/UgcObjectCube/output/UgcObjectCube00.fmdb":
// the file is the folder before "output", the model the name.
void SplitFmdb(const std::string& path, std::string& file, std::string& model) {
    const size_t out = path.find("/output/");
    const size_t slash = path.find_last_of('/');
    model = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const size_t dot = model.find(".fmdb");
    if (dot != std::string::npos) model.erase(dot);
    file.clear();
    if (out != std::string::npos) {
        const size_t prev = path.rfind('/', out - 1);
        file = path.substr(prev == std::string::npos ? 0 : prev + 1, out - (prev == std::string::npos ? 0 : prev + 1));
    }
    if (file.empty()) file = model;
}
std::string Base(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    std::string b = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const size_t dot = b.find('.');
    if (dot != std::string::npos) b.erase(dot);
    return b;
}

// Read once, by whichever thread asks first;
// the tables do not change after.
Mutex sTablesLock;
void LoadTablesLocked();
void LoadTables() {
    mutexLock(&sTablesLock);
    if (!sT.loaded) { sT.loaded = true; LoadTablesLocked(); }
    mutexUnlock(&sTablesLock);
}
void LoadTablesLocked() {
    const std::string root = GameData::Root();
    if (root.empty()) return;
    const std::string rsdb = root + "RSDB";
    auto rows = [&](const char* prefix, Val& out) {
        const std::string f = FindFile(rsdb, prefix);
        if (f.empty()) return false;
        sT.tableFiles.push_back(f.substr(root.size()));
        const Buf d = ReadFile(f);
        return !d.empty() && ParseByml(d.data(), d.size(), out) && out.t == Val::Arr;
    };
    Val v;
    if (rows("UgcClothParam.Product.", v))
        for (const Val& r : v.a) {
            const Val* t = r.Get("UgcType");
            const Val* c = r.Get("ClothID");
            if (t && c && c->t == Val::Str) sT.cloth[(uint32_t)t->AsInt()] = Base(c->s);
        }
    if (rows("UgcObj.Product.", v))
        for (const Val& r : v.a) {
            const Val* t = r.Get("UgcType");
            if (!t) continue;
            Target tg;
            // The object as placed (its "detail" model), else the item's own.
            const Val* det = r.Get("UvDetail");
            const Val* dm = det ? det->Get("Model") : nullptr;
            const Val* ml = r.Get("ModelList");
            std::string path = dm ? dm->AsStr() : std::string();
            if (path.empty() && ml && ml->t == Val::Arr && !ml->a.empty()) path = ml->a[0].AsStr();
            if (path.empty()) continue;
            SplitFmdb(path, tg.file, tg.model);
            const Val* mat = r.Get("MaterialName");
            tg.material = mat ? mat->AsStr() : std::string();
            sT.obj[(uint32_t)t->AsInt()] = tg;
        }
    if (rows("UgcTypeParam.Product.", v))
        for (const Val& r : v.a) {
            const Val* t = r.Get("Type");
            const Val* m = r.Get("MapObjectType");
            if (t && m) sT.mapType[(uint32_t)m->AsInt()] = (uint32_t)t->AsInt();
        }
}

bool Resolve(int kind, uint32_t type, Target& out) {
    LoadTables();
    if (kind == 1) {
        auto it = sT.cloth.find(type);
        if (it == sT.cloth.end()) return false;
        out = { it->second, std::string(), "mt_Body" };
        return true;
    }
    if (kind == 2 || kind == 5) {
        uint32_t t = type;
        if (kind == 5) { auto m = sT.mapType.find(type); if (m != sT.mapType.end()) t = m->second; }
        auto it = sT.obj.find(t);
        if (it == sT.obj.end()) return false;
        out = it->second;
        return true;
    }
    return false;
}

// What is drawn: the model's shapes (textured or plain), centred.
struct Mesh {
    std::vector<float> p, n, uv;       // per vertex
    std::vector<uint32_t> idx;
    std::vector<uint8_t> textured;     // per triangle
    float cx = 0, cy = 0, cz = 0, radius = 1;
};
struct State {
    std::string key;
    bool ok = false;
    Mesh mesh;
    std::vector<uint8_t> tex;          // RGBA
    int tw = 0, th = 0;
    SDL_Texture* out = nullptr;
    int outN = 0;
    std::vector<uint8_t> px;           // the frame, RGBA
    std::vector<float> z;
} sS;

bool Build(int kind, uint32_t type, const std::string& texPath) {
    sS.ok = false;
    sS.mesh = Mesh();
    Target tg;
    if (!Resolve(kind, type, tg)) return false;
    const std::string root = GameData::Root();
    if (root.empty()) return false;
    const Model m = Bfres(ReadFile(root + "Model/" + tg.file + ".bfres.zs"), tg.model);
    if (!m.ok) return false;
    RgbaImage img;
    if (!TextureProcessor::DecodeFile(texPath, img, false).empty() || img.width <= 0) return false;
    sS.tex = std::move(img.pixels);
    sS.tw = img.width; sS.th = img.height;
    Mesh& M = sS.mesh;
    float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
    for (const Shape& sh : m.shapes) {
        if (sh.name.find("Softmesh") != std::string::npos) continue;
        const bool textured = tg.material.empty() || sh.name.find(tg.material) != std::string::npos ||
                              sh.name.find("mt_Replace") != std::string::npos;
        const uint32_t base = (uint32_t)(M.p.size() / 3);
        const size_t nv = sh.p.size() / 3;
        for (size_t i = 0; i < nv; i++) {
            for (int k = 0; k < 3; k++) {
                const float c = sh.p[i * 3 + k];
                M.p.push_back(c);
                lo[k] = std::min(lo[k], c); hi[k] = std::max(hi[k], c);
                M.n.push_back(sh.n.size() >= nv * 3 ? sh.n[i * 3 + k] : 0.f);
            }
            M.uv.push_back(sh.uv.size() >= nv * 2 ? sh.uv[i * 2] : 0.f);
            M.uv.push_back(sh.uv.size() >= nv * 2 ? sh.uv[i * 2 + 1] : 0.f);
        }
        for (size_t t = 0; t + 2 < sh.idx.size(); t += 3) {
            for (int k = 0; k < 3; k++) M.idx.push_back(base + sh.idx[t + k]);
            M.textured.push_back(textured ? 1 : 0);
        }
    }
    if (M.idx.empty()) return false;
    M.cx = (lo[0] + hi[0]) / 2; M.cy = (lo[1] + hi[1]) / 2; M.cz = (lo[2] + hi[2]) / 2;
    float r = 0;
    for (size_t i = 0; i < M.p.size(); i += 3) {
        const float dx = M.p[i] - M.cx, dy = M.p[i + 1] - M.cy, dz = M.p[i + 2] - M.cz;
        r = std::max(r, sqrtf(dx * dx + dy * dy + dz * dz));
    }
    M.radius = std::max(1e-4f, r);
    sS.ok = true;
    return true;
}

// Bilinear, the texture repeating.
inline void Sample(float u, float v, float c[4]) {
    const int W = sS.tw, H = sS.th;
    u -= floorf(u); v -= floorf(v);
    const float x = u * W - 0.5f, y = v * H - 0.5f;
    const int x0 = (int)floorf(x), y0 = (int)floorf(y);
    const float fx = x - x0, fy = y - y0;
    float acc[4] = { 0, 0, 0, 0 };
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            const int xx = ((x0 + i) % W + W) % W, yy = ((y0 + j) % H + H) % H;
            const float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            const uint8_t* p = &sS.tex[((size_t)yy * W + xx) * 4];
            for (int k = 0; k < 4; k++) acc[k] += p[k] * w;
        }
    for (int k = 0; k < 4; k++) c[k] = acc[k] / 255.f;
}

void Render(int n, float yaw, float pitch) {
    const Mesh& M = sS.mesh;
    sS.px.assign((size_t)n * n * 4, 0);
    sS.z.assign((size_t)n * n, -1e9f);
    const float cyw = cosf(yaw), syw = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    const float scale = n * 0.44f / M.radius, half = n / 2.f;
    const size_t nv = M.p.size() / 3;
    std::vector<float> sx(nv), sy(nv), sz(nv), sh(nv);
    float L[3] = { -0.35f, 0.5f, 0.8f };
    const float ln = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    for (float& c : L) c /= ln;
    for (size_t i = 0; i < nv; i++) {
        // Turn around the item's middle: yaw about y, then tip by pitch about x.
        const float x = M.p[i * 3] - M.cx, y = M.p[i * 3 + 1] - M.cy, z = M.p[i * 3 + 2] - M.cz;
        const float x1 = x * cyw + z * syw, z1 = -x * syw + z * cyw;
        const float y2 = y * cp - z1 * sp, z2 = y * sp + z1 * cp;
        sx[i] = half + x1 * scale; sy[i] = half - y2 * scale; sz[i] = z2;
        const float nx = M.n[i * 3], ny = M.n[i * 3 + 1], nz = M.n[i * 3 + 2];
        const float nx1 = nx * cyw + nz * syw, nz1 = -nx * syw + nz * cyw;
        const float ny2 = ny * cp - nz1 * sp, nz2 = ny * sp + nz1 * cp;
        const float d = fabsf(nx1 * L[0] + ny2 * L[1] + nz2 * L[2]);      // both sides lit
        sh[i] = 0.5f + 0.55f * std::min(1.f, d);
    }
    for (size_t t = 0, tri = 0; t + 2 < M.idx.size(); t += 3, tri++) {
        const uint32_t i0 = M.idx[t], i1 = M.idx[t + 1], i2 = M.idx[t + 2];
        if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
        const float ax = sx[i0], ay = sy[i0], bx = sx[i1], by = sy[i1], cx = sx[i2], cy = sy[i2];
        const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (fabsf(area) < 1e-6f) continue;
        const int x0 = std::max((int)floorf(std::min(ax, std::min(bx, cx))), 0);
        const int x1 = std::min((int)ceilf(std::max(ax, std::max(bx, cx))), n - 1);
        const int y0 = std::max((int)floorf(std::min(ay, std::min(by, cy))), 0);
        const int y1 = std::min((int)ceilf(std::max(ay, std::max(by, cy))), n - 1);
        if (x1 < x0 || y1 < y0) continue;
        const bool tex = M.textured[tri] != 0;
        for (int y = y0; y <= y1; y++) {
            const float py = y + 0.5f;
            for (int x = x0; x <= x1; x++) {
                const float px = x + 0.5f;
                float w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) / area;
                float w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) / area;
                float w2 = ((ax - px) * (by - py) - (ay - py) * (bx - px)) / area;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float zz = w0 * sz[i0] + w1 * sz[i1] + w2 * sz[i2];
                float& zb = sS.z[(size_t)y * n + x];
                if (!(zz > zb)) continue;
                float c[4] = { 0.78f, 0.78f, 0.80f, 1.f };
                if (tex) {
                    const float u = w0 * M.uv[i0 * 2] + w1 * M.uv[i1 * 2] + w2 * M.uv[i2 * 2];
                    const float v = w0 * M.uv[i0 * 2 + 1] + w1 * M.uv[i1 * 2 + 1] + w2 * M.uv[i2 * 2 + 1];
                    Sample(u, v, c);
                    if (c[3] < 0.5f) continue;                 // a cut-out: see through
                }
                zb = zz;
                const float s = w0 * sh[i0] + w1 * sh[i1] + w2 * sh[i2];
                uint8_t* o = &sS.px[((size_t)y * n + x) * 4];
                for (int k = 0; k < 3; k++) o[k] = (uint8_t)std::min(255.f, c[k] * s * 255.f);
                o[3] = 255;
            }
        }
    }
}

}  // namespace

bool Has(int kind, uint32_t type) {
    if (!type) return false;
    Target t;
    return Resolve(kind, type, t);
}

SDL_Texture* Draw(SDL_Renderer* ren, const std::string& key, int kind, uint32_t type, const std::string& texPath, int n, float yaw) {
    if (key != sS.key) {
        sS.key = key;
        Build(kind, type, texPath);
    }
    if (!sS.ok || !ren) return nullptr;
    if (!sS.out || sS.outN != n) {
        if (sS.out) SDL_DestroyTexture(sS.out);
        sS.out = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, n, n);
        sS.outN = n;
        if (!sS.out) return nullptr;
        SDL_SetTextureBlendMode(sS.out, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(sS.out, SDL_ScaleModeLinear);     // drawn a little larger, smoothed down
    }
    // Clothes and goods are looked at from the front, objects a little from above.
    Render(n, yaw, kind == 5 ? 0.42f : 0.18f);
    SDL_UpdateTexture(sS.out, nullptr, sS.px.data(), n * 4);
    return sS.out;
}

std::vector<std::string> GameFiles() {
    LoadTables();
    std::vector<std::string> out = sT.tableFiles;
    auto add = [&](const std::string& file) {
        const std::string p = "Model/" + file + ".bfres.zs";
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };
    for (const auto& kv : sT.cloth) add(kv.second);
    for (const auto& kv : sT.obj) add(kv.second.file);
    return out;
}

void Clear() {
    if (sS.out) SDL_DestroyTexture(sS.out);
    sS = State();
}

}  // namespace ItemPreview
