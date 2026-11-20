// mii_art.cpp — TomoToolNX. See mii_art.h. tools/mii/compose.py draws the
// same pictures in Python: that is where the numbers below are explained.

#include "mii_art.h"
#include "texture_processor.h"
#include "ui_draw.h"
#include "ui_motion.h"

#include <SDL2/SDL_image.h>
#include <switch.h>
#include <sys/stat.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace MiiArt {
namespace {

// The pack the app carries (romfs/miiparts), else one on the SD card.
const char* PackDir() {
    static const char* dir = nullptr;
    if (!dir) {
        struct stat st;
        dir = stat("romfs:/miiparts/index.txt", &st) == 0 ? "romfs:/miiparts/" : "sdmc:/switch/TomoToolNX/miiparts/";
    }
    return dir;
}

// CharInfoEx offsets (tlmodding/charinfo-ex, MIT; the wrinkle and make-up
// groups as the game reads them)
enum : int {
    kFaceFlags = 0x2B, kFacelineType = 0x2C, kFacelineColor = 0x2D,
    kHairType = 0x44, kHairColor0 = 0x46, kHairColor1 = 0x47, kHairTypeFront = 0x48, kHairTypeBack = 0x49, kHairStyle = 0x4A,
    kEarType = 0x4B, kEarScale = 0x4C, kEarY = 0x4D,
    kEyeType = 0x4E, kEyeColor = 0x4F, kEyeScale = 0x50, kEyeAspect = 0x51, kEyeRotate = 0x52, kEyeX = 0x53, kEyeY = 0x54,
    kEyeShadowColor = 0x55,
    // type, scale, aspect, rotate, x, y each
    kHighlight = 0x56, kLashUpper = 0x5C, kLashLower = 0x62, kLidUpper = 0x68, kLidLower = 0x6E,
    kEyebrowType = 0x74, kEyebrowColor = 0x75, kEyebrowScale = 0x76, kEyebrowAspect = 0x77, kEyebrowRotate = 0x78,
    kEyebrowX = 0x79, kEyebrowY = 0x7A, kNoseType = 0x7B, kNoseScale = 0x7C, kNoseY = 0x7D,
    kMouthType = 0x7E, kMouthColor = 0x7F, kMouthScale = 0x80, kMouthAspect = 0x81, kMouthRotate = 0x82, kMouthY = 0x83,
    kBeardType = 0x84, kBeardColor = 0x85, kBeardShortType = 0x86, kBeardShortColor = 0x87,
    kMustacheType = 0x88, kMustacheColor = 0x89, kMustacheScale = 0x8A, kMustacheY = 0x8C,
    kGlassType = 0x8D, kGlassColor = 0x8E, kGlassScale = 0x8F, kGlassY = 0x91, kLensType = 0x92, kLensColor = 0x93,
    kMoleScale = 0x94, kMoleX = 0x95, kMoleY = 0x96, kMinSize = 0x97,
};

// The game's 100 colours (nn::mii CommonColorTable, sRGB).
const uint32_t kPalette[100] = {
    0x2d2828, 0x402010, 0x5c180a, 0x7c3a14, 0x787880, 0x4e3e10, 0x885818, 0xd0a04a, 0x000000, 0x6c7070, 0x663c2c, 0x605e30,
    0x4654a8, 0x387058, 0x603810, 0xa81008, 0x203068, 0xa86000, 0x787068, 0xd85208, 0xf00c08, 0xf54848, 0xf09a74, 0x8c5040,
    0x842626, 0xff7366, 0xffa6a6, 0xffc0ba, 0x732e3b, 0x991f3d, 0x8a173e, 0xb53e42, 0xc71e56, 0xb05381, 0xc7546e, 0xfa7597,
    0xfcacc9, 0xffc9d8, 0x311c40, 0x37283d, 0x4c184d, 0x6f42b3, 0x855cb8, 0xc083cc, 0xa893c9, 0xc5ace6, 0xeebefa, 0xd2c5ed,
    0x191f40, 0x123f66, 0x2a82d4, 0x57b4f2, 0x7ac5de, 0x89a6fa, 0x84bdfa, 0xa1e3ff, 0x0b2e36, 0x013d3b, 0x0d4f59, 0x236663,
    0x307e8c, 0x4faeb0, 0x7ac49e, 0x7fd4c0, 0x87e5b6, 0x0a4a35, 0x437a00, 0x027562, 0x369970, 0x4bad1a, 0x92bf0a, 0x63c788,
    0x9ee042, 0x96de7e, 0xbbf2aa, 0x99932b, 0xa69563, 0xccc039, 0xccb987, 0xd9cc82, 0xd5d96f, 0xd5e683, 0xd8fa9d, 0x7d4500,
    0xe6bb7a, 0xfee24a, 0xfade82, 0xf7ea9c, 0xfaf89b, 0xa64d1e, 0xff960d, 0xd19b69, 0xffb266, 0xffc28c, 0xe5cfb1, 0x414141,
    0x9b9b9b, 0xbebebe, 0xdcd7cd, 0xffffff,
};
// Skin tones 100..113: the system library's ten, then the game's own four.
const uint32_t kSkin[14] = { 0xFFD3AD, 0xFFB66B, 0xDE7942, 0xFFAA8C, 0xAD5129, 0x632C18, 0xFFBEA5,
                             0xFFC58F, 0x8C3C23, 0x3C2D23, 0xFCD9C0, 0xEE9167, 0xE29454, 0x583623 };
// The upper lip's shade of each colour (nn::mii UpperLipColorTable).
const uint32_t kUpperLip[100] = {
    0x171414, 0x201008, 0x2e0c05, 0x4a230c, 0x54545a, 0x271f08, 0x52350e, 0xb18028, 0x000000, 0x4c4e4e, 0x331e16, 0x3a381d,
    0x2a3265, 0x274e3e, 0x301c08, 0x650a05, 0x101834, 0x764300, 0x544e49, 0x823018, 0x780c0c, 0x882028, 0xdc7850, 0x461e0a,
    0x4f1717, 0x99453d, 0xe68585, 0xe6a19b, 0x451c23, 0x5c1325, 0x530e25, 0x6d2528, 0x771234, 0x6a324d, 0x773242, 0xaf526a,
    0xe38cac, 0xe6abbb, 0x190e20, 0x1c141f, 0x260c27, 0x43286b, 0x50376e, 0x865c8f, 0x76678d, 0xab90cf, 0xd4a0e1, 0xb8aad5,
    0x0d1020, 0x092033, 0x1d5b94, 0x3297da, 0x5cadc8, 0x6786e1, 0x629fe1, 0x80c7e6, 0x06171b, 0x011f1e, 0x07282d, 0x184745,
    0x225862, 0x308b8d, 0x60b087, 0x63bfa9, 0x69ce9b, 0x05251b, 0x284900, 0x01463b, 0x266b4e, 0x2c8a00, 0x6e9900, 0x47b36f,
    0x82ca1f, 0x7ac860, 0x9eda8c, 0x6b671e, 0x746845, 0xa39816, 0xb8a36d, 0xc3b565, 0xbfc351, 0xbdcf64, 0xbce17d, 0x4b2900,
    0xcfa15a, 0xe5c622, 0xe1c35f, 0xded07c, 0xe1df7a, 0x642e12, 0xcc780a, 0xbc824c, 0xe69240, 0xe6a469, 0xceb696, 0x212121,
    0x7c7c7c, 0xababab, 0xc6c1b6, 0xd9d9d9,
};

struct Rgb { float r, g, b; };
Rgb Hex(uint32_t c) { return { ((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f }; }
Rgb Colour(int i) {
    if (i >= 100 && i < 114) return Hex(kSkin[i - 100]);
    if (i >= 0 && i < 100) return Hex(kPalette[i]);
    return Hex(0x402010);
}
Rgb UpperLip(int i) { return i >= 0 && i < 100 ? Hex(kUpperLip[i]) : Rgb{ 0.f, 0.f, 0.f }; }
const Rgb kBlack{ 0.f, 0.f, 0.f }, kWhite{ 1.f, 1.f, 1.f };
const Rgb kMole{ 0x12 / 255.f, 0x0F / 255.f, 0x0F / 255.f };    // nn::mii MoleColor
const Rgb kShave{ 1.f / 3.f, 0.4f, 0.4f };                       // the shaved-stubble ink
const float kNoDepth = -std::numeric_limits<float>::infinity();
const float SPX = 0.88961464f, SQY = 0.9276675f;                  // the face texture's squash

// A picture of the pack, kept on the CPU (RGBA, 8 bits).
struct Pic {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    bool ok = false;
    const uint8_t* At(int x, int y) const { return &px[((size_t)y * w + x) * 4]; }
};

struct Tex { std::string name, noLip; int offsetRotate = 0; };
struct Acc { int x = 0, y = 0, s = 0, a = 0, r = 0; };
// Each eye's frame: its axis, and the default place, size, shape and turn of
// its lids (upper, lower), lashes (upper, lower) and shine.
struct EyeAcc { float axis[2] = { 0.f, 0.f }; Acc c[5]; };
struct Head { std::string shade, uv, fl, z; float nose[2] = { 64, 81 }, hair[2] = { 64, 110 }, beard[2] = { 64, 110 }, ear[2] = { 93, 81 }; };
struct Piece { std::string pic, z; int attach = 0; };

struct Pack {
    bool loaded = false, ok = false;
    int size = 128, version = 1;
    float zFar = -0.6f, zNear = 0.5f;
    std::map<std::pair<std::string, int>, Tex> tex;     // (category, PartsIndex)
    std::map<int, EyeAcc> eyeAcc;
    std::map<int, Head> heads;
    std::map<int, Piece> hairs, fronts, ears, beards;
    std::map<std::pair<int, int>, Piece> parts;         // (hair part, place)
    std::map<int, std::string> noses;
    std::map<int, std::pair<std::string, std::string>> glasses;
    std::unordered_map<std::string, Pic> pics;
} sPack;

std::unordered_map<uint64_t, SDL_Texture*> sCache;
// Face paints read, by file: looked at again every few seconds, a new one
// read when the file changed. A replaced one is kept (another thread may be
// drawing with it); that happens only when a Mii is imported over another.
struct PaintEntry { std::unique_ptr<FacePaint> fp; time_t mtime = 0; off_t size = 0; double checked = -1e9; };
std::unordered_map<std::string, PaintEntry> sPaints;
std::vector<std::unique_ptr<FacePaint>> sOldPaints;

const Pic& Load(const std::string& file) {
    auto it = sPack.pics.find(file);
    if (it != sPack.pics.end()) return it->second;
    // Pictures load as they are needed; a long session keeps a few hundred.
    if (sPack.pics.size() > 600) sPack.pics.clear();
    Pic& p = sPack.pics[file];
    if (file.empty() || file == "-") return p;
    if (SDL_Surface* s = IMG_Load((std::string(PackDir()) + file).c_str())) {
        if (SDL_Surface* c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0)) {
            p.w = c->w; p.h = c->h;
            p.px.resize((size_t)p.w * p.h * 4);
            for (int y = 0; y < p.h; y++) memcpy(&p.px[(size_t)y * p.w * 4], (const uint8_t*)c->pixels + y * c->pitch, (size_t)p.w * 4);
            p.ok = p.w > 0 && p.h > 0;
            SDL_FreeSurface(c);
        }
        SDL_FreeSurface(s);
    }
    return p;
}

std::vector<std::string> Split(const char* line) {
    std::vector<std::string> out;
    const char* p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        const char* b = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        if (p > b) out.emplace_back(b, p);
    }
    return out;
}

// "key=a,b,..." to up to n numbers; the key, or "" when there is no '='.
std::string KeyNums(const std::string& kv, float* out, int n) {
    const size_t eq = kv.find('=');
    if (eq == std::string::npos) return "";
    const char* p = kv.c_str() + eq + 1;
    for (int i = 0; i < n && *p; i++) {
        char* e = nullptr;
        out[i] = strtof(p, &e);
        if (e == p) break;
        p = *e == ',' ? e + 1 : e;
    }
    return kv.substr(0, eq);
}

void LoadIndex() {
    if (sPack.loaded) return;
    sPack.loaded = true;
    FILE* f = fopen((std::string(PackDir()) + "index.txt").c_str(), "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        const std::vector<std::string> t = Split(line);
        if (t.empty()) continue;
        const std::string& k = t[0];
        auto I = [&](size_t i) { return i < t.size() ? atoi(t[i].c_str()) : 0; };
        auto S = [&](size_t i) { return i < t.size() && t[i] != "-" ? t[i] : std::string(); };
        if (k == "size") sPack.size = std::max(16, std::min(512, I(1)));
        else if (k == "version") sPack.version = I(1);
        else if (k == "depth" && t.size() >= 3) { sPack.zFar = strtof(t[1].c_str(), nullptr); sPack.zNear = strtof(t[2].c_str(), nullptr); }
        else if (k == "tex" && t.size() >= 5) sPack.tex[{ t[1], I(2) }] = { t[3], S(5), I(4) };
        else if (k == "eyeacc" && t.size() >= 2) {
            static const char* kCats[5] = { "EyelidUpper", "EyelidLower", "EyelashUpper", "EyelashLower", "Highlight" };
            EyeAcc e;
            for (size_t i = 2; i < t.size(); i++) {
                float v[5] = { 0, 0, 0, 0, 0 };
                const std::string key = KeyNums(t[i], v, 5);
                if (key == "axis") { e.axis[0] = v[0]; e.axis[1] = v[1]; continue; }
                for (int c = 0; c < 5; c++)
                    if (key == kCats[c]) e.c[c] = { (int)lroundf(v[0]), (int)lroundf(v[1]), (int)lroundf(v[2]), (int)lroundf(v[3]), (int)lroundf(v[4]) };
            }
            sPack.eyeAcc[I(1)] = e;
        } else if (k == "head" && t.size() >= 4) {
            Head h;
            h.shade = S(2); h.uv = S(3);
            for (size_t i = 4; i < t.size(); i++) {
                const size_t eq = t[i].find('=');
                if (eq == std::string::npos) continue;
                const std::string key = t[i].substr(0, eq), val = t[i].substr(eq + 1);
                float v[2] = { 0, 0 };
                if (key == "fl") h.fl = val == "-" ? "" : val;
                else if (key == "z") h.z = val == "-" ? "" : val;
                else if (key == "set_nose") { KeyNums(t[i], v, 2); h.nose[0] = v[0]; h.nose[1] = v[1]; }
                else if (key == "set_hair") { KeyNums(t[i], v, 2); h.hair[0] = v[0]; h.hair[1] = v[1]; }
                else if (key == "set_beard") { KeyNums(t[i], v, 2); h.beard[0] = v[0]; h.beard[1] = v[1]; }
                else if (key == "set_ear") { KeyNums(t[i], v, 2); h.ear[0] = v[0]; h.ear[1] = v[1]; }
            }
            sPack.heads[I(1)] = h;
        }
        else if (k == "hair" && t.size() >= 5) sPack.hairs[I(1)] = { S(2), S(3), I(4) };
        else if (k == "front" && t.size() >= 4) sPack.fronts[I(1)] = { S(2), S(3), 0 };
        else if (k == "hairpart" && t.size() >= 5) sPack.parts[{ I(1), I(2) }] = { S(3), S(4), 0 };
        else if (k == "ear" && t.size() >= 4) sPack.ears[I(1)] = { S(2), S(3), 0 };
        else if (k == "nose" && t.size() >= 3) sPack.noses[I(1)] = S(2);
        else if (k == "beard" && t.size() >= 4) sPack.beards[I(1)] = { S(2), S(3), 0 };
        else if (k == "glass" && t.size() >= 4) sPack.glasses[I(1)] = { S(2), S(3) };
    }
    fclose(f);
    // An older pack (no depth pictures) is not drawn with: the app keeps its own faces.
    sPack.ok = sPack.version >= 3 && !sPack.heads.empty();
}

const Tex* TexOf(const char* cat, int idx) {
    auto it = sPack.tex.find({ cat, idx });
    return it == sPack.tex.end() ? nullptr : &it->second;
}
template <class M, class K>
const typename M::mapped_type* Find(const M& m, const K& k) {
    auto it = m.find(k);
    return it == m.end() ? nullptr : &it->second;
}

// ── Pictures in floats ───────────────────────────────────────────────────────
// A canvas, colours premultiplied.
struct Canvas {
    int w, h;
    std::vector<float> px;
    Canvas(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_ * 4, 0.f) {}
    void Over(int x, int y, float r, float g, float b, float a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0.f) return;
        float* p = &px[((size_t)y * w + x) * 4];
        const float k = 1.f - a;
        p[0] = r * a + p[0] * k; p[1] = g * a + p[1] * k; p[2] = b * a + p[2] * k; p[3] = a + p[3] * k;
    }
};
// A picture, colours straight (n x n).
struct Img {
    int n = 0;
    std::vector<float> px;
    explicit Img(int n_ = 0) : n(n_), px((size_t)n_ * n_ * 4, 0.f) {}
    float* At(int x, int y) { return &px[((size_t)y * n + x) * 4]; }
    const float* At(int x, int y) const { return &px[((size_t)y * n + x) * 4]; }
};

