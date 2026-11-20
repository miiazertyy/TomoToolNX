// mii_pack.cpp — TomoToolNX. See mii_pack.h.
//
// A port of tools/mii/build_pack.py and the readers it uses (nxfmt.py:
// SARC, BYML, BNTX, BFRES; raster.py: the software rasterizer), so a pack
// made here matches one made on a computer.

#include "mii_pack.h"
#include "save_mount.h"
#include "texture_processor.h"
#include "game_data.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <switch.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace MiiPack {
namespace {

const char* kOut = "sdmc:/switch/TomoToolNX/miiparts";
const char* kNew = "sdmc:/switch/TomoToolNX/miiparts.new";
const char* kOld = "sdmc:/switch/TomoToolNX/miiparts.old";

constexpr int PORTRAIT = 128, SS = 4, NOSE = 64;
constexpr float SCALE = 1.02f, ROOT_Y = 0.80f;

std::atomic<int> sState{ (int)State::Idle };
std::atomic<int> sDone{ 0 }, sTotal{ 1 };
std::atomic<bool> sFinished{ false };
std::atomic<bool> sCancel{ false };
Mutex sErrLock;
std::string sError;
Thread sThread;
bool sThreadLive = false;

void Fail(const std::string& e) {
    mutexLock(&sErrLock); sError = e; mutexUnlock(&sErrLock);
    sState = (int)State::Failed;
}

using namespace GameData;

// ── The rasterizer: orthographic, z-buffered, Lambert-lit (raster.py) ──
struct Canvas {
    int w, h;
    float scale, cx, cy;
    std::vector<float> col, z;           // colour premultiplied
    Canvas(int w_, int h_, float scale_, float cx_, float cy_)
        : w(w_), h(h_), scale(scale_), cx(cx_), cy(cy_), col((size_t)w_ * h_ * 4, 0.f), z((size_t)w_ * h_, -1e9f) {}
    // Triangles of points p (+off) with normals n, in colour rgb; or, with
    // uv (and uvMode), the face texture's coordinates as the colour.
    void Draw(const Shape& s, const float off[3], const float rgb[3], bool normals, float ambient, bool uvMode) {
        const size_t nv = s.p.size() / 3;
        std::vector<float> sx(nv), sy(nv), sz(nv), shade(nv, 1.f);
        float L[3] = { -0.35f, 0.45f, 0.82f };
        const float ln = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        for (float& c : L) c /= ln;
        const bool useN = normals && s.n.size() >= nv * 3;
        for (size_t i = 0; i < nv; i++) {
            const float X = s.p[i * 3] + off[0], Y = s.p[i * 3 + 1] + off[1], Z = s.p[i * 3 + 2] + off[2];
            sx[i] = cx + X * scale; sy[i] = cy - Y * scale; sz[i] = Z;
            if (useN) {
                const float d = s.n[i * 3] * L[0] + s.n[i * 3 + 1] * L[1] + s.n[i * 3 + 2] * L[2];
                shade[i] = ambient + (1.f - ambient) * std::min(1.f, std::max(0.f, d));
            }
        }
        const bool haveUv = uvMode && s.uv.size() >= nv * 2;
        for (size_t t = 0; t + 2 < s.idx.size(); t += 3) {
            const uint32_t i0 = s.idx[t], i1 = s.idx[t + 1], i2 = s.idx[t + 2];
            const float ax = sx[i0], ay = sy[i0], bx = sx[i1], by = sy[i1], cx_ = sx[i2], cy_ = sy[i2];
            const float area = (bx - ax) * (cy_ - ay) - (by - ay) * (cx_ - ax);
            if (area >= 0) continue;                      // back faces away
            const int x0 = std::max((int)floorf(std::min(ax, std::min(bx, cx_))), 0);
            const int x1 = std::min((int)ceilf(std::max(ax, std::max(bx, cx_))), w - 1);
            const int y0 = std::max((int)floorf(std::min(ay, std::min(by, cy_))), 0);
            const int y1 = std::min((int)ceilf(std::max(ay, std::max(by, cy_))), h - 1);
            if (x1 < x0 || y1 < y0) continue;
            for (int y = y0; y <= y1; y++) {
                const float py = y + 0.5f;
                for (int x = x0; x <= x1; x++) {
                    const float px = x + 0.5f;
                    const float w0 = ((bx - px) * (cy_ - py) - (by - py) * (cx_ - px)) / area;
                    const float w1 = ((cx_ - px) * (ay - py) - (cy_ - py) * (ax - px)) / area;
                    const float w2 = ((ax - px) * (by - py) - (ay - py) * (bx - px)) / area;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    const float zz = w0 * sz[i0] + w1 * sz[i1] + w2 * sz[i2];
                    float& zb = z[(size_t)y * w + x];
                    if (!(zz > zb)) continue;
                    const float sh = w0 * shade[i0] + w1 * shade[i1] + w2 * shade[i2];
                    float c[4] = { rgb[0] * sh, rgb[1] * sh, rgb[2] * sh, 1.f };
                    if (haveUv) {
                        // The face texture's (u, v) through a 256 x 256 lookup, as on the computer.
                        float u = w0 * s.uv[i0 * 2] + w1 * s.uv[i1 * 2] + w2 * s.uv[i2 * 2];
                        float v = w0 * s.uv[i0 * 2 + 1] + w1 * s.uv[i1 * 2 + 1] + w2 * s.uv[i2 * 2 + 1];
                        u -= floorf(u); v -= floorf(v);
                        const int tx = std::min(255, std::max(0, (int)(u * 256))), ty = std::min(255, std::max(0, (int)(v * 256)));
                        c[0] = (tx + 0.5f) / 256.f * sh; c[1] = (ty + 0.5f) / 256.f * sh; c[2] = 0; c[3] = 1.f;
                    }
                    zb = zz;
                    float* o = &col[((size_t)y * w + x) * 4];
                    const float a = c[3];
                    for (int k = 0; k < 3; k++) o[k] = o[k] * (1 - a) + c[k] * a;
                    o[3] = o[3] * (1 - a) + a;
                }
            }
        }
    }
};
inline uint8_t Q8(float v) { return (uint8_t)std::min(255.f, std::max(0.f, v * 255.f)); }
// The canvas (or a buffer the same size), reduced `ss` times, as a grey and
// alpha picture: R = G = B = the grey, A = alpha.
std::vector<uint8_t> GreyAlpha(const std::vector<float>& col, int w, int h, int ss) {
    const int W = w / ss, H = h / ss;
    std::vector<uint8_t> out((size_t)W * H * 4, 0);
    const float k = 1.f / (ss * ss);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float r = 0, a = 0;
            for (int j = 0; j < ss; j++)
                for (int i = 0; i < ss; i++) {
                    const float* p = &col[((size_t)(y * ss + j) * w + (x * ss + i)) * 4];
                    r += p[0]; a += p[3];
                }
            r *= k; a *= k;
            const uint8_t g = a > 0 ? Q8(r / a) : 0;
            uint8_t* o = &out[((size_t)y * W + x) * 4];
            o[0] = o[1] = o[2] = g; o[3] = Q8(a);
        }
    return out;
}
std::vector<uint8_t> Rgba(const std::vector<float>& col, int w, int h) {
    std::vector<uint8_t> out((size_t)w * h * 4, 0);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const float a = col[i * 4 + 3];
        for (int k = 0; k < 3; k++) out[i * 4 + k] = a > 0 ? Q8(col[i * 4 + k] / std::max(a, 1e-6f)) : 0;
        out[i * 4 + 3] = Q8(a);
    }
    return out;
}

