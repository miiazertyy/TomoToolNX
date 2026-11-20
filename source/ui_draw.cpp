// ui_draw.cpp — TomoToolNX
// Theme, fonts, text, shapes and motion for the on-console UI. See ui_draw.h.

#include "ui_draw.h"
#include "i18n.h"
#include "font_data.h"

#include <switch.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

SDL_Renderer* gRen = nullptr;

// ─── Colour helpers ───────────────────────────────────────────────────────────
static SDL_Color Hex(uint32_t rgb) {
    return { (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, 255 };
}

SDL_Color Mix(SDL_Color a, SDL_Color b, float t) {
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    return { (uint8_t)(a.r + (b.r - a.r) * t + 0.5f),
             (uint8_t)(a.g + (b.g - a.g) * t + 0.5f),
             (uint8_t)(a.b + (b.b - a.b) * t + 0.5f), 255 };
}

bool HexToColor(const std::string& v, SDL_Color& out) {
    if (v.size() != 6) return false;
    auto hx = [&](int o) -> int {
        int val = 0;
        for (int i = 0; i < 2; i++) {
            char c = v[o + i]; val <<= 4;
            if      (c >= '0' && c <= '9') val |= c - '0';
            else if (c >= 'a' && c <= 'f') val |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') val |= c - 'A' + 10;
            else return -1;
        }
        return val;
    };
    int r = hx(0), g = hx(2), b = hx(4);
    if (r < 0 || g < 0 || b < 0) return false;
    out = { (uint8_t)r, (uint8_t)g, (uint8_t)b, 255 };
    return true;
}

SDL_Color HSVtoRGB(float h, float s, float v) {
    h = fmodf(h, 360.f); if (h < 0) h += 360.f;
    float c = v * s, x = c * (1.f - fabsf(fmodf(h / 60.f, 2.f) - 1.f)), m = v - c;
    float r = 0, g = 0, b = 0;
    if      (h <  60) { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else              { r = c; b = x; }
    return { ClampU8((int)((r + m) * 255 + 0.5f)), ClampU8((int)((g + m) * 255 + 0.5f)),
             ClampU8((int)((b + m) * 255 + 0.5f)), 255 };
}

void RGBtoHSV(SDL_Color col, float& h, float& s, float& v) {
    float r = col.r / 255.f, g = col.g / 255.f, b = col.b / 255.f;
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    v = mx; s = (mx <= 0.f) ? 0.f : d / mx;
    if (d <= 0.f) { h = 0.f; return; }
    if      (mx == r) h = 60.f * fmodf((g - b) / d, 6.f);
    else if (mx == g) h = 60.f * ((b - r) / d + 2.f);
    else              h = 60.f * ((r - g) / d + 4.f);
    if (h < 0) h += 360.f;
}

// ─── Themes ───────────────────────────────────────────────────────────────────
// The palettes are the ones of the LLM-Selfbot panel (webui/src/lib/themes.ts):
// a depth ramp of page / card / edge, three inks, two accents and three signal
// colours. "Classic" keeps the gold-on-black of TomoToolNX 1.x.
namespace {
struct Palette {
    const char* code;
    const char* labelKey;
    uint32_t bg, card, edge, ink, muted, faint, accent, accent2, good, bad, warn;
};
const Palette kPalettes[] = {
    { "classic",  "theme.classic",  0x06070c, 0x14161e, 0x30333f, 0xe4e1d7, 0x82828c, 0x5a5a66, 0xdec678, 0x78aac8, 0x82c396, 0xcd6469, 0xe6b45a },
    { "midnight", "theme.midnight", 0x0a0a11, 0x16161f, 0x23232f, 0xececf5, 0x8e8ea8, 0x5c5c74, 0x7c8cff, 0x4ecdc4, 0x4ade80, 0xf87171, 0xfbbf24 },
    { "pastel",   "theme.pastel",   0x16141c, 0x252131, 0x332d42, 0xece7f5, 0xa396bb, 0x6f6489, 0xc3a6ff, 0x9ee6d4, 0xa8e6a3, 0xffa8b4, 0xffd9a0 },
    { "moss",     "theme.moss",     0x0f1412, 0x1b2620, 0x28352d, 0xe6f0e9, 0x93ab9d, 0x5f7568, 0x7fd7a4, 0xa8d8ff, 0x8fe3a8, 0xff9f9f, 0xffd48a },
    { "ember",    "theme.ember",    0x141010, 0x241c1a, 0x372a27, 0xf4eae6, 0xb39a92, 0x7a655e, 0xffab7a, 0xffd9a0, 0xa8dda0, 0xff9a8f, 0xffc978 },
    { "sakura",   "theme.sakura",   0x141018, 0x241d2c, 0x372c42, 0xf3e9f2, 0xb298b6, 0x7d6683, 0xff9ec4, 0xc8a6ff, 0x9fe0c0, 0xff8fa8, 0xffcf94 },
    { "slate",    "theme.slate",    0x111214, 0x202327, 0x2e3237, 0xe8eaed, 0x9aa1a9, 0x666e77, 0x9ab6d9, 0x8fd4d0, 0x84cc9a, 0xe28d8d, 0xdcb87a },
};
const int kPaletteCount = (int)(sizeof(kPalettes) / sizeof(kPalettes[0]));

Theme BuildTheme(const Palette& p) {
    Theme t{};
    const SDL_Color card = Hex(p.card), accent = Hex(p.accent);
    t.bg     = Hex(p.bg);
    t.panel  = card;
    t.panel2 = Mix(card, Hex(p.ink), 0.055f);
    t.sel    = Mix(card, accent, 0.22f);
    t.border = Mix(Hex(p.edge), Hex(p.ink), 0.06f);
    t.text   = Hex(p.ink);
    t.dim    = Hex(p.muted);
    t.faint  = Hex(p.faint);
    t.gold   = accent;
    t.accent = Hex(p.accent2);
    t.green  = Hex(p.good);
    t.red    = Hex(p.bad);
    t.warn   = Hex(p.warn);
    t.glow   = { accent.r, accent.g, accent.b, 130 };
    t.well   = Mix(card, SDL_Color{0, 0, 0, 255}, 0.34f);
    t.cornerRadius = 10;
    t.shadow = true;
    return t;
}

const Palette& PaletteFor(const std::string& code) {
    for (const auto& p : kPalettes) if (code == p.code) return p;
    return kPalettes[0];
}

std::string gThemeCode = "classic";
}  // namespace

Theme gTheme = BuildTheme(kPalettes[0]);

namespace ThemeNS {
std::vector<SDL_Color> Swatches(const std::string& code) {
    const Palette& p = PaletteFor(code);
    return { Hex(p.accent), Hex(p.accent2), Hex(p.card) };
}
const std::vector<Entry>& Available() {
    static const std::vector<Entry> list = [] {
        std::vector<Entry> v;
        for (const auto& p : kPalettes) v.push_back({ p.code, p.labelKey });
        return v;
    }();
    return list;
}
const std::string& Current() { return gThemeCode; }
void Apply(const std::string& code) { gTheme = BuildTheme(PaletteFor(code)); }
void SetCurrent(const std::string& code) {
    // 1.x had two themes. Both were the gold look, with and without rounding.
    std::string want = (code == "switch" || code == "original") ? "classic" : code;
    gThemeCode = PaletteFor(want).code;
    Apply(gThemeCode);
}
}  // namespace ThemeNS

bool      gAccentCustom = false;
SDL_Color gAccentColor  = { 124, 140, 255, 255 };
bool      gBgCustom     = false;
SDL_Color gBgColor      = { 10, 10, 17, 255 };

// Rebuild gTheme from the chosen theme plus the user's own accent / background.
// Always starts from the base theme, so repeated edits never compound.
void ApplyCustomColors() {
    ThemeNS::Apply(ThemeNS::Current());
    if (gBgCustom) {
        const bool dark = ColorLuma(gBgColor) < 128;
        const int  s    = dark ? 1 : -1;   // lighten cards on a dark page, darken on a light one
        gTheme.bg     = gBgColor;
        gTheme.panel  = ShadeBy(gBgColor, s * 14);
        gTheme.panel2 = ShadeBy(gBgColor, s * 24);
        gTheme.border = ShadeBy(gBgColor, s * 40);
        gTheme.well   = ShadeBy(gBgColor, dark ? -4 : -18);
        gTheme.text   = dark ? SDL_Color{236, 236, 245, 255} : SDL_Color{24, 24, 28, 255};
        gTheme.dim    = dark ? SDL_Color{146, 146, 166, 255} : SDL_Color{86, 86, 100, 255};
        gTheme.faint  = dark ? SDL_Color{98, 98, 118, 255}   : SDL_Color{130, 130, 144, 255};
        gTheme.sel    = Mix(gTheme.panel, gTheme.gold, 0.22f);
    }
    if (gAccentCustom) {
        gTheme.gold = gAccentColor;
        gTheme.glow = { gAccentColor.r, gAccentColor.g, gAccentColor.b, 130 };
        gTheme.sel  = Mix(gTheme.panel, gAccentColor, 0.22f);
    }
}

// ─── Fonts ────────────────────────────────────────────────────────────────────
// Mikhak in three weights. Regular is compiled in (font_data.h); Medium and
// SemiBold are read from romfs:/fonts/ once and kept in memory. If either file
// is missing the regular face stands in, so text never disappears.
namespace {
TTF_Font* sFonts[kFontCount] = {};   // indexed by Font
std::vector<uint8_t> sMedium, sSemiBold;
bool sFacesLoaded = false;

bool ReadAll(const char* path, std::vector<uint8_t>& out) {
    out.clear();
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return false; }
    out.resize((size_t)len);
    size_t n = fread(out.data(), 1, out.size(), f);
    fclose(f);
    if (n != out.size()) { out.clear(); return false; }
    return true;
}

struct Face { const void* data; size_t size; };

TTF_Font* OpenFace(Face face, int px) {
    if (!face.data || face.size == 0) return nullptr;
    SDL_RWops* rw = SDL_RWFromConstMem(face.data, (int)face.size);
    if (!rw) return nullptr;
    return TTF_OpenFontRW(rw, 1, px);   // 1 = the font owns and frees the RWops
}

// One of the console's own fonts.
bool SharedFaceOf(PlSharedFontType type, Face& out) {
    static bool plReady = false, plFailed = false;
    if (plFailed) return false;
    if (!plReady) {
        if (R_FAILED(plInitialize(PlServiceType_User))) { plFailed = true; return false; }
        plReady = true;
    }
    PlFontData pf{};
    if (R_FAILED(plGetSharedFontByType(&pf, type)) || !pf.address || !pf.size) return false;
    out = { pf.address, pf.size };
    return true;
}

// The console's own font for an interface in a script Mikhak does not cover.
bool SharedFace(const std::string& lang, Face& out) {
    if (lang == "zh") return SharedFaceOf(PlSharedFontType_ChineseSimplified, out);
    if (lang == "ja" || lang == "ru") return SharedFaceOf(PlSharedFontType_Standard, out);
    return false;
}

// Fallbacks: for a character the interface's font does not have (a Russian,
// Japanese or Chinese name in an English interface, the languages' own names
// in the language list), the console's fonts, in this order. Each is opened
// at the size of the Font that needs it, the first time it does.
const PlSharedFontType kFallbackTypes[] = { PlSharedFontType_Standard, PlSharedFontType_ChineseSimplified, PlSharedFontType_KO };
constexpr int kFallbacks = 3;
TTF_Font* sFall[kFontCount][kFallbacks] = {};
bool sFallTried[kFontCount][kFallbacks] = {};
int sPx[kFontCount] = {};                  // the size each Font was opened at
const void* sFaceOf[kFontCount] = {};      // and the face it uses

TTF_Font* Fallback(int i, int k) {
    if (sFallTried[i][k]) return sFall[i][k];
    sFallTried[i][k] = true;
    Face face{};
    if (!SharedFaceOf(kFallbackTypes[k], face) || face.data == sFaceOf[i]) return nullptr;
    sFall[i][k] = OpenFace(face, sPx[i]);
    return sFall[i][k];
}

void CloseFonts() {
    for (auto& f : sFonts) if (f) { TTF_CloseFont(f); f = nullptr; }
    for (int i = 0; i < kFontCount; i++)
        for (int k = 0; k < kFallbacks; k++) {
            if (sFall[i][k]) TTF_CloseFont(sFall[i][k]);
            sFall[i][k] = nullptr;
            sFallTried[i][k] = false;
        }
}
}  // namespace

void FontReload(const std::string& langCode) {
    // Cached textures were rasterised with the previous faces.
    TextCacheFlush();
    CloseFonts();

    if (!sFacesLoaded) {
        sFacesLoaded = true;
        ReadAll("romfs:/fonts/Mikhak-Medium.ttf",   sMedium);
        ReadAll("romfs:/fonts/Mikhak-SemiBold.ttf", sSemiBold);
    }
    const Face regular  = { FONT_DATA, FONT_DATA_SIZE };
    Face medium   = sMedium.empty()   ? regular : Face{ sMedium.data(),   sMedium.size()   };
    Face semibold = sSemiBold.empty() ? medium  : Face{ sSemiBold.data(), sSemiBold.size() };
    Face body     = regular;

    //                   Sm  Md  Lg  Xs  Xl  Body Row Title Hero
    int px[kFontCount] = { 16, 22, 32, 13, 46, 17,  19,  26,  60 };
    Face shared{};
    if (SharedFace(langCode, shared)) {
        body = medium = semibold = shared;
        // The shared Standard font is wider than Mikhak and Russian runs long.
        if (langCode == "ru") for (int& v : px) v = v * 7 / 8;
    }
    auto open = [](Font f, Face face, int size) {
        sFonts[(int)f] = OpenFace(face, size);
        sPx[(int)f] = size;
        sFaceOf[(int)f] = face.data;
    };
    open(Font::Xs,    medium,   px[3]);
    open(Font::Sm,    body,     px[0]);
    open(Font::Md,    medium,   px[1]);
    open(Font::Lg,    semibold, px[2]);
    open(Font::Xl,    semibold, px[4]);
    open(Font::Body,  body,     px[5]);
    open(Font::Row,   medium,   px[6]);
    open(Font::Title, semibold, px[7]);
    open(Font::Hero,  semibold, px[8]);
}

void FontInit() {
    if (!TTF_WasInit()) TTF_Init();
    FontReload(Lang::Current());
}

void FontQuit() {
    TextCacheFlush();
    CloseFonts();
    TTF_Quit();
}

TTF_Font* GetFont(Font f) {
    int i = (int)f;
    if (i < 0 || i >= kFontCount) i = 0;
    return sFonts[i] ? sFonts[i] : sFonts[(int)Font::Sm];
}

namespace {
uint32_t NextCodepoint(const std::string& s, size_t& i) {
    const unsigned char b = (unsigned char)s[i];
    int n = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 1;
    if (i + n > s.size()) n = 1;
    uint32_t cp = n == 1 ? b : n == 2 ? (b & 0x1Fu) : n == 3 ? (b & 0x0Fu) : (b & 0x07u);
    for (int k = 1; k < n; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3Fu);
    i += n;
    return cp;
}

// A piece of text and the font that draws it.
struct Run { TTF_Font* font; std::string text; };

// The text in runs: the Font's own face wherever it has the character, else
// the first of the console's fonts that does. False (and no runs) when the
// Font's face has every character, the usual case.
bool SplitRuns(const std::string& text, Font f, std::vector<Run>& runs) {
    runs.clear();
    TTF_Font* prim = GetFont(f);
    if (!prim) return false;
    bool all = true;
    for (size_t i = 0; i < text.size();) {
        if ((unsigned char)text[i] < 0x80) { i++; continue; }
        const uint32_t cp = NextCodepoint(text, i);
        if (!TTF_GlyphIsProvided32(prim, cp)) { all = false; break; }
    }
    if (all) return false;
    int fi = (int)f;
    if (fi < 0 || fi >= kFontCount || !sFonts[fi]) fi = (int)Font::Sm;
    for (size_t i = 0; i < text.size();) {
        const size_t start = i;
        const uint32_t cp = NextCodepoint(text, i);
        TTF_Font* use = prim;
        if (cp >= 0x80 && !TTF_GlyphIsProvided32(prim, cp))
            for (int k = 0; k < kFallbacks; k++)
                if (TTF_Font* fb = Fallback(fi, k)) if (TTF_GlyphIsProvided32(fb, cp)) { use = fb; break; }
        // A space or a mark between two pieces of the same fallback stays in it.
        if (use == prim && cp < 0x80 && !runs.empty() && runs.back().font != prim && (cp == ' ' || ispunct((int)cp)) &&
            TTF_GlyphIsProvided32(runs.back().font, cp))
            use = runs.back().font;
        if (runs.empty() || runs.back().font != use) runs.push_back({ use, std::string() });
        runs.back().text.append(text, start, i - start);
    }
    return true;
}

// The runs drawn side by side on one line, their baselines on the Font's.
SDL_Surface* RenderRuns(const std::vector<Run>& runs, Font f) {
    TTF_Font* prim = GetFont(f);
    const int H = TTF_FontHeight(prim), A = TTF_FontAscent(prim);
    std::vector<SDL_Surface*> parts;
    int W = 0;
    for (const Run& r : runs) {
        SDL_Surface* s = TTF_RenderUTF8_Blended(r.font, r.text.c_str(), SDL_Color{ 255, 255, 255, 255 });
        parts.push_back(s);
        if (s) W += s->w;
    }
    SDL_Surface* out = SDL_CreateRGBSurfaceWithFormat(0, std::max(1, W), std::max(1, H), 32, SDL_PIXELFORMAT_ARGB8888);
    int x = 0;
    for (size_t k = 0; k < parts.size(); k++) {
        SDL_Surface* s = parts[k];
        if (!s) continue;
        if (out) {
            SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_NONE);
            SDL_Rect dst{ x, A - TTF_FontAscent(runs[k].font), s->w, s->h };
            SDL_BlitSurface(s, nullptr, out, &dst);
        }
        x += s->w;
        SDL_FreeSurface(s);
    }
    return out;
}
}  // namespace