// A pack picture in a colour: R x 1.25 multiplies it, G lightens it, B darkens it.
inline void Lit(float R, float G, float B, Rgb t, float out[3]) {
    const float k = R * 1.25f;
    const float c[3] = { t.r * k, t.g * k, t.b * k };
    for (int i = 0; i < 3; i++) out[i] = std::min(1.f, std::max(0.f, (c[i] + (1.f - c[i]) * G) * (1.f - B)));
}

Img PicImg(const Pic& p) {
    Img o(p.w);
    for (size_t i = 0; i < (size_t)p.w * p.h * 4; i++) o.px[i] = p.px[i] * (1.f / 255.f);
    return o;
}
Img LitImg(const Pic& p, Rgb tint) {
    Img o(p.w);
    for (size_t i = 0; i < (size_t)p.w * p.h; i++) {
        const uint8_t* q = &p.px[i * 4];
        Lit(q[0] / 255.f, q[1] / 255.f, q[2] / 255.f, tint, &o.px[i * 4]);
        o.px[i * 4 + 3] = q[3] / 255.f;
    }
    return o;
}
// A hair picture in its colours: the second one where its masks say (B: two-
// tone back hair in the low 4 bits, bangs in the high 4), then lit.
Img HairImg(const Pic& p, Rgb c0, Rgb c1, bool backOn, bool bangsOn) {
    Img o(p.w);
    for (size_t i = 0; i < (size_t)p.w * p.h; i++) {
        const uint8_t* q = &p.px[i * 4];
        float w = 0.f;
        if (backOn) w += (q[2] & 15) / 15.f;
        if (bangsOn) w += (q[2] >> 4) / 15.f;
        w = std::min(1.f, w);
        const Rgb t{ c0.r + (c1.r - c0.r) * w, c0.g + (c1.g - c0.g) * w, c0.b + (c1.b - c0.b) * w };
        Lit(q[0] / 255.f, q[1] / 255.f, 0.f, t, &o.px[i * 4]);
        o.px[i * 4 + 3] = q[3] / 255.f;
    }
    return o;
}
// A depth picture as model z (nearer is larger), kNoDepth where empty.
std::vector<float> DepthOf(const Pic& p) {
    std::vector<float> z((size_t)p.w * p.h, kNoDepth);
    const float k = (sPack.zNear - sPack.zFar) / 254.f;
    for (size_t i = 0; i < z.size(); i++)
        if (const uint8_t v = p.px[i * 4]) z[i] = sPack.zFar + (v - 1) * k;
    return z;
}