bool SavePng(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom((void*)rgba.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_ABGR8888);
    if (!s) return false;
    const int r = IMG_SavePNG(s, path.c_str());
    SDL_FreeSurface(s);
    return r == 0;
}
bool AnyAlpha(const std::vector<float>& col) {
    for (size_t i = 3; i < col.size(); i += 4) if (col[i] > 0) return true;
    return false;
}

void RemoveTree(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (d) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            const std::string p = dir + "/" + n;
            struct stat st;
            if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) RemoveTree(p);
            else unlink(p.c_str());
        }
        closedir(d);
    }
    rmdir(dir.c_str());
}

std::string BaseNoExt(const std::string& fmdb) {
    std::string b = fmdb.substr(fmdb.find_last_of('/') == std::string::npos ? 0 : fmdb.find_last_of('/') + 1);
    const size_t p = b.find(".fmdb");
    if (p != std::string::npos) b.erase(p, 5);
    return b;
}
std::string Fmdb(const Val* part) {
    const Val* mu = part ? part->Get("ModelUnit") : nullptr;
    const Val* f = mu ? mu->Get("Fmdb") : nullptr;
    return f && f->t == Val::Str ? f->s : std::string();
}

// ── The build ──
void Build() {
    sDone = 0; sTotal = 1;
    // The game's files: the installed game, else (tests) a folder of them.
    const std::string root = GameData::Root();
    const std::string why = GameData::Why();
    auto reason = [&why]() -> std::string {
        return why == "emu" ? "miipack.emu" : why == "notrunning" ? "miipack.notrunning" : why == "notinstalled" ? "miipack.nogame"
             : why == "nofile" ? "miipack.nofile" : why.empty() ? "miipack.nogame" : "miipack.fserr|" + why;
    };
    if (root.empty()) { Fail(reason()); return; }
    auto done = []() {};

    // (The copy kept on the SD card has no parts: they are made into the pack
    // the first time the app can read the game.)
    const Buf packData = ReadFile(root + "Pack/MiiParts.pack.zs");
    if (packData.empty()) { done(); Fail(reason()); return; }
    // Every part, by category and number (a later one with the same number wins).
    std::vector<Val> partVals;
    for (auto& f : Sarc(packData)) {
        Val v;
        if (ParseByml(&packData[f.second.first], f.second.second, v) && v.t == Val::Dict && v.Get("PartsIndex")) partVals.push_back(std::move(v));
    }
    std::map<std::string, std::map<int, const Val*>> parts;
    for (const Val& v : partVals) {
        const Val* c = v.Get("Category");
        if (!c || c->t != Val::Str) continue;
        parts[c->s][(int)v.Get("PartsIndex")->AsInt()] = &v;
    }
    if (parts.empty()) { done(); Fail("parts list unreadable"); return; }

    const Buf texData = ReadFile(root + "Tex/Pack/MiiParts.bntx.zs");
    const std::vector<BntxTex> texs = Bntx(texData);
    sTotal = (int)texs.size() + (int)parts["Faceline"].size() + (int)parts["Hair"].size() + (int)parts["HairFront"].size() + (int)parts["Nose"].size() + 1;

    RemoveTree(kNew);
    mkdir(kNew, 0777);
    const std::string out = std::string(kNew) + "/";
    mkdir((out + "tex").c_str(), 0777);
    std::vector<std::string> lines = { "# TomoToolNX Mii parts pack v1 (made on the console from your game)", "size 128" };
    char buf[512];

    // The face parts, as the game keeps them.
    std::map<std::string, bool> texNames;
    auto stopped = [&]() { if (!sCancel) return false; done(); RemoveTree(kNew); sState = (int)State::Idle; return true; };
    for (const BntxTex& t : texs) {
        if (stopped()) return;
        std::vector<uint8_t> rgba;
        if (DecodeTex(texData, t, rgba) && SavePng(out + "tex/" + t.name + ".png", rgba, t.w, t.h)) texNames[t.name] = true;
        sDone++;
    }
    static const char* kTexCats[] = { "Eye", "Eyebrow", "Mouth", "Mustache", "Mole", "BeardShort", "MakeUpper", "MakeLower",
                                      "WrinkleUpper", "WrinkleLower", "Highlight", "EyelashUpper", "EyelashLower", "EyelidUpper", "EyelidLower" };
    for (const char* cat : kTexCats) {
        for (auto& kv : parts[cat]) {
            const Val* tn = kv.second->Get("TextureName");
            if (!tn || tn->t != Val::Str || !texNames.count(tn->s)) continue;
            const Val* rot = kv.second->Get("OffsetRotate");
            const Val* nl = kv.second->Get("NoLipTextureName");
            snprintf(buf, sizeof buf, "tex %s %d %s %d %s", cat, kv.first, tn->s.c_str(), rot ? (int)rot->AsInt() : 0,
                     nl && nl->t == Val::Str && !nl->s.empty() ? nl->s.c_str() : "-");
            lines.push_back(buf);
        }
    }

    auto model = [&](const std::string& fmdb) -> Model {
        if (fmdb.empty()) return Model();
        return Bfres(ReadFile(root + "Model/" + BaseNoExt(fmdb) + ".bfres.zs"));
    };
    const float white[3] = { 1, 1, 1 }, black[3] = { 0, 0, 0 }, zero[3] = { 0, 0, 0 };
    const int S = PORTRAIT * SS;

    // Head shapes: shading, where the face texture lies, the anchors.
    std::vector<float> refDepth;
    for (auto& kv : parts["Faceline"]) {
        if (stopped()) return;
        sDone++;
        const Model m = model(Fmdb(kv.second));
        if (!m.ok) continue;
        float at[3];
        BoneWorld(m, "Head", at);
        Canvas cv(S, S, S * SCALE, S / 2.f, S * ROOT_Y);
        for (const Shape& sh : m.shapes) if (sh.name.rfind("Head", 0) == 0) cv.Draw(sh, at, white, true, 0.42f, false);
        snprintf(buf, sizeof buf, "head_%02d.png", kv.first);
        const std::string headFile = buf;
        SavePng(out + headFile, GreyAlpha(cv.col, S, S, SS), PORTRAIT, PORTRAIT);
        if (refDepth.empty()) refDepth = cv.z;
        // the face texture's u,v over the head, at the pack's size
        Canvas uv(PORTRAIT, PORTRAIT, PORTRAIT * SCALE, PORTRAIT / 2.f, PORTRAIT * ROOT_Y);
        for (const Shape& sh : m.shapes) if (sh.name.rfind("Head", 0) == 0) uv.Draw(sh, at, black, true, 0.42f, false);
        std::fill(uv.col.begin(), uv.col.end(), 0.f);
        const float lift[3] = { 0, 0, 0.004f };
        for (const Shape& sh : m.shapes) if (sh.name.rfind("Mask", 0) == 0 && !sh.uv.empty()) uv.Draw(sh, lift, white, false, 1.f, true);
        snprintf(buf, sizeof buf, "headuv_%02d.png", kv.first);
        const std::string uvFile = buf;
        SavePng(out + uvFile, Rgba(uv.col, PORTRAIT, PORTRAIT), PORTRAIT, PORTRAIT);
        std::string anchors;
        for (const char* bn : { "set_nose", "set_hair", "set_beard", "set_ear" }) {
            float p[3];
            BoneWorld(m, bn, p);
            const float x = PORTRAIT / 2.f + p[0] * PORTRAIT * SCALE, y = PORTRAIT * ROOT_Y - p[1] * PORTRAIT * SCALE;
            snprintf(buf, sizeof buf, " %s=%.2f,%.2f", bn, x, y);
            anchors += buf;
        }
        snprintf(buf, sizeof buf, "head %d %s %s", kv.first, headFile.c_str(), uvFile.c_str());
        lines.push_back(buf + anchors);
    }
    if (refDepth.empty()) { done(); RemoveTree(kNew); Fail("no head models"); return; }

    // Hairstyles: what is in front of the head and what is behind it.
    auto hairPictures = [&](const Model& m, const std::string& tag, std::string files[2]) {
        Canvas cv(S, S, S * SCALE, S / 2.f, S * ROOT_Y);
        for (const Shape& sh : m.shapes) cv.Draw(sh, zero, white, true, 0.42f, false);
        std::vector<float> front = cv.col, behind = cv.col;
        for (size_t i = 0; i < cv.z.size(); i++) {
            const bool covered = cv.z[i] > -1e8f;
            const bool isFront = covered && cv.z[i] >= refDepth[i] - 0.002f;
            float* f = &front[i * 4];
            float* b = &behind[i * 4];
            if (isFront) b[0] = b[1] = b[2] = b[3] = 0; else f[0] = f[1] = f[2] = f[3] = 0;
        }
        const std::vector<float>* bufs[2] = { &front, &behind };
        const char* suffix[2] = { "f", "b" };
        for (int k = 0; k < 2; k++) {
            if (!AnyAlpha(*bufs[k])) { files[k] = "-"; continue; }
            files[k] = tag + "_" + suffix[k] + ".png";
            SavePng(out + files[k], GreyAlpha(*bufs[k], S, S, SS), PORTRAIT, PORTRAIT);
        }
    };
    for (auto& kv : parts["Hair"]) {
        if (stopped()) return;
        sDone++;
        const std::string f = Fmdb(kv.second);
        if (f.empty()) continue;
        const Model m = model(f);
        if (!m.ok) continue;
        snprintf(buf, sizeof buf, "hair_%03d", kv.first);
        std::string files[2];
        hairPictures(m, buf, files);
        const Val* att = kv.second->Get("IsAttachableHairFront");
        snprintf(buf, sizeof buf, "hair %d %s %s %d", kv.first, files[0].c_str(), files[1].c_str(), att && att->AsBool() ? 1 : 0);
        lines.push_back(buf);
    }
    for (auto& kv : parts["HairFront"]) {
        if (stopped()) return;
        sDone++;
        const Model m = model(Fmdb(kv.second));
        if (!m.ok) continue;
        snprintf(buf, sizeof buf, "front_%03d", kv.first);
        std::string files[2];
        hairPictures(m, buf, files);
        snprintf(buf, sizeof buf, "front %d %s %s", kv.first, files[0].c_str(), files[1].c_str());
        lines.push_back(buf);
    }

    // Noses, their anchor in the middle.
    for (auto& kv : parts["Nose"]) {
        if (stopped()) return;
        sDone++;
        const Model m = model(Fmdb(kv.second));
        if (!m.ok) continue;
        const int s = NOSE * SS;
        Canvas cv(s, s, PORTRAIT * SS * SCALE, s / 2.f, s / 2.f);
        bool drew = false;
        for (const Shape& sh : m.shapes) if (sh.name.rfind("Nose__", 0) == 0) { cv.Draw(sh, zero, white, true, 0.42f, false); drew = true; }
        if (!drew) continue;
        snprintf(buf, sizeof buf, "nose_%02d.png", kv.first);
        SavePng(out + buf, GreyAlpha(cv.col, s, s, SS), NOSE, NOSE);
        const std::string file = buf;
        snprintf(buf, sizeof buf, "nose %d %s", kv.first, file.c_str());
        lines.push_back(buf);
    }
    done();

    // The index last; then the new pack takes the old one's place.
    FILE* f = fopen((out + "index.txt").c_str(), "w");
    if (!f) { RemoveTree(kNew); Fail("could not write to the SD card"); return; }
    for (const std::string& l : lines) fprintf(f, "%s\n", l.c_str());
    fclose(f);
    RemoveTree(kOld);
    struct stat st;
    if (stat(kOut, &st) == 0) rename(kOut, kOld);
    if (rename(kNew, kOut) != 0) { if (stat(kOld, &st) == 0) rename(kOld, kOut); Fail("could not write to the SD card"); return; }
    RemoveTree(kOld);
    sDone = sTotal.load();
    sState = (int)State::Done;
    sFinished = true;
}