// ─── Text cache ───────────────────────────────────────────────────────────────
// Every distinct (text, font, colour) is rasterised once and kept as a texture;
// a repeat draw is a single RenderCopy. Least-recently-used entries are dropped
// once the cache is full.
Uint32 gTextCacheFrame = 0;
namespace {
struct TextCacheEntry { SDL_Texture* tex; int w, h; Uint32 lastUsed; };
std::unordered_map<std::string, TextCacheEntry> sTextCache;
const size_t kTextCacheMax = 768;
void sWidthCacheClear();
}  // namespace

void TextCacheFlush() {
    for (auto& kv : sTextCache) if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    sTextCache.clear();
    sWidthCacheClear();
}

SDL_Texture* GetTextTexture(const std::string& text, Font f, SDL_Color col, int& outW, int& outH) {
    // Rasterised once in white per (font, text) and tinted per draw (colour
    // and alpha mod), so fades and recolours never rasterise again.
    char prefix[8];
    int n = snprintf(prefix, sizeof(prefix), "%d|", (int)f);
    std::string key; key.reserve((size_t)n + text.size());
    key.append(prefix, (size_t)n).append(text);

    SDL_Texture* tex = nullptr;
    auto it = sTextCache.find(key);
    if (it != sTextCache.end()) {
        it->second.lastUsed = gTextCacheFrame;
        outW = it->second.w; outH = it->second.h;
        tex = it->second.tex;
    } else {
        TTF_Font* font = GetFont(f);
        if (!font || !gRen) return nullptr;
        std::vector<Run> runs;
        SDL_Surface* surf = SplitRuns(text, f, runs) ? RenderRuns(runs, f)
                                                     : TTF_RenderUTF8_Blended(font, text.c_str(), SDL_Color{ 255, 255, 255, 255 });
        if (!surf) return nullptr;
        tex = SDL_CreateTextureFromSurface(gRen, surf);
        const int w = surf->w, h = surf->h;
        SDL_FreeSurface(surf);
        if (!tex) return nullptr;
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        if (sTextCache.size() >= kTextCacheMax) {
            auto oldest = sTextCache.begin();
            for (auto cit = sTextCache.begin(); cit != sTextCache.end(); ++cit)
                if (cit->second.lastUsed < oldest->second.lastUsed) oldest = cit;
            if (oldest->second.tex) SDL_DestroyTexture(oldest->second.tex);
            sTextCache.erase(oldest);
        }
        sTextCache.emplace(std::move(key), TextCacheEntry{ tex, w, h, gTextCacheFrame });
        outW = w; outH = h;
    }
    SDL_SetTextureColorMod(tex, col.r, col.g, col.b);
    SDL_SetTextureAlphaMod(tex, col.a);
    return tex;
}