// The picture scaled by s about (ax, ay), moved by (dx, dy), maybe mirrored
// about the middle; bilinear, through premultiplied colour.
Img Warp(const Img& p, float ax, float ay, float s, float dx, float dy, bool mirror) {
    const int N = p.n;
    Img o(N);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float X = x + 0.5f;
            const float Y = y + 0.5f;
            if (mirror) X = N - X;
            const float sx = (X - ax - dx) / s + ax - 0.5f, sy = (Y - ay - dy) / s + ay - 0.5f;
            const int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
            const float fx = sx - x0, fy = sy - y0;
            float acc[4] = { 0, 0, 0, 0 };
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    const int xx = x0 + i, yy = y0 + j;
                    if (xx < 0 || yy < 0 || xx >= N || yy >= N) continue;
                    const float w = (i ? fx : 1.f - fx) * (j ? fy : 1.f - fy);
                    const float* q = p.At(xx, yy);
                    const float aw = q[3] * w;
                    acc[0] += q[0] * aw; acc[1] += q[1] * aw; acc[2] += q[2] * aw; acc[3] += aw;
                }
            float* r = o.At(x, y);
            if (acc[3] > 1e-5f) { r[0] = acc[0] / acc[3]; r[1] = acc[1] / acc[3]; r[2] = acc[2] / acc[3]; }
            r[3] = acc[3];
        }
    return o;
}
// A depth picture moved the same way (nearest).
std::vector<float> WarpZ(const std::vector<float>& z, int N, float ax, float ay, float s, float dx, float dy, bool mirror) {
    std::vector<float> o((size_t)N * N, kNoDepth);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float X = x + 0.5f;
            const float Y = y + 0.5f;
            if (mirror) X = N - X;
            const int sx = (int)floorf((X - ax - dx) / s + ax), sy = (int)floorf((Y - ay - dy) / s + ay);
            if (sx >= 0 && sy >= 0 && sx < N && sy < N) o[(size_t)y * N + x] = z[(size_t)sy * N + sx];
        }
    return o;
}