void ThreadFn(void*) { Build(); }

}  // namespace

bool Present() {
    struct stat st;
    return stat("romfs:/miiparts/index.txt", &st) == 0 || stat((std::string(kOut) + "/index.txt").c_str(), &st) == 0;
}
bool BuiltIn() {
    struct stat st;
    return stat("romfs:/miiparts/index.txt", &st) == 0;
}

void Start(bool force) {
    if (sState == (int)State::Running) return;
    if (!force && Present()) return;
    if (sThreadLive) { threadWaitForExit(&sThread); threadClose(&sThread); sThreadLive = false; }
    sCancel = false;
    mutexLock(&sErrLock); sError.clear(); mutexUnlock(&sErrLock);
    sState = (int)State::Running;
    // Below the interface's priority, with room for the models.
    if (R_FAILED(threadCreate(&sThread, ThreadFn, nullptr, nullptr, 512 * 1024, 0x2D, -2))) { Fail("no thread"); return; }
    if (R_FAILED(threadStart(&sThread))) { threadClose(&sThread); Fail("no thread"); return; }
    sThreadLive = true;
}

State GetState() { return (State)sState.load(); }
float Progress() { const int t = std::max(1, sTotal.load()); return std::min(1.f, sDone.load() / (float)t); }
std::string Error() { mutexLock(&sErrLock); std::string e = sError; mutexUnlock(&sErrLock); return e; }
bool TakeFinished() { return sFinished.exchange(false); }

void Stop() {
    if (!sThreadLive) return;
    sCancel = true;
    threadWaitForExit(&sThread);
    threadClose(&sThread);
    sThreadLive = false;
}

}  // namespace MiiPack