namespace {
std::unordered_map<std::string, int> sWidthCache;
void sWidthCacheClear() { sWidthCache.clear(); }
}

int TextW(const std::string& text, Font f) {
    if (text.empty()) return 0;
    char prefix[8];
    int n = snprintf(prefix, sizeof(prefix), "%d|", (int)f);
    std::string key; key.reserve((size_t)n + text.size());
    key.append(prefix, (size_t)n).append(text);
    auto it = sWidthCache.find(key);
    if (it != sWidthCache.end()) return it->second;
    int w = 0, h = 0;
    std::vector<Run> runs;
    if (SplitRuns(text, f, runs)) {
        for (const Run& r : runs) { int rw = 0, rh = 0; TTF_SizeUTF8(r.font, r.text.c_str(), &rw, &rh); w += rw; }
    } else if (TTF_Font* ft = GetFont(f)) {
        TTF_SizeUTF8(ft, text.c_str(), &w, &h);
    }
    if (sWidthCache.size() > 6000) sWidthCache.clear();
    sWidthCache.emplace(std::move(key), w);
    return w;
}

int TextH(Font f) {
    TTF_Font* ft = GetFont(f);
    return ft ? TTF_FontHeight(ft) : 0;
}

std::string FitText(const std::string& text, Font f, int maxW) {
    if (maxW <= 0) return std::string();
    if (TextW(text, f) <= maxW) return text;
    static const char* ELL = "\xE2\x80\xA6";   // U+2026
    // Character boundaries, then a binary search for the longest prefix that
    // fits with the ellipsis.
    std::vector<size_t> cuts;
    for (size_t i = 1; i < text.size(); i++)
        if (((unsigned char)text[i] & 0xC0) != 0x80) cuts.push_back(i);
    int lo = 0, hi = (int)cuts.size() - 1, best = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        std::string t = text.substr(0, cuts[mid]);
        while (!t.empty() && t.back() == ' ') t.pop_back();
        if (TextW(t + ELL, f) <= maxW) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (best < 0) return ELL;
    std::string t = text.substr(0, cuts[best]);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t + ELL;
}

std::string UpperUtf8(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'a' && c <= 'z') { out += (char)(c - 32); continue; }
        if (i + 1 < s.size()) {
            unsigned char d = (unsigned char)s[i + 1];
            if (c == 0xC3 && d >= 0xA0 && d <= 0xBE && d != 0xB7) {      // à..þ
                out += (char)c; out += (char)(d - 0x20); i++; continue;
            }
            if (c == 0xD0 && d >= 0xB0 && d <= 0xBF) {                    // а..п
                out += (char)0xD0; out += (char)(d - 0x20); i++; continue;
            }
            if (c == 0xD1 && d >= 0x80 && d <= 0x8F) {                    // р..я
                out += (char)0xD0; out += (char)(d + 0x20); i++; continue;
            }
        }
        out += (char)c;
    }
    return out;
}

std::string CapFirstUtf8(const std::string& s) {
    if (s.empty()) return s;
    const unsigned char c = (unsigned char)s[0];
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
    if (n > s.size()) return s;
    return UpperUtf8(s.substr(0, n)) + s.substr(n);
}

void DrawText(const std::string& text, int x, int y, SDL_Color col, Font f) {
    if (text.empty() || col.a == 0) return;
    int w = 0, h = 0;
    SDL_Texture* tex = GetTextTexture(text, f, col, w, h);
    if (!tex) return;
    SDL_Rect dst{ x, y, w, h };
    if (!XformIdentity()) { dst = XformRect(dst.x, dst.y, dst.w, dst.h); SDL_SetTextureAlphaMod(tex, XformAlpha(col.a)); }
    SDL_RenderCopy(gRen, tex, nullptr, &dst);
}