// p (W x H, 8 bits) at texel coordinates x, y (centres at +0.5), clamped.
void Sample(const uint8_t* px, int W, int H, float x, float y, float out[4]) {
    x = std::min((float)(W - 1), std::max(0.f, x - 0.5f));
    y = std::min((float)(H - 1), std::max(0.f, y - 0.5f));
    const int x0 = std::min((int)x, W > 1 ? W - 2 : 0), y0 = std::min((int)y, H > 1 ? H - 2 : 0);
    const float fx = x - x0, fy = y - y0;
    const int x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
    const uint8_t* a = &px[((size_t)y0 * W + x0) * 4];
    const uint8_t* b = &px[((size_t)y0 * W + x1) * 4];
    const uint8_t* c = &px[((size_t)y1 * W + x0) * 4];
    const uint8_t* d = &px[((size_t)y1 * W + x1) * 4];
    for (int i = 0; i < 4; i++)
        out[i] = ((a[i] * (1.f - fx) + b[i] * fx) * (1.f - fy) + (c[i] * (1.f - fx) + d[i] * fx) * fy) * (1.f / 255.f);
}

// ── The head's own texture: skin, make-up, wrinkles, stubble ────────────────
// Painted into the head's texture (half the face, mirrored down the middle):
// a part is a quad anchored on its side toward the middle; X steps it out, Y
// steps it down, by 1.29/32 of the texture's width (Y by half of that of its
// height, twice the width). In the game's drawing order.
struct FlPart { const char* cat; int at, colAt; float s0, aspectK; };
const FlPart kFlParts[4] = {
    { "MakeLower", 0x3E, 0x3F, 0.16f, 0.9f }, { "MakeUpper", 0x38, 0x39, 0.18f, 0.8f },
    { "WrinkleLower", 0x33, -1, 0.16f, 0.9f }, { "WrinkleUpper", 0x2E, -1, 0.16f, 0.8f },
};
const float kFlStep = 1.29f / 32.f;

// The head's colour per pixel (fl: each pixel's u, v on that texture).
void Faceline(const uint8_t* ci, const Pic& fl, Rgb skin, std::vector<float>& alb) {
    const size_t n = (size_t)fl.w * fl.h;
    alb.resize(n * 3);
    for (size_t i = 0; i < n; i++) { alb[i * 3] = skin.r; alb[i * 3 + 1] = skin.g; alb[i * 3 + 2] = skin.b; }
    struct Paint { const Pic* t; float u0, v0, w, h; Rgb ink[2]; bool two; };
    std::vector<Paint> paints;
    for (const FlPart& fp : kFlParts) {
        const int typ = ci[fp.at];
        const Tex* t = typ ? TexOf(fp.cat, typ) : nullptr;
        if (!t) continue;
        const Pic& p = Load("tex/" + t->name + ".png");
        if (!p.ok) continue;
        const int o = fp.at + (fp.colAt >= 0 ? 2 : 1);              // scale, aspect, x, y follow
        const float w = fp.s0 * (0.4f * ci[o] + 1.f);
        const float h = w * (0.12f * ci[o + 1] + 0.64f) * fp.aspectK * 0.5f;
        const float right = 1.f - ci[o + 2] * kFlStep, mid = 0.14f + ci[o + 3] * kFlStep * 0.5f;
        const Rgb ink = fp.colAt >= 0 ? Colour(ci[fp.colAt]) : kBlack;
        // blush and its freckles (black)
        paints.push_back({ &p, right - w, mid - h * 0.5f, w, h, { ink, kBlack }, fp.colAt == 0x3F });
    }
    // stubble covers the whole texture: the shaved shade, then its colour
    if (const int typ = ci[kBeardShortType])
        if (const Tex* t = TexOf("BeardShort", typ)) {
            const Pic& p = Load("tex/" + t->name + ".png");
            if (p.ok) paints.push_back({ &p, 0.f, 0.f, 1.f, 1.f, { kShave, Colour(ci[kBeardShortColor]) }, true });
        }
    if (paints.empty()) return;
    for (size_t i = 0; i < n; i++) {
        const uint8_t* q = &fl.px[i * 4];
        if (!q[3]) continue;
        const float u = q[0] / 255.f, v = q[1] / 255.f;
        float* a = &alb[i * 3];
        for (const Paint& pt : paints) {
            const float s = (u - pt.u0) / pt.w, tt = (v - pt.v0) / pt.h;
            if (s < 0.f || s >= 1.f || tt < 0.f || tt >= 1.f) continue;
            float smp[4];
            Sample(pt.t->px.data(), pt.t->w, pt.t->h, s * pt.t->w, tt * pt.t->h, smp);
            for (int ch = 0; ch < (pt.two ? 2 : 1); ch++) {
                const float k = smp[ch];
                const Rgb& ink = pt.ink[ch];
                a[0] = a[0] * (1.f - k) + ink.r * k; a[1] = a[1] * (1.f - k) + ink.g * k; a[2] = a[2] * (1.f - k) + ink.b * k;
            }
        }
    }
}

// ── The face texture: FFL's placement on a 64-unit grid ─────────────────────
// The quad scaled, turned, squashed (0.8896, 0.9277) and moved.
enum class Origin { Left, Right, Center };
template <class TintFn>
void Place(Canvas& mask, const Pic& t, float px, float py, float sx, float sy, float rotDeg, Origin o, bool mirror, TintFn tint) {
    if (!t.ok || sx <= 0 || sy <= 0) return;
    const float r = rotDeg * 3.14159265f / 180.f, c = cosf(-r), s = sinf(-r);
    const float add = o == Origin::Left ? -1.f : o == Origin::Right ? 0.f : -0.5f;
    const float reach = std::max(sx, sy) * 1.5f + 2.f;
    const int x0 = std::max(0, (int)(px - reach)), x1 = std::min(mask.w - 1, (int)(px + reach));
    const int y0 = std::max(0, (int)(py - reach)), y1 = std::min(mask.h - 1, (int)(py + reach));
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const float X = (x + 0.5f - px) / SPX, Y = (y + 0.5f - py) / SQY;
            float u = (X * c - Y * s) / sx - add;
            const float v = (X * s + Y * c) / sy + 0.5f;
            if (u < 0.f || u >= 1.f || v < 0.f || v >= 1.f) continue;
            if (mirror) u = 1.f - u;
            const int tx = std::min(t.w - 1, (int)(u * t.w)), ty = std::min(t.h - 1, (int)(v * t.h));
            float rr, gg, bb, aa;
            tint(t.At(tx, ty), rr, gg, bb, aa);
            mask.Over(x, y, rr, gg, bb, aa);
        }
}
auto FlatTint(Rgb c) {
    return [c](const uint8_t* q, float& r, float& g, float& b, float& a) { r = c.r; g = c.g; b = c.b; a = q[0] / 255.f; };
}
// A newer part's value: the eye's default moved by the stored one, kept in
// the field's range (0..top).
int Wrapped(int def, int stored, int top) { return ((def + stored) % (top + 1) + (top + 1)) % (top + 1); }

// Lids, lashes and the shine, each placed in its eye's own frame.
void EyeExtras(Canvas& mask, const uint8_t* ci, float base) {
    auto it = sPack.eyeAcc.find(ci[kEyeType]);
    if (it == sPack.eyeAcc.end()) return;
    const EyeAcc& e = it->second;
    const float bs = 0.4f * ci[kEyeScale] + 1.f;
    const float w = 5.34375f * bs, h = 4.5f * bs * (0.12f * ci[kEyeAspect] + 0.64f);
    const Tex* te = TexOf("Eye", ci[kEyeType]);
    const float rotL = (ci[kEyeRotate] - 4 + (te ? te->offsetRotate : 0)) * (360.f / 32.f);
    struct Kind { const char* cat; int at, topY, slot; bool lash; };
    static const Kind kKinds[4] = { { "EyelidUpper", kLidUpper, 36, 0, false }, { "EyelidLower", kLidLower, 36, 1, false },
                                    { "EyelashUpper", kLashUpper, 31, 2, true }, { "EyelashLower", kLashLower, 31, 3, true } };
    for (int side = -1; side <= 1; side += 2) {                     // -1: the eye on the left of the picture
        const float ax = 32.f + side * ci[kEyeX] * SPX, ay = ci[kEyeY] * 1.0760943f + 18.451524f;
        const float rot = side < 0 ? rotL : -rotL, rr = rot * 3.14159265f / 180.f;
        // the eye's pivot: its axis point, inside the quad from the inner corner
        const float px = ax + side * e.axis[0] * w * SPX, py = ay - e.axis[1] * h * SQY;
        auto at = [&](float dx, float dy, float& cx, float& cy) {
            const float ox = side * dx * SPX, oy = dy * SQY;
            cx = px + ox * cosf(rr) - oy * sinf(rr);
            cy = py + ox * sinf(rr) + oy * cosf(rr);
        };
        for (const Kind& k : kKinds) {
            const int typ = ci[k.at];
            const Tex* t = typ ? TexOf(k.cat, typ) : nullptr;
            if (!t) continue;
            const Acc& d = e.c[k.slot];
            const int X = Wrapped(d.x, ci[k.at + 4], 31), Y = Wrapped(d.y, ci[k.at + 5], k.topY);
            const int S = Wrapped(d.s, ci[k.at + 1], 14), A = Wrapped(d.a, ci[k.at + 2], 6), R = Wrapped(d.r, ci[k.at + 3], 31);
            const float kk = k.lash ? (S + 5) / 28.f : (S + 8) * 0.04f;
            const float sx = kk * w, sy = kk * w * (0.12f * A + 0.64f) * 0.8421053f;
            float cx, cy;
            at(w * (X / 31.f - e.axis[0]), h * (e.axis[1] + Y / 31.f - 0.5f), cx, cy);
            const float ang = rot - side * ((R < 16 ? R : R - 32) * 11.25f);
            Place(mask, Load("tex/" + t->name + ".png"), cx * base, cy * base, sx * base, sy * base, ang, Origin::Center, side > 0, FlatTint(kBlack));
        }
        const int typ = ci[kHighlight];
        if (const Tex* t = typ ? TexOf("Highlight", typ) : nullptr) {
            const Acc& d = e.c[4];
            const int X = Wrapped(d.x, ci[kHighlight + 4], 32), Y = Wrapped(d.y, ci[kHighlight + 5], 32);
            const int S = Wrapped(d.s, ci[kHighlight + 1], 14), A = Wrapped(d.a, ci[kHighlight + 2], 6);
            const float hs = 0.0345479f * S + 0.1554656f;
            const float sx = hs * std::min(w, h / 0.8421053f), sy = sx * (0.12f * A + 0.64f);
            const float cx = px + (0.5f - X / 32.f) * w * SPX, cy = py + (Y / 32.f - 0.5f) * h * SQY;
            Place(mask, Load("tex/" + t->name + ".png"), cx * base, cy * base, sx * base, sy * base, 0.f, Origin::Center, false, FlatTint(kWhite));
        }
    }
}

void ComposeFace(Canvas& mask, const uint8_t* ci) {
    const float base = mask.w / 64.f;
    const float POS_X_ADD = 3.5323312f, POS_Y_ADD = 4.629278f, POS_X_MUL = 1.7792293f, POS_Y_MUL = 1.0760943f;
    // Eyebrows: one mask in the eyebrow colour.
    if (const Tex* t = TexOf("Eyebrow", ci[kEyebrowType])) {
        const Pic& p = Load("tex/" + t->name + ".png");
        const float sx = ci[kEyebrowX] * SPX, bs = 0.4f * ci[kEyebrowScale] + 1.f;
        const float w = 5.0625f * bs * base, h = 4.5f * bs * (0.12f * ci[kEyebrowAspect] + 0.64f) * base;
        const float y = (ci[kEyebrowY] * POS_Y_MUL + POS_Y_ADD + 11.920528f) * base;
        const float rot = (ci[kEyebrowRotate] - 6 + t->offsetRotate) * (360.f / 32.f);
        Place(mask, p, (32 - sx) * base, y, w, h, rot, Origin::Left, false, FlatTint(Colour(ci[kEyebrowColor])));
        Place(mask, p, (32 + sx) * base, y, w, h, 360.f - rot, Origin::Right, true, FlatTint(Colour(ci[kEyebrowColor])));
    }
    // Eyes: white (G), iris (B), eyeshadow (R, when it is on), the outline black.
    if (const Tex* t = TexOf("Eye", ci[kEyeType])) {
        const Rgb iris = Colour(ci[kEyeColor]), shadow = Colour(ci[kEyeShadowColor]);
        const bool shadowOn = (ci[kFaceFlags] >> 4) & 1;
        auto tint = [iris, shadow, shadowOn](const uint8_t* q, float& r, float& g, float& b, float& a) {
            const float R = q[0] / 255.f, G = q[1] / 255.f, B = q[2] / 255.f, A = q[3] / 255.f;
            r = std::min(1.f, G + iris.r * B + (shadowOn ? shadow.r * R : 0.f));
            g = std::min(1.f, G + iris.g * B + (shadowOn ? shadow.g * R : 0.f));
            b = std::min(1.f, G + iris.b * B + (shadowOn ? shadow.b * R : 0.f));
            a = shadowOn ? A : std::max(0.f, A - R * std::max(0.f, 1.f - G - B));
        };
        const Pic& p = Load("tex/" + t->name + ".png");
        const float sx = ci[kEyeX] * SPX, bs = 0.4f * ci[kEyeScale] + 1.f;
        const float w = 5.34375f * bs * base, h = 4.5f * bs * (0.12f * ci[kEyeAspect] + 0.64f) * base;
        const float y = (ci[kEyeY] * POS_Y_MUL + POS_Y_ADD + 13.822246f) * base;
        const float rot = (ci[kEyeRotate] - 4 + t->offsetRotate) * (360.f / 32.f);
        Place(mask, p, (32 - sx) * base, y, w, h, rot, Origin::Left, false, tint);
        Place(mask, p, (32 + sx) * base, y, w, h, 360.f - rot, Origin::Right, true, tint);
    }
    EyeExtras(mask, ci, base);
    // Mouth: the lower lip (R) in the mouth colour, the upper lip (G) in its
    // darker shade, the rest black (the line). Without lipstick (flag bit 5)
    // the game draws the mouth's no-lip version, else only its line.
    if (const Tex* t = TexOf("Mouth", ci[kMouthType])) {
        const bool lips = (ci[kFaceFlags] >> 5) & 1;
        const Pic& p = Load("tex/" + (!t->noLip.empty() && !lips ? t->noLip : t->name) + ".png");
        const Rgb lower = Colour(ci[kMouthColor]), upper = UpperLip(ci[kMouthColor]);
        auto tint = [lips, lower, upper](const uint8_t* q, float& r, float& g, float& b, float& a) {
            const float R = q[0] / 255.f, G = q[1] / 255.f, A = q[3] / 255.f;
            if (lips) {
                r = std::min(1.f, lower.r * R + upper.r * G); g = std::min(1.f, lower.g * R + upper.g * G);
                b = std::min(1.f, lower.b * R + upper.b * G); a = A;
            } else {
                r = g = b = 0.f;
                a = A * std::min(1.f, std::max(0.f, 1.f - R - G));
            }
        };
        const float bs = 0.4f * ci[kMouthScale] + 1.f;
        const float w = 6.1875f * bs * base, h = 4.5f * bs * (0.12f * ci[kMouthAspect] + 0.64f) * base;
        const float y = (ci[kMouthY] * POS_Y_MUL + POS_Y_ADD + 24.629572f) * base;
        Place(mask, p, 32 * base, y, w, h, (ci[kMouthRotate] & 31) * 11.25f, Origin::Center, false, tint);
    }
    // Moustache: a mask in its colour, both halves.
    if (const Tex* t = TexOf("Mustache", ci[kMustacheType])) {
        const Pic& p = Load("tex/" + t->name + ".png");
        const float bs = 0.4f * ci[kMustacheScale] + 1.f;
        const float y = (ci[kMustacheY] * POS_Y_MUL + POS_Y_ADD + 27.134275f) * base;
        Place(mask, p, 32 * base, y, 4.5f * bs * base, 9.f * bs * base, 0.f, Origin::Left, false, FlatTint(Colour(ci[kMustacheColor])));
        Place(mask, p, 32 * base, y, 4.5f * bs * base, 9.f * bs * base, 0.f, Origin::Right, true, FlatTint(Colour(ci[kMustacheColor])));
    }
    // Mole.
    if ((ci[kFaceFlags] >> 7) & 1)
        if (const Tex* t = TexOf("Mole", 1)) {
            const float s = (0.4f * ci[kMoleScale] + 1.f) * base;
            Place(mask, Load("tex/" + t->name + ".png"), (ci[kMoleX] * POS_X_MUL + POS_X_ADD + 14.233834f) * base,
                  (ci[kMoleY] * POS_Y_MUL + POS_Y_ADD + 11.178394f + 2 * POS_Y_MUL) * base, s, s, 0.f, Origin::Center, false, FlatTint(kMole));
        }
}