void DrawTextVC(const std::string& text, int x, int cy, SDL_Color col, Font f) {
    if (text.empty() || col.a == 0) return;
    int w = 0, h = 0;
    SDL_Texture* tex = GetTextTexture(text, f, col, w, h);
    if (!tex) return;
    SDL_Rect dst{ x, cy - h / 2, w, h };
    if (!XformIdentity()) { dst = XformRect(dst.x, dst.y, dst.w, dst.h); SDL_SetTextureAlphaMod(tex, XformAlpha(col.a)); }
    SDL_RenderCopy(gRen, tex, nullptr, &dst);
}

void DrawTextC(const std::string& text, int cx, int cy, SDL_Color col, Font f) {
    if (text.empty() || col.a == 0) return;
    int w = 0, h = 0;
    SDL_Texture* tex = GetTextTexture(text, f, col, w, h);
    if (!tex) return;
    SDL_Rect dst{ cx - w / 2, cy - h / 2, w, h };
    if (!XformIdentity()) { dst = XformRect(dst.x, dst.y, dst.w, dst.h); SDL_SetTextureAlphaMod(tex, XformAlpha(col.a)); }
    SDL_RenderCopy(gRen, tex, nullptr, &dst);
}

void DrawTextInkC(const std::string& text, int cx, int cy, SDL_Color col, Font f) {
    if (text.empty() || col.a == 0) return;
    TTF_Font* font = GetFont(f);
    // The glyphs' boxes along the pen, measured from the baseline (up is +).
    int minX = 0, maxX = 0, minY = 0, maxY = 0, pen = 0;
    bool any = false, ok = font != nullptr;
    for (size_t i = 0; ok && i < text.size();) {
        const unsigned char b = (unsigned char)text[i];
        const size_t len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : 4;
        uint32_t cp = len == 1 ? b : len == 2 ? (b & 0x1F) : len == 3 ? (b & 0x0F) : (b & 0x07);
        for (size_t k = 1; k < len && i + k < text.size(); k++) cp = (cp << 6) | ((unsigned char)text[i + k] & 0x3F);
        i += len;
        int gx0 = 0, gx1 = 0, gy0 = 0, gy1 = 0, adv = 0;
        if (!TTF_GlyphIsProvided32(font, cp) || TTF_GlyphMetrics32(font, cp, &gx0, &gx1, &gy0, &gy1, &adv) != 0) { ok = false; break; }
        if (gx1 > gx0 && gy1 > gy0) {
            if (!any) { minX = pen + gx0; maxX = pen + gx1; minY = gy0; maxY = gy1; any = true; }
            else { minX = std::min(minX, pen + gx0); maxX = std::max(maxX, pen + gx1); minY = std::min(minY, gy0); maxY = std::max(maxY, gy1); }
        }
        pen += adv;
    }
    if (!ok || !any) { DrawTextC(text, cx, cy, col, f); return; }
    int w = 0, h = 0;
    SDL_Texture* tex = GetTextTexture(text, f, col, w, h);
    if (!tex) return;
    // In the picture of the text the baseline is the font's ascent down.
    const float inkX = (minX + maxX) / 2.f, inkY = TTF_FontAscent(font) - (minY + maxY) / 2.f;
    SDL_Rect dst{ (int)lroundf(cx - inkX), (int)lroundf(cy - inkY), w, h };
    if (!XformIdentity()) { dst = XformRect(dst.x, dst.y, dst.w, dst.h); SDL_SetTextureAlphaMod(tex, XformAlpha(col.a)); }
    SDL_RenderCopy(gRen, tex, nullptr, &dst);
}

void DrawTextR(const std::string& text, int rx, int cy, SDL_Color col, Font f) {
    if (text.empty() || col.a == 0) return;
    int w = 0, h = 0;
    SDL_Texture* tex = GetTextTexture(text, f, col, w, h);
    if (!tex) return;
    SDL_Rect dst{ rx - w, cy - h / 2, w, h };
    if (!XformIdentity()) { dst = XformRect(dst.x, dst.y, dst.w, dst.h); SDL_SetTextureAlphaMod(tex, XformAlpha(col.a)); }
    SDL_RenderCopy(gRen, tex, nullptr, &dst);
}

// ─── Shapes ───────────────────────────────────────────────────────────────────
// The renderer stays in BLEND mode for good. An opaque colour blends to itself,
// so nothing has to switch modes back and forth, and a translucent colour
// passed to any of these does what it says.
static inline void SetColor(SDL_Color c) {
    SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(gRen, c.r, c.g, c.b, c.a);
}

// ─── Transform ────────────────────────────────────────────────────────────────
// Kept as p' = k·p + t (uniform scale) with an alpha multiplier; pushes compose.
namespace {
struct XfState { float k = 1, tx = 0, ty = 0, a = 1; };
XfState sXf;
std::vector<XfState> sXfStack;
}
void PushXform(float dx, float dy, float scale, float cx, float cy, float alpha) {
    sXfStack.push_back(sXf);
    // new: q = s·p + (c·(1-s) + d); composed with the current one
    const float nt_x = cx * (1 - scale) + dx, nt_y = cy * (1 - scale) + dy;
    XfState n;
    n.k  = sXf.k * scale;
    n.tx = sXf.k * nt_x + sXf.tx;
    n.ty = sXf.k * nt_y + sXf.ty;
    n.a  = sXf.a * (alpha < 0 ? 0 : (alpha > 1 ? 1 : alpha));
    sXf = n;
}
void PopXform() { if (!sXfStack.empty()) { sXf = sXfStack.back(); sXfStack.pop_back(); } }
void PushXformIdentity() { sXfStack.push_back(sXf); sXf = XfState(); }
bool XformIdentity() { return sXf.k == 1.f && sXf.tx == 0.f && sXf.ty == 0.f && sXf.a == 1.f; }
float XformScale() { return sXf.k; }
void XformPoint(float& x, float& y) { x = sXf.k * x + sXf.tx; y = sXf.k * y + sXf.ty; }
SDL_Rect XformRect(int x, int y, int w, int h) {
    if (sXf.k == 1.f && sXf.tx == 0.f && sXf.ty == 0.f) return SDL_Rect{ x, y, w, h };
    const float x0 = sXf.k * x + sXf.tx, y0 = sXf.k * y + sXf.ty;
    const float x1 = sXf.k * (x + w) + sXf.tx, y1 = sXf.k * (y + h) + sXf.ty;
    const int ix = (int)lroundf(x0), iy = (int)lroundf(y0);
    return SDL_Rect{ ix, iy, (int)lroundf(x1) - ix, (int)lroundf(y1) - iy };
}
uint8_t XformAlpha(int a) {
    const int v = (int)lroundf((float)a * sXf.a);
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}
void DrawTexture(SDL_Texture* t, const SDL_Rect* src, SDL_Rect dst, uint8_t alpha) {
    if (!t) return;
    dst = XformRect(dst.x, dst.y, dst.w, dst.h);
    // Opaque pictures (JPEG avatars) come without blending, and would ignore
    // the fade of the block they are in.
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(t, XformAlpha(alpha));
    SDL_RenderCopy(gRen, t, src, &dst);
}

void DrawTextureFlipped(SDL_Texture* t, const SDL_Rect* src, SDL_Rect dst, uint8_t alpha, bool flipX, bool flipY) {
    if (!t) return;
    dst = XformRect(dst.x, dst.y, dst.w, dst.h);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(t, XformAlpha(alpha));
    const int flip = (flipX ? SDL_FLIP_HORIZONTAL : 0) | (flipY ? SDL_FLIP_VERTICAL : 0);
    SDL_RenderCopyEx(gRen, t, src, &dst, 0.0, nullptr, (SDL_RendererFlip)flip);
}

void FillRectSquare(int x, int y, int w, int h, SDL_Color c) {
    if (w <= 0 || h <= 0 || c.a == 0) return;
    c.a = XformAlpha(c.a);
    SetColor(c);
    SDL_Rect r = XformRect(x, y, w, h);
    SDL_RenderFillRect(gRen, &r);
}