// The face paint on the face texture: centred a little high, 0.75 of its size
// (the game's FacePaintOffset / FacePaintSize).
void PaintLayer(Canvas& layer, const FacePaint& fp) {
    const int res = layer.w;
    const float size = 0.75f * res, x0 = (res - size) * 0.5f, y0 = (res - size) * 0.5f - 0.03f * res;
    for (int y = 0; y < res; y++)
        for (int x = 0; x < res; x++) {
            const float u = (x + 0.5f - x0) / size, v = (y + 0.5f - y0) / size;
            if (u < 0.f || u >= 1.f || v < 0.f || v >= 1.f) continue;
            float q[4];
            Sample(fp.rgba.data(), fp.w, fp.h, u * fp.w, v * fp.h, q);
            layer.Over(x, y, std::min(1.f, q[0]), std::min(1.f, q[1]), std::min(1.f, q[2]), std::min(1.f, q[3]));
        }
}
// Half the size (2 x 2 box): read at about one texel a pixel, fine lines hold.
Canvas Half(const Canvas& c) {
    Canvas o(c.w / 2, c.h / 2);
    for (int y = 0; y < o.h; y++)
        for (int x = 0; x < o.w; x++)
            for (int k = 0; k < 4; k++)
                o.px[((size_t)y * o.w + x) * 4 + k] = 0.25f * (c.px[((size_t)(2 * y) * c.w + 2 * x) * 4 + k] + c.px[((size_t)(2 * y) * c.w + 2 * x + 1) * 4 + k] +
                                                               c.px[((size_t)(2 * y + 1) * c.w + 2 * x) * 4 + k] + c.px[((size_t)(2 * y + 1) * c.w + 2 * x + 1) * 4 + k]);
    return o;
}

uint64_t Key(const uint8_t* ci, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0x27; i < n && i < 0x98; i++) h = (h ^ ci[i]) * 1099511628211ull;
    return h;
}

}  // namespace

// The pack's pictures load on first use: the WebUI's thread and the main
// one take turns.
static Mutex sArtMutex;

bool Available() { mutexLock(&sArtMutex); LoadIndex(); const bool ok = sPack.ok; mutexUnlock(&sArtMutex); return ok; }

const FacePaint* LoadFacePaint(const std::string& canvasPath) {
    const double now = Motion::Now();
    mutexLock(&sArtMutex);
    PaintEntry& e = sPaints[canvasPath];
    if (now - e.checked < 3.0) { const FacePaint* r = e.fp.get(); mutexUnlock(&sArtMutex); return r; }
    e.checked = now;
    struct stat st;
    const bool there = stat(canvasPath.c_str(), &st) == 0 && st.st_size > 0;
    if (there && e.fp && st.st_mtime == e.mtime && st.st_size == e.size) { const FacePaint* r = e.fp.get(); mutexUnlock(&sArtMutex); return r; }
    if (e.fp) sOldPaints.push_back(std::move(e.fp));
    RgbaImage img;
    if (there && TextureProcessor::DecodeFile(canvasPath, img, false).empty() && img.width >= 2 && img.height >= 2 &&
        img.pixels.size() >= (size_t)img.width * img.height * 4) {
        // kept at half size (it is read at about that): 64 KB a paint
        auto fp = std::make_unique<FacePaint>();
        fp->w = img.width / 2;
        fp->h = img.height / 2;
        fp->rgba.resize((size_t)fp->w * fp->h * 4);
        for (int y = 0; y < fp->h; y++)
            for (int x = 0; x < fp->w; x++)
                for (int c = 0; c < 4; c++) {
                    const uint8_t* s = &img.pixels[((size_t)(2 * y) * img.width + 2 * x) * 4 + c];
                    const size_t row = (size_t)img.width * 4;
                    fp->rgba[((size_t)y * fp->w + x) * 4 + c] = (uint8_t)((s[0] + s[4] + s[row] + s[row + 4] + 2) / 4);
                }
        uint64_t h = 1469598103934665603ull;
        for (char c : canvasPath) h = (h ^ (uint8_t)c) * 1099511628211ull;
        h ^= (uint64_t)st.st_size * 0x9E3779B97F4A7C15ull ^ (uint64_t)st.st_mtime;
        fp->key = h | 1;
        e.fp = std::move(fp);
        e.mtime = st.st_mtime;
        e.size = st.st_size;
    }
    const FacePaint* r = e.fp.get();
    mutexUnlock(&sArtMutex);
    return r;
}

// The whole picture onto cv (N x N, the pack's size). Holds sArtMutex.
// The 3D pieces (hair, hair part, front piece, ears, the head with all on it)
// are laid down by depth, pixel by pixel, farthest first; glasses go over all.
static void Compose(const uint8_t* ci, const FacePaint* paint, bool paintFront, Canvas& cv) {
    const int N = sPack.size;
    const Rgb skin = Colour(ci[kFacelineColor]);
    const int flags = ci[kFaceFlags];
    const Rgb hc0 = Colour(ci[kHairColor0]), hc1 = Colour(ci[kHairColor1]);
    const bool backOn = (flags >> 2) & 1, bangsOn = (flags >> 3) & 1;    // two-tone back hair, bangs
    auto headIt = sPack.heads.find(ci[kFacelineType]);
    if (headIt == sPack.heads.end()) headIt = sPack.heads.begin();
    const Head& head = headIt->second;
    const Head& h0 = sPack.heads.begin()->second;
    const float dHair[2] = { head.hair[0] - h0.hair[0], head.hair[1] - h0.hair[1] };
    const int hairType = ci[kHairType] | (ci[kHairType + 1] << 8);
    const Piece* hair = Find(sPack.hairs, hairType);
    const Piece* front = hair && hair->attach >= 1 ? Find(sPack.fronts, (int)ci[kHairTypeFront]) : nullptr;
    const Piece* part = hair && hair->attach >= 2 ? Find(sPack.parts, std::make_pair((int)ci[kHairTypeBack], (int)ci[kHairStyle])) : nullptr;

    struct Layer { Img rgba; std::vector<float> z; };
    std::vector<Layer> pieces;
    // Hair: the style, its part, its front piece, moved to this head.
    for (const Piece* pc : { hair, part, front }) {
        if (!pc) continue;
        const Pic& p = Load(pc->pic);
        const Pic& zp = Load(pc->z);
        if (!p.ok || !zp.ok || p.w != N || zp.w != N) continue;
        Img q = HairImg(p, hc0, hc1, backOn, bangsOn);
        std::vector<float> z = DepthOf(zp);
        if (dHair[0] != 0.f || dHair[1] != 0.f) {
            q = Warp(q, 0, 0, 1.f, dHair[0], dHair[1], false);
            z = WarpZ(z, N, 0, 0, 1.f, dHair[0], dHair[1], false);
        }
        pieces.push_back({ std::move(q), std::move(z) });
    }
    // Ears, both sides.
    if (const Piece* e = Find(sPack.ears, (int)ci[kEarType])) {
        const Pic& p = Load(e->pic);
        const Pic& zp = Load(e->z);
        if (p.ok && zp.ok && p.w == N && zp.w == N) {
            const float ax = h0.ear[0], ay = h0.ear[1];
            const float dx = head.ear[0] - ax, dy = head.ear[1] - ay + (ci[kEarY] - 4) * 1.f;
            const float es = 1.f + 0.1f * (ci[kEarScale] - 2);
            const Img lit = LitImg(p, skin);
            const std::vector<float> z = DepthOf(zp);
            for (bool mirror : { false, true })
                pieces.push_back({ Warp(lit, ax, ay, es, dx, dy, mirror), WarpZ(z, N, ax, ay, es, dx, dy, mirror) });
        }
    }
    // The head and all on it: its texture (skin, make-up, wrinkles, stubble),
    // the face paint and the face's parts, the beard, the nose.
    Canvas hcv(N, N);
    const Pic& shade = Load(head.shade);
    if (shade.ok && shade.w == N) {
        const Pic& fl = Load(head.fl);
        std::vector<float> alb;
        const bool haveFl = fl.ok && fl.w == N;
        if (haveFl) Faceline(ci, fl, skin, alb);
        for (int i = 0; i < N * N; i++) {
            const uint8_t* q = &shade.px[(size_t)i * 4];
            if (!q[3]) continue;
            const Rgb t = haveFl ? Rgb{ alb[i * 3], alb[i * 3 + 1], alb[i * 3 + 2] } : skin;
            float c[3];
            Lit(q[0] / 255.f, q[1] / 255.f, q[2] / 255.f, t, c);
            hcv.Over(i % N, i / N, c[0], c[1], c[2], q[3] / 255.f);
        }
    }
    {
        const Pic& uv = Load(head.uv);
        if (uv.ok && uv.w == N) {
            Canvas face(256, 256);
            ComposeFace(face, ci);
            const Canvas faceH = Half(face);
            const bool havePaint = paint && paint->w > 0 && paint->h > 0 && paint->rgba.size() >= (size_t)paint->w * paint->h * 4;
            Canvas paintH(1, 1);
            if (havePaint) {
                Canvas pc(256, 256);
                PaintLayer(pc, *paint);
                paintH = Half(pc);
            }
            // cover: B for the face's parts (on the head), A for the paint
            // (the face texture's whole sheet, which reaches past the head)
            auto lay = [&](const Canvas& m, int coverCh) {
                for (int i = 0; i < N * N; i++) {
                    const uint8_t* q = &uv.px[(size_t)i * 4];
                    if (!q[coverCh]) continue;
                    const float* c = &m.px[((size_t)(q[1] / 2) * m.w + q[0] / 2) * 4];
                    if (c[3] <= 0.f) continue;
                    hcv.Over(i % N, i / N, c[0] / c[3], c[1] / c[3], c[2] / c[3], c[3] * q[coverCh] / 255.f);
                }
            };
            if (havePaint && !paintFront) lay(paintH, 3);
            lay(faceH, 2);
            if (havePaint && paintFront) lay(paintH, 3);
        }
    }
    std::vector<float> zh;
    if (const Pic& zp = Load(head.z); zp.ok && zp.w == N) zh = DepthOf(zp);
    else {
        zh.assign((size_t)N * N, kNoDepth);
        if (shade.ok && shade.w == N)
            for (int i = 0; i < N * N; i++) if (shade.px[(size_t)i * 4 + 3]) zh[i] = 0.f;
    }
    if (const Piece* b = Find(sPack.beards, (int)ci[kBeardType])) {
        const Pic& p = Load(b->pic);
        if (p.ok && p.w == N) {
            const float dx = head.beard[0] - h0.beard[0], dy = head.beard[1] - h0.beard[1];
            Img lit = LitImg(p, Colour(ci[kBeardColor]));
            if (dx != 0.f || dy != 0.f) lit = Warp(lit, 0, 0, 1.f, dx, dy, false);
            for (int i = 0; i < N * N; i++) {
                const float* q = &lit.px[(size_t)i * 4];
                if (q[3] > 0.f) hcv.Over(i % N, i / N, q[0], q[1], q[2], q[3]);
            }
            const Pic& zp = Load(b->z);
            if (zp.ok && zp.w == N) {
                std::vector<float> zb = DepthOf(zp);
                if (dx != 0.f || dy != 0.f) zb = WarpZ(zb, N, 0, 0, 1.f, dx, dy, false);
                for (int i = 0; i < N * N; i++) zh[i] = std::max(zh[i], zb[i]);
            }
        }
    }
    if (const std::string* nose = Find(sPack.noses, (int)ci[kNoseType])) {
        const Pic& p = Load(*nose);
        if (p.ok && p.w == N) {
            const float ax = h0.nose[0], ay = h0.nose[1];
            const float ns = 1.f + 0.1f * (ci[kNoseScale] - 4);
            const Img w = Warp(PicImg(p), ax, ay, ns, head.nose[0] - ax, head.nose[1] - ay + (ci[kNoseY] - 9) * 1.1f, false);
            for (int i = 0; i < N * N; i++) {
                const float* q = &w.px[(size_t)i * 4];
                if (q[3] <= 0.f) continue;
                float c[3];
                Lit(q[0], q[1], q[2], skin, c);
                hcv.Over(i % N, i / N, c[0], c[1], c[2], q[3]);
            }
        }
    }
    {
        Layer hl{ Img(N), std::move(zh) };
        for (int i = 0; i < N * N; i++) {
            const float* c = &hcv.px[(size_t)i * 4];
            float* o = &hl.rgba.px[(size_t)i * 4];
            if (c[3] <= 0.f) continue;
            o[0] = c[0] / c[3]; o[1] = c[1] / c[3]; o[2] = c[2] / c[3]; o[3] = c[3];
            // what lies past the head (face paint, the nose's tip): just before the face
            if (std::isinf(hl.z[i])) hl.z[i] = 0.25f;
        }
        pieces.push_back(std::move(hl));
    }

    // Farthest first, pixel by pixel (equal depths keep their order).
    const int L = (int)pieces.size();
    for (int i = 0; i < N * N; i++) {
        int order[8];
        int m = 0;
        for (int k = 0; k < L && m < 8; k++) {
            const float z = pieces[k].z[i];
            if (std::isinf(z) || pieces[k].rgba.px[(size_t)i * 4 + 3] <= 0.f) continue;
            int j = m++;
            while (j > 0 && pieces[order[j - 1]].z[i] > z) { order[j] = order[j - 1]; j--; }
            order[j] = k;
        }
        for (int j = 0; j < m; j++) {
            const float* c = &pieces[order[j]].rgba.px[(size_t)i * 4];
            cv.Over(i % N, i / N, c[0], c[1], c[2], c[3]);
        }
    }
    // Glasses, over everything.
    if (const auto* g = Find(sPack.glasses, (int)ci[kGlassType])) {
        const Pic& fr = Load(g->first);
        const Pic& ln = Load(g->second);
        const float gs = 1.f + 0.1f * (ci[kGlassScale] - 4);
        const float dy = head.nose[1] - 9.f + (ci[kGlassY] - 11) * 1.1f - N / 2.f;
        if (ln.ok && ln.w == N && ci[kLensType]) {
            const Img w = Warp(PicImg(ln), N / 2.f, N / 2.f, gs, 0.f, dy, false);
            const Rgb lc = Colour(ci[kLensColor]);
            for (int i = 0; i < N * N; i++) {
                const float a = w.px[(size_t)i * 4 + 3] * 0.7f;
                if (a > 0.f) cv.Over(i % N, i / N, lc.r, lc.g, lc.b, a);
            }
        }
        if (fr.ok && fr.w == N) {
            const Img w = Warp(PicImg(fr), N / 2.f, N / 2.f, gs, 0.f, dy, false);
            const Rgb gc = Colour(ci[kGlassColor]);
            for (int i = 0; i < N * N; i++) {
                const float* q = &w.px[(size_t)i * 4];
                if (q[3] <= 0.f) continue;
                float c[3];
                Lit(q[0], q[1], q[2], gc, c);
                cv.Over(i % N, i / N, c[0], c[1], c[2], q[3]);
            }
        }
    }
}