void DrawRectSquare(int x, int y, int w, int h, SDL_Color c) {
    if (w <= 0 || h <= 0 || c.a == 0) return;
    c.a = XformAlpha(c.a);
    SetColor(c);
    SDL_Rect r = XformRect(x, y, w, h);
    SDL_RenderDrawRect(gRen, &r);
}

// Anti-aliased corners: one white quarter-disc per radius, baked with 4x4
// supersampling, tinted and flipped into the four corners.
namespace {
struct CornerTex { SDL_Texture* fill = nullptr; SDL_Texture* ring = nullptr; SDL_Texture* mask = nullptr; };
std::map<int, CornerTex> sCornerCache;

SDL_Texture* BuildCornerTexture(int r, bool ring, bool invert = false) {
    std::vector<uint32_t> px((size_t)r * r, 0);
    const int SS = 4;
    for (int y = 0; y < r; y++) {
        for (int x = 0; x < r; x++) {
            int inside = 0;
            for (int sy = 0; sy < SS; sy++)
                for (int sx = 0; sx < SS; sx++) {
                    float fx = (float)x + (sx + 0.5f) / SS;
                    float fy = (float)y + (sy + 0.5f) / SS;
                    float dx = fx - (float)r, dy = fy - (float)r;
                    float d  = sqrtf(dx * dx + dy * dy);
                    bool in = ring ? (d <= (float)r && d >= (float)r - 1.0f) : (d <= (float)r);
                    if (in) inside++;
                }
            uint8_t a = (uint8_t)(255 * inside / (SS * SS));
            if (invert) a = (uint8_t)(255 - a);                              // outside the arc
            px[(size_t)y * r + x] = ((uint32_t)a << 24) | 0x00FFFFFFu;   // ABGR8888, white
        }
    }
    SDL_Texture* t = SDL_CreateTexture(gRen, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, r, r);
    if (t) {
        SDL_UpdateTexture(t, nullptr, px.data(), r * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }
    return t;
}

const CornerTex& GetCornerTex(int r) {
    auto it = sCornerCache.find(r);
    if (it != sCornerCache.end()) return it->second;
    CornerTex ct;
    ct.fill = BuildCornerTexture(r, false);
    ct.ring = BuildCornerTexture(r, true);
    ct.mask = BuildCornerTexture(r, false, true);
    return sCornerCache.emplace(r, ct).first->second;
}

void BlitCorners(SDL_Texture* tile, int x, int y, int w, int h, int r, SDL_Color c) {
    if (!tile) return;
    SDL_SetTextureColorMod(tile, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(tile, c.a);
    SDL_Rect d;
    d = { x,         y,         r, r }; SDL_RenderCopyEx(gRen, tile, nullptr, &d, 0, nullptr, SDL_FLIP_NONE);
    d = { x + w - r, y,         r, r }; SDL_RenderCopyEx(gRen, tile, nullptr, &d, 0, nullptr, SDL_FLIP_HORIZONTAL);
    d = { x,         y + h - r, r, r }; SDL_RenderCopyEx(gRen, tile, nullptr, &d, 0, nullptr, SDL_FLIP_VERTICAL);
    d = { x + w - r, y + h - r, r, r }; SDL_RenderCopyEx(gRen, tile, nullptr, &d, 0, nullptr,
                                                         (SDL_RendererFlip)(SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL));
}

inline int ClampRadius(int r, int w, int h) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r > 96) r = 96;          // keeps the corner cache small
    return r;
}
}  // namespace

void FillRectRounded(int x, int y, int w, int h, int r, SDL_Color c) {
    if (w <= 0 || h <= 0 || c.a == 0) return;
    if (!XformIdentity()) {
        const SDL_Rect t = XformRect(x, y, w, h);
        r = (int)lroundf(r * XformScale());
        x = t.x; y = t.y; w = t.w; h = t.h;
        c.a = XformAlpha(c.a);
        if (w <= 0 || h <= 0 || c.a == 0) return;
    }
    r = ClampRadius(r, w, h);
    if (r <= 0) { SetColor(c); SDL_Rect q{ x, y, w, h }; SDL_RenderFillRect(gRen, &q); return; }
    SetColor(c);
    // Three bands that do not overlap (a translucent colour must not be laid
    // down twice), then the four corner tiles.
    if (h - 2 * r > 0) { SDL_Rect mid{ x, y + r, w, h - 2 * r }; SDL_RenderFillRect(gRen, &mid); }
    if (w - 2 * r > 0) {
        SDL_Rect top{ x + r, y,         w - 2 * r, r }; SDL_RenderFillRect(gRen, &top);
        SDL_Rect bot{ x + r, y + h - r, w - 2 * r, r }; SDL_RenderFillRect(gRen, &bot);
    }
    BlitCorners(GetCornerTex(r).fill, x, y, w, h, r, c);
}

void DrawRectRounded(int x, int y, int w, int h, int r, SDL_Color c) {
    if (w <= 0 || h <= 0 || c.a == 0) return;
    if (!XformIdentity()) {
        const SDL_Rect t = XformRect(x, y, w, h);
        r = (int)lroundf(r * XformScale());
        x = t.x; y = t.y; w = t.w; h = t.h;
        c.a = XformAlpha(c.a);
        if (w <= 0 || h <= 0 || c.a == 0) return;
    }
    r = ClampRadius(r, w, h);
    if (r <= 0) { SetColor(c); SDL_Rect q{ x, y, w, h }; SDL_RenderDrawRect(gRen, &q); return; }
    SetColor(c);
    if (w - 2 * r > 0) {
        SDL_RenderDrawLine(gRen, x + r, y,         x + w - 1 - r, y);
        SDL_RenderDrawLine(gRen, x + r, y + h - 1, x + w - 1 - r, y + h - 1);
    }
    if (h - 2 * r > 0) {
        SDL_RenderDrawLine(gRen, x,         y + r, x,         y + h - 1 - r);
        SDL_RenderDrawLine(gRen, x + w - 1, y + r, x + w - 1, y + h - 1 - r);
    }
    BlitCorners(GetCornerTex(r).ring, x, y, w, h, r, c);
}

int UiRadiusFor(int w, int h) {
    const int m    = w < h ? w : h;
    const int M    = w < h ? h : w;
    const int base = gTheme.cornerRadius;
    // Hairlines, rules, swatches and grid cells stay square: rounding an
    // 8-pixel cell turns a grid of them into a field of dots.
    if (base <= 0 || m <= 8) return 0;
    if (m < 2 * base + 4) {
        if (M >= 3 * m) return m / 2;           // a thin bar: a pill
        return m / 4 > 3 ? m / 4 : 3;           // a small box: just softened
    }
    if (m >= 150) return base + 6;              // cards
    if (m >= 60)  return base + 2;              // tiles
    return base;                                // rows, buttons, fields
}

void MaskCorners(int x, int y, int w, int h, int r, SDL_Color bg) {
    if (w <= 0 || h <= 0) return;
    if (!XformIdentity()) {
        const SDL_Rect t = XformRect(x, y, w, h);
        r = (int)lroundf(r * XformScale());
        x = t.x; y = t.y; w = t.w; h = t.h;
        bg.a = XformAlpha(bg.a);
    }
    r = ClampRadius(r, w, h);
    if (r <= 0) return;
    BlitCorners(GetCornerTex(r).mask, x, y, w, h, r, bg);
}

void FillRect(int x, int y, int w, int h, SDL_Color c) {
    FillRectRounded(x, y, w, h, UiRadiusFor(w, h), c);
}

void DrawRect(int x, int y, int w, int h, SDL_Color c) {
    DrawRectRounded(x, y, w, h, UiRadiusFor(w, h), c);
}

void DrawShadow(int x, int y, int w, int h, int r) {
    if (!gTheme.shadow) return;
    if (r <= 0) r = UiRadiusFor(w, h);
    // Four stacked, widening layers read as one soft shadow under the box.
    for (int i = 4; i >= 1; i--)
        FillRectRounded(x - i, y + 2 + i * 2, w + 2 * i, h, r + i, SDL_Color{ 0, 0, 0, (uint8_t)(34 - i * 6) });
}

void DrawGlowRect(int x, int y, int w, int h, int r, SDL_Color core, SDL_Color glow, float pulseT) {
    float breath = 0.5f - 0.5f * cosf(pulseT * 2.f * (float)M_PI);
    int   extra  = (int)(breath * 3.f);
    float aMul   = 0.55f + 0.45f * breath;
    int   layers = 4 + extra;
    for (int g = layers; g >= 1; g--) {
        SDL_Color step = glow;
        int a = (int)glow.a * (layers + 1 - g) / (layers + 2);
        step.a = (Uint8)(a * aMul);
        DrawRectRounded(x - g, y - g, w + 2 * g, h + 2 * g, r + g, step);
    }
    SDL_Color outer = core;
    outer.a = (Uint8)(180 + 75 * breath);
    DrawRectRounded(x,     y,     w,     h,     r,                 outer);
    DrawRectRounded(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 1, core);
}

void DrawFocusRing(int x, int y, int w, int h, int r) {
    if (w <= 4 || h <= 4) return;
    if (r < 0) r = UiRadiusFor(w, h);
    const SDL_Color a = gTheme.gold;
    static const int kHalo[3] = { 62, 30, 12 };
    for (int g = 3; g >= 1; g--)
        DrawRectRounded(x - g, y - g, w + 2 * g, h + 2 * g, r + g, Alpha(a, kHalo[g - 1]));
    DrawRectRounded(x,     y,     w,     h,     r,                 a);
    DrawRectRounded(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 1, Alpha(a, 210));
}

// Discs and rings reuse the anti-aliased corner tiles: a rounded rectangle
// whose radius is half its size.
void FillCircle(int cx, int cy, int r, SDL_Color c) {
    if (r <= 0) return;
    FillRectRounded(cx - r, cy - r, 2 * r, 2 * r, r, c);
}
void DrawCircle(int cx, int cy, int r, SDL_Color c) {
    if (r <= 0) return;
    DrawRectRounded(cx - r, cy - r, 2 * r, 2 * r, r, c);
}
void DrawRing(int cx, int cy, int r, SDL_Color c) {
    if (r <= 1) return;
    DrawCircle(cx, cy, r,     c);
    DrawCircle(cx, cy, r - 1, c);
}

// ─── Backdrop ─────────────────────────────────────────────────────────────────

namespace {
// LLM-Selfbot's CSS aurora (BPBackdrop fallback): four large soft blobs of the
// theme's colours, screen-blended, drifting on slow alternate cycles, under a
// vignette. One white blob texture (dithered) is tinted and stretched per blob.
SDL_Texture* sBlob = nullptr;
SDL_Texture* sVignette = nullptr;
float sFocusX = 0.5f, sFocusY = 0.45f, sLeanX = 0.5f, sLeanY = 0.45f;

SDL_Texture* BuildBlob() {
    const int N = 192;
    static const float kBayer[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };
    std::vector<uint32_t> px((size_t)N * N);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            const float dx = (x + 0.5f) / N * 2 - 1, dy = (y + 0.5f) / N * 2 - 1;
            const float d = sqrtf(dx * dx + dy * dy);
            // the CSS stops: 88% at 0, 62% at .25, 28% at .5, 7% at .68, 0 at .88
            float a;
            if (d < 0.25f)      a = 0.88f + (0.62f - 0.88f) * (d / 0.25f);
            else if (d < 0.5f)  a = 0.62f + (0.28f - 0.62f) * ((d - 0.25f) / 0.25f);
            else if (d < 0.68f) a = 0.28f + (0.07f - 0.28f) * ((d - 0.5f) / 0.18f);
            else if (d < 0.88f) a = 0.07f * (1 - (d - 0.68f) / 0.2f);
            else                a = 0;
            int ia = (int)(a * 255.f + kBayer[y & 3][x & 3] / 16.f);
            ia = ia < 0 ? 0 : (ia > 255 ? 255 : ia);
            px[(size_t)y * N + x] = ((uint32_t)ia << 24) | 0x00FFFFFFu;
        }
    SDL_Texture* t = SDL_CreateTexture(gRen, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, N, N);
    if (t) {
        SDL_UpdateTexture(t, nullptr, px.data(), N * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_ADD);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    }
    return t;
}

SDL_Texture* BuildVignette() {
    const int W = 160, H = 90;
    std::vector<uint32_t> px((size_t)W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            // radial-gradient(ellipse 90% 80% at 50% 45%, transparent 48%, rgb(0 0 0 / .55))
            const float u = ((x + 0.5f) / W - 0.5f) / 0.9f, v = ((y + 0.5f) / H - 0.45f) / 0.8f;
            const float d = sqrtf(u * u + v * v) * 2.f;
            float a = d < 0.48f ? 0.f : (d > 1.f ? 0.55f : (d - 0.48f) / 0.52f * 0.55f);
            // + linear top/bottom darkening
            const float fy = (y + 0.5f) / H;
            if (fy < 0.2f) a += 0.3f * (1 - fy / 0.2f) * (1 - a);
            if (fy > 0.76f) a += 0.4f * ((fy - 0.76f) / 0.24f) * (1 - a);
            const int ia = (int)(a * 255.f);
            px[(size_t)y * W + x] = ((uint32_t)(ia > 255 ? 255 : ia) << 24);
        }
    SDL_Texture* t = SDL_CreateTexture(gRen, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, W, H);
    if (t) {
        SDL_UpdateTexture(t, nullptr, px.data(), W * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    }
    return t;
}

SDL_Color Turn(SDL_Color c, float deg) {
    float h, s, v;
    RGBtoHSV(c, h, s, v);
    h += deg; while (h < 0) h += 360; while (h >= 360) h -= 360;
    return HSVtoRGB(h, s, v);
}
float Pingpong(float t, float period) {          // 0 → 1 → 0, eased (ease-in-out)
    float u = fmodf(t, period * 2) / period;
    if (u > 1) u = 2 - u;
    return u * u * (3 - 2 * u);
}
}  // namespace

void SetBackdropFocus(float x01, float y01) { sFocusX = x01; sFocusY = y01; }

void ClearScreen() {
    SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_BLEND);
    const SDL_Color bg = gTheme.bg;
    SDL_SetRenderDrawColor(gRen, bg.r, bg.g, bg.b, 255);
    SDL_RenderClear(gRen);
    if (!sBlob) sBlob = BuildBlob();
    if (!sVignette) sVignette = BuildVignette();
    if (!sBlob) return;
    // The light leans toward what has focus (Big Picture: 0.7 of the way).
    const float k = gReduceMotion ? 1.f : 1.f - expf(-gDtSec * 1.7f);
    sLeanX += (sFocusX - sLeanX) * k; sLeanY += (sFocusY - sLeanY) * k;
    const float lx = (sLeanX - 0.5f) * 0.18f, ly = (sLeanY - 0.45f) * 0.18f;
    const float t = gReduceMotion ? 0.f : gElapsedSec;
    const float vmax = (float)SCREEN_W;                          // 1 vmax
    const SDL_Color c0 = gTheme.gold, c1 = gTheme.accent, c2 = Turn(gTheme.gold, -35), c3 = Turn(gTheme.accent, 40);
    // A light page (custom background) gets a much fainter glow.
    const float gain = ColorLuma(bg) < 128 ? 1.f : 0.3f;
    struct B { SDL_Color c; float x, y, size, op, period, ex, ey, es; };
    const B blobs[4] = {
        { c0, -0.16f, -0.22f, 0.62f, 0.50f, 34.f,  0.22f,  0.13f, 1.20f },
        { c1,  0.78f,  0.04f, 0.62f, 0.38f, 42.f, -0.20f,  0.17f, 0.85f },
        { c2,  0.18f,  0.60f, 0.62f, 0.34f, 50.f, -0.15f, -0.16f, 1.25f },
        { c3,  0.62f,  0.55f, 0.46f, 0.30f, 38.f, -0.11f, -0.13f, 1.15f },
    };
    for (const B& b : blobs) {
        const float e = Pingpong(t, b.period);
        const float size = b.size * vmax * (1 + (b.es - 1) * e);
        const float x = (b.x + b.ex * e + lx) * vmax, y = (b.y + b.ey * e + ly) * vmax;
        SDL_Rect d{ (int)x, (int)y, (int)size, (int)size };
        // ADD of the colour at the CSS opacity, softened: screen-blending a
        // colour this bright over a near-black page is close to adding a
        // fraction of it.
        const float a = b.op * 0.32f * gain;
        SDL_SetTextureColorMod(sBlob, b.c.r, b.c.g, b.c.b);
        SDL_SetTextureAlphaMod(sBlob, (uint8_t)(a * 255));
        SDL_RenderCopy(gRen, sBlob, nullptr, &d);
    }
    if (sVignette) SDL_RenderCopy(gRen, sVignette, nullptr, nullptr);
}