static void ToBytes(const Canvas& cv, uint8_t* out, int pitch) {
    for (int y = 0; y < cv.h; y++) {
        uint8_t* row = out + (size_t)y * pitch;
        for (int x = 0; x < cv.w; x++) {
            const float* p = &cv.px[((size_t)y * cv.w + x) * 4];
            const float a = p[3];
            for (int c = 0; c < 3; c++) row[x * 4 + c] = (uint8_t)std::min(255.f, a > 0 ? p[c] / a * 255.f + 0.5f : 0.f);
            row[x * 4 + 3] = (uint8_t)std::min(255.f, a * 255.f + 0.5f);
        }
    }
}

bool PortraitRgba(const uint8_t* ci, size_t size, std::vector<uint8_t>& rgba, int& n, const FacePaint* paint, bool paintFront) {
    if (!ci || size < (size_t)kMinSize || !Available()) return false;
    mutexLock(&sArtMutex);
    const int N = sPack.size;
    Canvas cv(N, N);
    Compose(ci, paint, paintFront, cv);
    mutexUnlock(&sArtMutex);
    n = N;
    rgba.resize((size_t)N * N * 4);
    ToBytes(cv, rgba.data(), N * 4);
    return true;
}

SDL_Texture* Portrait(const uint8_t* ci, size_t size, const FacePaint* paint, bool paintFront) {
    if (!ci || size < (size_t)kMinSize || !Available() || !gRen) return nullptr;
    const uint64_t key = Key(ci, size) ^ (paint ? paint->key * 0x9E3779B97F4A7C15ull : 0) ^ (paintFront ? 0x5bd1e995ull : 0);
    auto it = sCache.find(key);
    if (it != sCache.end()) return it->second;
    // A few new pictures a frame (a list of seventy would stall): the others
    // keep their drawn face until their turn.
    static double sFrame = -1;
    static int sBuilt = 0;
    if (Motion::Now() != sFrame) { sFrame = Motion::Now(); sBuilt = 0; }
    if (sBuilt >= 2) return nullptr;
    sBuilt++;
    if (sCache.size() > 160) { for (auto& kv : sCache) if (kv.second) SDL_DestroyTexture(kv.second); sCache.clear(); }
    mutexLock(&sArtMutex);
    const int N = sPack.size;
    Canvas cv(N, N);
    Compose(ci, paint, paintFront, cv);
    mutexUnlock(&sArtMutex);
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, N, N, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!s) return nullptr;
    ToBytes(cv, (uint8_t*)s->pixels, s->pitch);
    SDL_Texture* t = SDL_CreateTextureFromSurface(gRen, s);
    SDL_FreeSurface(s);
    if (t) { SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear); }
    sCache[key] = t;
    return t;
}

void Purge() {
    for (auto& kv : sCache) if (kv.second) SDL_DestroyTexture(kv.second);
    sCache.clear();
    mutexLock(&sArtMutex);
    sPack.pics.clear();
    for (auto& kv : sPaints) kv.second.checked = -1e9;     // looked at again on next use
    mutexUnlock(&sArtMutex);
}

void Reload() {
    for (auto& kv : sCache) if (kv.second) SDL_DestroyTexture(kv.second);
    sCache.clear();
    mutexLock(&sArtMutex);
    sPack = Pack();
    for (auto& kv : sPaints) kv.second.checked = -1e9;
    mutexUnlock(&sArtMutex);
}

}  // namespace MiiArt