void DimScreen(int alpha) {
    SetColor(SDL_Color{ 0, 0, 0, ClampU8(alpha) });
    SDL_Rect r{ 0, 0, SCREEN_W, SCREEN_H };
    SDL_RenderFillRect(gRen, &r);
}

// ─── Geometry shapes ──────────────────────────────────────────────────────────
// Triangles with per-vertex colour. Edges are feathered over about one pixel
// (an outer ring fading to transparent), which reads as anti-aliased.
namespace {
std::vector<SDL_Vertex> sVerts;
std::vector<int>        sIdx;
inline SDL_Vertex V(float x, float y, SDL_Color c) {
    XformPoint(x, y);
    SDL_Vertex v; v.position = { x, y }; v.color = c; v.tex_coord = { 0, 0 };
    return v;
}
void Flush() {
    if (!sVerts.empty()) {
        SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_BLEND);
        SDL_RenderGeometry(gRen, nullptr, sVerts.data(), (int)sVerts.size(), sIdx.data(), (int)sIdx.size());
    }
    sVerts.clear(); sIdx.clear();
}
}  // namespace

void FillDisc(float cx, float cy, float r, SDL_Color c) {
    if (r <= 0.5f || c.a == 0) return;
    c.a = XformAlpha(c.a);
    const float fe = 1.0f / (XformScale() > 0.01f ? XformScale() : 1.f);   // one screen pixel
    const int seg = r < 8 ? 16 : (r < 40 ? 32 : (r < 120 ? 56 : 96));
    SDL_Color edge = c; edge.a = 0;
    const int base = (int)sVerts.size();
    sVerts.push_back(V(cx, cy, c));
    for (int i = 0; i < seg; i++) {
        const float a = (float)i / seg * 6.2831853f;
        const float s = sinf(a), co = cosf(a);
        sVerts.push_back(V(cx + s * (r - fe * 0.5f), cy - co * (r - fe * 0.5f), c));
        sVerts.push_back(V(cx + s * (r + fe * 0.5f), cy - co * (r + fe * 0.5f), edge));
    }
    for (int i = 0; i < seg; i++) {
        const int j = (i + 1) % seg;
        const int in0 = base + 1 + i * 2, out0 = in0 + 1, in1 = base + 1 + j * 2, out1 = in1 + 1;
        sIdx.insert(sIdx.end(), { base, in0, in1, in0, out0, out1, in0, out1, in1 });
    }
    Flush();
}

void StrokeArc(float cx, float cy, float r, float width, float a0, float a1, SDL_Color c0, SDL_Color c1) {
    if (width <= 0 || a1 <= a0) return;
    c0.a = XformAlpha(c0.a); c1.a = XformAlpha(c1.a);
    const float fe = 1.0f / (XformScale() > 0.01f ? XformScale() : 1.f);
    const float span = a1 - a0;
    const int seg = std::max(4, (int)(span / 3.f));
    const float ri = r - width / 2, ro = r + width / 2;
    const int base = (int)sVerts.size();
    for (int i = 0; i <= seg; i++) {
        const float t = (float)i / seg;
        const float a = (a0 + span * t) * 3.14159265f / 180.f;
        const float s = sinf(a), co = cosf(a);
        SDL_Color c = { (uint8_t)(c0.r + (c1.r - c0.r) * t), (uint8_t)(c0.g + (c1.g - c0.g) * t),
                        (uint8_t)(c0.b + (c1.b - c0.b) * t), (uint8_t)(c0.a + (c1.a - c0.a) * t) };
        SDL_Color z = c; z.a = 0;
        sVerts.push_back(V(cx + s * (ri - fe), cy - co * (ri - fe), z));
        sVerts.push_back(V(cx + s * ri,        cy - co * ri,        c));
        sVerts.push_back(V(cx + s * ro,        cy - co * ro,        c));
        sVerts.push_back(V(cx + s * (ro + fe), cy - co * (ro + fe), z));
    }
    for (int i = 0; i < seg; i++) {
        const int a = base + i * 4, b = a + 4;
        for (int k = 0; k < 3; k++)
            sIdx.insert(sIdx.end(), { a + k, b + k, a + k + 1, a + k + 1, b + k, b + k + 1 });
    }
    // Round caps (a closed ring has none: two translucent caps on the same
    // spot left a mark at twelve o'clock).
    Flush();
    if (span >= 359.5f) return;
    const float ae0 = a0 * 3.14159265f / 180.f, ae1 = a1 * 3.14159265f / 180.f;
    FillDisc(cx + sinf(ae0) * r, cy - cosf(ae0) * r, width / 2, c0);
    FillDisc(cx + sinf(ae1) * r, cy - cosf(ae1) * r, width / 2, c1);
}

namespace {
// A rounded rectangle's outline, walked clockwise by length from the left end
// of its top edge: four straight pieces and four quarter circles.
struct RoundPath {
    float x, y, w, h, r;
    float len[8];
    float total;
};
RoundPath MakeRoundPath(float x, float y, float w, float h, float r) {
    RoundPath p{ x, y, w, h, std::max(0.f, std::min(r, std::min(w, h) * 0.5f)), {}, 0.f };
    const float a = std::max(0.f, w - 2 * p.r), b = std::max(0.f, h - 2 * p.r), q = 1.5707963f * p.r;
    const float l[8] = { a, q, b, q, a, q, b, q };
    for (int i = 0; i < 8; i++) { p.len[i] = l[i]; p.total += l[i]; }
    return p;
}
// Point on the outline at distance s, and the outward normal there.
void RoundPathAt(const RoundPath& p, float s, float& px, float& py, float& nx, float& ny) {
    if (p.total <= 0) { px = p.x; py = p.y; nx = 0; ny = -1; return; }
    s = fmodf(s, p.total);
    if (s < 0) s += p.total;
    const float r = p.r;
    for (int i = 0; i < 8; i++) {
        if (s > p.len[i] && i < 7) { s -= p.len[i]; continue; }
        switch (i) {
            case 0: px = p.x + r + s;       py = p.y;               nx = 0;  ny = -1; return;
            case 2: px = p.x + p.w;         py = p.y + r + s;       nx = 1;  ny = 0;  return;
            case 4: px = p.x + p.w - r - s; py = p.y + p.h;         nx = 0;  ny = 1;  return;
            case 6: px = p.x;               py = p.y + p.h - r - s; nx = -1; ny = 0;  return;
            default: {
                float cx, cy, a0;   // degrees clockwise from twelve o'clock, like StrokeArc
                if (i == 1)      { cx = p.x + p.w - r; cy = p.y + r;       a0 = 0.f; }
                else if (i == 3) { cx = p.x + p.w - r; cy = p.y + p.h - r; a0 = 90.f; }
                else if (i == 5) { cx = p.x + r;       cy = p.y + p.h - r; a0 = 180.f; }
                else             { cx = p.x + r;       cy = p.y + r;       a0 = 270.f; }
                const float t = p.len[i] > 0 ? s / p.len[i] : 0.f;
                const float a = (a0 + 90.f * t) * 0.017453293f;
                nx = sinf(a); ny = -cosf(a);
                px = cx + nx * r; py = cy + ny * r;
                return;
            }
        }
    }
}
// A feathered band along the outline from s0 to s1; `closed` joins the ends.
// `flush` = false leaves the geometry queued, so many dashes draw at once.
void RoundPathBand(const RoundPath& p, float s0, float s1, float width, SDL_Color c, bool closed, bool flush = true) {
    c.a = XformAlpha(c.a);
    if (c.a == 0 || s1 <= s0) return;
    const float fe = 1.0f / (XformScale() > 0.01f ? XformScale() : 1.f);
    const float hw = width * 0.5f;
    const int n = std::max(2, (int)ceilf((s1 - s0) / 2.5f) + (closed ? 0 : 1));
    SDL_Color z = c; z.a = 0;
    const int base = (int)sVerts.size();
    for (int k = 0; k < n; k++) {
        const float s = closed ? s0 + (s1 - s0) * k / n : s0 + (s1 - s0) * k / (n - 1);
        float px, py, nx, ny;
        RoundPathAt(p, s, px, py, nx, ny);
        sVerts.push_back(V(px - nx * (hw + fe), py - ny * (hw + fe), z));
        sVerts.push_back(V(px - nx * hw,        py - ny * hw,        c));
        sVerts.push_back(V(px + nx * hw,        py + ny * hw,        c));
        sVerts.push_back(V(px + nx * (hw + fe), py + ny * (hw + fe), z));
    }
    const int segs = closed ? n : n - 1;
    for (int i = 0; i < segs; i++) {
        const int a = base + i * 4, b = base + ((i + 1) % n) * 4;
        for (int k = 0; k < 3; k++)
            sIdx.insert(sIdx.end(), { a + k, b + k, a + k + 1, a + k + 1, b + k, b + k + 1 });
    }
    if (flush) Flush();
}
}  // namespace

void StrokeRectRounded(float x, float y, float w, float h, float r, float width, SDL_Color c) {
    if (w <= 0 || h <= 0 || width <= 0) return;
    const RoundPath p = MakeRoundPath(x, y, w, h, r);
    RoundPathBand(p, 0.f, p.total, width, c, true);
}

void DashRectRounded(float x, float y, float w, float h, float r, float width, float dash, float gap, SDL_Color c) {
    if (w <= 0 || h <= 0 || width <= 0 || dash <= 0) return;
    const RoundPath p = MakeRoundPath(x, y, w, h, r);
    // A whole number of dash + gap pairs round the outline, so the last gap
    // matches the others instead of leaving a stub.
    const int count = std::max(1, (int)lroundf(p.total / (dash + std::max(0.f, gap))));
    const float step = p.total / count;
    const float d = std::min(dash, step * 0.8f);
    // Start half a dash before twelve o'clock on the top edge's middle, so the
    // pattern is symmetric left to right.
    const float start = (p.len[0] * 0.5f) - d * 0.5f;
    for (int i = 0; i < count; i++) {
        const float s = start + i * step;
        RoundPathBand(p, s, s + d, width, c, false, false);
    }
    Flush();
}

void FillGradientV(int x, int y, int w, int h, SDL_Color top, SDL_Color bottom) {
    if (w <= 0 || h <= 0) return;
    top.a = XformAlpha(top.a); bottom.a = XformAlpha(bottom.a);
    const int base = (int)sVerts.size();
    sVerts.push_back(V((float)x, (float)y, top));
    sVerts.push_back(V((float)(x + w), (float)y, top));
    sVerts.push_back(V((float)(x + w), (float)(y + h), bottom));
    sVerts.push_back(V((float)x, (float)(y + h), bottom));
    sIdx.insert(sIdx.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    Flush();
}

// ─── Motion ───────────────────────────────────────────────────────────────────
float  gDtSec        = 1.f / 60.f;
Uint32 gLastTickMs   = 0;
float  gElapsedSec   = 0.f;
bool   gReduceMotion = false;

static inline float EaseK(float dt) { return gReduceMotion ? 1.f : 1.f - expf(-16.f * dt); }

void AnimRect::Step(int tx, int ty, int tw, int th, float dt) {
    if (!ready) { Snap(tx, ty, tw, th); return; }
    // A far target is a different place, not a move: jump.
    if (fabsf(x - tx) > 280.f || fabsf(y - ty) > 200.f) { Snap(tx, ty, tw, th); return; }
    StepSmooth(tx, ty, tw, th, dt);
}
void AnimRect::StepSmooth(int tx, int ty, int tw, int th, float dt) {
    if (!ready) { Snap(tx, ty, tw, th); return; }
    const float k = EaseK(dt);
    x += (tx - x) * k; y += (ty - y) * k;
    w += (tw - w) * k; h += (th - h) * k;
    // Land exactly, so the ring does not shimmer by a pixel at rest.
    if (fabsf(tx - x) < 0.5f && fabsf(ty - y) < 0.5f && fabsf(tw - w) < 0.5f && fabsf(th - h) < 0.5f)
        Snap(tx, ty, tw, th);
}
void AnimRect::Snap(int tx, int ty, int tw, int th) {
    x = (float)tx; y = (float)ty; w = (float)tw; h = (float)th; ready = true;
}
SDL_Rect AnimRect::ToRect() const {
    return SDL_Rect{ (int)(x + 0.5f), (int)(y + 0.5f), (int)(w + 0.5f), (int)(h + 0.5f) };
}

namespace {
AnimRect sHighlight[kHighlightSlots];
int      sHlOwnerScreen[kHighlightSlots] = { -1, -1, -1, -1 };
int      sHlOwnerSub[kHighlightSlots]    = { -1, -1, -1, -1 };
AnimRect sTabBar[kTabUnderlineSlots];
int      sTabBarOwner[kTabUnderlineSlots] = { -1, -1, -1, -1 };
}  // namespace

void RequestHighlight(int ownerScreen, int ownerSub, int tx, int ty, int tw, int th, float dt, int slot) {
    if (slot < 0 || slot >= kHighlightSlots) slot = 0;
    if (sHlOwnerScreen[slot] != ownerScreen || sHlOwnerSub[slot] != ownerSub) {
        sHighlight[slot].Snap(tx, ty, tw, th);
        sHlOwnerScreen[slot] = ownerScreen;
        sHlOwnerSub[slot]    = ownerSub;
    } else {
        sHighlight[slot].StepSmooth(tx, ty, tw, th, dt);
    }
}

void SnapHighlight(int ownerScreen, int ownerSub, int x, int y, int w, int h, int slot) {
    if (slot < 0 || slot >= kHighlightSlots) slot = 0;
    sHighlight[slot].Snap(x, y, w, h);
    sHlOwnerScreen[slot] = ownerScreen;
    sHlOwnerSub[slot]    = ownerSub;
}

void DrawAnimatedHighlight(int slot) {
    if (slot < 0 || slot >= kHighlightSlots) slot = 0;
    if (!sHighlight[slot].ready) return;
    SDL_Rect r = sHighlight[slot].ToRect();
    DrawFocusRing(r.x, r.y, r.w, r.h);
}

void RequestTabUnderline(int slot, int ownerTag, int tx, int ty, int tw, int th, float dt) {
    if (slot < 0 || slot >= kTabUnderlineSlots) slot = 0;
    if (sTabBarOwner[slot] != ownerTag) {
        sTabBar[slot].Snap(tx, ty, tw, th);
        sTabBarOwner[slot] = ownerTag;
    } else {
        sTabBar[slot].StepSmooth(tx, ty, tw, th, dt);
    }
}

void DrawTabUnderline(int slot) {
    if (slot < 0 || slot >= kTabUnderlineSlots) slot = 0;
    if (!sTabBar[slot].ready) return;
    SDL_Rect r = sTabBar[slot].ToRect();
    FillRectRounded(r.x - 1, r.y - 1, r.w + 2, r.h + 2, (r.h + 2) / 2, Alpha(gTheme.gold, 60));   // soft glow
    FillRectRounded(r.x, r.y, r.w, r.h, r.h / 2, gTheme.gold);
}

void DrawTabPill(int slot) {
    if (slot < 0 || slot >= kTabUnderlineSlots) slot = 0;
    if (!sTabBar[slot].ready) return;
    SDL_Rect r = sTabBar[slot].ToRect();
    FillRectRounded(r.x, r.y, r.w, r.h, r.h / 2, Alpha(gTheme.gold, 46));
    DrawRectRounded(r.x, r.y, r.w, r.h, r.h / 2, Alpha(gTheme.gold, 72));
}

ScreenFade gFade;
void ScreenFade::Begin() { alpha = gReduceMotion ? 0.f : 170.f; }
void ScreenFade::Step(float dt) {
    if (alpha <= 0.f) return;
    alpha -= dt * 900.f;            // gone in under a fifth of a second
    if (alpha < 0.f) alpha = 0.f;
}
void ScreenFade::Draw() {
    if (alpha <= 0.f) return;
    FillRectSquare(0, 0, SCREEN_W, SCREEN_H, Alpha(gTheme.bg, (int)alpha));
}
