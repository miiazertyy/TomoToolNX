// main.cpp — TomoToolNX
// After user select: choose WebUI mode or On-Switch mode.
// WebUI: HTTP server, use browser on PC/phone.
// On-Switch: browse textures and import PNGs directly on console.

#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "save_mount.h"
#include "http_server.h"
#include "texture_processor.h"
#include "ugc_scanner.h"
#include "backup.h"
#include "mii_manager.h"
#include "qrcode.hpp"
#include "updater.h"

#include "mtp_server.h"

#include "save_editor.h"
#include "map_data.h"
#include "belongings_data.h"
#include "habits_data.h"
#include "wishes_data.h"
#include "u2net_infer.h"
#include "i18n.h"
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <climits>
#include <map>
#include <set>
#include <unordered_map>
#include <curl/curl.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ─── Constants ────────────────────────────────────────────────────────────────

static const int SCREEN_W = 1280;
static const int SCREEN_H = 720;
static const int HTTP_PORT = 8080;

// ─── Theme system ─────────────────────────────────────────────────────────────
// All UI colors live on the active theme so users can toggle palettes at
// runtime (Settings → Theme). The legacy "Original" theme reproduces the
// pre-revamp look pixel-for-pixel; "Switch" is the new default.
struct Theme {
    SDL_Color bg, panel, panel2, sel, border, text, dim, gold, red, accent, green, glow;
    int  cornerRadius;
    bool shadow;
};

static const Theme THEME_ORIGINAL = {
    /*bg*/     {10,  10,  10, 255},
    /*panel*/  {22,  22,  22, 255},
    /*panel2*/ {32,  32,  32, 255},
    /*sel*/    {50,  50,  50, 255},
    /*border*/ {55,  55,  55, 255},
    /*text*/   {220, 220, 220, 255},
    /*dim*/    {100, 100, 100, 255},
    /*gold*/   {200, 170, 100, 255},
    /*red*/    {190, 90,  90,  255},
    /*accent*/ {120, 180, 200, 255},
    /*green*/  {100, 190, 120, 255},
    /*glow*/   {200, 170, 100, 0},     // unused while shadow=false
    /*cornerRadius*/ 0,
    /*shadow*/       false,
};

// "Switch" theme — muted warm-gold primary, dusty-blue secondary. Pulled back
// from the previous neon-yellow / cyan combo so it reads as a calm, dreamy
// Tomodachi Life mood on an OLED panel rather than a flashy native UI.
static const Theme THEME_SWITCH = {
    /*bg*/     {6,    7,  12, 255},   // OLED black with a faint blue undertone
    /*panel*/  {20,  22,  30, 255},   // dusk-blue card
    /*panel2*/ {28,  31,  40, 255},
    /*sel*/    {38,  42,  54, 255},   // soft selected fill
    /*border*/ {62,  66,  80, 255},   // cool resting border
    /*text*/   {228,225, 215, 255},   // warm white — slightly cream so the
                                      //   accents don't look acid against it
    /*dim*/    {130,130, 140, 255},
    /*gold*/   {222,198, 120, 255},   // muted warm gold (was neon yellow)
    /*red*/    {205,100, 105, 255},   // dusty red
    /*accent*/ {120,170, 200, 255},   // dusty blue (was bright cyan)
    /*green*/  {130,195, 150, 255},   // sage green
    /*glow*/   {222,198, 120, 130},   // matches gold, lower alpha for halos
    /*cornerRadius*/ 6,
    /*shadow*/       true,
};

static Theme gTheme = THEME_SWITCH;

// All existing call sites use these macro names; expansion keeps them
// pointing at the active theme without touching ~700 references.
#define COL_BG     (gTheme.bg)
#define COL_PANEL  (gTheme.panel)
#define COL_PANEL2 (gTheme.panel2)
#define COL_SEL    (gTheme.sel)
#define COL_BORDER (gTheme.border)
#define COL_TEXT   (gTheme.text)
#define COL_DIM    (gTheme.dim)
#define COL_GOLD   (gTheme.gold)
#define COL_RED    (gTheme.red)
#define COL_ACCENT (gTheme.accent)
#define COL_GREEN  (gTheme.green)

namespace ThemeNS {
    struct Entry { std::string code; std::string labelKey; };
    static const std::vector<Entry> kAvailable = {
        { "switch",   "theme.switch"   },
        { "original", "theme.original" },
    };
    static std::string g_current = "switch";

    static void Apply(const std::string& code) {
        if      (code == "original") gTheme = THEME_ORIGINAL;
        else                         gTheme = THEME_SWITCH;
    }
    static void SetCurrent(const std::string& code) {
        for (const auto& e : kAvailable) if (e.code == code) { g_current = code; Apply(code); return; }
        g_current = "switch"; Apply("switch");
    }
    static const std::string&         Current()   { return g_current; }
    static const std::vector<Entry>&  Available() { return kAvailable; }
    [[maybe_unused]] static void Init() { Apply(g_current); }
}

static PadState      gPad;
static SDL_Renderer* gRen = nullptr;

// ─── SDL_ttf font renderer ────────────────────────────────────────────────────

#include <SDL2/SDL_ttf.h>
#include "font_data.h"

static TTF_Font* gFontSm  = nullptr; // 16px — body text, labels
static TTF_Font* gFontMd  = nullptr; // 22px — section titles, selected items
static TTF_Font* gFontLg  = nullptr; // 32px — screen titles, hero text

// Embedded font has no Cyrillic / CJK glyphs; for ru/zh borrow the
// system shared font via pl.
static const void* gActiveFontData = nullptr;
static size_t      gActiveFontSize = 0;
static void PickFontForLang(const std::string& code, const void*& outData, size_t& outSize) {
    outData = FONT_DATA; outSize = FONT_DATA_SIZE;
    PlSharedFontType type;
    bool wantShared = false;
    if      (code == "zh") { type = PlSharedFontType_ChineseSimplified; wantShared = true; }
    else if (code == "ru") { type = PlSharedFontType_Standard;          wantShared = true; }
    if (!wantShared) return;
    static bool plReady = false;
    if (!plReady) {
        if (R_FAILED(plInitialize(PlServiceType_User))) return;
        plReady = true;
    }
    PlFontData pf{};
    if (R_SUCCEEDED(plGetSharedFontByType(&pf, type))) {
        outData = pf.address; outSize = pf.size;
    }
}
static void TextCacheFlush();  // defined below; called here on language swap
static void FontReload(const std::string& langCode) {
    // Cached glyph textures came from the previous font and would render
    // mismatched after a language swap — drop them so the new font is used.
    TextCacheFlush();
    if (gFontSm) { TTF_CloseFont(gFontSm); gFontSm = nullptr; }
    if (gFontMd) { TTF_CloseFont(gFontMd); gFontMd = nullptr; }
    if (gFontLg) { TTF_CloseFont(gFontLg); gFontLg = nullptr; }
    PickFontForLang(langCode, gActiveFontData, gActiveFontSize);
    SDL_RWops* rw = SDL_RWFromConstMem(gActiveFontData, (int)gActiveFontSize);
    if (!rw) return;
    // Russian: the Switch shared Standard font is wider than the embedded
    // Latin font and translations run longer — shrink a notch to fit.
    int szSm = 16, szMd = 22, szLg = 32;
    if (langCode == "ru") { szSm = 14; szMd = 19; szLg = 28; }
    gFontSm = TTF_OpenFontRW(rw, 0, szSm);
    SDL_RWseek(rw, 0, RW_SEEK_SET);
    gFontMd = TTF_OpenFontRW(rw, 0, szMd);
    SDL_RWseek(rw, 0, RW_SEEK_SET);
    gFontLg = TTF_OpenFontRW(rw, 1, szLg); // 1 = free rw when done
}
static void FontInit() {
    TTF_Init();
    FontReload(Lang::Current());
}
static void FontQuit() {
    TextCacheFlush();
    if(gFontSm) TTF_CloseFont(gFontSm);
    if(gFontMd) TTF_CloseFont(gFontMd);
    if(gFontLg) TTF_CloseFont(gFontLg);
    TTF_Quit();
}

enum class Font { Sm, Md, Lg };
static TTF_Font* GetFont(Font f) {
    switch(f){ case Font::Md: return gFontMd; case Font::Lg: return gFontLg; default: return gFontSm; }
}

// ─── Text texture cache ───────────────────────────────────────────────────────
// Without this every DrawText call rasterised the glyphs with TTF_Render*Blended
// and uploaded a fresh GPU texture, then destroyed it — easily ~50 alloc/upload
// cycles per frame on busy screens (Mii Stats, Map, Settings), which blew past
// the 16.6ms vsync window and pinned the framerate at 30fps. Caching by
// (text, font-size, color) lets every repeat draw collapse into a single
// SDL_RenderCopy. LRU-evicted at a bounded size so memory stays in check.
struct TextCacheEntry { SDL_Texture* tex; int w, h; Uint32 lastUsed; };
static std::unordered_map<std::string, TextCacheEntry> gTextCache;
static Uint32  gTextCacheFrame = 0;
static const size_t kTextCacheMax = 512;

static void TextCacheFlush() {
    for (auto& kv : gTextCache) if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    gTextCache.clear();
}

static SDL_Texture* GetTextTexture(const std::string& text, Font f, SDL_Color col,
                                   int& outW, int& outH) {
    // Compact composite key — 16 bytes fixed prefix + the text.
    char prefix[24];
    int n = snprintf(prefix, sizeof(prefix), "%d|%02x%02x%02x%02x|",
                     (int)f, col.r, col.g, col.b, col.a);
    std::string key; key.reserve((size_t)n + text.size());
    key.append(prefix, (size_t)n).append(text);

    auto it = gTextCache.find(key);
    if (it != gTextCache.end()) {
        it->second.lastUsed = gTextCacheFrame;
        outW = it->second.w; outH = it->second.h;
        return it->second.tex;
    }
    TTF_Font* font = GetFont(f);
    if (!font) return nullptr;
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text.c_str(), col);
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(gRen, surf);
    int w = surf->w, h = surf->h;
    SDL_FreeSurface(surf);
    if (!tex) return nullptr;

    if (gTextCache.size() >= kTextCacheMax) {
        // Evict the least-recently-used entry. Linear scan is fine at this size.
        auto oldest = gTextCache.begin();
        for (auto cit = gTextCache.begin(); cit != gTextCache.end(); ++cit)
            if (cit->second.lastUsed < oldest->second.lastUsed) oldest = cit;
        if (oldest->second.tex) SDL_DestroyTexture(oldest->second.tex);
        gTextCache.erase(oldest);
    }
    gTextCache.emplace(std::move(key),
                       TextCacheEntry{ tex, w, h, gTextCacheFrame });
    outW = w; outH = h;
    return tex;
}

// Draw text at (x,y) top-left — cached.
static void DrawText(const std::string& text, int x, int y, SDL_Color col, Font f=Font::Sm) {
    if (text.empty()) return;
    int w = 0, h = 0;
    SDL_Texture* tex = GetTextTexture(text, f, col, w, h);
    if (!tex) return;
    SDL_Rect dst{x, y, w, h};
    SDL_RenderCopy(gRen, tex, nullptr, &dst);
}

// Draw text centered at (cx,cy)
static void DrawTextC(const std::string& text, int cx, int cy, SDL_Color col, Font f=Font::Sm) {
    if (text.empty()) return;
    int w = 0, h = 0;
    SDL_Texture* tex = GetTextTexture(text, f, col, w, h);
    if (!tex) return;
    SDL_Rect dst{ cx - w/2, cy - h/2, w, h };
    SDL_RenderCopy(gRen, tex, nullptr, &dst);
}

// ─── Rect primitives ──────────────────────────────────────────────────────────
// FillRect / DrawRect auto-route to rounded variants when the active theme has
// cornerRadius > 0 and the rect is large enough that rounding actually fits.
// Square-corner themes (Original) hit the legacy fast path.

static void FillRectSquare(int x,int y,int w,int h,SDL_Color c){
    SDL_SetRenderDrawColor(gRen,c.r,c.g,c.b,c.a);
    SDL_Rect r{x,y,w,h};SDL_RenderFillRect(gRen,&r);
}
static void DrawRectSquare(int x,int y,int w,int h,SDL_Color c){
    SDL_SetRenderDrawColor(gRen,c.r,c.g,c.b,c.a);
    SDL_Rect r{x,y,w,h};SDL_RenderDrawRect(gRen,&r);
}

// Filled rounded rectangle via scan-line: middle rows draw full width, top/bottom
// rows use circle math for the corner inset. ~h fill-rect calls per rect.
static void FillRectRounded(int x,int y,int w,int h,int r,SDL_Color c){
    if (r <= 0 || w < 2*r || h < 2*r) { FillRectSquare(x,y,w,h,c); return; }
    SDL_SetRenderDrawColor(gRen, c.r, c.g, c.b, c.a);
    // Middle band — single fill
    SDL_Rect mid{ x, y + r, w, h - 2*r };
    SDL_RenderFillRect(gRen, &mid);
    // Top / bottom caps — scanline with circle inset
    for (int dy = 0; dy < r; dy++) {
        float fy = (float)(r - dy) - 0.5f;
        int   off = r - (int)sqrtf((float)(r*r) - fy*fy);
        SDL_Rect top{ x + off, y + dy,             w - 2*off, 1 };
        SDL_Rect bot{ x + off, y + h - 1 - dy,     w - 2*off, 1 };
        SDL_RenderFillRect(gRen, &top);
        SDL_RenderFillRect(gRen, &bot);
    }
}

// Outline variant — 4 straight edges + 4 corner arcs.
static void DrawRectRounded(int x,int y,int w,int h,int r,SDL_Color c){
    if (r <= 0 || w < 2*r || h < 2*r) { DrawRectSquare(x,y,w,h,c); return; }
    SDL_SetRenderDrawColor(gRen, c.r, c.g, c.b, c.a);
    // Straight edges
    SDL_RenderDrawLine(gRen, x + r,         y,           x + w - 1 - r, y);
    SDL_RenderDrawLine(gRen, x + r,         y + h - 1,   x + w - 1 - r, y + h - 1);
    SDL_RenderDrawLine(gRen, x,             y + r,       x,             y + h - 1 - r);
    SDL_RenderDrawLine(gRen, x + w - 1,     y + r,       x + w - 1,     y + h - 1 - r);
    // Corner arcs — sample each "edge pixel" of the quarter circle
    for (int dy = 0; dy < r; dy++) {
        float fy = (float)(r - dy) - 0.5f;
        int   off = r - (int)sqrtf((float)(r*r) - fy*fy);
        SDL_RenderDrawPoint(gRen, x + off,             y + dy);
        SDL_RenderDrawPoint(gRen, x + w - 1 - off,     y + dy);
        SDL_RenderDrawPoint(gRen, x + off,             y + h - 1 - dy);
        SDL_RenderDrawPoint(gRen, x + w - 1 - off,     y + h - 1 - dy);
    }
}

// Soft drop shadow — two stacked translucent rounded rects offset downward.
// Cheap on Tegra; no-op when the theme doesn't want shadows.
static void DrawShadow(int x,int y,int w,int h,int r){
    if (!gTheme.shadow) return;
    SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_BLEND);
    SDL_Color s1{ 0, 0, 0, 60 };
    SDL_Color s2{ 0, 0, 0, 30 };
    if (r > 0 && w >= 2*r && h >= 2*r) {
        FillRectRounded(x, y + 3, w, h, r, s1);
        FillRectRounded(x, y + 6, w, h, r, s2);
    } else {
        FillRectSquare(x, y + 3, w, h, s1);
        FillRectSquare(x, y + 6, w, h, s2);
    }
    SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_NONE);
}

// Glowing outline helper — kept on standby for future use. The current
// selection highlight uses a flat outline instead (see DrawAnimatedHighlight).
[[maybe_unused]] static void DrawGlowRect(int x,int y,int w,int h,int r,SDL_Color core,SDL_Color glow,float pulseT=0.f){
    SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_BLEND);
    // breath: 0..1..0 triangle-ish wave; controls extra glow radius + alpha.
    float breath = 0.5f - 0.5f * cosf(pulseT * 2.f * (float)M_PI);
    int   extra  = (int)(breath * 3.f);
    float aMul   = 0.55f + 0.45f * breath;
    int   layers = 4 + extra;
    for (int g = layers; g >= 1; g--) {
        SDL_Color step = glow;
        int a = (int)glow.a * (layers + 1 - g) / (layers + 2);
        step.a = (Uint8)(a * aMul);
        DrawRectRounded(x - g, y - g, w + 2*g, h + 2*g, r + g, step);
    }
    // Inner core border — slightly brighter at the breath peak.
    SDL_Color innerOuter = core;
    innerOuter.a = (Uint8)(180 + 75 * breath);
    DrawRectRounded(x,     y,     w,     h,     r,     innerOuter);
    DrawRectRounded(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 1, core);
    SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_NONE);
}

// Public primitives — auto-round when the theme asks for it.
static inline void FillRect(int x,int y,int w,int h,SDL_Color c){
    int r = gTheme.cornerRadius;
    if (r > 0 && w >= 2*r && h >= 2*r) FillRectRounded(x, y, w, h, r, c);
    else                                FillRectSquare(x, y, w, h, c);
}
static inline void DrawRect(int x,int y,int w,int h,SDL_Color c){
    int r = gTheme.cornerRadius;
    if (r > 0 && w >= 2*r && h >= 2*r) DrawRectRounded(x, y, w, h, r, c);
    else                                DrawRectSquare(x, y, w, h, c);
}

// ─── Circles ─────────────────────────────────────────────────────────────────
// Filled circle via scan-line — one SDL fill-rect per row keeps the SDL call
// count low and gives clean edges. Outline uses 8-way symmetric midpoint.
static void FillCircle(int cx, int cy, int r, SDL_Color c) {
    if (r <= 0) return;
    SDL_SetRenderDrawColor(gRen, c.r, c.g, c.b, c.a);
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrtf((float)(r*r - dy*dy));
        SDL_Rect row{ cx - dx, cy + dy, 2*dx + 1, 1 };
        SDL_RenderFillRect(gRen, &row);
    }
}
static void DrawCircle(int cx, int cy, int r, SDL_Color c) {
    if (r <= 0) return;
    SDL_SetRenderDrawColor(gRen, c.r, c.g, c.b, c.a);
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        SDL_RenderDrawPoint(gRen, cx + x, cy + y);
        SDL_RenderDrawPoint(gRen, cx - x, cy + y);
        SDL_RenderDrawPoint(gRen, cx + x, cy - y);
        SDL_RenderDrawPoint(gRen, cx - x, cy - y);
        SDL_RenderDrawPoint(gRen, cx + y, cy + x);
        SDL_RenderDrawPoint(gRen, cx - y, cy + x);
        SDL_RenderDrawPoint(gRen, cx + y, cy - x);
        SDL_RenderDrawPoint(gRen, cx - y, cy - x);
        y++;
        if (err <= 0) err += 2*y + 1;
        else          { x--; err += 2*(y - x) + 1; }
    }
}
// A pleasant 2-px-thick ring — just two stacked circle outlines.
static void DrawRing(int cx, int cy, int r, SDL_Color c) {
    if (r <= 0) return;
    DrawCircle(cx, cy, r,     c);
    DrawCircle(cx, cy, r - 1, c);
}

// ─── Motion ────────────────────────────────────────────────────────────────────
static float  gDtSec       = 1.f/60.f;
static Uint32 gLastTickMs  = 0;
static float  gElapsedSec  = 0.f;

// Exponential lerp toward a target rect; settles in ~6 frames @ 60fps.
// Snaps on first use or on a far jump so screen changes don't slide.
struct AnimRect {
    float x=0, y=0, w=0, h=0;
    bool  ready=false;
    void Step(int tx, int ty, int tw, int th, float dt) {
        if (!ready) { x=tx; y=ty; w=tw; h=th; ready=true; return; }
        // If the target is far away, snap (avoids a long, silly slide across
        // the screen when changing screens or scrolling a long list).
        if (fabsf(x - tx) > 280.f || fabsf(y - ty) > 200.f) {
            x=tx; y=ty; w=tw; h=th; return;
        }
        float k = 1.f - expf(-14.f * dt);
        x += (tx - x) * k;
        y += (ty - y) * k;
        w += (tw - w) * k;
        h += (th - h) * k;
    }
    void Snap(int tx, int ty, int tw, int th) {
        x=tx; y=ty; w=tw; h=th; ready=true;
    }
    // Step without the snap-on-far guard; used by the tab underline so the
    // bar glides all the way across instead of teleporting on big jumps.
    void StepSmooth(int tx, int ty, int tw, int th, float dt) {
        if (!ready) { x=tx; y=ty; w=tw; h=th; ready=true; return; }
        float k = 1.f - expf(-14.f * dt);
        x += (tx - x) * k;
        y += (ty - y) * k;
        w += (tw - w) * k;
        h += (th - h) * k;
    }
    SDL_Rect ToRect() const {
        return SDL_Rect{ (int)(x+0.5f), (int)(y+0.5f), (int)(w+0.5f), (int)(h+0.5f) };
    }
};

// Tab-bar underline animators. One per bar (main row + Mii sub-row); state
// persists across screens so the underline glides during a tab switch.
static AnimRect gMainTabBar;
static AnimRect gMiiSubTabBar;

// N slots so screens with two simultaneous selections (Mii left list + middle
// field list etc.) can outline both at once.
static constexpr int    kHighlightSlots = 4;
static AnimRect gHighlight     [kHighlightSlots];
static int      gHlOwnerScreen [kHighlightSlots] = { -1, -1, -1, -1 };
static int      gHlOwnerSub    [kHighlightSlots] = { -1, -1, -1, -1 };

// Drives the focused-cell outline. `slot` lets multiple outlines coexist
// on one screen (e.g. left mii list + middle field list).
static void RequestHighlight(int ownerScreen, int ownerSub,
                             int tx, int ty, int tw, int th, float dt,
                             int slot = 0) {
    if (slot < 0 || slot >= kHighlightSlots) slot = 0;
    if (gHlOwnerScreen[slot] != ownerScreen || gHlOwnerSub[slot] != ownerSub) {
        gHighlight[slot].Snap(tx, ty, tw, th);
        gHlOwnerScreen[slot] = ownerScreen;
        gHlOwnerSub   [slot] = ownerSub;
    } else {
        // Smooth even on big jumps so wrap-around (rightmost → leftmost)
        // glides. Snap-on-screen-change is handled by the owner branch above.
        gHighlight[slot].StepSmooth(tx, ty, tw, th, dt);
    }
}

// ── Tab-bar underline animator ───────────────────────────────────────────────
// Slot 0 = main 5-tab strip, slot 1 = Mii sub-tab pill row. ownerTag tags
// the bar so changing bars snaps instead of sliding diagonally.
static constexpr int kTabUnderlineSlots = 2;
static AnimRect gTabBar      [kTabUnderlineSlots];
static int      gTabBarOwner [kTabUnderlineSlots] = { -1, -1 };

static void RequestTabUnderline(int slot, int ownerTag,
                                int tx, int ty, int tw, int th, float dt) {
    if (slot < 0 || slot >= kTabUnderlineSlots) slot = 0;
    if (gTabBarOwner[slot] != ownerTag) {
        gTabBar[slot].Snap(tx, ty, tw, th);
        gTabBarOwner[slot] = ownerTag;
    } else {
        gTabBar[slot].StepSmooth(tx, ty, tw, th, dt);
    }
}

static void DrawTabUnderline(int slot = 0) {
    if (slot < 0 || slot >= kTabUnderlineSlots) slot = 0;
    if (!gTabBar[slot].ready) return;
    SDL_Rect r = gTabBar[slot].ToRect();
    FillRectSquare(r.x, r.y, r.w, r.h, gTheme.gold);
}

static void DrawAnimatedHighlight(int slot = 0) {
    if (slot < 0 || slot >= kHighlightSlots) slot = 0;
    if (!gHighlight[slot].ready || gTheme.cornerRadius <= 0) return;
    SDL_Rect r = gHighlight[slot].ToRect();
    // Clamp radius so short rows don't render as pills.
    int rr = gTheme.cornerRadius;
    if (rr * 2 > r.h - 2) rr = std::max(2, (r.h - 2) / 2);
    if (rr * 2 > r.w - 2) rr = std::max(2, (r.w - 2) / 2);
    DrawRectRounded(r.x,     r.y,     r.w,     r.h,     rr,                 gTheme.gold);
    DrawRectRounded(r.x + 1, r.y + 1, r.w - 2, r.h - 2, rr > 1 ? rr - 1 : 1, gTheme.gold);
}

// Forward-declared so EmitPrevNextHighlight can sit beside the other
// highlight helpers; full definition / runtime use lives in the main loop.
static u64 gFrameKDown = 0;   // this frame's merged kDown
static int gPNDir      = -1;  // -1 unset, 0 left, 1 right
enum class Screen { AppletWarning, UpdateCheck, UpdateAvailable, UserPick,
                    BackupPrompt, BackingUp, Mounting, OnSwitch, SaveFeedback,
                    Error, Settings };

// Animated outline that hops to whichever side of a < / > pair was last pressed.
static void EmitPrevNextHighlight(int subTag,
                                  int bxL, int bxR,
                                  int btnY, int btnW, int btnH) {
    if (gFrameKDown & (HidNpadButton_Left  | HidNpadButton_StickLLeft  | HidNpadButton_StickRLeft))
        gPNDir = 0;
    if (gFrameKDown & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_StickRRight))
        gPNDir = 1;
    if (gPNDir < 0) return;
    int hlX = (gPNDir == 0) ? bxL : bxR;
    RequestHighlight((int)Screen::OnSwitch, subTag, hlX, btnY, btnW, btnH, gDtSec,
                     /*slot=*/2);
    DrawAnimatedHighlight(/*slot=*/2);
}

// Hover pulse — a 1.00 -> 1.03 -> 1.00 scale curve over ~800ms applied to the
// focused cell rect. Returns the (slightly enlarged) rect to draw into.
[[maybe_unused]] static SDL_Rect HoverPulse(SDL_Rect base, float timeSec) {
    float t = sinf(timeSec * (float)M_PI * 2.5f) * 0.5f + 0.5f; // 0..1
    float s = 1.f + 0.03f * t;
    int dw = (int)(base.w * (s - 1.f));
    int dh = (int)(base.h * (s - 1.f));
    return SDL_Rect{ base.x - dw/2, base.y - dh/2, base.w + dw, base.h + dh };
}

// Screen fade — black flash that decays after a screen change. Animation only;
// rendering is unaffected when no transition is active.
struct ScreenFade {
    float alpha = 0.f;  // 0..255
    void Begin() { alpha = 220.f; }
    void Step(float dt) {
        if (alpha <= 0.f) return;
        alpha -= dt * 700.f;  // ~3 frames at 60fps to fully clear
        if (alpha < 0.f) alpha = 0.f;
    }
    void Draw() {
        if (alpha <= 0.f) return;
        SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(gRen, 0, 0, 0, (Uint8)alpha);
        SDL_Rect r{0,0,SCREEN_W,SCREEN_H};
        SDL_RenderFillRect(gRen, &r);
        SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_NONE);
    }
};
static ScreenFade gFade;

// Final present wrapper — paints the fade overlay (if active) on top of
// whatever the current screen rendered, then flips the back buffer.
static inline void Present() {
    gFade.Draw();
    SDL_RenderPresent(gRen);
}

// ─── Audio (UI sound effects) ────────────────────────────────────────────────
// SDL2_mixer-backed click/nav cues from romfs:/sfx/{click,nav}.mp3.
// Init is non-fatal — Play() is a silent no-op when the device or files
// aren't available.
#include <SDL2/SDL_mixer.h>
namespace Audio {
    enum Sfx { SfxClick, SfxNav };
    static bool       g_ready = false;
    static Mix_Chunk* g_click = nullptr;
    static Mix_Chunk* g_nav   = nullptr;

    static void Init() {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;
        if ((Mix_Init(MIX_INIT_MP3) & MIX_INIT_MP3) == 0) return;
        if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 1024) != 0) {
            Mix_Quit();
            return;
        }
        Mix_AllocateChannels(8);
        g_click = Mix_LoadWAV("romfs:/sfx/click.mp3");
        g_nav   = Mix_LoadWAV("romfs:/sfx/nav.mp3");
        if (g_click) Mix_VolumeChunk(g_click, MIX_MAX_VOLUME * 80 / 100);
        if (g_nav)   Mix_VolumeChunk(g_nav,   MIX_MAX_VOLUME * 70 / 100);
        g_ready = true;
    }

    static void Quit() {
        if (!g_ready) return;
        if (g_click) Mix_FreeChunk(g_click);
        if (g_nav)   Mix_FreeChunk(g_nav);
        g_click = g_nav = nullptr;
        Mix_CloseAudio();
        Mix_Quit();
        g_ready = false;
    }

    static void Play(Sfx id) {
        if (!g_ready) return;
        Mix_Chunk* c = (id == SfxClick) ? g_click : g_nav;
        if (!c) return;
        Mix_PlayChannel(-1, c, 0);
    }
}

// ─── Gear / settings icon ─────────────────────────────────────────────────────
// Draw a gear silhouette centered at (cx,cy) with outer radius r.
// 8 teeth, proper hub hole, clean proportions matching the WebUI SVG gear.
static void DrawGearIcon(int cx, int cy, int r, SDL_Color col) {
    if (r < 4) return;
    SDL_SetRenderDrawColor(gRen, col.r, col.g, col.b, col.a);

    // Proportions tuned to match the WebUI feather gear icon
    float rOuter  = (float)r;
    float rInner  = rOuter * 0.68f;  // ring inner edge
    float rHub    = rOuter * 0.30f;  // centre hole
    float toothH  = rOuter * 0.24f;  // how far teeth stick out beyond rOuter
    float toothHW = rOuter * 0.16f;  // half-width of each tooth
    int   nTeeth  = 8;

    // Scan-line fill: for each row, compute X spans for ring + teeth + hub hole
    for (int dy = -(int)(rOuter + toothH) - 1; dy <= (int)(rOuter + toothH) + 1; dy++) {
        float y = (float)dy;
        // Collect all x-interval pairs [xL, xR] filled on this row
        struct Span { float l, r; };
        Span spans[16]; int nSpans = 0;

        // 1. Ring band (rHub < dist < rInner) — annulus scan
        float dOuter = rOuter*rOuter - y*y;
        float dInner = rInner*rInner - y*y;
        float dHub   = rHub  *rHub   - y*y;
        if (dOuter > 0.f) {
            float xO = sqrtf(dOuter);
            float xI = (dInner > 0.f) ? sqrtf(dInner) : 0.f;
            float xH = (dHub   > 0.f) ? sqrtf(dHub)   : 0.f;
            // left arm of ring: from -xO to -(xI or 0), minus hub
            if (xO > xH) {
                spans[nSpans++] = { -(xO), -(std::max(xI, xH)) };
            }
            // right arm of ring
            if (xO > xH) {
                spans[nSpans++] = { std::max(xI, xH), xO };
            }
            // inner ring (between xI and xH) if ring reaches there
            if (dInner > 0.f && xI > xH) {
                spans[nSpans++] = { -xI, -xH };
                spans[nSpans++] = {  xH,  xI };
            }
        }

        // 2. Teeth: rectangular blocks rotated around the gear
        for (int ti = 0; ti < nTeeth; ti++) {
            float a  = (float)(2.0 * M_PI * ti / nTeeth);
            float nx = cosf(a), ny = sinf(a);   // radial outward direction
            float tx = -sinf(a), ty = cosf(a);  // tangential direction

            // Tooth rectangle from rInner to rOuter+toothH, half-width toothHW
            float corners[4][2] = {
                { nx*(rOuter+toothH) + tx*toothHW,  ny*(rOuter+toothH) + ty*toothHW  },
                { nx*(rOuter+toothH) - tx*toothHW,  ny*(rOuter+toothH) - ty*toothHW  },
                { nx*(rInner*0.92f)  - tx*toothHW,  ny*(rInner*0.92f)  - ty*toothHW  },
                { nx*(rInner*0.92f)  + tx*toothHW,  ny*(rInner*0.92f)  + ty*toothHW  },
            };

            float minY = corners[0][1], maxY = corners[0][1];
            for (int k = 1; k < 4; k++) {
                if (corners[k][1] < minY) minY = corners[k][1];
                if (corners[k][1] > maxY) maxY = corners[k][1];
            }
            if (y < minY - 0.5f || y > maxY + 0.5f) continue;

            float xL =  1e9f, xR = -1e9f;
            for (int e = 0; e < 4; e++) {
                float y0 = corners[e][1], y1 = corners[(e+1)%4][1];
                float x0 = corners[e][0], x1 = corners[(e+1)%4][0];
                float dy2 = y1 - y0;
                if (fabsf(dy2) < 0.001f) continue;
                float t = (y - y0) / dy2;
                if (t < -0.001f || t > 1.001f) continue;
                float xi = x0 + (x1-x0)*t;
                if (xi < xL) xL = xi;
                if (xi > xR) xR = xi;
            }
            if (xR > xL) {
                // Clip tooth to outside rHub to keep hole clean
                float xH2 = (dHub > 0.f) ? sqrtf(dHub) : 0.f;
                if (xR > xH2)  spans[nSpans++] = { std::max(xL,  xH2), xR };
                if (xL < -xH2) spans[nSpans++] = { xL, std::min(xR, -xH2) };
            }
        }

        // Render all spans for this row
        for (int s = 0; s < nSpans; s++) {
            int x0 = cx + (int)floorf(spans[s].l);
            int x1 = cx + (int)ceilf (spans[s].r);
            if (x1 >= x0) {
                SDL_Rect sr{ x0, cy + dy, x1 - x0 + 1, 1 };
                SDL_RenderFillRect(gRen, &sr);
            }
        }
    }
}









// ─── Screens ──────────────────────────────────────────────────────────────────
// (Screen enum was forward-declared earlier so the highlight helper could use it.)

static Screen      gScreen  = Screen::UserPick;
static std::string gError;
static std::string gIP;
static std::vector<SaveMount::UserInfo> gUsers;
static int         gUserSel   = 0;
static bool        gBackupFull = false;  // true when MAX_BACKUPS slots are used
static std::vector<SDL_Texture*> gAvatarTextures;

// ─── Log (WebUI screen) ───────────────────────────────────────────────────────

struct LogLine { std::string text; SDL_Color col; };
static const int LOG_MAX = 10;
static std::vector<LogLine> gLog;
static void Log(const std::string& msg, SDL_Color col){
    gLog.push_back({msg,col});
    if((int)gLog.size()>LOG_MAX) gLog.erase(gLog.begin());
}
static void LogINF(const std::string& m){Log(m,COL_TEXT);}
static void LogOK (const std::string& m){Log(m,COL_GREEN);}
static void LogERR(const std::string& m){Log(m,COL_RED);}

// ─── On-Switch editor state ───────────────────────────────────────────────────

enum class OnSwitchMode { UGC, Mii, Player, MiiStats, WebUI, TexSettings, Map };
static OnSwitchMode gOnSwitchMode = OnSwitchMode::WebUI;

static std::vector<UgcTextureEntry> gEntries;
static int          gEntrySel    = 0;
static int          gEntryScroll = 0;          // position in filtered list (top of visible window)
static std::string  gUgcFilter;                // active substring filter (lowercase)
static std::vector<int> gFilteredEntries;      // indices into gEntries that match the filter
static SDL_Texture* gPreviewTex  = nullptr;
static std::string  gPreviewStem;
static std::string  gOnSwitchMsg;
static SDL_Color    gOnSwitchMsgCol = COL_TEXT;
static bool         gPreviewNoSrgb  = false; // color encoding toggle: false=sRGB, true=Linear
static TextureProcessor::Bc1Encoder gEncMode = TextureProcessor::Bc1Encoder::Custom;
static TextureProcessor::Bc1Mode    gBc1Mode = TextureProcessor::Bc1Mode::Auto;
static TextureProcessor::FitMode    gFitMode = TextureProcessor::FitMode::Cover;
static TextureProcessor::Matte      gMatte;            // transparent by default

// File browser
struct BrowseEntry { std::string name; bool isDir; };
static bool gShowFileBrowser = false;
static bool gBrowseForMii    = false;
static std::string gBrowseCurPath;
static std::string gBrowsePngPath = "/switch/TomoToolNX";
static std::string gBrowseLtdPath = "/switch/TomoToolNX";
static bool        gBrowseForExportDir = false;
static std::string gExportPath = "/switch/TomoToolNX/Exports";
static std::vector<BrowseEntry> gBrowseEntries;
static int gBrowseSel    = 0;
static int gBrowseScroll = 0;

// Mii state
static std::vector<MiiManager::MiiSlot> gMiis;


// ─── Touch / hitbox system ────────────────────────────────────────────────────
// Function pointers avoid std::function overhead under -fno-exceptions/-fno-rtti.
typedef void (*TouchFn)(int);
struct TouchHit { int x,y,w,h; TouchFn fn; int param; };
static TouchHit gHits[256];
static int      gHitCount = 0;
static u64      gSimKDown = 0;
// gFrameKDown / gPNDir live higher up the file so EmitPrevNextHighlight can
// reference them — see the forward-declared block near DrawAnimatedHighlight.
static int      gTouchScale = 1; // multiplier for nudge actions

static void HitClear() { gHitCount = 0; }
static void HitAdd(int x,int y,int w,int h, TouchFn fn, int param=0) {
    if (gHitCount < 256) gHits[gHitCount++] = {x,y,w,h,fn,param};
}
// Last fired-on touch coords, exposed for callbacks that need pixel-level info
// (e.g. the Map tab's canvas-wide hit zone uses this to derive tile X/Y).
static int gLastTouchFireX = 0;
static int gLastTouchFireY = 0;
static void HitFire(int tx, int ty) {
    // Reverse so inner / last-registered hits win over the container they
    // sit inside (e.g. Settings prev/next arrows over the row hit).
    for (int i = gHitCount - 1; i >= 0; i--) {
        auto& h = gHits[i];
        if (tx>=h.x && tx<h.x+h.w && ty>=h.y && ty<h.y+h.h) {
            gLastTouchFireX = tx; gLastTouchFireY = ty;
            // Callbacks that go through TSimBtn add to gSimKDown and let
            // the main-loop audio dispatch pick the sound; pure-state
            // callbacks (TSelUgc, TSetMode…) leave gSimKDown alone, so
            // play a click here to keep touch audible.
            u64 simBefore = gSimKDown;
            h.fn(h.param);
            if (gSimKDown == simBefore) Audio::Play(Audio::SfxClick);
            break;
        }
    }
}

// Touch handler helpers
static void TSimBtn (int btn)   { gSimKDown |= (u64)(uint32_t)btn; }
static void TSetMode(int mode)  { gOnSwitchMode = (OnSwitchMode)mode; gOnSwitchMsg = ""; }

// Defined after their target globals are declared (forward refs below via lambdas are fine as fn ptrs)
static void TSelUser (int idx);
static void TSelUgc  (int idx);
static void TUgcOpenSearch(int);
static void TUgcClearSearch(int);
static void TSelBrowse(int idx);
static void TAdjust  (int delta); // encoded: negative=left, positive=right; |val| = scale
static void TUndoNameLang(int);
static void TUndoIslandLang(int);
static void TSelPlayerField(int idx);
static void TSetSkinTone(int idx);
static void TSetIslandSize(int val);
static void TSelMiiStatsMii(int idx);
static void TSelShareMii(int idx);
static void TSelMiiStatsField(int idx);
static void TSelMiiWordsSlot(int idx);
static void TSelMiiRelation(int idx);
static void TRelMeterAdj(int delta);
static void TRelDateKbd(int idx);
static void TRelBatchKnow(int unused);
static void TSetSocialView(int v);
static void TSelHabitCat(int c);
static void TSelHabitItem(int posInCat);
static void TToggleHabitOwn(int posInCat);
static void TBLSel(int idx);
static void TBLPickSel(int idx);
static void BlOpenPicker(int blSel, int miiIdx);
static void TAckSaveWarning(int);
static void TPlayerKbd(int);
static void TMiiStatsKbd(int);
static void DrawSaveWarning();
static void DrawFileBrowser();
static void TUndoPlayer(int);
static void TUndoMii(int);
static void TUndoWords(int);
static void DrawBackPrompt();
static void TAckBackYes(int);
static void TAckBackNo(int);
static void TDismissThumbTip(int);
static void DrawThumbTip();
static void SaveConfig();
static void TWishesUnlockTap(int);
static void TWishesResetTap(int);
static void TShareDoImport(int);
static void TShareCloseDetail(int);
static void TShareImportSlotPrev(int);
static void TShareImportSlotNext(int);

// ─── Save editor state ────────────────────────────────────────────────────────
// Map.sav is loaded read-only — we only need it for house names (housing tab).
static SaveEditor::SavFile gPlayerSav, gMiiSav, gMapSav;
static bool   gPlayerSavDirty   = false, gMiiSavDirty = false, gUgcDirty = false, gWebUiDirty = false;
static int    gPlayerFieldSel   = 0;
static int    gPlayerScroll     = 0;
static int    gMiiStatsMiiSel   = 0; // index into gMiis
static int    gMiiStatsFieldSel = 0;
static int    gMiiStatsScroll   = 0;
static std::string gPlayerMsg, gMiiStatsMsg;
static SDL_Color   gPlayerMsgCol = COL_TEXT, gMiiStatsMsgCol = COL_TEXT;
static int         gMiiStatsMsgFrames = 0;  // auto-fade countdown for transient habit/edit messages

// Undo state (single-step undo per field)
static int32_t gPlayerUndoVal[16]   = {};
static bool    gPlayerUndoValid[16] = {};
// Wishes bulk-action confirmation state: 0=idle, 1=unlock armed, 2=reset armed.
// Cleared after ~3 seconds (180 frames at 60 fps) so a single misclick can't
// be confirmed by an unrelated later press.
static int    gPlayerActionArmed       = 0;
static int    gPlayerActionArmedFrames = 0;
static std::string gPlayerActionMsg;
static SDL_Color   gPlayerActionMsgCol = {180,180,180,255};
static int         gPlayerActionMsgFrames = 0;
static int32_t gNameLangUndo        = 0; static bool gNameLangUndoValid    = false;
static int32_t gIslandLangUndo      = 0; static bool gIslandLangUndoValid  = false;
static int32_t gMiiUndoVal[16]     = {};
static bool    gMiiUndoValid[16]   = {};

// Words tab undo — snapshot of the whole slot before the first edit
struct WordSlotUndo {
    uint32_t kind = 0;
    std::string text;
    std::string how;
    bool valid    = false;
};
static WordSlotUndo gWordUndo;

// Button focus (joystick navigation of +/- buttons)
static int  gPlayerBtnSel   = 3;     // 0-7 for 8 Player numeric buttons
static int  gMiiBtnSel      = 2;     // 0-5 for 6 MiiStats numeric buttons
static int  gMiiLtdBtnSel   = 0;     // 0=import .ltd  1=export .ltd  2=download from TomodachiShare (Name field)
static bool gMiiNameKbdReq  = false; // touch-triggered open of name keyboard
static bool gBtnTouchActive = false; // set by TAdjust (touch), cleared after use
static bool gRelDateKbdReq  = false;
static int  gRelDatePairIdx = -1;

// Back-prompt state
static bool gShowBackPrompt       = false;
static bool gBackPromptIsMii      = false;
static bool gBackPromptToUserPick = false; // true = save/discard→UserPick; false = save/discard→quit app
static bool gQuitApp              = false;

// Save-feedback state (brief "Saved!" screen after B or + save)
static int  gSaveFeedbackFrames = 0;
static bool gSaveFeedbackQuit   = false;

// Thumb-tip modal (shown once after first successful UGC import)
static bool gShowThumbTip      = false;
static int  gThumbTipCountdown = 0;  // frames remaining before button enables
static bool gThumbTipSeen      = false;

// Background-removal state
static bool gShowBgRemovePrompt = false;

static constexpr const char* kBgModelPath = "romfs:/u2netp.bin";
static int  gMaxBackups        = 8;


static int         gSettingsSel    = 0;
// TexSettings (texture-tab encoding drawer) selection: 0=Encoder, 1=BC1 Mode
static int         gTexSettingsSel = 0;
static std::string gSettingsMsg;
static SDL_Color   gSettingsMsgCol = COL_TEXT;

static bool                     gShowRestorePicker = false;
static std::vector<std::string> gRestoreList;
static int                      gRestoreSel        = 0;
static int                      gRestoreScroll     = 0;
// Confirmation modal kind for the restore picker:
//   0 = no modal, 1 = "Restore this backup?", 2 = "Delete this backup?"
static int                      gRestoreConfirmKind = 0;

static void TOpenSettings(int)      { gSettingsMsg = ""; gScreen = Screen::Settings; }
static void TSelSettingsItem(int i) { gSettingsSel = i; }
// Settings prev/next arrow taps: focus the tapped row before simulating the
// D-pad press, so the input handler dispatches it to the right row.
static void TSettingsRowLeft (int row) { gSettingsSel = row; gSimKDown |= HidNpadButton_Left;  }
static void TSettingsRowRight(int row) { gSettingsSel = row; gSimKDown |= HidNpadButton_Right; }
static void TSelTexSettingsItem(int i) { gTexSettingsSel = i; }
static void TRestorePickSel(int i)  { gRestoreSel  = i; }
static void TRestoreConfirmA(int)   { gSimKDown |= HidNpadButton_A; }
static void TRestoreConfirmB(int)   { gSimKDown |= HidNpadButton_B; }

// ─── Player field table ───────────────────────────────────────────────────────
struct SaveFieldDef {
    const char* label;
    const char* fieldName;  // "" when rawHash is used
    uint32_t    rawHash;    // 0 = hash fieldName; nonzero = use directly
    bool isStr;      // WStr32 keyboard edit
    bool isUInt;     // UInt nudge (GetUInt/SetUInt)
    bool isEnum;     // Currency enum cycle
    bool isInt;      // Signed int nudge (GetAnyScalar/SetAnyScalar)
    bool isSkinTone; // UInt 0-5 swatch buttons
    bool isIslandSz; // Any-scalar, 4 safe preset buttons (values 1-4)
    bool isRegion;   // Named region enum cycle (9 values)
    bool isLang;     // Named language enum cycle (preset labels)
    bool isRawEnum;  // Raw unsigned nudge (GetAnyScalar/SetAnyScalar)
    int32_t minVal, maxVal; // -1 = unclamped
    const char* desc;
    // Action-only fields (no save hash); A press runs an action with a
    // two-stage confirmation so a misclick can't fire it.
    bool        isAction;   // when true, fieldName/rawHash are ignored
    int         actionId;   // 1=unlock all wishes, 2=reset liberated wishes
};
static uint32_t PFHash(const SaveFieldDef& fd) {
    return fd.rawHash ? fd.rawHash : SaveEditor::Hash(fd.fieldName);
}
static const struct { const char* name; const char* label; } REGION_OPTIONS[] = {
    {"Japan",          "Japan"},
    {"Europe",         "Europe"},
    {"NorthAmerica",   "North America"},
    {"SouthAmericaN",  "S.America (N)"},
    {"SouthAmericaS",  "S.America (S)"},
    {"Australia",      "Australia/NZ"},
    {"Asia",           "HK/TW/Korea"},
    {"OthersN",        "Other (N)"},
    {"OthersS",        "Other (S)"},
};
static const int REGION_OPTION_COUNT = 9;
static const struct { const char* name; const char* label; } LANG_OPTIONS[] = {
    {"JPja",  "Japanese"},
    {"USen",  "English (US)"},
    {"EUen",  "English (EU)"},
    {"EUfr",  "French (EU)"},
    {"EUde",  "German (EU)"},
    {"EUes",  "Spanish (EU)"},
    {"EUit",  "Italian (EU)"},
    {"EUnl",  "Dutch (EU)"},
    {"CNzh",  "Chinese (Simplified)"},
    {"KRko",  "Korean"},
    {"TWzh",  "Chinese (Traditional)"},
};
static const int LANG_OPTION_COUNT = 11;
//                    label           fieldName                              rawHash      Str    UInt   Enum   Int    Skin   Island Region Lang   RawEn  min  max  desc                                                                       Action  actionId
static const SaveFieldDef PLAYER_FIELDS[] = {
    {"Name",          "Player.Name",                           0,           true,  false, false, false, false, false, false, false, false, 0,   0,   "Your player's display name.",                                              false, 0},
    {"Island",        "Player.IslandName",                     0,           true,  false, false, false, false, false, false, false, false, 0,   0,   "The name of your island.",                                                 false, 0},
    {"Money",         "Player.Money",                          0,           false, true,  false, false, false, false, false, false, false, 0,   -1,  "Current coin balance.",                                                    false, 0},
    {"Currency",      "Player.Currency",                       0,           false, false, true,  false, false, false, false, false, false, 0,   -1,  "Regional currency symbol shown in-game.",                                  false, 0},
    {"Boot Count",    "Player.BootNum",                        0,           false, true,  false, false, false, false, false, false, false, 0,   -1,  "Number of times the game has been launched.",                              false, 0},
    {"Skin Tone",     "Player.SkinColorIndex",                 0,           false, false, false, false, true,  false, false, false, false, 0,   5,   "Player hand/skin color (0-5).",                                            false, 0},
    {"Bday Day",      "",                                      0xdb7786bbu, false, false, false, false, false, false, false, false, true,  1,   31,  "Birthday day of the month (1-31).",                                        false, 0},
    {"Bday Month",    "",                                      0xc754bef3u, false, false, false, false, false, false, false, false, true,  1,   12,  "Birthday month (1-12).",                                                   false, 0},
    {"Bday Year",     "",                                      0x11996629u, false, false, false, true,  false, false, false, false, false, -1,  -1,  "Birthday year.",                                                           false, 0},
    {"Island Size",   "",                                      0x870a807cu, false, false, false, false, false, true,  false, false, false, 1,   4,   "Island size preset. Other values corrupt the save!",                       false, 0},
    {"Fountain Lv",   "Liberation.FountainLevel",              0,           false, false, false, false, false, false, false, false, true,  0,   -1,  "Wishing fountain upgrade level.",                                          false, 0},
    {"Wishes",        "",                                      0xa32f7e47u, false, false, false, false, false, false, false, false, true,  0,   -1,  "Number of wishes made.",                                                   false, 0},
    {"Region",        "Player.Region",                         0,           false, false, false, false, false, false, true,  false, false, 0,   -1,  "Player region (affects seasons and weather).",                             false, 0},
};
static const int PLAYER_FIELD_COUNT = 13;

// ─── Mii stats field table ─────────────────────────────────────────────────────
struct MiiStatsDef {
    const char* label;
    const char* fieldName;
    bool isStr;   // WStr32Array
    bool isUInt;  // UIntArray; otherwise IntArray or EnumArray
    bool isEnum;  // EnumArray
    bool isAction; // navigable action item (no fieldName used)
    int actionId;  // 1=import .ltd, 2=export .ltd
    int dispOffset; // add when displaying (level is stored 0-based, shown +1)
    int32_t minVal, maxVal;
    const char* desc;
};
static const MiiStatsDef MII_STATS_FIELDS[] = {
    {"Name",        "Mii.Name.Name",                       true, false,false, false,0, 0,  0,  0, "The Mii's in-game display name."},
    {"Level",       "Mii.MiiMisc.SatisfyInfo.Level",      false,false,false, false,0, 1,  0, -1, "Friendship level with the player."},
    {"Lv Meter",    "Mii.MiiMisc.SatisfyInfo.Meter",      false,false,false, false,0, 0,  0,100, "Progress bar toward the next level (0-100)."},
    {"Money",       "Mii.Belongings.Money",                false,true, false, false,0, 0,  0, -1, "Coins currently held by this Mii."},
    {"Bday Day",    "Mii.MiiMisc.BirthdayInfo.Day",       false,false,false, false,0, 0,  1, 31, "Birthday day of the month (1-31)."},
    {"Bday Month",  "Mii.MiiMisc.BirthdayInfo.Month",     false,false,false, false,0, 0,  1, 12, "Birthday month (1-12)."},
    {"Birth Year",  "Mii.MiiMisc.BirthdayInfo.Year",      false,false,false, false,0, 0, -1, -1, "Year of birth."},
    {"Direct Age",  "Mii.MiiMisc.BirthdayInfo.DirectAge", false,false,false, false,0, 0, -1, -1, "Age override. -1 means auto (from birthday)."},
    {"Activeness",  "Mii.CharacterParam.Activeness",       false,false,false, false,0, 0,  1, 10, "How active and energetic the Mii is (1-10)."},
    {"Audacity",    "Mii.CharacterParam.Audaciousness",    false,false,false, false,0, 0,  1, 10, "How bold and daring the Mii is (1-10)."},
    {"Commonsense", "Mii.CharacterParam.Commonsense",      false,false,false, false,0, 0,  1, 10, "How wise and sensible the Mii is (1-10)."},
    {"Gaiety",      "Mii.CharacterParam.Gaiety",           false,false,false, false,0, 0,  1, 10, "How cheerful and upbeat the Mii is (1-10)."},
    {"Sociability", "Mii.CharacterParam.Sociability",      false,false,false, false,0, 0,  1, 10, "How sociable and outgoing the Mii is (1-10)."},
    {"Bond Meter",  "Mii.MiiMisc.BondInfo.Meter",         false,false,false, false,0, 0,  0,100, "Strength of the bond with the player (0-100)."},
    {"Mood",        "Mii.Feeling.Type",                    false,false,true,  false,0, 0,  0, -1, "Current emotional state."},
    {"Fullness",    "Mii.MiiMisc.EatInfo.EatFullness",     false,false,false, false,0, 0,  0, -1, "How well-fed the Mii is. Can exceed 100 with certain items."},
};
static const int MII_STATS_FIELD_COUNT = 16;

// Feeling.Type enum hash → display label (from Mii.Feeling.Type EnumArray)
static const struct { uint32_t hash; const char* label; } FEELING_LABELS[] = {
    { 0xb6eede09u, "None"     },
    { 0x390b012du, "Normal"   },
    { 0x62189afbu, "Good"     },
    { 0xab185a53u, "Irritate" },
    { 0xabc36d9fu, "Deject"   },
    { 0x86e2036fu, "Careless" },
    { 0xf3046534u, "Angry"    },
    { 0xf54cf52eu, "Depress"  },
    { 0x651b7eddu, "Worry"    },
};
static const int FEELING_LABEL_COUNT = 9;
static const char* FeelingName(uint32_t hash) {
    for (int i = 0; i < FEELING_LABEL_COUNT; i++)
        if (FEELING_LABELS[i].hash == hash) return FEELING_LABELS[i].label;
    return nullptr;
}

// Word kind options (Mii.MiiMisc.WordInfo.WordArray.WordKind)
static const struct { const char* name; const char* label; } WORD_KIND_LIST[] = {
    { "Invalid",     "(empty)"     },
    { "TalkStart",   "Talk Start"  },
    { "TalkEnd",     "Talk End"    },
    { "Phrase",      "Phrase"      },
    { "Happy",       "Happy"       },
    { "Sad",         "Sad"         },
    { "Angry",       "Angry"       },
    { "Greeting",    "Greeting"    },
    { "TalkInSleep", "Sleep Talk"  },
    { "ShoutToSea",  "Shout Sea"   },
    { "BeforeEat",   "Before Eat"  },
};
static const int WORD_KIND_COUNT = 11;
static int WordKindIndex(uint32_t hash) {
    for (int i = 0; i < WORD_KIND_COUNT; i++)
        if (SaveEditor::Hash(WORD_KIND_LIST[i].name) == hash) return i;
    return -1;
}
static const char* WordKindLabel(uint32_t hash) {
    int i = WordKindIndex(hash);
    return i >= 0 ? WORD_KIND_LIST[i].label : nullptr;
}

// Player.Currency enum hash → symbol / name  (hash of 'Player.Currency' = 0xdc88f139)
static const struct { uint32_t hash; const char* symbol; const char* name; } CURRENCY_LABELS[] = {
    { 0x7e3d1e46u, "---",  "Invalid"    },
    { 0xce8d18e1u, "\xc2\xa5", "Yen"   }, // ¥
    { 0x2bef288au, "$",    "Dollar"     },
    { 0x038c20a9u, "\xe2\x82\xac", "Euro" }, // €
    { 0x2f084465u, "\xc2\xa3", "Pound"  }, // £
    { 0x78915cbeu, "HK$",  "AsiaDollar" },
    { 0xd73beb47u, "KRW",  "Won"        },
    { 0xaca685c9u, "\xc2\xa5", "Yuan"   }, // ¥
    { 0x4616c424u, "RUB",  "Rouble"     },
    { 0xa60f72e0u, "PHP",  "Peso"       },
    { 0x6362048bu, "G$",   "GeneralUse" },
};
static const int CURRENCY_LABEL_COUNT = 11;
static int CurrencyIndex(uint32_t hash) {
    for (int i = 0; i < CURRENCY_LABEL_COUNT; i++)
        if (CURRENCY_LABELS[i].hash == hash) return i;
    return -1;
}
static const char* CurrencySymbol(uint32_t hash) {
    int i = CurrencyIndex(hash); return i >= 0 ? CURRENCY_LABELS[i].symbol : "?";
}
static const char* CurrencyName(uint32_t hash) {
    int i = CurrencyIndex(hash); return i >= 0 ? CURRENCY_LABELS[i].name : "?";
}

// Relation.Info.DirectionalInfo.BaseRelationType enum hash → display name
static const struct { uint32_t hash; const char* name; } RELATION_TYPE_NAMES[] = {
    { 0x0784a8dcu, "Unknown"     }, { 0x7e3d1e46u, "Invalid"     },
    { 0x354a0515u, "Know"        }, { 0xba939a42u, "Friend"      },
    { 0x535e93a8u, "Ex-Friend"   }, { 0xc2d067a7u, "Couple"      },
    { 0xb7ce0c18u, "Lover"       }, { 0x7783d4c3u, "Ex-Lover"    },
    { 0xfe59f825u, "Divorce"     }, { 0xdcfc7603u, "Parent"      },
    { 0xe193c5a2u, "Child"       }, { 0x1918f808u, "SiblingOld"  },
    { 0x3b1d200au, "SiblingYng"  }, { 0x2fd9785bu, "Grandparent" },
    { 0x7e3cd550u, "Grandchild"  }, { 0x804172f3u, "Relative"    },
};
static const int RELATION_TYPE_COUNT = 16;
static const char* RelTypeName(uint32_t hash) {
    for (int i = 0; i < RELATION_TYPE_COUNT; i++)
        if (RELATION_TYPE_NAMES[i].hash == hash) return RELATION_TYPE_NAMES[i].name;
    return "Unknown";
}
static SDL_Color RelTypeColor(uint32_t hash) {
    if (hash==0xba939a42u) return {230,210, 60,255}; // Friend   → yellow
    if (hash==0xc2d067a7u) return {255,150,180,255}; // Couple
    if (hash==0xb7ce0c18u) return {255,100,100,255}; // Lover
    if (hash==0x354a0515u) return {100,210,120,255}; // Know     → green
    if (hash==0x535e93a8u) return {160,180, 80,255}; // Ex-Friend → muted yellow-green
    if (hash==0x7783d4c3u) return {160, 80,160,255}; // Ex-Lover
    if (hash==0xfe59f825u) return {160, 60, 60,255}; // Divorce
    if (hash==0xdcfc7603u||hash==0xe193c5a2u) return {220,150, 80,255}; // Parent/Child
    if (hash==0x1918f808u||hash==0x3b1d200au) return {220,200, 80,255}; // Sibling
    if (hash==0x2fd9785bu||hash==0x7e3cd550u) return {200,170,120,255}; // Grandparent/child
    if (hash==0x804172f3u) return {160,160,160,255}; // Relative
    return {100,100,100,255};
}
static uint32_t RelTypeCounterpart(uint32_t hash) {
    if (hash==0xdcfc7603u) return 0xe193c5a2u; // Parent → Child
    if (hash==0xe193c5a2u) return 0xdcfc7603u; // Child → Parent
    if (hash==0x1918f808u) return 0x3b1d200au; // SiblingOld → SiblingYng
    if (hash==0x3b1d200au) return 0x1918f808u; // SiblingYng → SiblingOld
    if (hash==0x2fd9785bu) return 0x7e3cd550u; // Grandparent → Grandchild
    if (hash==0x7e3cd550u) return 0x2fd9785bu; // Grandchild → Grandparent
    return hash; // all other types are symmetric
}
static bool RelMeterFixed(uint32_t hash) {
    return hash==0x0784a8dcu || hash==0x7e3d1e46u; // Other or Invalid
}

// Fit a UTF-8 string to maxW pixels wide using font f, appending "…" if truncated.
static std::string FitNodeName(const std::string& s, TTF_Font* f, int maxW) {
    const char* ELL = "\xE2\x80\xA6"; // U+2026 HORIZONTAL ELLIPSIS
    const std::string src = s.empty() ? "?" : s;
    if (!f) return src;
    int w = 0, h = 0;
    TTF_SizeUTF8(f, src.c_str(), &w, &h);
    if (w <= maxW) return src;
    const char* tail = TTF_GlyphIsProvided(f, 0x2026) ? ELL : "...";
    std::string t = src;
    while (!t.empty()) {
        // strip trailing bytes until we're at a UTF-8 codepoint boundary
        do { t.pop_back(); } while (!t.empty() && (uint8_t(t.back()) & 0xC0) == 0x80);
        std::string cand = t + tail;
        TTF_SizeUTF8(f, cand.c_str(), &w, &h);
        if (w <= maxW) return cand;
    }
    return tail;
}

// MiiStats sub-tab state
enum class MiiStatsSubTab { Stats=0, Belongings=1, Habits=2, Words=3, Relations=4, Housing=5, Social=6, Browse=7 };
// Number of sub-tabs reachable via the pill bar and the Y-cycle (Stats..Housing).
// Browse (7) sits outside this cycle and is only reachable via the upload-icon
// button next to "export .ltd" on the Stats sub-tab.
static const int MII_SUBTAB_COUNT = 7;
static MiiStatsSubTab gMiiStatsSubTab  = MiiStatsSubTab::Stats;

// ── TomodachiShare browser state ─────────────────────────────────────────────
// One worker thread does the libcurl I/O. Main thread polls a "done" flag and
// reads results. Textures are created on the main thread only (SDL constraint).
struct ShareMii {
    int id;
    std::string name;
    std::string author;
    std::string platform;
    std::string gender;
    int likes;
    bool fromSav;
    bool allowCopying;
    std::vector<std::string> tags;
    SDL_Texture* thumb;      // lazy-loaded, owned, freed on page change
    bool thumbRequested;     // we've asked the worker for the bytes
    std::vector<uint8_t> thumbBytes; // raw PNG bytes once the worker finishes
    bool thumbReady;         // true when thumbBytes is populated
};
enum class ShareJobKind { None, ListPage, Thumb, Detail, Download };
struct ShareJob {
    ShareJobKind kind;
    int targetMiiIdx;    // index into gShareMiis (for Thumb/Detail), or -1
    std::string url;
    std::vector<uint8_t> body;
    long status;
    std::string error;
    bool done;
};
static std::vector<ShareMii>  gShareMiis;
static int                    gSharePage    = 1;
static int                    gShareLastPage= 1;
static int                    gShareTotal   = 0;
static int                    gShareSel     = 0;        // selection in list
static int                    gShareScroll  = 0;
static std::string            gShareQuery;
static int                    gShareSort    = 0;        // 0=newest 1=likes 2=oldest
static bool                   gShareFromSavOnly = true;
static bool                   gShareDetailOpen = false;
static SDL_Texture*           gShareDetailTex  = nullptr;
static std::vector<uint8_t>   gShareDetailBytes; // raw png pending decode
static bool                   gShareDetailReady= false; // bytes ready, need decode
static std::string            gShareDetailErr;
static std::string            gShareStatusMsg;
static SDL_Color              gShareStatusCol  = {180,180,180,255};
static int                    gShareStatusFrames = 0;
static int                    gShareImportSlot = 1; // 1-based
static bool                   gShareListLoading = false; // gates rapid page presses

static Thread                 gShareWorker;
static Mutex                  gShareJobMutex;
static CondVar                gShareJobCond;
static std::vector<ShareJob>  gShareJobQueue;   // pending jobs (worker pops front)
static std::vector<ShareJob>  gShareJobDone;    // completed jobs (main pops front)
static bool                   gShareWorkerRunning = false;
static bool                   gShareWorkerStop    = false;
static void ShareWorkerFn(void*);
static void ShareWorkerStart();
static void ShareWorkerStopFn();
static void ShareSubmitJob(ShareJobKind kind, const std::string& url, int targetMiiIdx);
static bool ShareTryConsumeJob(ShareJob& out); // main thread call
static void ShareFreeThumbs();
static void ShareFreeDetail();

// ── Caches so flipping pages / searching back to a previous page is instant ──
// Thumb cache: id → SDL_Texture, owned by the cache (not by ShareMii). Pages
// reuse already-decoded thumbs even after the list refreshes. LRU-evicted.
// List cache: parsed list responses keyed by query+sort+savFilter+page so the
// API doesn't get hit twice for the same page during a browsing session.
static std::map<int, SDL_Texture*>           gShareThumbCache;
static std::vector<int>                      gShareThumbCacheLRU;   // back = MRU
static constexpr size_t                      kShareThumbCacheMax = 256;

struct ShareListSnapshot { std::vector<ShareMii> miis; int total=0; int lastPage=1; };
static std::map<std::string, ShareListSnapshot> gShareListCache;
static std::vector<std::string>                 gShareListCacheLRU; // back = MRU
static constexpr size_t                         kShareListCacheMax = 24;

// Housing sub-tab state
static const uint32_t HASH_HOUSE_MAPID = SaveEditor::Hash("Mii.Location.HouseMapId");
static const uint32_t HASH_ROOM_INDEX  = SaveEditor::Hash("Mii.Location.RoomIndex");
static const uint32_t HASH_MII_NAME    = SaveEditor::Hash("Mii.Name.Name");
// Map.sav field hashes (verified against LtdSaveEditorTemplate's generated schema).
// We don't hash these from a string because the original schema field path is
// inferred from yaml mappings, not directly hashable.
static const uint32_t HASH_MAP_HOUSE_MAPID = 0x0b5276e9u; // House.MapId — IntArray
static const uint32_t HASH_MAP_HOUSE_NAME  = 0x0d96409eu; // House.RoommateGroupName — WStr32Array

// Map.sav's House.MapId array is the source of truth for which house IDs
// actually have an actor placed on the island. Writing a Mii location to an ID
// that isn't in this list points the Mii at a non-existent house and corrupts
// the save (the game can't find the actor → reset / fix-up on next launch).
// The WebUI editor we ported from refuses writes to unknown IDs for the same
// reason.
static bool HouseIdIsKnown(int houseId) {
    if (!gMapSav.loaded || houseId < 0) return false;
    int n = SaveEditor::ArraySize(gMapSav, HASH_MAP_HOUSE_MAPID);
    for (int i = 0; i < n; i++) {
        if (SaveEditor::GetIntAt(gMapSav, HASH_MAP_HOUSE_MAPID, i) == houseId) return true;
    }
    return false;
}
// Without the per-actor schema we can't know each house's exact room capacity,
// so we approximate it conservatively: the largest roomIndex currently in use
// in that house + 1. This means edits can re-shuffle existing rooms but never
// extend beyond rooms the save already references. Going past the real cap
// (e.g. writing room 4 into a 1-room HouseOneRoom) would corrupt the save.
static int HousingRoomCapHeuristic(int houseId) {
    int observed = -1;
    for (auto& m : gMiis) {
        int idx = m.slot - 1;
        int h = SaveEditor::GetIntAt(gMiiSav, HASH_HOUSE_MAPID, idx);
        if (h != houseId) continue;
        int r = SaveEditor::GetIntAt(gMiiSav, HASH_ROOM_INDEX, idx);
        if (r > observed) observed = r;
    }
    int cap = std::max(observed + 1, 1);
    if (cap > 8) cap = 8;
    return cap;
}

static std::string HouseNameForId(int houseId) {
    if (!gMapSav.loaded || houseId < 0) return "";
    int n = SaveEditor::ArraySize(gMapSav, HASH_MAP_HOUSE_MAPID);
    for (int i = 0; i < n; i++) {
        if (SaveEditor::GetIntAt(gMapSav, HASH_MAP_HOUSE_MAPID, i) == houseId) {
            return SaveEditor::GetWStr32At(gMapSav, HASH_MAP_HOUSE_NAME, i);
        }
    }
    return "";
}
static std::string HouseDisplayLabel(int houseId) {
    std::string n = HouseNameForId(houseId);
    if (!n.empty()) return n;
    char buf[32]; snprintf(buf, sizeof(buf), "House #%d", houseId);
    return buf;
}
// Each row is either a section header or a Mii row.
enum class HousingRowKind { Header, Resident, Unhoused };
struct HousingRow {
    HousingRowKind kind;
    int  miiSlotIdx;   // 0-based slot index into Mii.sav (valid for Resident/Unhoused)
    int  houseId;      // valid for Resident
    int  roomIdx;      // valid for Resident
    bool conflict;     // valid for Resident
    std::string label; // valid for Header
    int  headerHouseId;// valid for Header (>=0 for a house, -1 for "unhoused")
    int  headerCount;  // valid for Header
};
static std::vector<HousingRow> gHousingRows;
static int  gHousingSel    = 0;   // index into selectable rows (skipping headers)
static int  gHousingScroll = 0;   // first visible row index (into gHousingRows)
// Conflict resolution overlay
static bool gHousingConfirmOpen   = false;
static int  gHousingConfirmEditor = -1;   // mii slot idx that triggered the conflict
static int  gHousingConfirmOccup  = -1;   // mii slot idx of the displaced occupant
static int  gHousingConfirmPrevH  = -1;   // editor's house before the edit (for swap)
static int  gHousingConfirmPrevR  = -1;   // editor's room before the edit (for swap)
static int  gHousingConfirmHouse  = -1;   // contested house
static int  gHousingConfirmRoom   = -1;   // contested room
// Tap action ids
static void THousingSel(int rowIdx);
static void THousingEditHouse(int rowIdx);
static void THousingEditRoom(int rowIdx);
static void THousingEvict(int rowIdx);
static void THousingConfirm(int choice); // 0=swap 1=move 2=evict 3=cancel

// ── Card-UI state (pick-and-drop housing flow matching the WebUI) ─────────────
// gHousingPicked is the slot index of the Mii being moved (-1 = no pick).
// gHousingPickedPrevH/R remember where it lived so swap-with-occupant works.
static int gHousingPicked      = -1;
static int gHousingPickedPrevH = -1;
static int gHousingPickedPrevR = -1;

// Each clickable element in the housing panel registers a HousingNavItem so
// touch (HitAdd) and controller nav (Up/Down/Left/Right) share one selection
// model. Rebuilt every frame.
struct HousingNavItem {
    int kind;          // 0=unhoused chip  1=room slot (occupied)  2=room slot (empty)
                       // 3=card "add resident" / drop-here button
                       // 4=card vacate button
                       // 5=action-bar button (abAction tells which one)
                       // 6=card delete button
    int miiSlotIdx;    // valid for kinds 0, 1
    int houseId;       // valid for kinds 1..4
    int roomIdx;       // valid for kinds 1, 2, 3
    int abAction;      // valid for kind 5 (0=new-house, 1=evict-picked, 2=cancel-pick)
    int x, y, w, h;    // screen-space hit rect (used for 2D controller nav)
    bool conflict;     // two miis in the same (house, room)
};
static std::vector<HousingNavItem> gHousingNav;
static int gHousingNavSel  = 0;     // index into gHousingNav
static int gHousingScrollY = 0;     // pixel scroll inside the content area

static void THousingNavAct(int idx);

// Habits sub-tab state
struct HabitHashes { uint32_t isOwn, isChecked, state; };
static HabitHashes gHabitHashes[HABIT_COUNT];
static uint32_t    gHabitStateOwnH        = 0;
static uint32_t    gHabitStateUnownH      = 0;
static uint32_t    gHabitStateNeverOwnedH = 0;
static bool        gHabitHashesReady      = false;
static int         gHabitCatSel           = 0; // category index (0..HABIT_CAT_COUNT-1)
static int         gHabitItemSel          = 0; // index within current category
static int         gHabitScroll           = 0; // first visible row within current category

static void EnsureHabitHashes() {
    if (gHabitHashesReady) return;
    for (int i = 0; i < HABIT_COUNT; i++) {
        std::string base = std::string("Mii.Belongings.HabitOwnInfo.") + HABITS[i].name;
        gHabitHashes[i].isOwn     = SaveEditor::Hash((base + ".IsOwn").c_str());
        gHabitHashes[i].isChecked = SaveEditor::Hash((base + ".IsChecked").c_str());
        gHabitHashes[i].state     = SaveEditor::Hash((base + ".State").c_str());
    }
    gHabitStateOwnH        = SaveEditor::Hash("Own");
    gHabitStateUnownH      = SaveEditor::Hash("Unown");
    gHabitStateNeverOwnedH = SaveEditor::Hash("NeverOwned");
    gHabitHashesReady = true;
}

// Returns the global indices of habits in the given category.
static std::vector<int> HabitsInCategory(int cat) {
    std::vector<int> out;
    out.reserve(16);
    for (int i = 0; i < HABIT_COUNT; i++)
        if (HABITS[i].category == cat) out.push_back(i);
    return out;
}
static bool           gSocialExpanded  = false;
static int            gSocialScroll    = 0;
static int            gBlSel           = 0;   // selected item in Belongings (0–24)
static int            gBlScroll        = 0;   // visual-row scroll for Belongings
static bool           gBlPickerOpen    = false;
static int            gBlPickerSel     = 0;          // index into gBlFiltered
static int            gBlPickerScroll  = 0;
static std::vector<uint32_t>    gBlPickerHashes;
static std::vector<const char*> gBlPickerLabels;
static std::string              gBlFilter;          // active substring filter (lowercase)
static std::vector<int>         gBlFiltered;        // indices into gBlPickerHashes/Labels
static int            gMiiWordsSlotSel = 0;   // 0-11: selected word slot
static int            gMiiRelSel       = 0;   // selected row in filtered relations list
static int            gMiiRelScroll    = 0;   // first visible row in filtered relations list
static int            gMiiRelCount     = 0;   // total filtered rows for the selected Mii (updated each draw)
static int            gMiiRelPairIdx   = -1;  // actual pair array index (set during draw)
static bool           gMiiRelSelfA     = true; // selected Mii is 'A' in the pair

// WebUI standby prevention
static bool            gStandbyOff      = false;
static OnSwitchMode    gPrevOnSwitchMode = OnSwitchMode::UGC;

// Save editor first-launch warning
static bool         gSaveWarningAcked    = false;
static bool         gShowSaveWarning     = false;
static int          gSaveWarningCountdown = 0;
static OnSwitchMode gSaveWarningTarget   = OnSwitchMode::Player;

// Windows-style accelerating key repeat for directional navigation.
// Returns kDown OR any held directional buttons that should fire this frame.
static int s_navHold[64] = {};
static u64 NavRepeat(u64 kDown, u64 kHeld) {
    u64 result = kDown;
    static const u64 MASK =
        HidNpadButton_Up    | HidNpadButton_Down  |
        HidNpadButton_Left  | HidNpadButton_Right |
        HidNpadButton_ZL    | HidNpadButton_ZR    |
        HidNpadButton_StickLUp   | HidNpadButton_StickLDown  |
        HidNpadButton_StickLLeft | HidNpadButton_StickLRight |
        HidNpadButton_StickRUp   | HidNpadButton_StickRDown  |
        HidNpadButton_StickRLeft | HidNpadButton_StickRRight;
    for (u64 m = MASK, bit; m; m &= ~bit) {
        bit = m & (u64)(-(s64)m);
        int idx = __builtin_ctzll(bit);
        if (kHeld & bit) {
            int f = ++s_navHold[idx];
            if (f > 15 && f % 2 == 0) result |= bit;
        } else {
            s_navHold[idx] = 0;
        }
    }
    return result;
}

// ─── Touch handler implementations (need globals defined above) ───────────────
static void TSelUser(int idx) {
    if (idx>=0 && idx<(int)gUsers.size()) { gUserSel=idx; gSimKDown|=HidNpadButton_A; }
}
// TSelUgc defined after VISIBLE and LoadPreview are declared (below)
static void TSelBrowse(int idx) {
    if (idx>=0 && idx<(int)gBrowseEntries.size()) { gBrowseSel=idx; gSimKDown|=HidNpadButton_A; }
}
static void TAdjust(int delta) {
    // |delta| is the scale; sign is direction
    if (delta<0) { gTouchScale=-delta; gSimKDown|=HidNpadButton_Left; }
    else         { gTouchScale= delta; gSimKDown|=HidNpadButton_Right; }
    gBtnTouchActive = true;
}
static void TSelPlayerField(int idx) { gPlayerFieldSel=idx; }
static void TSetSkinTone(int idx) {
    if (!gPlayerSav.loaded) return;
    uint32_t h = PFHash(PLAYER_FIELDS[gPlayerFieldSel]);
    uint32_t cur = SaveEditor::GetUInt(gPlayerSav, h);
    if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
    SaveEditor::SetUInt(gPlayerSav, h, (uint32_t)idx);
    gPlayerSavDirty = true;
}
static void TSetIslandSize(int val) {
    if (!gPlayerSav.loaded) return;
    uint32_t h = PFHash(PLAYER_FIELDS[gPlayerFieldSel]);
    uint32_t cur = SaveEditor::GetAnyScalar(gPlayerSav, h);
    if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
    SaveEditor::SetAnyScalar(gPlayerSav, h, (uint32_t)val);
    gPlayerSavDirty = true;
}
static void TSelMiiStatsMii(int idx) {
    if (idx>=0 && idx<(int)gMiis.size()) { gMiiStatsMiiSel=idx; gMiiRelSel=0; gMiiRelScroll=0; }
}
// Tap on a TomodachiShare card. Sets gShareSel (NOT gMiiStatsMiiSel — that's
// the left-side mii list).
static void TSelShareMii(int idx) {
    if (idx>=0 && idx<(int)gShareMiis.size()) gShareSel = idx;
}
static void TSelMiiStatsField(int idx) { gMiiStatsFieldSel=idx; }
static void TSelMiiWordsSlot(int idx)  { gMiiWordsSlotSel=idx; gWordUndo.valid=false; }
static void TSelMiiRelation(int idx)   { gMiiRelSel=idx; }
static void TRelDateKbd(int idx)       { gRelDateKbdReq=true; gRelDatePairIdx=idx; }
static void TRelMeterAdj(int delta) {
    if (gMiiRelPairIdx < 0 || !gMiiSav.loaded) return;
    static const uint32_t H_BASE_RA  = 0x8b41897eu;
    static const uint32_t H_METER_RA = 0x42c2fc2fu;
    int myDir = gMiiRelSelfA ? gMiiRelPairIdx*2   : gMiiRelPairIdx*2+1;
    int otDir = gMiiRelSelfA ? gMiiRelPairIdx*2+1 : gMiiRelPairIdx*2;
    uint32_t myType = SaveEditor::GetEnumAt(gMiiSav, H_BASE_RA, myDir, 0x0784a8dcu);
    if (RelMeterFixed(myType)) return;
    auto adjM = [&](int dir) {
        int32_t m = SaveEditor::GetIntAt(gMiiSav, H_METER_RA, dir);
        m = std::max(0, std::min(100, m + delta));
        SaveEditor::SetIntAt(gMiiSav, H_METER_RA, dir, m);
    };
    adjM(myDir);
    adjM(otDir);
    gMiiSavDirty = true;
}

// Batch-convert every Unknown relation for the currently-selected Mii to
// Know. Meter is set to a natural in-game value (the discrete progress
// thresholds the game itself stamps for new acquaintances) and the "Since"
// timestamp is bumped to now so it looks like the friendship just started.
static void TRelBatchKnow(int /*unused*/) {
    if (!gMiiSav.loaded || gMiis.empty()) return;
    if (gMiiStatsMiiSel < 0 || gMiiStatsMiiSel >= (int)gMiis.size()) return;
    static const uint32_t H_IDA   = 0xf7420afbu;
    static const uint32_t H_IDB   = 0x4071f71cu;
    static const uint32_t H_BASE  = 0x8b41897eu;
    static const uint32_t H_METER = 0x42c2fc2fu;
    static const uint32_t H_TST   = 0x1a892e50u;
    static const uint32_t H_UNKNOWN = 0x0784a8dcu;
    static const uint32_t H_KNOW    = 0x354a0515u;
    // Natural meter values the game actually stamps for the Know type (0..200
    // range). Anything else technically works but isn't a value the game
    // would ever set on its own.
    static const int NATURAL[] = { 0, 20, 40, 60, 100, 120, 140 };
    static const int NATURAL_N = (int)(sizeof(NATURAL)/sizeof(NATURAL[0]));

    static bool s_seeded = false;
    if (!s_seeded) { std::srand((unsigned)std::time(nullptr)); s_seeded = true; }

    int miiSlotIdx = gMiis[gMiiStatsMiiSel].slot - 1;
    uint64_t nowTs = (uint64_t)std::time(nullptr);
    int n = SaveEditor::ArraySize(gMiiSav, H_IDA);
    int changed = 0;
    for (int pi = 0; pi < n; pi++) {
        int a = SaveEditor::GetIntAt(gMiiSav, H_IDA, pi);
        int b = SaveEditor::GetIntAt(gMiiSav, H_IDB, pi);
        if (a < 0 || b < 0) continue;
        if (a != miiSlotIdx && b != miiSlotIdx) continue;
        bool selfA = (a == miiSlotIdx);
        int myDir = selfA ? pi*2   : pi*2+1;
        int otDir = selfA ? pi*2+1 : pi*2;
        uint32_t myType = SaveEditor::GetEnumAt(gMiiSav, H_BASE, myDir, H_UNKNOWN);
        if (myType != H_UNKNOWN) continue;
        // Know is a symmetric type — set both directions to the same hash.
        SaveEditor::SetEnumAt(gMiiSav, H_BASE, myDir, H_KNOW);
        SaveEditor::SetEnumAt(gMiiSav, H_BASE, otDir, H_KNOW);
        int meter = NATURAL[std::rand() % NATURAL_N];
        SaveEditor::SetIntAt(gMiiSav, H_METER, myDir, meter);
        SaveEditor::SetIntAt(gMiiSav, H_METER, otDir, meter);
        SaveEditor::SetUInt64At(gMiiSav, H_TST, pi, nowTs);
        changed++;
    }
    if (changed > 0) gMiiSavDirty = true;
}
static void TSelHabitCat(int c) {
    if (c >= 0 && c < HABIT_CAT_COUNT) {
        gHabitCatSel = c; gHabitItemSel = 0; gHabitScroll = 0;
    }
}
// Apply the same logic the controller's A button does, but inline so the touch path
// doesn't depend on cross-frame gSimKDown ordering.
static void TSelHabitItem(int posInCat) {
    gHabitItemSel = posInCat;
    if (!gMiiSav.loaded || gMiis.empty() || gMiiStatsMiiSel >= (int)gMiis.size()) return;
    EnsureHabitHashes();
    int miiIdx = gMiis[gMiiStatsMiiSel].slot - 1;
    std::vector<int> items = HabitsInCategory(gHabitCatSel);
    if (posInCat < 0 || posInCat >= (int)items.size()) return;
    int chosen = items[posInCat];
    bool wasActive = SaveEditor::GetBoolAt(gMiiSav, gHabitHashes[chosen].isChecked, miiIdx, false);
    if (wasActive) {
        // Tapping the active gold box again: unset it (allows touch users to clear without keyboard).
        SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[chosen].isChecked, miiIdx, false);
    } else {
        // Make this habit the active one in its category (exclusive).
        for (int idx : items) {
            bool active = (idx == chosen);
            SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[idx].isChecked, miiIdx, active);
            if (active) {
                SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[idx].isOwn, miiIdx, true);
                SaveEditor::SetEnumAt(gMiiSav, gHabitHashes[idx].state, miiIdx, gHabitStateOwnH);
            }
        }
    }
    gMiiSavDirty = true;
    gMiiStatsMsg = std::string(wasActive?"Cleared: ":"Active: ") + HABITS[chosen].label;
    gMiiStatsMsgCol = COL_GREEN;
    gMiiStatsMsgFrames = 120;
}
static void TToggleHabitOwn(int posInCat) {
    gHabitItemSel = posInCat;
    if (!gMiiSav.loaded || gMiis.empty() || gMiiStatsMiiSel >= (int)gMiis.size()) return;
    EnsureHabitHashes();
    int miiIdx = gMiis[gMiiStatsMiiSel].slot - 1;
    std::vector<int> items = HabitsInCategory(gHabitCatSel);
    if (posInCat < 0 || posInCat >= (int)items.size()) return;
    int chosen = items[posInCat];
    bool isOwn = SaveEditor::GetBoolAt(gMiiSav, gHabitHashes[chosen].isOwn, miiIdx, false);
    if (isOwn) {
        uint32_t st = SaveEditor::GetEnumAt(gMiiSav, gHabitHashes[chosen].state, miiIdx, gHabitStateNeverOwnedH);
        SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[chosen].isOwn,     miiIdx, false);
        SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[chosen].isChecked, miiIdx, false);
        if (st == gHabitStateOwnH)
            SaveEditor::SetEnumAt(gMiiSav, gHabitHashes[chosen].state, miiIdx, gHabitStateUnownH);
    } else {
        SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[chosen].isOwn, miiIdx, true);
        SaveEditor::SetEnumAt(gMiiSav, gHabitHashes[chosen].state,  miiIdx, gHabitStateOwnH);
    }
    gMiiSavDirty = true;
    gMiiStatsMsg = std::string(isOwn?"Removed: ":"Added: ") + HABITS[chosen].label;
    gMiiStatsMsgCol = COL_GREEN;
    gMiiStatsMsgFrames = 120;
}
static void TSetSocialView(int v) {
    gMiiStatsSubTab = (MiiStatsSubTab)v;
    gSocialScroll = 0;
    if (gMiiStatsSubTab != MiiStatsSubTab::Social) gSocialExpanded = false;
}
static void TBLSel(int idx) {
    gBlSel = idx;
    if (idx < 21 && !gMiis.empty() && gMiiStatsMiiSel < (int)gMiis.size())
        BlOpenPicker(idx, gMiis[gMiiStatsMiiSel].slot - 1);
}
static void TBLPickSel(int idx) {
    if (idx >= 0 && idx < (int)gBlFiltered.size()) {
        gBlPickerSel = idx;
        gSimKDown |= HidNpadButton_A; // confirm immediately on tap
    }
}

// Rebuild gBlFiltered from gBlPickerLabels using gBlFilter (case-insensitive substring).
// Empty filter → identity (all indices). Always at least keeps "(none)"/"(empty)" sentinel at index 0.
static void BlRebuildFilter() {
    gBlFiltered.clear();
    if (gBlFilter.empty()) {
        gBlFiltered.reserve(gBlPickerLabels.size());
        for (int i = 0; i < (int)gBlPickerLabels.size(); i++) gBlFiltered.push_back(i);
        return;
    }
    // Always include sentinel at index 0 so the user can still clear the slot
    if (!gBlPickerLabels.empty()) gBlFiltered.push_back(0);
    for (int i = 1; i < (int)gBlPickerLabels.size(); i++) {
        const char* lbl = gBlPickerLabels[i];
        if (!lbl) continue;
        // case-insensitive substring match
        bool match = false;
        const char* hay = lbl;
        const char* needle = gBlFilter.c_str();
        size_t nlen = gBlFilter.size();
        for (const char* p = hay; *p; p++) {
            bool eq = true;
            for (size_t k = 0; k < nlen; k++) {
                char a = p[k]; if (!a) { eq = false; break; }
                char b = needle[k];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { eq = false; break; }
            }
            if (eq) { match = true; break; }
        }
        if (match) gBlFiltered.push_back(i);
    }
}
static void TAckSaveWarning(int) {
    gSaveWarningAcked = true; gShowSaveWarning = false; SaveConfig(); HttpServer::SetSaveWarnAcked(true);
    gOnSwitchMode = gSaveWarningTarget;
    if (gSaveWarningTarget == OnSwitchMode::Player && !gPlayerSav.loaded) {
        std::string err;
        if (!SaveEditor::Load(SAVE_PLAYER_SAV, gPlayerSav, err)) gPlayerMsg = "Load error: " + err;
    }
    if (gSaveWarningTarget == OnSwitchMode::MiiStats && !gMiiSav.loaded) {
        std::string err;
        if (!SaveEditor::Load(SAVE_MII_SAV, gMiiSav, err)) gMiiStatsMsg = "Load error: " + err;
        // Best-effort: Map.sav holds the per-house display name ("RoommateGroupName"
        // in the original schema). If it's missing or doesn't have the field, the
        // housing tab falls back to "House #N" labels.
        std::string mapErr;
        SaveEditor::Load(SAVE_MAP_SAV, gMapSav, mapErr);
    }
}
static void TPlayerKbd(int) { gSimKDown|=HidNpadButton_A; }
static void TMiiStatsKbd(int) { gMiiNameKbdReq = true; }

// ─── On-screen keyboard ───────────────────────────────────────────────────────
static std::string ShowKeyboard(const std::string& guide, const std::string& initial, int maxLen=32) {
    SwkbdConfig kbd;
    // swkbdCreate allocates the applet's internal storage — without it swkbdShow crashes.
    if (R_FAILED(swkbdCreate(&kbd, 0))) return initial;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, guide.c_str());
    swkbdConfigSetStringLenMin(&kbd, 0);
    swkbdConfigSetStringLenMax(&kbd, (u32)maxLen);
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    char buf[256] = {};
    Result rc = swkbdShow(&kbd, buf, sizeof(buf));
    swkbdClose(&kbd); // must be paired with every successful swkbdCreate
    return R_SUCCEEDED(rc) ? std::string(buf) : initial;
}

// ─── Housing helpers ──────────────────────────────────────────────────────────
static bool HousingAvailable() {
    if (!gMiiSav.loaded) return false;
    // Both arrays must exist as DT_IntArray (type 3).
    bool ok = false;
    for (const auto& e : gMiiSav.entries) {
        if (e.hash == HASH_HOUSE_MAPID && e.type == SaveEditor::DT_IntArray) { ok = true; break; }
    }
    if (!ok) return false;
    ok = false;
    for (const auto& e : gMiiSav.entries) {
        if (e.hash == HASH_ROOM_INDEX && e.type == SaveEditor::DT_IntArray) { ok = true; break; }
    }
    return ok;
}
static int HousingLowestFreeRoom(int houseId, int excludeMiiA, int excludeMiiB) {
    int cap = HousingRoomCapHeuristic(houseId);
    std::vector<bool> used(cap, false);
    int n = (int)gMiis.size();
    for (int k = 0; k < n; k++) {
        int idx = gMiis[k].slot - 1;
        if (idx == excludeMiiA || idx == excludeMiiB) continue;
        int h = SaveEditor::GetIntAt(gMiiSav, HASH_HOUSE_MAPID, idx);
        if (h != houseId) continue;
        int r = SaveEditor::GetIntAt(gMiiSav, HASH_ROOM_INDEX, idx);
        if (r >= 0 && r < cap) used[r] = true;
    }
    for (int r = 0; r < cap; r++) if (!used[r]) return r;
    return -1; // house full — caller should refuse the operation
}
// Returns the slot index of the mii currently at (house, room), or -1.
// Skips excludeMii.
static int HousingFindOccupant(int houseId, int room, int excludeMii) {
    int n = (int)gMiis.size();
    for (int k = 0; k < n; k++) {
        int idx = gMiis[k].slot - 1;
        if (idx == excludeMii) continue;
        int h = SaveEditor::GetIntAt(gMiiSav, HASH_HOUSE_MAPID, idx);
        int r = SaveEditor::GetIntAt(gMiiSav, HASH_ROOM_INDEX,  idx);
        if (h == houseId && r == room) return idx;
    }
    return -1;
}
static void HousingWriteLoc(int miiSlotIdx, int house, int room) {
    SaveEditor::SetIntAt(gMiiSav, HASH_HOUSE_MAPID, miiSlotIdx, house);
    SaveEditor::SetIntAt(gMiiSav, HASH_ROOM_INDEX,  miiSlotIdx, room);
    gMiiSavDirty = true;
}
// Rebuild the flat row list used by the housing panel.
// Groups by house id; appends an "Unhoused" section at the end.
static std::vector<int> HousingAllHouseIds(); // defined below
static void HousingRebuild() {
    gHousingRows.clear();
    if (!HousingAvailable() || gMiis.empty()) return;

    struct Resident { int idx; int room; };
    std::map<int, std::vector<Resident>> byHouse;
    std::vector<int> unhoused;
    // Pre-seed every known house from Map.sav so empty (vacated) houses still
    // show up in the editor — they're only pruned at save time. Without this
    // seed the rebuild only enumerates houses that currently have residents.
    for (int hid : HousingAllHouseIds()) byHouse[hid];
    for (const auto& m : gMiis) {
        int idx = m.slot - 1;
        int h = SaveEditor::GetIntAt(gMiiSav, HASH_HOUSE_MAPID, idx);
        int r = SaveEditor::GetIntAt(gMiiSav, HASH_ROOM_INDEX,  idx);
        if (h < 0) { unhoused.push_back(idx); continue; }
        byHouse[h].push_back({idx, r});
    }

    for (auto& kv : byHouse) {
        int hid = kv.first;
        auto& list = kv.second;
        std::sort(list.begin(), list.end(),
                  [](const Resident& a, const Resident& b){
                      return a.room != b.room ? a.room < b.room : a.idx < b.idx;
                  });
        // Conflict detection — pre-count room occupancy.
        std::map<int, int> roomCount;
        for (auto& r : list) roomCount[r.room]++;

        HousingRow hdr;
        hdr.kind = HousingRowKind::Header;
        hdr.headerHouseId = hid;
        hdr.headerCount = (int)list.size();
        char buf[160];
        std::string nm = HouseNameForId(hid);
        if (!nm.empty()) {
            snprintf(buf, sizeof(buf), "%s  —  %d resident%s",
                     nm.c_str(), (int)list.size(), list.size()==1 ? "" : "s");
        } else {
            snprintf(buf, sizeof(buf), "HOUSE #%d  —  %d resident%s",
                     hid, (int)list.size(), list.size()==1 ? "" : "s");
        }
        hdr.label = buf;
        gHousingRows.push_back(hdr);

        for (auto& r : list) {
            HousingRow row;
            row.kind = HousingRowKind::Resident;
            row.miiSlotIdx = r.idx;
            row.houseId = hid;
            row.roomIdx = r.room;
            row.conflict = (roomCount[r.room] > 1);
            gHousingRows.push_back(row);
        }
    }

    if (!unhoused.empty()) {
        HousingRow hdr;
        hdr.kind = HousingRowKind::Header;
        hdr.headerHouseId = -1;
        hdr.headerCount = (int)unhoused.size();
        char buf[96];
        snprintf(buf, sizeof(buf), "UNHOUSED  —  %d mii%s",
                 (int)unhoused.size(), unhoused.size()==1 ? "" : "s");
        hdr.label = buf;
        gHousingRows.push_back(hdr);
        for (int idx : unhoused) {
            HousingRow row;
            row.kind = HousingRowKind::Unhoused;
            row.miiSlotIdx = idx;
            row.houseId = -1;
            row.roomIdx = -1;
            row.conflict = false;
            gHousingRows.push_back(row);
        }
    }
}
static int HousingFirstSelectable() {
    for (int i = 0; i < (int)gHousingRows.size(); i++)
        if (gHousingRows[i].kind != HousingRowKind::Header) return i;
    return -1;
}
// Step selection up/down skipping header rows. Wraps around.
static void HousingStepSel(int dir) {
    int n = (int)gHousingRows.size();
    if (n == 0) return;
    int cur = std::max(0, std::min(gHousingSel, n - 1));
    for (int step = 0; step < n; step++) {
        cur = (cur + dir + n) % n;
        if (gHousingRows[cur].kind != HousingRowKind::Header) {
            gHousingSel = cur;
            return;
        }
    }
}
// After writing the editor's new location, check for a conflict with another mii
// in the same (house, room). If found, open the resolution overlay.
static void HousingMaybeOpenConflict(int editorSlotIdx, int prevHouse, int prevRoom) {
    int h = SaveEditor::GetIntAt(gMiiSav, HASH_HOUSE_MAPID, editorSlotIdx);
    int r = SaveEditor::GetIntAt(gMiiSav, HASH_ROOM_INDEX,  editorSlotIdx);
    if (h < 0 || r < 0) return;
    int occ = HousingFindOccupant(h, r, editorSlotIdx);
    if (occ < 0) return;
    gHousingConfirmOpen   = true;
    gHousingConfirmEditor = editorSlotIdx;
    gHousingConfirmOccup  = occ;
    gHousingConfirmPrevH  = prevHouse;
    gHousingConfirmPrevR  = prevRoom;
    gHousingConfirmHouse  = h;
    gHousingConfirmRoom   = r;
}
// Prompt the on-screen keyboard for a non-negative integer (-1 also accepted to clear).
// Returns the typed integer, or oldValue if the user cancelled or typed garbage.
static int HousingPromptInt(const char* guide, int oldValue, int maxLen=6) {
    char init[16];
    snprintf(init, sizeof(init), "%d", oldValue);
    std::string nv = ShowKeyboard(guide, init, maxLen);
    if (nv.empty()) return oldValue;
    bool neg = false; size_t i = 0;
    if (nv[0] == '-') { neg = true; i = 1; }
    if (i >= nv.size()) return oldValue;
    int v = 0;
    for (; i < nv.size(); i++) {
        char c = nv[i];
        if (c < '0' || c > '9') return oldValue;
        v = v * 10 + (c - '0');
    }
    return neg ? -v : v;
}

static void THousingSel(int rowIdx) {
    if (rowIdx < 0 || rowIdx >= (int)gHousingRows.size()) return;
    if (gHousingRows[rowIdx].kind == HousingRowKind::Header) return;
    gHousingSel = rowIdx;
}
static void THousingEditHouse(int rowIdx) {
    if (rowIdx < 0 || rowIdx >= (int)gHousingRows.size()) return;
    auto& row = gHousingRows[rowIdx];
    if (row.kind == HousingRowKind::Header) return;
    int idx = row.miiSlotIdx;
    int prevH = SaveEditor::GetIntAt(gMiiSav, HASH_HOUSE_MAPID, idx);
    int prevR = SaveEditor::GetIntAt(gMiiSav, HASH_ROOM_INDEX,  idx);
    int nh = HousingPromptInt("House id (-1 = unhoused)", prevH);
    if (nh == prevH) return;
    if (nh < 0) {
        HousingWriteLoc(idx, -1, -1);
    } else {
        int nr = (prevR < 0) ? 0 : prevR;
        HousingWriteLoc(idx, nh, nr);
        HousingMaybeOpenConflict(idx, prevH, prevR);
    }
    HousingRebuild();
}
static void THousingEditRoom(int rowIdx) {
    if (rowIdx < 0 || rowIdx >= (int)gHousingRows.size()) return;
    auto& row = gHousingRows[rowIdx];
    if (row.kind == HousingRowKind::Header) return;
    int idx = row.miiSlotIdx;
    int prevH = SaveEditor::GetIntAt(gMiiSav, HASH_HOUSE_MAPID, idx);
    int prevR = SaveEditor::GetIntAt(gMiiSav, HASH_ROOM_INDEX,  idx);
    int nr = HousingPromptInt("Room number (-1 = unhoused)", prevR);
    if (nr == prevR) return;
    if (nr < 0) {
        HousingWriteLoc(idx, -1, -1);
    } else if (prevH < 0) {
        // Editing room while unhoused: prompt for a house too.
        int nh = HousingPromptInt("House id for this mii", 0);
        if (nh < 0) return;
        HousingWriteLoc(idx, nh, nr);
        HousingMaybeOpenConflict(idx, prevH, prevR);
    } else {
        HousingWriteLoc(idx, prevH, nr);
        HousingMaybeOpenConflict(idx, prevH, prevR);
    }
    HousingRebuild();
}
static void THousingEvict(int rowIdx) {
    if (rowIdx < 0 || rowIdx >= (int)gHousingRows.size()) return;
    auto& row = gHousingRows[rowIdx];
    if (row.kind == HousingRowKind::Header || row.kind == HousingRowKind::Unhoused) return;
    HousingWriteLoc(row.miiSlotIdx, -1, -1);
    HousingRebuild();
}
static void THousingConfirm(int choice) {
    if (!gHousingConfirmOpen) return;
    int occ  = gHousingConfirmOccup;
    int edH  = gHousingConfirmHouse;
    int prvH = gHousingConfirmPrevH;
    int prvR = gHousingConfirmPrevR;
    int ed   = gHousingConfirmEditor;
    switch (choice) {
        case 0: // swap
            if (prvH >= 0 && prvR >= 0) HousingWriteLoc(occ, prvH, prvR);
            else                         HousingWriteLoc(occ, -1, -1);
            break;
        case 1: { // move occupant to next free room of the same house
            int fr = HousingLowestFreeRoom(edH, ed, occ);
            if (fr < 0) HousingWriteLoc(occ, -1, -1);
            else        HousingWriteLoc(occ, edH, fr);
            break;
        }
        case 2: // evict occupant
            HousingWriteLoc(occ, -1, -1);
            break;
        case 3: // cancel — leave the conflict (will be flagged visually)
        default:
            break;
    }
    gHousingConfirmOpen = false;
    HousingRebuild();
}

// ── Card-UI pick / drop helpers ──────────────────────────────────────────────
static void HousingDoCancel() {
    gHousingPicked      = -1;
    gHousingPickedPrevH = -1;
    gHousingPickedPrevR = -1;
}
static void HousingDoPick(int miiSlotIdx) {
    if (miiSlotIdx < 0) return;
    if (gHousingPicked == miiSlotIdx) { HousingDoCancel(); return; } // toggle
    gHousingPicked      = miiSlotIdx;
    gHousingPickedPrevH = SaveEditor::GetIntAt(gMiiSav, HASH_HOUSE_MAPID, miiSlotIdx);
    gHousingPickedPrevR = SaveEditor::GetIntAt(gMiiSav, HASH_ROOM_INDEX,  miiSlotIdx);
}
static void HousingDoDrop(int houseId, int room) {
    if (gHousingPicked < 0) return;
    int picked = gHousingPicked;
    int prvH = gHousingPickedPrevH, prvR = gHousingPickedPrevR;

    // Safety: refuse to write a Mii location to a house that doesn't exist in
    // Map.sav. Doing so corrupts the save because the game can't find the
    // matching house actor on the island.
    if (!HouseIdIsKnown(houseId)) {
        gMiiStatsMsg = "Can't move there: that house isn't on your island.";
        gMiiStatsMsgCol = COL_RED;
        gMiiStatsMsgFrames = 240;
        return;
    }
    int cap = HousingRoomCapHeuristic(houseId);
    if (room < 0 || room >= cap) {
        // Try to fall back to lowest free room in this house instead of
        // writing an out-of-range index.
        int fallback = HousingLowestFreeRoom(houseId, picked, -1);
        if (fallback < 0) {
            gMiiStatsMsg = "That house is full.";
            gMiiStatsMsgCol = COL_RED;
            gMiiStatsMsgFrames = 240;
            return;
        }
        room = fallback;
    }

    int occ = HousingFindOccupant(houseId, room, picked);
    HousingWriteLoc(picked, houseId, room);
    if (occ >= 0) {
        // Occupant gets pushed to the picked Mii's old slot if that's a valid
        // (known) house with a free room; otherwise the lowest free room in
        // the same destination house; otherwise unhoused.
        bool placed = false;
        if (prvH >= 0 && prvR >= 0 && !(prvH == houseId && prvR == room) &&
            HouseIdIsKnown(prvH) && prvR < HousingRoomCapHeuristic(prvH)) {
            HousingWriteLoc(occ, prvH, prvR);
            placed = true;
        }
        if (!placed) {
            int free = HousingLowestFreeRoom(houseId, picked, occ);
            if (free < 0) HousingWriteLoc(occ, -1, -1);
            else          HousingWriteLoc(occ, houseId, free);
        }
    }
    HousingDoCancel();
    HousingRebuild();
}
static void HousingDoEvictPicked() {
    if (gHousingPicked < 0) return;
    HousingWriteLoc(gHousingPicked, -1, -1);
    HousingDoCancel();
    HousingRebuild();
}
static void HousingDoVacate(int houseId) {
    for (auto& m : gMiis) {
        int idx = m.slot - 1;
        int h = SaveEditor::GetIntAt(gMiiSav, HASH_HOUSE_MAPID, idx);
        if (h == houseId) HousingWriteLoc(idx, -1, -1);
    }
    HousingRebuild();
}
// Set true whenever we mutate gMapSav so the back-prompt save flow writes it
// back to disk. Declared here so the Housing helpers below (which clear house
// actors) can flip it; the Map tab also flips it from its own input handlers.
static bool gMapDirty = false;

// ── House deletion (Map.sav writes) ──────────────────────────────────────────
// Mirrors upstream's deleteHouseByMapId in ltd-save-editor exactly so we
// can't corrupt saves:
//
//   * House.MapId is treated as READ-ONLY — upstream never writes to it; the
//     array is the canonical "this id maps to a real plot" source-of-truth.
//   * MapObject.ActorKey is the only field we touch on delete: setting it to
//     0 marks that slot as empty (same convention liveRows uses to filter).
//     LinkedMapId, position, rotation, etc. stay as-is, matching upstream's
//     clearSlot(idx) → setActor(idx, 0).
//   * Only group == ACTOR_HOUSE (HouseDollHouse / HouseOneRoom) counts as a
//     deletable house. UGC houses (HouseUgcXX_YY, group == ACTOR_UGC) are
//     decorative buildings, not residences — upstream's isHouseActor excludes
//     them so we do too.
//
// "Auto-cleanup on save" calls the same delete for every house with zero
// residents at commit time, so a vacated-and-not-repopulated house drops on
// save even if the user didn't click Delete.
static int HousingMapObjectSlotForHouse(int houseId) {
    if (!gMapSav.loaded || houseId < 0) return -1;
    uint32_t hAct  = SaveEditor::Hash(MapKeys::ActorKey);
    uint32_t hLink = SaveEditor::Hash(MapKeys::LinkedMapId);
    int n = SaveEditor::ArraySize(gMapSav, hAct);
    for (int i = 0; i < n; i++) {
        uint32_t a = SaveEditor::GetUIntAt(gMapSav, hAct, i, 0);
        if (!a) continue;
        const MapData::ActorInfo* info = MapData::ActorLookup(a);
        if (!info || info->group != MapData::ACTOR_HOUSE) continue;
        int link = SaveEditor::GetIntAt(gMapSav, hLink, i, -1);
        if (link == houseId) return i;
    }
    return -1;
}
// Live list of house mapIds on the island. Mirrors upstream's pattern:
// enumerate MapObject slots whose ActorKey is a real house actor (ACTOR_HOUSE)
// and collect their LinkedMapId. Slots with ActorKey == 0 are skipped, so once
// a house is deleted (its ActorKey cleared) it auto-vanishes from this list.
// We deliberately don't read House.MapId for this — that array is the static
// plot registry and may include legacy ids whose actor has already been wiped.
static std::vector<int> HousingAllHouseIds() {
    std::vector<int> out;
    if (!gMapSav.loaded) return out;
    uint32_t hAct  = SaveEditor::Hash(MapKeys::ActorKey);
    uint32_t hLink = SaveEditor::Hash(MapKeys::LinkedMapId);
    int n = SaveEditor::ArraySize(gMapSav, hAct);
    for (int i = 0; i < n; i++) {
        uint32_t a = SaveEditor::GetUIntAt(gMapSav, hAct, i, 0);
        if (!a) continue;
        const MapData::ActorInfo* info = MapData::ActorLookup(a);
        if (!info || info->group != MapData::ACTOR_HOUSE) continue;
        int link = SaveEditor::GetIntAt(gMapSav, hLink, i, -1);
        if (link >= 0) out.push_back(link);
    }
    return out;
}
static int HousingResidentCount(int houseId) {
    if (!gMiiSav.loaded || houseId < 0) return 0;
    int count = 0;
    for (auto& m : gMiis) {
        int idx = m.slot - 1;
        int h = SaveEditor::GetIntAt(gMiiSav, HASH_HOUSE_MAPID, idx);
        if (h == houseId) count++;
    }
    return count;
}
// Wipe the house actor in Map.sav. Caller is responsible for kicking residents
// first so no Mii is left pointing at a houseId whose actor was just cleared.
// House.MapId is left untouched — that mirrors upstream and avoids confusing
// the game's load-time integrity check.
static bool HousingScrubMapHouse(int houseId) {
    if (!gMapSav.loaded || houseId < 0) return false;
    int mapSlot = HousingMapObjectSlotForHouse(houseId);
    if (mapSlot < 0) return false;
    uint32_t hAct = SaveEditor::Hash(MapKeys::ActorKey);
    SaveEditor::SetUIntAt(gMapSav, hAct, mapSlot, 0);
    gMapDirty = true;
    return true;
}
static void HousingDoDeleteHouse(int houseId) {
    HousingDoVacate(houseId);     // kick residents (Mii.sav)
    HousingScrubMapHouse(houseId); // clear MapObject.ActorKey only
    HousingRebuild();
}
// Called from the save flow: every empty house gets removed from Map.sav.
static void HousingCleanupEmptyHouses() {
    if (!gMapSav.loaded || !gMiiSav.loaded) return;
    for (int hid : HousingAllHouseIds()) {
        if (HousingResidentCount(hid) == 0) HousingScrubMapHouse(hid);
    }
}
// Houses are placed on the island in-game (Map.sav stores the actor entries),
// not created from the save editor. We can only assign Miis to houses that
// already exist; trying to write an arbitrary new house id would point the Mii
// at a non-existent actor and corrupt the save. This used to be wired to a
// "New House" button — that's now removed.
static void HousingDoNewHouse() {
    gMiiStatsMsg = "Houses can only be placed in-game on your island.";
    gMiiStatsMsgCol = COL_DIM;
    gMiiStatsMsgFrames = 240;
}
// Tap entry-point: route the click to the right action based on the nav item
// stored at idx in gHousingNav.
static void THousingNavAct(int idx) {
    if (idx < 0 || idx >= (int)gHousingNav.size()) return;
    gHousingNavSel = idx;
    const auto& it = gHousingNav[idx];
    switch (it.kind) {
        case 0: // unhoused chip
            HousingDoPick(it.miiSlotIdx);
            break;
        case 1: // occupied slot
            if (gHousingPicked < 0 || gHousingPicked == it.miiSlotIdx) {
                HousingDoPick(it.miiSlotIdx);
            } else {
                HousingDoDrop(it.houseId, it.roomIdx);
            }
            break;
        case 2: // empty slot — drop here if picked, else hint
            if (gHousingPicked >= 0) HousingDoDrop(it.houseId, it.roomIdx);
            break;
        case 3: // add-resident / drop-into-next-room
            if (gHousingPicked >= 0) {
                int free = HousingLowestFreeRoom(it.houseId, gHousingPicked, -1);
                if (free < 0) free = 0;
                HousingDoDrop(it.houseId, free);
            } else {
                gMiiStatsMsg = "Pick a Mii first, then tap + add.";
                gMiiStatsMsgCol = COL_DIM;
                gMiiStatsMsgFrames = 180;
            }
            break;
        case 4: // vacate — kick all residents but keep the house
            HousingDoVacate(it.houseId);
            break;
        case 6: // delete — kick residents AND remove the house from Map.sav
            HousingDoDeleteHouse(it.houseId);
            break;
        case 5: // action-bar
            if      (it.abAction == 0) HousingDoNewHouse();
            else if (it.abAction == 1) HousingDoEvictPicked();
            else if (it.abAction == 2) HousingDoCancel();
            break;
    }
}

// ─── TomodachiShare worker thread ────────────────────────────────────────────
// The worker runs one ShareJob at a time. Main thread submits via ShareSubmitJob
// and polls via ShareTryConsumeJob. Curl performs the actual HTTPS request.
static size_t ShareWriteFn(void* ptr, size_t sz, size_t n, void* userp) {
    auto* buf = (std::vector<uint8_t>*)userp;
    size_t total = sz * n;
    buf->insert(buf->end(), (uint8_t*)ptr, (uint8_t*)ptr + total);
    return total;
}
static void ShareWorkerFn(void*) {
    while (true) {
        ShareJob job;
        mutexLock(&gShareJobMutex);
        while (gShareJobQueue.empty() && !gShareWorkerStop)
            condvarWait(&gShareJobCond, &gShareJobMutex);
        if (gShareWorkerStop) { mutexUnlock(&gShareJobMutex); break; }
        job = std::move(gShareJobQueue.front());
        gShareJobQueue.erase(gShareJobQueue.begin());
        mutexUnlock(&gShareJobMutex);

        std::vector<uint8_t> body;
        std::string err;
        long status = 0;
        CURL* c = curl_easy_init();
        if (!c) { err = "curl_easy_init failed"; }
        else {
            curl_easy_setopt(c, CURLOPT_URL, job.url.c_str());
            curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(c, CURLOPT_USERAGENT, "TomoToolNX/share");
            curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, ShareWriteFn);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
            CURLcode res = curl_easy_perform(c);
            if (res != CURLE_OK) err = std::string("network: ") + curl_easy_strerror(res);
            else curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
            curl_easy_cleanup(c);
        }
        job.body   = std::move(body);
        job.error  = err;
        job.status = status;
        job.done   = true;
        mutexLock(&gShareJobMutex);
        gShareJobDone.push_back(std::move(job));
        mutexUnlock(&gShareJobMutex);
    }
}
static void ShareWorkerStart() {
    if (gShareWorkerRunning) return;
    mutexInit(&gShareJobMutex);
    condvarInit(&gShareJobCond);
    gShareWorkerStop = false;
    Result r = threadCreate(&gShareWorker, ShareWorkerFn, nullptr, nullptr, 0x4000, 0x2C, -2);
    if (R_FAILED(r)) return;
    threadStart(&gShareWorker);
    gShareWorkerRunning = true;
}
static void ShareWorkerStopFn() {
    if (!gShareWorkerRunning) return;
    mutexLock(&gShareJobMutex);
    gShareWorkerStop = true;
    condvarWakeAll(&gShareJobCond);
    mutexUnlock(&gShareJobMutex);
    threadWaitForExit(&gShareWorker);
    threadClose(&gShareWorker);
    gShareWorkerRunning = false;
}
static void ShareSubmitJob(ShareJobKind kind, const std::string& url, int targetMiiIdx) {
    if (!gShareWorkerRunning) ShareWorkerStart();
    mutexLock(&gShareJobMutex);
    ShareJob j{};
    j.kind         = kind;
    j.url          = url;
    j.targetMiiIdx = targetMiiIdx;
    j.status       = 0;
    j.done         = false;
    // User-initiated requests jump the queue; drop pending thumb fetches.
    if (kind != ShareJobKind::Thumb) {
        gShareJobQueue.erase(
            std::remove_if(gShareJobQueue.begin(), gShareJobQueue.end(),
                           [](const ShareJob& q){ return q.kind == ShareJobKind::Thumb; }),
            gShareJobQueue.end());
        gShareJobQueue.insert(gShareJobQueue.begin(), std::move(j));
    } else {
        gShareJobQueue.push_back(std::move(j));
    }
    condvarWakeOne(&gShareJobCond);
    mutexUnlock(&gShareJobMutex);
}
// Returns true if a completed job was consumed; out contains the result.
static bool ShareTryConsumeJob(ShareJob& out) {
    mutexLock(&gShareJobMutex);
    bool ready = !gShareJobDone.empty();
    if (ready) {
        out = std::move(gShareJobDone.front());
        gShareJobDone.erase(gShareJobDone.begin());
    }
    mutexUnlock(&gShareJobMutex);
    return ready;
}

// Build a URL for the local-side proxy endpoints — we hit api.tomodachishare.com
// directly here since we're the C++ side; no need for the localhost proxy.
static std::string ShareUrlList() {
    std::string url = "https://api.tomodachishare.com/api/mii/list?page=" + std::to_string(gSharePage) + "&limit=24";
    static const char* SORTS[] = {"newest","likes","oldest"};
    url += "&sort=";
    url += SORTS[std::max(0,std::min(2,gShareSort))];
    // tomodachishare's API requires q to be at least 2 characters (the
    // shared zod schema rejects shorter inputs with HTTP 400). Drop the
    // parameter entirely when the user has only typed one letter so they
    // see "no filter" results instead of an error banner.
    if (gShareQuery.size() >= 2) {
        CURL* esc = curl_easy_init();
        char* enc = esc ? curl_easy_escape(esc, gShareQuery.c_str(), (int)gShareQuery.size()) : nullptr;
        url += "&q=";
        url += enc ? enc : "";
        if (enc) curl_free(enc);
        if (esc) curl_easy_cleanup(esc);
    }
    if (gShareFromSavOnly) url += "&isFromSaveFile=true";
    return url;
}
static std::string ShareUrlImage(int miiId, const char* type) {
    return std::string("https://api.tomodachishare.com/mii/") + std::to_string(miiId) + "/image?type=" + type;
}
static std::string ShareUrlDownload(int miiId) {
    return std::string("https://api.tomodachishare.com/mii/") + std::to_string(miiId) + "/download";
}

// Minimal JSON parser for the subset of fields we care about in the list response.
// Avoids pulling in a full JSON library. Treats input as UTF-8.
static void ShareJsonSkipWS(const std::string& s, size_t& p) {
    while (p < s.size() && (s[p]==' '||s[p]=='\t'||s[p]=='\n'||s[p]=='\r')) p++;
}
static bool ShareJsonReadString(const std::string& s, size_t& p, std::string& out) {
    ShareJsonSkipWS(s, p);
    if (p >= s.size() || s[p] != '"') return false;
    p++;
    out.clear();
    while (p < s.size() && s[p] != '"') {
        char c = s[p++];
        if (c == '\\' && p < s.size()) {
            char e = s[p++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'u': {
                    if (p + 4 > s.size()) return false;
                    unsigned cp = 0;
                    for (int i = 0; i < 4; i++) {
                        char hx = s[p++];
                        cp <<= 4;
                        if (hx >= '0' && hx <= '9') cp |= (hx - '0');
                        else if (hx >= 'a' && hx <= 'f') cp |= 10 + (hx - 'a');
                        else if (hx >= 'A' && hx <= 'F') cp |= 10 + (hx - 'A');
                        else return false;
                    }
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) { out += (char)(0xC0|(cp>>6)); out += (char)(0x80|(cp&0x3F)); }
                    else { out += (char)(0xE0|(cp>>12)); out += (char)(0x80|((cp>>6)&0x3F)); out += (char)(0x80|(cp&0x3F)); }
                    break;
                }
                default: out += e; break;
            }
        } else out += c;
    }
    if (p < s.size() && s[p] == '"') { p++; return true; }
    return false;
}
static bool ShareJsonReadInt(const std::string& s, size_t& p, long& out) {
    ShareJsonSkipWS(s, p);
    size_t start = p;
    if (p < s.size() && (s[p]=='-'||s[p]=='+')) p++;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') p++;
    if (p == start) return false;
    char* end = nullptr;
    const char* cstr = s.c_str() + start;
    out = std::strtol(cstr, &end, 10);
    if (end == cstr) return false;
    return true;
}
// Find a top-level "key": value inside the substring [start, end) of s and
// fill `valueStart` / `valueEnd` with the value's range.
static bool ShareJsonFindKey(const std::string& s, size_t start, size_t end, const char* key, size_t& vStart, size_t& vEnd) {
    int depth = 0;
    bool inStr = false; bool esc = false;
    std::string needle = std::string("\"") + key + "\"";
    for (size_t i = start; i < end; i++) {
        char c = s[i];
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') {
            if (depth == 1 && i + needle.size() <= end &&
                s.compare(i, needle.size(), needle) == 0) {
                size_t j = i + needle.size();
                while (j < end && (s[j]==' '||s[j]=='\t'||s[j]=='\n'||s[j]=='\r')) j++;
                if (j < end && s[j] == ':') {
                    j++;
                    while (j < end && (s[j]==' '||s[j]=='\t'||s[j]=='\n'||s[j]=='\r')) j++;
                    vStart = j;
                    int d2 = 0; bool inS2 = false; bool esc2 = false;
                    size_t k = j;
                    for (; k < end; k++) {
                        char cc = s[k];
                        if (inS2) { if (esc2) esc2 = false; else if (cc=='\\') esc2 = true; else if (cc=='"') inS2 = false; continue; }
                        if (cc == '"') { inS2 = true; continue; }
                        if (cc == '{' || cc == '[') d2++;
                        else if (cc == '}' || cc == ']') { if (d2 == 0) break; d2--; }
                        else if (cc == ',' && d2 == 0) break;
                    }
                    vEnd = k;
                    return true;
                }
            }
            inStr = true;
        }
        else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') depth--;
    }
    return false;
}
// Parse the list JSON into gShareMiis + totalCount + lastPage. Best-effort.
static void ShareParseListJson(const std::string& s) {
    gShareMiis.clear();
    gShareTotal = 0;
    gShareLastPage = 1;
    if (s.empty()) return;
    // miis array
    size_t mStart = 0, mEnd = 0;
    if (!ShareJsonFindKey(s, 0, s.size(), "miis", mStart, mEnd)) return;
    // skip '['
    size_t p = mStart;
    while (p < mEnd && (s[p]==' '||s[p]=='\t'||s[p]=='\n'||s[p]=='\r')) p++;
    if (p >= mEnd || s[p] != '[') return;
    p++;
    while (p < mEnd) {
        while (p < mEnd && (s[p]==' '||s[p]=='\t'||s[p]=='\n'||s[p]=='\r'||s[p]==',')) p++;
        if (p >= mEnd || s[p] != '{') break;
        // find matching '}'
        size_t objStart = p;
        int depth = 0; bool inStr = false; bool esc = false;
        while (p < mEnd) {
            char c = s[p++];
            if (inStr) { if (esc) esc = false; else if (c=='\\') esc = true; else if (c=='"') inStr = false; continue; }
            if (c == '"') inStr = true;
            else if (c == '{') depth++;
            else if (c == '}') { depth--; if (depth == 0) break; }
        }
        size_t objEnd = p;
        ShareMii m{}; m.id = 0; m.likes = 0; m.fromSav = false; m.allowCopying = false;
        m.thumb = nullptr; m.thumbRequested = false; m.thumbReady = false;
        size_t vS, vE;
        if (ShareJsonFindKey(s, objStart, objEnd, "id", vS, vE)) { long v; size_t pp = vS; if (ShareJsonReadInt(s, pp, v)) m.id = (int)v; }
        if (ShareJsonFindKey(s, objStart, objEnd, "name", vS, vE)) { size_t pp = vS; ShareJsonReadString(s, pp, m.name); }
        if (ShareJsonFindKey(s, objStart, objEnd, "platform", vS, vE)) { size_t pp = vS; ShareJsonReadString(s, pp, m.platform); }
        if (ShareJsonFindKey(s, objStart, objEnd, "gender", vS, vE)) { size_t pp = vS; ShareJsonReadString(s, pp, m.gender); }
        if (ShareJsonFindKey(s, objStart, objEnd, "likeCount", vS, vE)) { long v; size_t pp = vS; if (ShareJsonReadInt(s, pp, v)) m.likes = (int)v; }
        if (ShareJsonFindKey(s, objStart, objEnd, "user", vS, vE)) {
            size_t uS, uE;
            if (ShareJsonFindKey(s, vS, vE, "name", uS, uE)) { size_t pp = uS; ShareJsonReadString(s, pp, m.author); }
        }
        if (m.id > 0) gShareMiis.push_back(std::move(m));
    }
    size_t tS, tE;
    if (ShareJsonFindKey(s, 0, s.size(), "totalCount", tS, tE)) { long v; size_t pp = tS; if (ShareJsonReadInt(s, pp, v)) gShareTotal = (int)v; }
    if (ShareJsonFindKey(s, 0, s.size(), "lastPage", tS, tE)) { long v; size_t pp = tS; if (ShareJsonReadInt(s, pp, v)) gShareLastPage = (int)std::max(1L, v); }
}
// Drop only the per-entry runtime state; textures live in gShareThumbCache.
static void ShareFreeThumbs() {
    for (auto& m : gShareMiis) {
        m.thumb = nullptr;
        m.thumbReady = false;
        m.thumbRequested = false;
        m.thumbBytes.clear();
    }
}

// Thumb-cache helpers. The cache owns every texture it stores; ShareMii
// just holds a non-owning pointer. Evicts oldest entry when over capacity.
static void ShareThumbCachePut(int id, SDL_Texture* tex) {
    if (id <= 0 || !tex) return;
    auto it = gShareThumbCache.find(id);
    if (it != gShareThumbCache.end()) {
        if (it->second != tex) SDL_DestroyTexture(it->second);
        it->second = tex;
    } else {
        while (gShareThumbCacheLRU.size() >= kShareThumbCacheMax) {
            int oldId = gShareThumbCacheLRU.front();
            gShareThumbCacheLRU.erase(gShareThumbCacheLRU.begin());
            auto eit = gShareThumbCache.find(oldId);
            if (eit != gShareThumbCache.end()) {
                if (eit->second) SDL_DestroyTexture(eit->second);
                gShareThumbCache.erase(eit);
            }
        }
        gShareThumbCache[id] = tex;
    }
    auto lit = std::find(gShareThumbCacheLRU.begin(), gShareThumbCacheLRU.end(), id);
    if (lit != gShareThumbCacheLRU.end()) gShareThumbCacheLRU.erase(lit);
    gShareThumbCacheLRU.push_back(id);
}
static SDL_Texture* ShareThumbCacheGet(int id) {
    auto it = gShareThumbCache.find(id);
    if (it == gShareThumbCache.end()) return nullptr;
    auto lit = std::find(gShareThumbCacheLRU.begin(), gShareThumbCacheLRU.end(), id);
    if (lit != gShareThumbCacheLRU.end()) {
        gShareThumbCacheLRU.erase(lit);
        gShareThumbCacheLRU.push_back(id);
    }
    return it->second;
}

// Build a cache key from the current list-query parameters. Two miis with the
// same id can appear on different pages depending on sort, so we include all
// the parameters that affect ordering.
static std::string ShareListCacheKey(int page) {
    char head[64];
    snprintf(head, sizeof(head), "%d|%d|%d|", page, gShareSort, gShareFromSavOnly?1:0);
    return std::string(head) + gShareQuery;
}
static void ShareListCacheStore(const std::string& key, const ShareListSnapshot& snap) {
    auto it = gShareListCache.find(key);
    if (it != gShareListCache.end()) it->second = snap;
    else {
        while (gShareListCacheLRU.size() >= kShareListCacheMax) {
            gShareListCache.erase(gShareListCacheLRU.front());
            gShareListCacheLRU.erase(gShareListCacheLRU.begin());
        }
        gShareListCache[key] = snap;
    }
    auto lit = std::find(gShareListCacheLRU.begin(), gShareListCacheLRU.end(), key);
    if (lit != gShareListCacheLRU.end()) gShareListCacheLRU.erase(lit);
    gShareListCacheLRU.push_back(key);
}
static const ShareListSnapshot* ShareListCacheLookup(const std::string& key) {
    auto it = gShareListCache.find(key);
    if (it == gShareListCache.end()) return nullptr;
    auto lit = std::find(gShareListCacheLRU.begin(), gShareListCacheLRU.end(), key);
    if (lit != gShareListCacheLRU.end()) {
        gShareListCacheLRU.erase(lit);
        gShareListCacheLRU.push_back(key);
    }
    return &it->second;
}
// Wire up gShareMiis after restoring from a list snapshot: re-attach any
// thumb textures that are still in the thumb cache so they don't have to
// be re-fetched.
static void ShareRehydrateThumbsFromCache() {
    for (auto& m : gShareMiis) {
        SDL_Texture* t = ShareThumbCacheGet(m.id);
        if (t) {
            m.thumb = t;
            m.thumbRequested = true;
            m.thumbReady = false;
        } else {
            m.thumb = nullptr;
            m.thumbRequested = false;
            m.thumbReady = false;
            m.thumbBytes.clear();
        }
    }
}
static void ShareFreeDetail() {
    if (gShareDetailTex) { SDL_DestroyTexture(gShareDetailTex); gShareDetailTex = nullptr; }
    gShareDetailBytes.clear();
    gShareDetailReady = false;
    gShareDetailErr.clear();
}
static SDL_Texture* SharePngToTexture(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return nullptr;
    SDL_RWops* rw = SDL_RWFromConstMem(bytes.data(), (int)bytes.size());
    if (!rw) return nullptr;
    SDL_Surface* surf = IMG_Load_RW(rw, 1);
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(gRen, surf);
    SDL_FreeSurface(surf);
    return tex;
}
static void ShareSetStatus(const std::string& msg, SDL_Color c, int frames=180) {
    gShareStatusMsg = msg; gShareStatusCol = c; gShareStatusFrames = frames;
}
static void ShareLoadPage() {
    ShareFreeThumbs();
    gShareMiis.clear();
    gShareSel = 0; gShareScroll = 0;
    // Try the cache before hitting the API — flipping back to a page you
    // already visited should be instant.
    const ShareListSnapshot* hit = ShareListCacheLookup(ShareListCacheKey(gSharePage));
    if (hit) {
        gShareMiis    = hit->miis;
        gShareTotal   = hit->total;
        gShareLastPage= hit->lastPage;
        gShareListLoading = false;
        ShareRehydrateThumbsFromCache();
        return;
    }
    gShareListLoading = true;
    ShareSetStatus("loading…", {180,180,180,255}, 600);
    ShareSubmitJob(ShareJobKind::ListPage, ShareUrlList(), -1);
}
static void ShareRequestThumb(int idx) {
    if (idx < 0 || idx >= (int)gShareMiis.size()) return;
    auto& m = gShareMiis[idx];
    if (m.thumbRequested) return;
    // Check the thumb cache first — re-attach the texture and skip the fetch.
    SDL_Texture* cached = ShareThumbCacheGet(m.id);
    if (cached) {
        m.thumb = cached;
        m.thumbRequested = true;
        return;
    }
    m.thumbRequested = true;
    ShareSubmitJob(ShareJobKind::Thumb, ShareUrlImage(m.id, "mii"), idx);
}
static void ShareOpenDetail() {
    if (gShareSel < 0 || gShareSel >= (int)gShareMiis.size()) return;
    gShareDetailOpen = true;
    ShareFreeDetail();
    // Default the import slot to the slot the user has selected in the rest
    // of the Mii tab, falling back to slot 1.
    if (!gMiis.empty() && gMiiStatsMiiSel >= 0 && gMiiStatsMiiSel < (int)gMiis.size())
        gShareImportSlot = gMiis[gMiiStatsMiiSel].slot;
    else
        gShareImportSlot = 1;
    ShareSetStatus("loading mii image…", {180,180,180,255}, 600);
    ShareSubmitJob(ShareJobKind::Detail, ShareUrlImage(gShareMiis[gShareSel].id, "mii"), gShareSel);
}
static void ShareCloseDetail() {
    gShareDetailOpen = false;
    ShareFreeDetail();
}
static void ShareDoImport() {
    if (gShareSel < 0 || gShareSel >= (int)gShareMiis.size()) return;
    int slot = gShareImportSlot;
    if (slot < 1 || slot > MII_MAX_SLOTS) { ShareSetStatus("bad slot", {220,80,80,255}); return; }
    ShareSetStatus("downloading…", {180,180,180,255}, 600);
    ShareSubmitJob(ShareJobKind::Download, ShareUrlDownload(gShareMiis[gShareSel].id), gShareSel);
}
static void TShareDoImport(int)     { ShareDoImport(); }
static void TShareCloseDetail(int)  { ShareCloseDetail(); }
static void TShareImportSlotPrev(int) {
    gShareImportSlot = (gShareImportSlot > 1) ? gShareImportSlot - 1 : MII_MAX_SLOTS;
}
static void TShareImportSlotNext(int) {
    gShareImportSlot = (gShareImportSlot < MII_MAX_SLOTS) ? gShareImportSlot + 1 : 1;
}
// Drain whatever the worker has finished. Called once per frame from the
// MiiStats input/draw path while the Browse sub-tab is active.
static void SharePump() {
    ShareJob j;
    while (ShareTryConsumeJob(j)) {
        if (j.kind == ShareJobKind::ListPage) {
            gShareListLoading = false;
            if (!j.error.empty() || j.status != 200) {
                std::string msg = j.error.empty() ? ("HTTP " + std::to_string(j.status)) : j.error;
                ShareSetStatus("Failed: " + msg, {220,80,80,255}, 300);
            } else {
                std::string s((const char*)j.body.data(), j.body.size());
                ShareParseListJson(s);
                // Re-use already-decoded thumb textures from earlier visits
                // before queuing fresh HTTP fetches.
                ShareRehydrateThumbsFromCache();
                // If the user got pushed past the end (e.g. rapid ZR presses),
                // clamp and re-fetch so we never display "0 miis · page 6 / 1".
                if (gSharePage > gShareLastPage && gShareLastPage >= 1) {
                    gSharePage = gShareLastPage;
                    ShareLoadPage();
                    continue;
                }
                // Snapshot this page so the next visit skips the network.
                {
                    ShareListSnapshot snap;
                    snap.miis     = gShareMiis;
                    snap.total    = gShareTotal;
                    snap.lastPage = gShareLastPage;
                    // Strip live texture pointers — the snapshot is parameter
                    // data only; thumbs live in the thumb cache.
                    for (auto& sm : snap.miis) {
                        sm.thumb = nullptr;
                        sm.thumbReady = false;
                        sm.thumbRequested = false;
                        sm.thumbBytes.clear();
                    }
                    ShareListCacheStore(ShareListCacheKey(gSharePage), snap);
                }
                // Prefetch any thumbs the cache doesn't already have.
                for (int i = 0; i < (int)gShareMiis.size(); i++) ShareRequestThumb(i);
                char buf[80];
                snprintf(buf, sizeof(buf), "%d miis · page %d / %d",
                         gShareTotal, gSharePage, gShareLastPage);
                ShareSetStatus(buf, {180,180,180,255}, 240);
            }
        } else if (j.kind == ShareJobKind::Thumb) {
            if (j.targetMiiIdx >= 0 && j.targetMiiIdx < (int)gShareMiis.size()
                && j.error.empty() && j.status == 200) {
                gShareMiis[j.targetMiiIdx].thumbBytes = std::move(j.body);
                gShareMiis[j.targetMiiIdx].thumbReady = true;
            }
        } else if (j.kind == ShareJobKind::Detail) {
            if (j.error.empty() && j.status == 200) {
                gShareDetailBytes = std::move(j.body);
                gShareDetailReady = true;
                gShareStatusFrames = 0;
            } else {
                std::string msg = j.error.empty() ? ("HTTP " + std::to_string(j.status)) : j.error;
                gShareDetailErr = msg;
                ShareSetStatus("image failed: " + msg, {220,80,80,255}, 300);
            }
        } else if (j.kind == ShareJobKind::Download) {
            if (j.error.empty() && j.status == 200 && !j.body.empty()) {
                mkdir("/switch/TomoToolNX", 0777);
                std::string tmp = "/switch/TomoToolNX/.share_import_tmp.ltd";
                FILE* f = fopen(tmp.c_str(), "wb");
                if (!f) { ShareSetStatus("Cannot write temp file", {220,80,80,255}, 300); continue; }
                fwrite(j.body.data(), 1, j.body.size(), f); fclose(f);
                std::string err = MiiManager::ImportMii(gShareImportSlot, tmp);
                remove(tmp.c_str());
                if (!err.empty()) ShareSetStatus("Import failed: " + err, {220,80,80,255}, 300);
                else {
                    ShareSetStatus("Imported to slot " + std::to_string(gShareImportSlot), {0,200,0,255}, 300);
                    gMiis = MiiManager::ListMiis();
                }
            } else if (j.status == 404) {
                ShareSetStatus("No .ltd available for this mii", {220,80,80,255}, 300);
            } else {
                std::string msg = j.error.empty() ? ("HTTP " + std::to_string(j.status)) : j.error;
                ShareSetStatus("Download failed: " + msg, {220,80,80,255}, 300);
            }
        }
    }
    // Decode any thumbnails / detail images we now have bytes for. Decoded
    // textures get parked in the thumb cache so later page revisits skip
    // both the network fetch AND the PNG decode.
    for (auto& m : gShareMiis) {
        if (m.thumbReady && !m.thumb) {
            SDL_Texture* tex = SharePngToTexture(m.thumbBytes);
            m.thumbBytes.clear();
            m.thumbReady = false;
            if (tex) {
                ShareThumbCachePut(m.id, tex);
                m.thumb = tex;
            }
        }
    }
    if (gShareDetailReady && !gShareDetailTex) {
        gShareDetailTex = SharePngToTexture(gShareDetailBytes);
        gShareDetailBytes.clear();
        gShareDetailReady = false;
    }
}

static void FreePreview() {
    if (gPreviewTex) { SDL_DestroyTexture(gPreviewTex); gPreviewTex=nullptr; }
    gPreviewStem = "";
}

static std::string FormatStem(const std::string& stem) {
    static const struct { const char* key; const char* label; } MAP[] = {
        {"ugcfood",       "Food"},
        {"ugccloth",      "Cloth"},
        {"ugcgoods",      "Goods"},
        {"ugcinterior",   "Interior"},
        {"ugcexterior",   "Exterior"},
        {"ugcmapobject",  "Map Object"},
        {"ugcmapfloor",   "Map Floor"},
        {"ugcfacepaint",  "Face Paint"},
    };
    std::string low = stem;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    for (auto& m : MAP) {
        size_t kl = strlen(m.key);
        if (low.size() > kl && low.compare(0, kl, m.key) == 0) {
            std::string num = stem.substr(kl);
            size_t nz = num.find_first_not_of('0');
            num = (nz == std::string::npos) ? "0" : num.substr(nz);
            return std::string(m.label) + " " + num;
        }
    }
    return stem;
}

// ── UGC list filter ─────────────────────────────────────────────────────────
// Case-insensitive substring match against display name AND stem.
static bool UgcMatchesFilter(int gEntriesIdx) {
    if (gUgcFilter.empty()) return true;
    auto contains = [&](const std::string& hay) -> bool {
        size_t nlen = gUgcFilter.size();
        if (hay.size() < nlen) return false;
        for (size_t p = 0; p + nlen <= hay.size(); p++) {
            bool eq = true;
            for (size_t k = 0; k < nlen; k++) {
                char a = hay[p+k]; char b = gUgcFilter[k];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { eq = false; break; }
            }
            if (eq) return true;
        }
        return false;
    };
    const auto& e = gEntries[gEntriesIdx];
    std::string name = MiiManager::GetUgcName(e.stem);
    if (!name.empty() && contains(name)) return true;
    return contains(e.stem);
}

// Rebuild gFilteredEntries from gEntries using gUgcFilter.
// Ensures gEntrySel still points at a visible entry; if not, snaps to first one.
static void RebuildUgcFilter() {
    gFilteredEntries.clear();
    gFilteredEntries.reserve(gEntries.size());
    for (int i = 0; i < (int)gEntries.size(); i++)
        if (UgcMatchesFilter(i)) gFilteredEntries.push_back(i);

    bool selVisible = false;
    for (int i : gFilteredEntries) if (i == gEntrySel) { selVisible = true; break; }
    if (!selVisible && !gFilteredEntries.empty()) gEntrySel = gFilteredEntries[0];
    if (gEntryScroll < 0) gEntryScroll = 0;
    if (gEntryScroll > std::max(0, (int)gFilteredEntries.size() - 1))
        gEntryScroll = std::max(0, (int)gFilteredEntries.size() - 1);
}

// Find the position of gEntries index `realIdx` in gFilteredEntries; -1 if absent.
static int UgcFilterPos(int realIdx) {
    for (int p = 0; p < (int)gFilteredEntries.size(); p++)
        if (gFilteredEntries[p] == realIdx) return p;
    return -1;
}

static void LoadPreview(const UgcTextureEntry& e) {
    FreePreview();
    RgbaImage img;
    std::string err = TextureProcessor::DecodeFile(e.ugctexPath, img, gPreviewNoSrgb);
    if (!err.empty()) { gOnSwitchMsg=err; gOnSwitchMsgCol=COL_RED; return; }
    SDL_Surface* surf = SDL_CreateRGBSurfaceFrom(
        img.pixels.data(), img.width, img.height, 32, img.width*4,
        0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (!surf) return;
    gPreviewTex  = SDL_CreateTextureFromSurface(gRen, surf);
    SDL_FreeSurface(surf);
    gPreviewStem = e.stem;
    gOnSwitchMsg = "";
}

static void BrowseRefresh(const std::string& path) {
    gBrowseCurPath = path;
    if (gBrowseCurPath.size() > 1 && gBrowseCurPath.back() == '/')
        gBrowseCurPath.pop_back();
    gBrowseEntries.clear();
    gBrowseSel = 0;
    gBrowseScroll = 0;

    static const char* kUgcImportExts[] = {".png",".ltdf",".ltdc",".ltdg",".ltdi",".ltde",".ltdo",".ltdl",nullptr};

    DIR* d = opendir(gBrowseCurPath.c_str());
    if (!d) return;

    std::vector<std::string> dirs, files;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        std::string full = gBrowseCurPath + "/" + name;
        bool isDir = (de->d_type == DT_DIR);
        if (de->d_type == DT_UNKNOWN) {
            struct stat st;
            if (stat(full.c_str(), &st) == 0) isDir = S_ISDIR(st.st_mode);
        }
        if (isDir) {
            dirs.push_back(name);
        } else {
            std::string lower = name;
            for (auto& c : lower) c = (char)tolower((unsigned char)c);
            bool keep = false;
            if (gBrowseForMii) {
                keep = lower.size() >= 4 && lower.compare(lower.size()-4, 4, ".ltd") == 0;
            } else if (!gBrowseForExportDir) {
                for (int ei = 0; kUgcImportExts[ei]; ei++) {
                    size_t el = strlen(kUgcImportExts[ei]);
                    if (lower.size() >= el && lower.compare(lower.size()-el, el, kUgcImportExts[ei]) == 0) { keep = true; break; }
                }
            }
            if (keep) files.push_back(name);
        }
    }
    closedir(d);

    std::sort(dirs.begin(), dirs.end());
    std::sort(files.begin(), files.end());

    if (gBrowseCurPath != "/") gBrowseEntries.push_back({"[..]", true});
    for (auto& dn : dirs)  gBrowseEntries.push_back({dn, true});
    for (auto& fn : files) gBrowseEntries.push_back({fn, false});
}
static void TOpenImportBrowser(int) {
    gBrowseForMii=true; gBrowseForExportDir=false;
    BrowseRefresh(gBrowseLtdPath);
    gShowFileBrowser=true;
}
// Opens the TomodachiShare browser as a sub-tab. Reachable only via the
// upload-icon button next to "export .ltd" on the Stats sub-tab.
static void TOpenMiiShareBrowser(int) {
    gMiiStatsSubTab = MiiStatsSubTab::Browse;
    if (!gShareWorkerRunning) ShareWorkerStart();
    if (gShareMiis.empty() && gShareStatusFrames == 0) ShareLoadPage();
    // Default the import slot to whatever the user is currently looking at.
    if (!gMiis.empty() && gMiiStatsMiiSel >= 0 && gMiiStatsMiiSel < (int)gMiis.size())
        gShareImportSlot = gMiis[gMiiStatsMiiSel].slot;
}
// Strip characters that would break a filename on FAT/exFAT, but keep multi-byte
// UTF-8 letters (accents, kana, etc.) intact since exFAT stores filenames as
// UTF-16 and the Switch handles them fine.
static std::string SanitizeFilename(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20) continue;                    // control bytes — drop
        if (c == '/' || c == '\\' || c == ':' ||
            c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') { out += '_'; continue; }
        out += (char)c;
    }
    // Trim leading/trailing spaces and dots (Windows would reject these).
    while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
    while (!out.empty() && (out.back()  == ' ' || out.back()  == '.')) out.pop_back();
    return out;
}
static void TDoMiiExport(int) {
    if (gMiis.empty() || gMiiStatsMiiSel >= (int)gMiis.size()) return;
    int slot=gMiis[gMiiStatsMiiSel].slot;
    std::string stem = SanitizeFilename(gMiis[gMiiStatsMiiSel].name);
    if (stem.empty()) stem = "Mii_slot" + std::to_string(slot);
    std::string fname = stem + ".ltd";
    std::string outPath = gExportPath + "/" + fname;
    // If a file with the same name already exists (e.g. two Miis share a name),
    // disambiguate by appending the slot so we don't silently overwrite.
    {
        FILE* probe = fopen(outPath.c_str(), "rb");
        if (probe) {
            fclose(probe);
            fname   = stem + " (slot " + std::to_string(slot) + ").ltd";
            outPath = gExportPath + "/" + fname;
        }
    }
    LogINF("Exporting Mii slot "+std::to_string(slot)+"...");
    std::string err=MiiManager::ExportMii(slot,outPath);
    if(!err.empty()){gMiiStatsMsg=err;gMiiStatsMsgCol=COL_RED;LogERR("Mii export failed: "+err);}
    else{gMiiStatsMsg="Exported: "+fname;gMiiStatsMsgCol=COL_GREEN;LogOK("Exported to: "+outPath);}
}

static void DoMiiImport(const std::string& ltdPath) {
    if (gMiis.empty()) return;
    int slot = gMiis[gMiiStatsMiiSel].slot;
    LogINF("Importing Mii slot "+std::to_string(slot)+"...");
    std::string err = MiiManager::ImportMii(slot, ltdPath);
    if (!err.empty()) { gMiiStatsMsg=err; gMiiStatsMsgCol=COL_RED; LogERR("Mii import failed: "+err); return; }
    // Reload gMiiSav so in-memory state reflects the imported Mii, then mark dirty
    // so the normal save/discard prompt will commit (or revert) on exit.
    gMiiSav = SaveEditor::SavFile{};
    std::string lerr;
    if (!SaveEditor::Load(SAVE_MII_SAV, gMiiSav, lerr)) {
        gMiiStatsMsg = "Reload error: " + lerr; gMiiStatsMsgCol = COL_RED;
        LogERR("Mii save reload failed: " + lerr);
    } else {
        gMiiSavDirty = true;
    }
    gMiis = MiiManager::ListMiis();
    if (gMiiStatsMiiSel >= (int)gMiis.size()) gMiiStatsMiiSel=(int)gMiis.size()-1;
    gMiiStatsMsg="Mii imported — save or discard on exit"; gMiiStatsMsgCol=COL_GREEN;
    LogOK("Mii import OK: slot "+std::to_string(slot));
}

static void DoOnSwitchImport(const std::string& pngPath) {
    if (gEntries.empty()) return;
    // Copy all needed fields before gEntries is replaced by the rescan below
    const std::string stem    = gEntries[gEntrySel].stem;
    TextureProcessor::ImportOptions opts;
    opts.pngPath            = pngPath;
    opts.destStem           = gEntries[gEntrySel].directory()+"/"+stem;
    opts.writeCanvas        = gEntries[gEntrySel].hasCanvas();
    opts.writeThumb         = gEntries[gEntrySel].hasThumb();
    opts.thumbPath          = gEntries[gEntrySel].thumbPath;
    opts.noSrgb             = gPreviewNoSrgb;
    opts.originalUgctexPath = gEntries[gEntrySel].ugctexPath;
    opts.encoder            = gEncMode;
    opts.bc1Mode            = gBc1Mode;
    opts.fitMode            = gFitMode;
    opts.matte              = gMatte;

    LogINF("Importing "+stem+"...");
    std::string err = TextureProcessor::ImportPng(opts);
    if (!err.empty()) {
        gOnSwitchMsg=err; gOnSwitchMsgCol=COL_RED;
        LogERR("Import failed: "+err); return;
    }
    // Mark dirty so the normal save/discard prompt will commit (or revert) on exit.
    gUgcDirty = true;
    // Reload entry list and preview (invalidates any prior reference into gEntries)
    gEntries = UgcScanner::Scan(SAVE_UGC_PATH);
    MiiManager::LoadUgcNames();
    RebuildUgcFilter();
    if (gEntrySel >= (int)gEntries.size()) gEntrySel=(int)gEntries.size()-1;
    if (!gEntries.empty()) LoadPreview(gEntries[gEntrySel]);
    gOnSwitchMsg="Imported — save or discard on exit"; gOnSwitchMsgCol=COL_GREEN;
    LogOK("Import OK: "+stem);
}

static void DoUgcExportLtd() {
    if (gEntries.empty() || gEntrySel < 0 || gEntrySel >= (int)gEntries.size()) return;
    const std::string& stem = gEntries[gEntrySel].stem;
    static const char* kPfx[] = {"Food","Cloth","Goods","Interior","Exterior","MapObject","MapFloor"};
    static const char* kExt[] = {".ltdf",".ltdc",".ltdg",".ltdi",".ltde",".ltdo",".ltdl"};
    int kind = -1, slot = 0;
    if (stem.size() > 3 && stem.substr(0,3) == "Ugc") {
        std::string rest = stem.substr(3);
        for (int k = 0; k < 7; k++) {
            size_t pl = strlen(kPfx[k]);
            if (rest.size() > pl && rest.substr(0,pl) == kPfx[k]) {
                kind = k; slot = atoi(rest.substr(pl).c_str()); break;
            }
        }
    }
    if (kind < 0) { gOnSwitchMsg="Not a UGC item"; gOnSwitchMsgCol=COL_RED; return; }
    std::string outPath = gExportPath + "/" + stem;
    std::string err = MiiManager::ExportUgc(kind, slot + 1, outPath);
    if (!err.empty()) { gOnSwitchMsg=err; gOnSwitchMsgCol=COL_RED; LogERR("Export .ltd failed: "+err); return; }
    gOnSwitchMsg = "Exported: " + stem + kExt[kind];
    gOnSwitchMsgCol = COL_GREEN;
    LogOK("Exported .ltd: " + outPath + kExt[kind]);
}
static void TDoUgcExportLtd(int) { DoUgcExportLtd(); }
static void TSetEncMode(int v) { gEncMode = (TextureProcessor::Bc1Encoder)v; SaveConfig(); }
static void TSetBc1Mode(int v) { gBc1Mode = (TextureProcessor::Bc1Mode)v; SaveConfig(); }
static void TSetFitMode(int v) { gFitMode = (TextureProcessor::FitMode)v; SaveConfig(); }
// Three matte presets for on-Switch. The WebUI has a full hex picker; on a
// controller a 3-button stepper covers the typical cases without keyboard.
static void TSetMatte  (int v) {
    if      (v == 0) gMatte = TextureProcessor::Matte{};                 // transparent
    else if (v == 1) gMatte = TextureProcessor::Matte{255, 255, 255, 255}; // white
    else             gMatte = TextureProcessor::Matte{  0,   0,   0, 255}; // black
    SaveConfig();
}

static void DoUgcImportLtd(const std::string& ltdPath) {
    if (gEntries.empty() || gEntrySel < 0 || gEntrySel >= (int)gEntries.size()) return;
    const std::string& stem = gEntries[gEntrySel].stem;
    static const char* kPfx[] = {"Food","Cloth","Goods","Interior","Exterior","MapObject","MapFloor"};
    static const char* kExt[] = {".ltdf",".ltdc",".ltdg",".ltdi",".ltde",".ltdo",".ltdl"};
    int kind = -1, slot = 0;
    if (stem.size() > 3 && stem.substr(0,3) == "Ugc") {
        std::string rest = stem.substr(3);
        for (int k = 0; k < 7; k++) {
            size_t pl = strlen(kPfx[k]);
            if (rest.size() > pl && rest.substr(0,pl) == kPfx[k]) {
                kind = k; slot = atoi(rest.substr(pl).c_str()); break;
            }
        }
    }
    if (kind < 0) { gOnSwitchMsg="Not a UGC slot"; gOnSwitchMsgCol=COL_RED; return; }
    // Validate that the file extension matches the expected kind
    std::string lower = ltdPath;
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    size_t el = strlen(kExt[kind]);
    if (lower.size() < el || lower.compare(lower.size()-el, el, kExt[kind]) != 0) {
        gOnSwitchMsg = std::string("Wrong file type for ") + kPfx[kind] + " slot (expected " + kExt[kind] + ")";
        gOnSwitchMsgCol = COL_RED; return;
    }
    LogINF("Importing .ltd: " + ltdPath + " → " + stem);
    std::string err = MiiManager::ImportUgc(kind, slot + 1, ltdPath, false);
    if (!err.empty()) { gOnSwitchMsg=err; gOnSwitchMsgCol=COL_RED; LogERR("Import .ltd failed: "+err); return; }
    gUgcDirty = true;
    gPlayerSavDirty = true;
    gEntries = UgcScanner::Scan(SAVE_UGC_PATH);
    RebuildUgcFilter();
    if (gEntrySel >= (int)gEntries.size()) gEntrySel = (int)gEntries.size()-1;
    if (!gEntries.empty()) LoadPreview(gEntries[gEntrySel]);
    gOnSwitchMsg = "Imported " + stem + " — save or discard on exit";
    gOnSwitchMsgCol = COL_GREEN;
    LogOK("Import .ltd OK: " + stem);
}

// ─── Draw helpers ─────────────────────────────────────────────────────────────

static void DrawHeader(const std::string& title) {
    FillRect(0,0,SCREEN_W,28,COL_PANEL);
    DrawRect(0,27,SCREEN_W,1,COL_BORDER);
    TTF_Font* fsm=GetFont(Font::Sm);
    int tw=0,fh=0;
    if(fsm) TTF_SizeUTF8(fsm,"TomoToolNX",&tw,&fh);
    int y=(28-fh)/2;
    DrawText("TomoToolNX",10,y,COL_GOLD);
    if (!title.empty())
        DrawText(" / "+title, 10+tw, y, COL_DIM);
    int vw=0;
    if(fsm) TTF_SizeUTF8(fsm,"v" APP_VERSION,&vw,&fh);
    DrawText("v" APP_VERSION, SCREEN_W-vw-10, y, COL_DIM);
}

static void DrawFooter(const std::string& hints) {
    FillRect(0,SCREEN_H-30,SCREEN_W,30,COL_PANEL);
    DrawRect(0,SCREEN_H-30,SCREEN_W,1,COL_BORDER);
    DrawTextC(hints, SCREEN_W/2, SCREEN_H-15, COL_DIM);
}

// ─── Screen draws ─────────────────────────────────────────────────────────────

static void DrawUpdateCheck() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC(Lang::T("update.checking"), SCREEN_W/2, SCREEN_H/2, COL_DIM, Font::Md);
    DrawFooter(Lang::T("update.footer.checking"));
    Present();
}

static void DrawUpdateAvailable() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC(Lang::T("update.available"), SCREEN_W/2, SCREEN_H/2-60, COL_GREEN, Font::Lg);
    DrawTextC("v" APP_VERSION + std::string("  ->  v") + Updater::GetLatestVersion(),
              SCREEN_W/2, SCREEN_H/2-18, COL_TEXT, Font::Md);
    DrawTextC(Lang::T("update.download.from"), SCREEN_W/2, SCREEN_H/2+18, COL_DIM);
    DrawTextC("github.com/" GITHUB_REPO "/releases/latest",
              SCREEN_W/2, SCREEN_H/2+42, COL_ACCENT, Font::Md);
    DrawFooter(Lang::T("update.footer.available"));
    Present();
}

// Parse "BACKUP_ROOT/save_DD-MM-YYYY_HH-MM" into its parts. Returns false if
// the trailing component doesn't match the expected timestamp shape.
struct BackupTs { int day=0, mon=0, year=0, hour=0, min=0; bool ok=false; };
static BackupTs ParseBackupName(const std::string& fullPath) {
    BackupTs t;
    std::string n = fullPath;
    size_t sl = n.rfind('/');
    if (sl != std::string::npos) n = n.substr(sl + 1);
    if (n.size() < 5 || n.compare(0, 5, "save_") != 0) return t;
    int d, mo, y, h, mi;
    if (sscanf(n.c_str() + 5, "%d-%d-%d_%d-%d", &d, &mo, &y, &h, &mi) == 5) {
        t.day = d; t.mon = mo; t.year = y; t.hour = h; t.min = mi;
        t.ok = true;
    }
    return t;
}
static const char* BackupMonShort(int m) {
    static const char* names[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec"};
    return (m >= 1 && m <= 12) ? names[m-1] : "???";
}

static void DrawRestorePicker() {
    HitClear();
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawHeader(Lang::T("restore.header"));

    if (gRestoreList.empty()) {
        const int cW = 520, cH = 180;
        const int cx = (SCREEN_W - cW) / 2;
        const int cy = (SCREEN_H - cH) / 2 - 10;
        FillRect(cx, cy, cW, cH, COL_PANEL);
        DrawRect(cx, cy, cW, cH, COL_BORDER);
        // Empty-folder icon.
        int icx = cx + cW/2;
        int icy = cy + 50;
        int fw = 60, fh = 44;
        FillRect(icx-fw/2,    icy-fh/2+6, fw,      fh-6, {28,28,28,255});
        DrawRect(icx-fw/2,    icy-fh/2+6, fw,      fh-6, COL_BORDER);
        FillRect(icx-fw/2,    icy-fh/2,   fw/2-4,  10,   {28,28,28,255});
        DrawRect(icx-fw/2,    icy-fh/2,   fw/2-4,  10,   COL_BORDER);
        DrawTextC(Lang::T("restore.empty.title"), SCREEN_W/2, cy + cH - 60, COL_TEXT, Font::Md);
        DrawTextC(Lang::T("restore.empty.hint"),
                  SCREEN_W/2, cy + cH - 32, COL_DIM, Font::Sm);
        DrawFooter(Lang::T("footer.back"));
        return;
    }

    // Layout constants
    const int listW = 720, listX = (SCREEN_W - listW) / 2;
    const int rowH  = 72,  rowGap = 6;
    const int listY = 104;
    const int RVIS  = 6;

    // ── Summary strip above the list ───────────────────────────────────────
    {
        int sH = 30, sY = listY - sH - 8;
        FillRect(listX, sY, listW, sH, {26,26,26,255});
        DrawRect(listX, sY, listW, sH, COL_BORDER);
        FillRect(listX, sY, 4, sH, COL_GOLD);
        TTF_Font* fsm = GetFont(Font::Sm);
        std::string left = std::to_string(gRestoreList.size()) + " / "
                         + std::to_string(gMaxBackups) + " " + Lang::T("restore.backups.word");
        int lw=0, lh=0;
        if (fsm) TTF_SizeUTF8(fsm, left.c_str(), &lw, &lh);
        DrawText(left, listX + 16, sY + (sH - lh)/2, COL_TEXT, Font::Sm);

        auto top = ParseBackupName(gRestoreList[0]);
        if (top.ok) {
            char r[64];
            snprintf(r, sizeof(r), "%s  %s %d, %d  ·  %02d:%02d",
                     Lang::T("restore.newest.prefix").c_str(),
                     BackupMonShort(top.mon), top.day, top.year, top.hour, top.min);
            int tw=0, th=0;
            if (fsm) TTF_SizeUTF8(fsm, r, &tw, &th);
            DrawText(r, listX + listW - tw - 14, sY + (sH - th)/2, COL_DIM, Font::Sm);
        }
    }

    int rpSelRowY = listY;
    bool rpSawSel = false;
    for (int i = 0; i < RVIS; i++) {
        int idx = gRestoreScroll + i;
        if (idx >= (int)gRestoreList.size()) break;
        bool sel = (idx == gRestoreSel);
        int ry = listY + i * (rowH + rowGap);
        if (sel) { rpSelRowY = ry; rpSawSel = true; }
        HitAdd(listX, ry, listW, rowH, TRestorePickSel, idx);
        if (sel) DrawShadow(listX, ry, listW, rowH, gTheme.cornerRadius);
        FillRect(listX, ry, listW, rowH, sel ? COL_SEL : COL_PANEL);
        DrawRect(listX, ry, listW, rowH,
                 (sel && gTheme.cornerRadius == 0) ? COL_GOLD : COL_BORDER);
        // Gold left edge to anchor the row visually when selected
        FillRect(listX, ry, 4, rowH, sel ? COL_GOLD : SDL_Color{60,60,60,255});

        // ── Date + time (left-aligned, no icon) ───────────────────────────
        auto ts = ParseBackupName(gRestoreList[idx]);
        std::string dateStr, timeStr;
        if (ts.ok) {
            char d[32], t[16];
            snprintf(d, sizeof(d), "%s %d, %d", BackupMonShort(ts.mon), ts.day, ts.year);
            snprintf(t, sizeof(t), "%02d:%02d", ts.hour, ts.min);
            dateStr = d; timeStr = t;
        } else {
            // Fallback to the raw folder name if parsing failed
            std::string n = gRestoreList[idx];
            size_t sl = n.rfind('/');
            if (sl != std::string::npos) n = n.substr(sl + 1);
            dateStr = n;
        }
        int textX = listX + 16;
        DrawText(dateStr, textX, ry + 14, sel ? COL_TEXT : COL_DIM, Font::Md);
        if (!timeStr.empty())
            DrawText(timeStr, textX, ry + rowH - 24, COL_DIM, Font::Sm);

        // ── Right-side: position badge + (when selected) action chips ────
        std::string badgeStr;
        char nbuf[16];
        if (idx == 0) badgeStr = Lang::T("restore.badge.newest");
        else if (idx == (int)gRestoreList.size() - 1) badgeStr = Lang::T("restore.badge.oldest");
        else { snprintf(nbuf, sizeof(nbuf), "#%d", idx + 1); badgeStr = nbuf; }
        const char* badge = badgeStr.c_str();
        TTF_Font* fsm = GetFont(Font::Sm); int bw=0, bh=0;
        if (fsm) TTF_SizeUTF8(fsm, badge, &bw, &bh);
        int badgeW = bw + 16, badgeH = 18;
        int badgeX = listX + listW - badgeW - 16, badgeY = ry + 12;
        SDL_Color badgeCol = (idx == 0) ? COL_GOLD : COL_DIM;
        FillRect(badgeX, badgeY, badgeW, badgeH, {28,28,28,255});
        DrawRect(badgeX, badgeY, badgeW, badgeH, badgeCol);
        DrawTextC(badge, badgeX + badgeW/2, badgeY + badgeH/2, badgeCol, Font::Sm);

        if (sel) {
            int chipW = 92, chipH = 20, chipGap = 6;
            int chipX2 = listX + listW - chipW - 16;
            int chipX1 = chipX2 - chipW - chipGap;
            int chipY  = ry + rowH - chipH - 10;
            // A  restore (gold)
            FillRect(chipX1, chipY, chipW, chipH, {32,26,12,255});
            DrawRect(chipX1, chipY, chipW, chipH, COL_GOLD);
            DrawTextC("A  " + Lang::T("restore.chip.restore"), chipX1 + chipW/2, chipY + chipH/2, COL_GOLD, Font::Sm);
            // X  delete (red)
            SDL_Color cRed = {220, 90, 90, 255};
            FillRect(chipX2, chipY, chipW, chipH, {40,16,16,255});
            DrawRect(chipX2, chipY, chipW, chipH, cRed);
            DrawTextC("X  " + Lang::T("restore.chip.delete"), chipX2 + chipW/2, chipY + chipH/2, cRed, Font::Sm);
        }
    }

    if (rpSawSel) {
        RequestHighlight((int)Screen::UserPick, /*sub=restore picker*/4,
                         listX, rpSelRowY, listW, rowH, gDtSec);
        DrawAnimatedHighlight();
    }

    if ((int)gRestoreList.size() > RVIS) {
        int total = (int)gRestoreList.size();
        int barAreaH = RVIS * (rowH + rowGap) - rowGap;
        int barH = std::max(16, barAreaH * RVIS / total);
        int barY = listY + barAreaH * gRestoreScroll / total;
        FillRect(listX + listW + 6, barY, 4, barH, COL_BORDER);
    }

    DrawFooter(Lang::T("restore.footer"));

    // ── Confirmation modal ─────────────────────────────────────────────────
    if (gRestoreConfirmKind != 0
        && gRestoreSel >= 0 && gRestoreSel < (int)gRestoreList.size()) {
        // Dim the picker behind the modal
        SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(gRen, 0, 0, 0, 190);
        SDL_Rect full = { 0, 0, SCREEN_W, SCREEN_H };
        SDL_RenderFillRect(gRen, &full);

        // Modal panel
        const int mW = 580, mH = 220;
        const int mx = (SCREEN_W - mW) / 2;
        const int my = (SCREEN_H - mH) / 2;
        bool isDelete = (gRestoreConfirmKind == 2);
        SDL_Color accent = isDelete ? SDL_Color{220, 80, 80, 255} : COL_GOLD;
        FillRect(mx, my, mW, mH, {18, 14, 10, 255});
        DrawRect(mx, my, mW, mH, accent);
        FillRect(mx, my, mW, 4, accent);

        DrawTextC(Lang::T(isDelete ? "restore.confirm.delete.title" : "restore.confirm.restore.title"),
                  SCREEN_W/2, my + 36, accent, Font::Lg);

        std::string nm = gRestoreList[gRestoreSel];
        size_t sl = nm.rfind('/');
        if (sl != std::string::npos) nm = nm.substr(sl + 1);
        if (nm.size() > 5 && nm.substr(0, 5) == "save_") nm = nm.substr(5);
        for (char& c : nm) if (c == '_') c = ' ';
        DrawTextC(nm, SCREEN_W/2, my + 72, COL_TEXT, Font::Md);

        if (isDelete) {
            DrawTextC(Lang::T("restore.confirm.delete.l1"),
                      SCREEN_W/2, my + 102, COL_DIM, Font::Sm);
            DrawTextC(Lang::T("restore.confirm.delete.l2"),
                      SCREEN_W/2, my + 122, COL_DIM, Font::Sm);
        } else {
            DrawTextC(Lang::T("restore.confirm.restore.l1"),
                      SCREEN_W/2, my + 102, COL_DIM, Font::Sm);
            DrawTextC(Lang::T("restore.confirm.restore.l2"),
                      SCREEN_W/2, my + 122, COL_DIM, Font::Sm);
        }

        // Buttons — touch hit zones for A (confirm) and B (cancel)
        const int bW = 200, bH = 42, bGap = 16;
        const int byBtn = my + mH - bH - 16;
        const int bxA = SCREEN_W/2 - bGap/2 - bW;
        const int bxB = SCREEN_W/2 + bGap/2;
        HitAdd(bxA, byBtn, bW, bH, TRestoreConfirmA, 0);
        FillRect(bxA, byBtn, bW, bH, COL_SEL);
        DrawRect(bxA, byBtn, bW, bH, accent);
        DrawTextC("A  " + Lang::T(isDelete ? "restore.chip.delete" : "restore.chip.restore"),
                  bxA + bW/2, byBtn + bH/2, accent, Font::Md);
        HitAdd(bxB, byBtn, bW, bH, TRestoreConfirmB, 0);
        FillRect(bxB, byBtn, bW, bH, COL_PANEL);
        DrawRect(bxB, byBtn, bW, bH, COL_BORDER);
        DrawTextC("B  " + Lang::T("cancel"), bxB + bW/2, byBtn + bH/2, COL_DIM, Font::Md);
    }

    Present();
}

static void DrawSettings() {
    HitClear();
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);

    if (gShowFileBrowser)    { DrawFileBrowser();    Present(); return; }
    if (gShowRestorePicker)  { DrawRestorePicker();  Present(); return; }

    DrawHeader(Lang::T("settings.title"));

    struct SettingsItem { const char* labelKey; const char* descKey; };
    // Row order: Language → Theme → Export Path → Max Backups → Restore.
    // Language + Theme grouped at the top as the two "display" prefs.
    static const SettingsItem ITEMS[] = {
        { "settings.item.lang",       "settings.item.lang.desc"    },
        { "settings.item.theme",      "settings.item.theme.desc"   },
        { "settings.item.export",     "settings.item.export.desc"  },
        { "settings.item.maxbk",      "settings.item.maxbk.desc"   },
        { "settings.item.restore",    "settings.item.restore.desc" },
    };
    static const int ITEM_COUNT = 5;

    const int listW = 720, listX = (SCREEN_W - listW) / 2;
    const int rowH  = 68,  rowGap = 5;
    int listY = 68;

    int selRowY = listY;
    for (int i = 0; i < ITEM_COUNT; i++) {
        bool sel = (i == gSettingsSel);
        int ry = listY + i * (rowH + rowGap);
        if (sel) selRowY = ry;
        HitAdd(listX, ry, listW, rowH, TSelSettingsItem, i);
        if (sel) DrawShadow(listX, ry, listW, rowH, gTheme.cornerRadius);
        FillRect(listX, ry, listW, rowH, sel ? COL_SEL : COL_PANEL);
        // Switch theme defers the gold outline to the animated highlight below.
        DrawRect(listX, ry, listW, rowH,
                 (sel && gTheme.cornerRadius == 0) ? COL_GOLD : COL_BORDER);

        SDL_Color labelCol = sel ? COL_TEXT : COL_DIM;
        SDL_Color descCol  = COL_DIM;
        DrawText(Lang::T(ITEMS[i].labelKey), listX + 16, ry + 10, labelCol, Font::Md);
        DrawText(Lang::T(ITEMS[i].descKey),  listX + 16, ry + 40, descCol,  Font::Sm);

        if (i == 2) {
            TTF_Font* fsm = GetFont(Font::Sm);
            int vw = 0, vh = 0;
            if (fsm) TTF_SizeUTF8(fsm, gExportPath.c_str(), &vw, &vh);
            int maxValW = listW / 2 - 20;
            std::string disp = gExportPath;
            while (fsm && vw > maxValW && disp.size() > 4) {
                disp = ".." + disp.substr(disp.size() - (disp.size() * 3 / 4));
                TTF_SizeUTF8(fsm, disp.c_str(), &vw, &vh);
            }
            DrawText(disp,        listX + listW - vw - 16, ry + 12, sel ? COL_ACCENT : COL_DIM, Font::Sm);
            DrawText("A  " + Lang::T("settings.change"), listX + listW - 100,     ry + 40, sel ? COL_GOLD   : COL_DIM, Font::Sm);
        } else if (i == 4) {
            int cnt = BackupService::CountBackups();
            std::string v = (cnt == 0) ? Lang::T("settings.backups.none") : std::to_string(cnt) + " " + Lang::T("settings.backups.available");
            DrawText(v,          listX + listW - 130, ry + 12, sel ? COL_ACCENT : COL_DIM, Font::Sm);
            DrawText("A  " + Lang::T("settings.open"),  listX + listW - 100, ry + 40, sel ? COL_GOLD   : COL_DIM, Font::Sm);
        } else if (i == 3) {
            const int arrowW = 36, numW = 48, ctrlH = 34;
            int ctrlX = listX + listW - arrowW - numW - arrowW - 20;
            int ctrlY = ry + (rowH - ctrlH) / 2;
            HitAdd(ctrlX, ctrlY, arrowW, ctrlH, TSettingsRowLeft, i);
            FillRect(ctrlX, ctrlY, arrowW, ctrlH, COL_PANEL);
            DrawRect(ctrlX, ctrlY, arrowW, ctrlH, sel ? COL_GOLD : COL_BORDER);
            DrawTextC("<", ctrlX + arrowW/2, ctrlY + ctrlH/2, sel ? COL_GOLD : COL_DIM, Font::Md);
            FillRect(ctrlX + arrowW, ctrlY, numW, ctrlH, COL_PANEL);
            DrawRect(ctrlX + arrowW, ctrlY, numW, ctrlH, sel ? COL_GOLD : COL_BORDER);
            DrawTextC(std::to_string(gMaxBackups), ctrlX + arrowW + numW/2, ctrlY + ctrlH/2, sel ? COL_TEXT : COL_DIM, Font::Md);
            HitAdd(ctrlX + arrowW + numW, ctrlY, arrowW, ctrlH, TSettingsRowRight, i);
            FillRect(ctrlX + arrowW + numW, ctrlY, arrowW, ctrlH, COL_PANEL);
            DrawRect(ctrlX + arrowW + numW, ctrlY, arrowW, ctrlH, sel ? COL_GOLD : COL_BORDER);
            DrawTextC(">", ctrlX + arrowW + numW + arrowW/2, ctrlY + ctrlH/2, sel ? COL_GOLD : COL_DIM, Font::Md);
        } else if (i == 0) {
            // Language: < [Current] > stepper. Left/Right cycles; A also cycles forward.
            const auto& avail = Lang::Available();
            std::string curLabel = Lang::Current();
            for (const auto& e : avail) if (e.code == Lang::Current()) { curLabel = e.label; break; }
            const int arrowW = 36, chipW = 160, ctrlH = 34;
            int ctrlX = listX + listW - arrowW - chipW - arrowW - 20;
            int ctrlY = ry + (rowH - ctrlH) / 2;
            FillRect(ctrlX, ctrlY, arrowW, ctrlH, COL_PANEL);
            DrawRect(ctrlX, ctrlY, arrowW, ctrlH, sel ? COL_GOLD : COL_BORDER);
            DrawTextC("<", ctrlX + arrowW/2, ctrlY + ctrlH/2, sel ? COL_GOLD : COL_DIM, Font::Md);
            HitAdd(ctrlX, ctrlY, arrowW, ctrlH, TSettingsRowLeft, i);
            FillRect(ctrlX + arrowW, ctrlY, chipW, ctrlH, COL_PANEL);
            DrawRect(ctrlX + arrowW, ctrlY, chipW, ctrlH, sel ? COL_GOLD : COL_BORDER);
            DrawTextC(curLabel, ctrlX + arrowW + chipW/2, ctrlY + ctrlH/2, sel ? COL_GOLD : COL_DIM, Font::Md);
            FillRect(ctrlX + arrowW + chipW, ctrlY, arrowW, ctrlH, COL_PANEL);
            DrawRect(ctrlX + arrowW + chipW, ctrlY, arrowW, ctrlH, sel ? COL_GOLD : COL_BORDER);
            DrawTextC(">", ctrlX + arrowW + chipW + arrowW/2, ctrlY + ctrlH/2, sel ? COL_GOLD : COL_DIM, Font::Md);
            HitAdd(ctrlX + arrowW + chipW, ctrlY, arrowW, ctrlH, TSettingsRowRight, i);
        } else if (i == 1) {
            // Theme: < [Current] > stepper. Identical pattern to the language row.
            std::string curLabel = Lang::T("theme." + ThemeNS::Current());
            const int arrowW = 36, chipW = 160, ctrlH = 34;
            int ctrlX = listX + listW - arrowW - chipW - arrowW - 20;
            int ctrlY = ry + (rowH - ctrlH) / 2;
            FillRect(ctrlX, ctrlY, arrowW, ctrlH, COL_PANEL);
            DrawRect(ctrlX, ctrlY, arrowW, ctrlH, sel ? COL_GOLD : COL_BORDER);
            DrawTextC("<", ctrlX + arrowW/2, ctrlY + ctrlH/2, sel ? COL_GOLD : COL_DIM, Font::Md);
            HitAdd(ctrlX, ctrlY, arrowW, ctrlH, TSettingsRowLeft, i);
            FillRect(ctrlX + arrowW, ctrlY, chipW, ctrlH, COL_PANEL);
            DrawRect(ctrlX + arrowW, ctrlY, chipW, ctrlH, sel ? COL_GOLD : COL_BORDER);
            DrawTextC(curLabel, ctrlX + arrowW + chipW/2, ctrlY + ctrlH/2, sel ? COL_GOLD : COL_DIM, Font::Md);
            FillRect(ctrlX + arrowW + chipW, ctrlY, arrowW, ctrlH, COL_PANEL);
            DrawRect(ctrlX + arrowW + chipW, ctrlY, arrowW, ctrlH, sel ? COL_GOLD : COL_BORDER);
            DrawTextC(">", ctrlX + arrowW + chipW + arrowW/2, ctrlY + ctrlH/2, sel ? COL_GOLD : COL_DIM, Font::Md);
            HitAdd(ctrlX + arrowW + chipW, ctrlY, arrowW, ctrlH, TSettingsRowRight, i);
        }
    }

    // Animated selection outline — glides between rows on Switch theme.
    RequestHighlight((int)Screen::Settings, /*sub*/0,
                     listX, selRowY, listW, rowH, gDtSec);
    DrawAnimatedHighlight();

    if (!gSettingsMsg.empty())
        DrawTextC(gSettingsMsg, SCREEN_W/2, listY + ITEM_COUNT*(rowH+rowGap) + 10,
                  gSettingsMsgCol, Font::Sm);

    DrawFooter(Lang::T("settings.footer.main"));
    Present();
}

static void DrawUserPick() {
    HitClear();
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC(Lang::T("userpick.title"), SCREEN_W/2, 44, COL_DIM, Font::Md);

    int n = (int)gUsers.size();
    if (n == 0) { DrawFooter(Lang::T("footer.quit")); Present(); return; }

    const int GAP      = 24;
    const int MAX_VIS  = std::min(n, 5);
    const int CARD_W   = std::min(220, (SCREEN_W - 120 - (MAX_VIS-1)*GAP) / MAX_VIS);
    const int AV_SIZE  = CARD_W - 30;
    const int CARD_H   = AV_SIZE + 66;

    int scroll = 0;
    if (n > MAX_VIS) {
        scroll = gUserSel - MAX_VIS/2;
        scroll = std::max(0, std::min(scroll, n - MAX_VIS));
    }

    int totalW = MAX_VIS*CARD_W + (MAX_VIS-1)*GAP;
    int startX = (SCREEN_W - totalW) / 2;
    int startY = 68 + (SCREEN_H - 36 - 68 - CARD_H) / 2;

    int selCardX = startX, selCardY = startY;
    for (int i = 0; i < MAX_VIS; i++) {
        int idx = scroll + i;
        if (idx >= n) break;
        bool sel = (idx == gUserSel);
        int cx = startX + i*(CARD_W + GAP);
        if (sel) { selCardX = cx; selCardY = startY; }

        HitAdd(cx, startY, CARD_W, CARD_H, TSelUser, idx);

        if (sel) DrawShadow(cx, startY, CARD_W, CARD_H, gTheme.cornerRadius);
        FillRect(cx, startY, CARD_W, CARD_H, sel ? COL_SEL : COL_PANEL);
        DrawRect(cx, startY, CARD_W, CARD_H,
                 (sel && gTheme.cornerRadius == 0) ? COL_GOLD : COL_BORDER);
        // Legacy "double border" only on the Original theme (the animated
        // glow does this job on Switch).
        if (sel && gTheme.cornerRadius == 0)
            DrawRectSquare(cx+1, startY+1, CARD_W-2, CARD_H-2, COL_GOLD);

        int avX = cx + (CARD_W - AV_SIZE)/2;
        int avY = startY + 14;
        if (idx < (int)gAvatarTextures.size() && gAvatarTextures[idx]) {
            SDL_Rect dst{avX, avY, AV_SIZE, AV_SIZE};
            SDL_RenderCopy(gRen, gAvatarTextures[idx], nullptr, &dst);
        } else {
            FillRect(avX, avY, AV_SIZE, AV_SIZE, COL_PANEL2);
            DrawRect(avX, avY, AV_SIZE, AV_SIZE, COL_BORDER);
            DrawTextC("?", cx+CARD_W/2, avY+AV_SIZE/2, COL_DIM, Font::Lg);
        }

        DrawTextC(gUsers[idx].nickname, cx+CARD_W/2, avY+AV_SIZE+20,
                  sel ? COL_TEXT : COL_DIM, Font::Sm);
    }

    // Animated selection outline glides between user cards.
    RequestHighlight((int)Screen::UserPick, /*sub*/0,
                     selCardX, selCardY, CARD_W, CARD_H, gDtSec);
    DrawAnimatedHighlight();

    if (scroll > 0)
        DrawTextC("<", startX-18, startY+CARD_H/2, COL_DIM, Font::Md);
    if (scroll + MAX_VIS < n)
        DrawTextC(">", startX+totalW+18, startY+CARD_H/2, COL_DIM, Font::Md);

    // Settings gear — square icon button in bottom-right above the footer,
    // matching the texture tab's gear button style.
    const int sBtnH = 40;
    const int sBtnW = sBtnH;
    int sBtnX = SCREEN_W - sBtnW - 14;
    int sBtnY = SCREEN_H - 30 - sBtnH - 8;
    HitAdd(sBtnX, sBtnY, sBtnW, sBtnH, TOpenSettings, 0);
    FillRect(sBtnX, sBtnY, sBtnW, sBtnH, COL_PANEL);
    DrawRect(sBtnX, sBtnY, sBtnW, sBtnH, COL_BORDER);
    DrawGearIcon(sBtnX + sBtnW/2, sBtnY + sBtnH/2, sBtnH * 30 / 100, COL_DIM);

    DrawFooter(Lang::T("userpick.footer"));
    Present();
}

static void DrawBackupPrompt() {
    HitClear();
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader("");

    const int cw  = 460;
    const int ch  = 148;
    const int gap = 20;
    const int cx  = SCREEN_W/2 - (2*cw + gap)/2;
    const int cy  = SCREEN_H/2 - ch/2 + 10;
    int x1=cx, x2=cx+cw+gap;

    HitAdd(x1,cy,cw,ch, TSimBtn, (int)HidNpadButton_A);
    HitAdd(x2,cy,cw,ch, TSimBtn, (int)HidNpadButton_B);

    int curCount = BackupService::CountBackups();
    auto sub = [](std::string s, const std::string& tok, const std::string& val) {
        size_t p = 0;
        while ((p = s.find(tok, p)) != std::string::npos) { s.replace(p, tok.size(), val); p += val.size(); }
        return s;
    };
    std::string cur = std::to_string(curCount), mx = std::to_string(gMaxBackups);
    if (gBackupFull) {
        std::string fullMsg = sub(sub(Lang::T("backup.full.title"), "{cur}", cur), "{max}", mx);
        DrawTextC(fullMsg, SCREEN_W/2, 44, COL_DIM, Font::Md);
        FillRect(x1,cy,cw,ch,COL_PANEL); DrawRect(x1,cy,cw,ch,COL_GOLD);
        DrawTextC("[A]",                              x1+cw/2, cy+36,  COL_GOLD, Font::Lg);
        DrawTextC(Lang::T("backup.full.action"),       x1+cw/2, cy+86,  COL_GOLD, Font::Md);
        DrawTextC(Lang::T("backup.full.hint"),         x1+cw/2, cy+116, COL_DIM,  Font::Sm);
        DrawFooter(Lang::T("backup.footer.full"));
    } else {
        DrawTextC(Lang::T("backup.title"), SCREEN_W/2, 44, COL_DIM, Font::Md);
        FillRect(x1,cy,cw,ch,COL_PANEL); DrawRect(x1,cy,cw,ch,COL_ACCENT);
        DrawTextC("[A]",                              x1+cw/2, cy+36,  COL_ACCENT, Font::Lg);
        DrawTextC(Lang::T("backup.action"),            x1+cw/2, cy+86,  COL_TEXT,   Font::Md);
        DrawTextC(Lang::T("backup.action.hint"),       x1+cw/2, cy+116, COL_DIM,    Font::Sm);
        DrawFooter(Lang::T("backup.footer"));
    }

    FillRect(x2,cy,cw,ch,COL_PANEL); DrawRect(x2,cy,cw,ch,COL_RED);
    DrawTextC("[B]",                          x2+cw/2, cy+36,  COL_RED,  Font::Lg);
    DrawTextC(Lang::T("backup.skip"),          x2+cw/2, cy+86,  COL_TEXT, Font::Md);
    DrawTextC(Lang::T("backup.skip.hint"),     x2+cw/2, cy+116, COL_DIM,  Font::Sm);

    // tip: current / max backups
    std::string tip = sub(sub(Lang::T("backup.tip"), "{cur}", cur), "{max}", mx);
    DrawTextC(tip, SCREEN_W/2, cy+ch+18, COL_DIM, Font::Sm);

    Present();
}

static void DrawBackingUp() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC(Lang::T("backup.progress.title"), SCREEN_W/2, SCREEN_H/2-30, COL_DIM, Font::Md);
    float prog = BackupService::BackupProgress();
    int bw=1000, bh=10, bx=SCREEN_W/2-bw/2, by=SCREEN_H/2;
    FillRect(bx,by,bw,bh,COL_PANEL);
    DrawRect(bx,by,bw,bh,COL_BORDER);
    FillRect(bx,by,(int)(bw*prog),bh,COL_ACCENT);
    char pct[16]; snprintf(pct,sizeof(pct),"%d%%",(int)(prog*100));
    DrawTextC(pct, SCREEN_W/2, by+bh+18, COL_DIM);
    DrawTextC(Lang::T("backup.progress.warn"), SCREEN_W/2, by+bh+42, COL_DIM);
    Present();
}

static void DrawMounting() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawTextC(Lang::T("mounting.title"), SCREEN_W/2, SCREEN_H/2, COL_DIM, Font::Md);
    Present();
}

static const int LIST_X=12, LIST_W=380, LIST_Y=64, LIST_H=SCREEN_H-96, ITEM_H=28;
static const int LIST_PAD_TOP=6; // padding between box top and first item
static const int PREVIEW_X=400, PREVIEW_Y=LIST_Y, PREVIEW_W=868, PREVIEW_H=LIST_H;
// UGC list sits below a search bar; these match the renderer's locals so
// touch / scroll / input handlers reach every drawn row.
static const int UGC_SEARCH_H   = 30;
static const int UGC_SEARCH_GAP = 10;
static const int UGC_LIST_TOP_Y = LIST_Y + UGC_SEARCH_H + UGC_SEARCH_GAP + 4;
static const int VISIBLE = (LIST_H - UGC_SEARCH_H - UGC_SEARCH_GAP - 4) / ITEM_H;

// Implementations deferred to here because they reference VISIBLE and LoadPreview
static void TSelUgc(int idx) {
    if (idx>=0 && idx<(int)gEntries.size()) {
        gEntrySel=idx;
        // Scroll position is in filtered space; keep selected entry visible
        int p = UgcFilterPos(gEntrySel);
        if (p >= 0) {
            if (p >= gEntryScroll+VISIBLE) gEntryScroll = p - VISIBLE + 1;
            if (p <  gEntryScroll)         gEntryScroll = p;
        }
        LoadPreview(gEntries[gEntrySel]);
    }
}

static void TUgcOpenSearch(int) {
    std::string q = ShowKeyboard("Filter textures (substring)", gUgcFilter, 30);
    for (auto& c : q) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    gUgcFilter = q;
    RebuildUgcFilter();
    gEntryScroll = 0;
    if (!gFilteredEntries.empty() && UgcFilterPos(gEntrySel) < 0) {
        gEntrySel = gFilteredEntries[0];
        if (!gEntries.empty()) LoadPreview(gEntries[gEntrySel]);
    }
}
static void TUgcClearSearch(int) {
    gUgcFilter.clear();
    RebuildUgcFilter();
    gEntryScroll = 0;
    int p = UgcFilterPos(gEntrySel);
    if (p >= 0) {
        if (p >= gEntryScroll+VISIBLE) gEntryScroll = p - VISIBLE + 1;
        if (p <  gEntryScroll)         gEntryScroll = p;
    }
}


static const char* CONFIG_PATH = "/switch/TomoToolNX/config.ini";

// Map the Switch's system locale to one we ship; unknown → "en".
static std::string DetectSystemLanguage() {
    std::string code = "en";
    if (R_SUCCEEDED(setInitialize())) {
        u64 langCode = 0;
        SetLanguage lang;
        if (R_SUCCEEDED(setGetSystemLanguage(&langCode)) &&
            R_SUCCEEDED(setMakeLanguage(langCode, &lang))) {
            switch (lang) {
                case SetLanguage_FR:
                case SetLanguage_FRCA:  code = "fr"; break;
                case SetLanguage_ES:
                case SetLanguage_ES419: code = "es"; break;
                case SetLanguage_DE:    code = "de"; break;
                case SetLanguage_RU:    code = "ru"; break;
                case SetLanguage_ZHCN:
                case SetLanguage_ZHTW:
                case SetLanguage_ZHHANS:
                case SetLanguage_ZHHANT: code = "zh"; break;
                default:                 code = "en"; break;
            }
        }
        setExit();
    }
    return code;
}

static void LoadConfig() {
    bool langFromConfig = false;
    FILE* f = fopen(CONFIG_PATH, "r");
    if (!f) {
        // First launch: seed locale from the Switch's system language.
        Lang::SetCurrent(DetectSystemLanguage());
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        while (!val.empty() && (val.back()=='\n'||val.back()=='\r'||val.back()==' '))
            val.pop_back();
        if (key == "export_path")      gExportPath      = val;
        if (key == "thumb_tip_seen")   gThumbTipSeen    = (val == "1");
        if (key == "save_warn_acked")  gSaveWarningAcked = (val == "1");
        if (key == "max_backups")    { int v = atoi(val.c_str()); if (v >= 1 && v <= 99) gMaxBackups = v; }
        if (key == "encoder") {
            if      (val == "custom") gEncMode = TextureProcessor::Bc1Encoder::Custom;
            else if (val == "pca")         gEncMode = TextureProcessor::Bc1Encoder::PCA;
        }
        if (key == "bc1_mode") {
            if      (val == "auto")     gBc1Mode = TextureProcessor::Bc1Mode::Auto;
            else if (val == "4color")   gBc1Mode = TextureProcessor::Bc1Mode::FourColor;
            else if (val == "3color")   gBc1Mode = TextureProcessor::Bc1Mode::ThreeColor;
        }
        if (key == "fit_mode") {
            if      (val == "cover")    gFitMode = TextureProcessor::FitMode::Cover;
            else if (val == "contain")  gFitMode = TextureProcessor::FitMode::Contain;
            else if (val == "fill")     gFitMode = TextureProcessor::FitMode::Fill;
        }
        if (key == "matte") {
            // Format: "transparent" or "rrggbb" hex; empty/invalid → transparent.
            if (val == "transparent") gMatte = TextureProcessor::Matte{};
            else if (val.size() == 6) {
                auto h=[&](int o){int v=0;for(int i=0;i<2;i++){char c=val[o+i];v<<=4;if(c>='0'&&c<='9')v|=c-'0';else if(c>='a'&&c<='f')v|=c-'a'+10;else if(c>='A'&&c<='F')v|=c-'A'+10;}return (uint8_t)v;};
                gMatte = {h(0),h(2),h(4),255};
            }
        }
        if (key == "language") {
            Lang::SetCurrent(val);
            langFromConfig = true;
        }
        if (key == "theme") {
            ThemeNS::SetCurrent(val);
        }
    }
    fclose(f);
    // Pre-i18n configs have no language= line; treat as first launch.
    if (!langFromConfig) {
        Lang::SetCurrent(DetectSystemLanguage());
    }
}

static void SaveConfig() {
    FILE* f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    fprintf(f, "export_path=%s\n",     gExportPath.c_str());
    fprintf(f, "thumb_tip_seen=%d\n",  gThumbTipSeen     ? 1 : 0);
    fprintf(f, "save_warn_acked=%d\n", gSaveWarningAcked ? 1 : 0);
    fprintf(f, "max_backups=%d\n",     gMaxBackups);
    fprintf(f, "encoder=%s\n",
            gEncMode == TextureProcessor::Bc1Encoder::Custom ? "custom" : "pca");
    fprintf(f, "bc1_mode=%s\n",
            gBc1Mode == TextureProcessor::Bc1Mode::Auto      ? "auto" :
            gBc1Mode == TextureProcessor::Bc1Mode::FourColor ? "4color" : "3color");
    fprintf(f, "fit_mode=%s\n",
            gFitMode == TextureProcessor::FitMode::Cover   ? "cover" :
            gFitMode == TextureProcessor::FitMode::Contain ? "contain" : "fill");
    if (gMatte.a == 0) {
        fprintf(f, "matte=transparent\n");
    } else {
        fprintf(f, "matte=%02x%02x%02x\n", gMatte.r, gMatte.g, gMatte.b);
    }
    fprintf(f, "language=%s\n", Lang::Current().c_str());
    fprintf(f, "theme=%s\n",    ThemeNS::Current().c_str());
    fclose(f);
}

static void DrawFileBrowser() {
    HitClear();
    FillRect(40,36,SCREEN_W-80,SCREEN_H-66,COL_PANEL);
    DrawRect(40,36,SCREEN_W-80,SCREEN_H-66,COL_BORDER);
    if (gBrowseForExportDir)
        DrawTextC(Lang::T("browse.title.export"), SCREEN_W/2, 52, COL_GOLD, Font::Md);
    else
        DrawTextC(Lang::T(gBrowseForMii ? "browse.title.ltd" : "browse.title.png_ltd"), SCREEN_W/2, 52, COL_DIM, Font::Md);
    DrawTextC(gBrowseCurPath, SCREEN_W/2, 72, COL_DIM);
    if (gBrowseForExportDir)
        DrawTextC(Lang::T("browse.current") + ": " + gExportPath, SCREEN_W/2, 90, COL_DIM);
    if (gBrowseEntries.empty()) {
        DrawTextC(Lang::T("browse.empty"), SCREEN_W/2, SCREEN_H/2, COL_DIM);
    } else {
        int py = gBrowseForExportDir ? 106 : 88, ph=26;
        int pvis=(SCREEN_H-(gBrowseForExportDir?166:148))/ph;
        TTF_Font* f=GetFont(Font::Sm); int fh=0,fw=0;
        if(f) TTF_SizeUTF8(f,"A",&fw,&fh);
        int selRowY = py;
        bool sawSel = false;
        for (int i=0;i<pvis;i++){
            int idx=gBrowseScroll+i;
            if (idx>=(int)gBrowseEntries.size()) break;
            bool isSel=(idx==gBrowseSel);
            auto& entry=gBrowseEntries[idx];
            int ry = py+i*ph;
            if (isSel) { selRowY = ry; sawSel = true; }
            HitAdd(52,ry,SCREEN_W-104,ph-2, TSelBrowse, idx);
            FillRect(52,ry,SCREEN_W-104,ph-2, isSel?COL_SEL:COL_PANEL2);
            if (isSel && gTheme.cornerRadius == 0)
                DrawRectSquare(52,ry,SCREEN_W-104,ph-2,COL_GOLD);
            std::string label = (entry.isDir && entry.name!="[..]") ? "/"+entry.name : entry.name;
            DrawText(label, 60, ry+(ph-2-fh)/2, isSel?COL_TEXT:COL_DIM);
        }
        if (sawSel) {
            RequestHighlight((int)Screen::OnSwitch, /*sub=file browser*/7,
                             52, selRowY, SCREEN_W-104, ph-2, gDtSec);
            DrawAnimatedHighlight();
        }
    }
    if (gBrowseForExportDir)
        DrawFooter(Lang::T("browse.footer.export"));
    else
        DrawFooter(Lang::T("browse.footer.pick"));
}

// ─── Texture encoding settings tab ───────────────────────────────────────────
static void DrawTexSettings() {
    HitClear();
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawHeader(Lang::T("settings.header"));

    // Tab bar (same as DrawOnSwitch, with gear tab selected) — underline style.
    {
        int tw=118, th=28, gap=4, gearTw=36;
        int totalW=tw*5+gap*5+gearTw, tx=SCREEN_W/2-totalW/2, ty=28;
        struct { std::string label; OnSwitchMode mode; } tabs[]={
            {Lang::T("tab.webui"),    OnSwitchMode::WebUI},
            {Lang::T("tab.textures"), OnSwitchMode::UGC},
            {Lang::T("tab.mii"),      OnSwitchMode::MiiStats},
            {Lang::T("tab.player"),   OnSwitchMode::Player},
            {Lang::T("tab.map"),      OnSwitchMode::Map}};
        // None of the 5 base tabs are "selected" on this screen — the gear is.
        for (auto& t : tabs) {
            HitAdd(tx,ty,tw,th,TSetMode,(int)t.mode);
            DrawTextC(t.label,tx+tw/2,ty+th/2,COL_DIM);
            tx+=tw+gap;
        }
        // Gear represents the texture-settings tab.
        HitAdd(tx,ty,gearTw,th,TSetMode,(int)OnSwitchMode::TexSettings);
        DrawGearIcon(tx+gearTw/2,ty+th/2,th*38/100,COL_GOLD);
        // Underline slides from wherever it last sat (e.g. "Textures") to
        // beneath the gear, since the owner tag matches the other tab bars.
        RequestTabUnderline(0, /*owner=main tab bar*/100,
                            tx + (gearTw-24)/2, ty + th + 2, 24, 3, gDtSec);
        DrawTabUnderline(0);
    }

    // Content area — four compact rows (Encoder, BC1 mode, Fit, Background),
    // matching the WebUI's pill-button style: legend on the left, pill buttons
    // on the right, one-line hint below.
    const int contentX = (SCREEN_W - 720) / 2;
    const int contentY = 68;
    const int sectionW = 720;
    const int rowH     = 62;
    const int rowGap   = 8;

    auto drawPillRow = [&](int rowIdx, int sy, const std::string& label,
                           const std::string& hint, bool disabled) -> void {
        bool selRow = (gTexSettingsSel == rowIdx);
        SDL_Color bg = selRow ? COL_SEL : COL_PANEL;
        SDL_Color br = disabled ? SDL_Color{45,45,45,255}
                                : (selRow ? COL_GOLD : COL_BORDER);
        FillRect(contentX, sy, sectionW, rowH, bg);
        DrawRect(contentX, sy, sectionW, rowH, br);
        HitAdd(contentX, sy, sectionW, rowH, TSelTexSettingsItem, rowIdx);
        SDL_Color lblCol = disabled ? COL_DIM : (selRow ? COL_TEXT : COL_DIM);
        SDL_Color dscCol = disabled ? SDL_Color{55,55,55,255} : COL_DIM;
        DrawText(label, contentX + 16, sy + 8,  lblCol, Font::Md);
        DrawText(hint,  contentX + 16, sy + 36, dscCol, Font::Sm);
    };

    auto drawPills = [&](int sy, int btnCount, int btnW, int btnH,
                         int curIdx, bool disabled, int rowIdx,
                         const char* const* names, TouchFn cb) -> void {
        const int btnGap = 6;
        int bx = contentX + sectionW - btnCount*(btnW+btnGap) - 10;
        int by = sy + (rowH - btnH) / 2;
        bool selRow = (gTexSettingsSel == rowIdx);
        for (int k = 0; k < btnCount; k++) {
            bool on = (!disabled && curIdx == k);
            SDL_Color bg = disabled ? SDL_Color{30,30,30,255}
                                    : (on ? COL_GOLD : COL_BG);
            SDL_Color br = disabled ? SDL_Color{45,45,45,255}
                                    : (on ? COL_GOLD : (selRow ? COL_GOLD : COL_BORDER));
            SDL_Color tc = disabled ? SDL_Color{55,55,55,255}
                                    : (on ? COL_BG : COL_DIM);
            FillRect(bx, by, btnW, btnH, bg);
            DrawRect(bx, by, btnW, btnH, br);
            DrawTextC(names[k], bx + btnW/2, by + btnH/2, tc, Font::Sm);
            if (!disabled) HitAdd(bx, by, btnW, btnH, cb, k);
            bx += btnW + btnGap;
        }
    };

    // ── Row 0: Encoder ───────────────────────────────────────────────────────
    {
        int sy = contentY;
        std::string hint = (gEncMode == TextureProcessor::Bc1Encoder::Custom)
            ? Lang::T("settings.encoder.hint.custom")
            : Lang::T("settings.encoder.hint.pca");
        drawPillRow(0, sy, Lang::T("settings.encoder"), hint, false);
        const char* names[] = { "Gamma-aware", "PCA" };
        drawPills(sy, 2, 130, 28, (int)gEncMode, false, 0, names, TSetEncMode);
    }

    // ── Row 1: BC1 Mode ──────────────────────────────────────────────────────
    {
        bool disabled = (gEncMode != TextureProcessor::Bc1Encoder::Custom);
        int sy = contentY + (rowH + rowGap);
        std::string hint = disabled ? Lang::T("settings.bc1.disabled") :
            (gBc1Mode == TextureProcessor::Bc1Mode::Auto)       ? Lang::T("settings.bc1.hint.auto") :
            (gBc1Mode == TextureProcessor::Bc1Mode::FourColor)  ? Lang::T("settings.bc1.hint.fourColor") :
                                                                   Lang::T("settings.bc1.hint.threeColor");
        drawPillRow(1, sy, Lang::T("settings.bc1"), hint, disabled);
        const char* names[] = { "Auto", "4-color", "3-color" };
        drawPills(sy, 3, 80, 28, (int)gBc1Mode, disabled, 1, names, TSetBc1Mode);
    }

    // ── Row 2: Fit mode ──────────────────────────────────────────────────────
    {
        int sy = contentY + 2 * (rowH + rowGap);
        std::string hint =
            (gFitMode == TextureProcessor::FitMode::Cover)   ? Lang::T("settings.fit.hint.cover")   :
            (gFitMode == TextureProcessor::FitMode::Contain) ? Lang::T("settings.fit.hint.contain") :
                                                                Lang::T("settings.fit.hint.fill");
        drawPillRow(2, sy, Lang::T("settings.fit"), hint, false);
        const char* names[] = { "Cover", "Contain", "Fill" };
        drawPills(sy, 3, 80, 28, (int)gFitMode, false, 2, names, TSetFitMode);
    }

    // ── Row 3: Background (matte) — only relevant when Fit == Contain ───────
    {
        bool disabled = (gFitMode != TextureProcessor::FitMode::Contain);
        int sy = contentY + 3 * (rowH + rowGap);
        int matteIdx = (gMatte.a == 0)                       ? 0
                     : (gMatte.r==255 && gMatte.g==255 && gMatte.b==255) ? 1
                     : (gMatte.r==0   && gMatte.g==0   && gMatte.b==0)   ? 2
                                                                          : 0;
        std::string hint = disabled
            ? Lang::T("settings.matte.disabled")
            : Lang::T("settings.matte.desc");
        drawPillRow(3, sy, Lang::T("settings.matte"), hint, disabled);
        const char* names[] = { "Transparent", "White", "Black" };
        drawPills(sy, 3, 96, 28, matteIdx, disabled, 3, names, TSetMatte);
    }

    DrawFooter(Lang::T("settings.footer"));
    Present();
}

static void DrawOnSwitch() {
    HitClear();
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);

    // Header with subtitle
    std::string subtitle;
    switch(gOnSwitchMode){
        case OnSwitchMode::UGC:      subtitle=Lang::T("tab.textures"); break;
        case OnSwitchMode::Player:   subtitle=Lang::T("tab.player"); break;
        case OnSwitchMode::MiiStats: subtitle=Lang::T("tab.mii"); break;
        default:                     subtitle=Lang::T("tab.webui"); break;
    }
    DrawHeader(subtitle);

    // Tab bar — 5 text tabs
    {
        int tw=130, th=22, gap=4;
        int totalW=tw*5+gap*4, tx=SCREEN_W/2-totalW/2, ty=28;
        struct { std::string label; OnSwitchMode mode; } tabs[]={
            {Lang::T("tab.webui"),    OnSwitchMode::WebUI},
            {Lang::T("tab.textures"), OnSwitchMode::UGC},
            {Lang::T("tab.mii"),      OnSwitchMode::MiiStats},
            {Lang::T("tab.player"),   OnSwitchMode::Player},
            {Lang::T("tab.map"),      OnSwitchMode::Map},
        };
        int ulX = tx, ulY = ty + th + 2;
        bool ulHave = false;
        for (auto& t : tabs) {
            bool sel = gOnSwitchMode==t.mode;
            HitAdd(tx,ty,tw,th, TSetMode, (int)t.mode);
            DrawTextC(t.label, tx+tw/2, ty+th/2, sel?COL_GOLD:COL_DIM);
            if (sel) { ulX = tx + (tw-72)/2; ulHave = true; }
            tx+=tw+gap;
        }
        if (ulHave) {
            RequestTabUnderline(/*slot*/0, /*owner=main tab bar*/100,
                                ulX, ulY, 72, 3, gDtSec);
            DrawTabUnderline(/*slot*/0);
        }
    }

    // ── File browser (overlay) ───────────────────────────────────────────────
    if (gShowFileBrowser) {
        DrawFileBrowser();
        Present(); return;
    }

    // ── UGC tab ──────────────────────────────────────────────────────────────
    if (gOnSwitchMode == OnSwitchMode::UGC) {
        // Search bar above the list. Sizes live at file scope (UGC_SEARCH_*)
        // so input/touch handlers stay aligned with the render.
        const int SEARCH_H   = UGC_SEARCH_H;
        const int SEARCH_GAP = UGC_SEARCH_GAP;
        int sbX = LIST_X, sbY = LIST_Y, sbW = LIST_W;
        bool searchActive = !gUgcFilter.empty();
        // Hit-area: tap to open keyboard; clear button hits separately when active
        int clearW = searchActive ? 28 : 0;
        HitAdd(sbX, sbY, sbW - clearW, SEARCH_H, TUgcOpenSearch, 0);
        FillRect(sbX, sbY, sbW, SEARCH_H, COL_BG);
        DrawRect(sbX, sbY, sbW, SEARCH_H, searchActive ? COL_ACCENT : COL_BORDER);
        TTF_Font* fsm0 = GetFont(Font::Sm);
        int sh = 0;
        if (searchActive) {
            std::string lbl = Lang::T("ugc.filter") + ":  " + gUgcFilter;
            int tw0=0; if (fsm0) TTF_SizeUTF8(fsm0, lbl.c_str(), &tw0, &sh);
            DrawText(lbl, sbX+8, sbY+(SEARCH_H-sh)/2, COL_ACCENT);
            // Clear (✕) button
            HitAdd(sbX+sbW-clearW, sbY, clearW, SEARCH_H, TUgcClearSearch, 0);
            DrawRect(sbX+sbW-clearW, sbY, clearW, SEARCH_H, COL_BORDER);
            DrawTextC("X", sbX+sbW-clearW/2, sbY+SEARCH_H/2, COL_DIM, Font::Md);
        } else {
            std::string placeholder = Lang::T("ugc.search.placeholder");
            int tw0=0; if (fsm0) TTF_SizeUTF8(fsm0, placeholder.c_str(), &tw0, &sh);
            DrawText(placeholder, sbX+8, sbY+(SEARCH_H-sh)/2, COL_DIM);
        }

        // List panel below the search bar (with a clear gap so the two read as separate)
        int listPanelY = LIST_Y + SEARCH_H + SEARCH_GAP;
        int listPanelH = LIST_H - SEARCH_H - SEARCH_GAP;
        FillRect(LIST_X-4, listPanelY, LIST_W+8, listPanelH+4, COL_PANEL);
        DrawRect(LIST_X-4, listPanelY, LIST_W+8, listPanelH+4, COL_BORDER);

        const int LIST_TOP_Y = listPanelY + 4;
        const int LIST_VIS_H = listPanelH - 4;
        const int LIST_VISIBLE = LIST_VIS_H / ITEM_H;
        int totalFiltered = (int)gFilteredEntries.size();

        if (gEntryScroll < 0) gEntryScroll = 0;
        if (totalFiltered > 0 && gEntryScroll > totalFiltered - LIST_VISIBLE)
            gEntryScroll = std::max(0, totalFiltered - LIST_VISIBLE);

        int selRowY = LIST_TOP_Y;
        bool sawSel = false;
        for (int i = 0; i < LIST_VISIBLE; i++) {
            int filtPos = gEntryScroll + i;
            if (filtPos >= totalFiltered) break;
            int realIdx = gFilteredEntries[filtPos];
            bool sel = (realIdx == gEntrySel);
            int ry = LIST_TOP_Y + i*ITEM_H;
            if (sel) { selRowY = ry; sawSel = true; }
            HitAdd(LIST_X, ry, LIST_W, ITEM_H-1, TSelUgc, realIdx);
            FillRect(LIST_X, ry, LIST_W, ITEM_H-1, sel?COL_SEL:COL_BG);
            if (sel && gTheme.cornerRadius == 0)
                DrawRectSquare(LIST_X, ry, LIST_W, ITEM_H-1, COL_GOLD);
            std::string dn = MiiManager::GetUgcName(gEntries[realIdx].stem);
            if (dn.empty()) dn = FormatStem(gEntries[realIdx].stem);
            TTF_Font* f = GetFont(Font::Sm); int fw=0,fh=0; if(f)TTF_SizeUTF8(f,dn.c_str(),&fw,&fh);
            DrawText(dn, LIST_X+6, ry+(ITEM_H-fh)/2, sel?COL_TEXT:COL_DIM);
        }

        if (sawSel) {
            RequestHighlight((int)Screen::OnSwitch, /*sub=ugc list*/1,
                             LIST_X, selRowY, LIST_W, ITEM_H-1, gDtSec);
            DrawAnimatedHighlight();
        }

        if (totalFiltered == 0 && !gEntries.empty()) {
            DrawTextC(Lang::T("ugc.no.matches"), LIST_X+LIST_W/2, LIST_TOP_Y + LIST_VIS_H/2, COL_DIM, Font::Md);
        }

        if (totalFiltered > LIST_VISIBLE) {
            int barH = listPanelH * LIST_VISIBLE / totalFiltered;
            int barY = listPanelY + listPanelH * gEntryScroll / totalFiltered;
            FillRect(LIST_X+LIST_W+2, barY, 4, barH, COL_BORDER);
        }
        FillRect(PREVIEW_X,PREVIEW_Y,PREVIEW_W,PREVIEW_H,{8,8,8,255});
        DrawRect(PREVIEW_X,PREVIEW_Y,PREVIEW_W,PREVIEW_H,COL_BORDER);
        // Header strip: show export/import message if active, else file stem
        if (!gOnSwitchMsg.empty())
            DrawTextC(gOnSwitchMsg, PREVIEW_X+PREVIEW_W/2, PREVIEW_Y+14, gOnSwitchMsgCol);
        else
            DrawTextC(gPreviewStem, PREVIEW_X+PREVIEW_W/2, PREVIEW_Y+14, COL_TEXT);
        {
            // Reserve space at the bottom for the import/export buttons
            const int kBtnH=46, kBtnGap=8, kBtnSlot=kBtnH+kBtnGap+4; // 58px total
            int imgY = PREVIEW_Y + 28;
            int imgH = PREVIEW_H - 28 - kBtnSlot;
            if (gPreviewTex) {
                int tw,th; SDL_QueryTexture(gPreviewTex,nullptr,nullptr,&tw,&th);
                float scale=std::min((float)PREVIEW_W/tw,(float)imgH/th);
                int dw=(int)(tw*scale), dh=(int)(th*scale);
                SDL_Rect dst{PREVIEW_X+(PREVIEW_W-dw)/2, imgY+(imgH-dh)/2, dw, dh};
                SDL_RenderCopy(gRen,gPreviewTex,nullptr,&dst);
            } else {
                DrawTextC(Lang::T("ugc.no.preview"), PREVIEW_X+PREVIEW_W/2, imgY+imgH/2, COL_BORDER);
            }
            if (!gEntries.empty()) {
                int cx   = PREVIEW_X + PREVIEW_W/2;
                int btnY = imgY + imgH + kBtnGap;
                const int btnW=148, btnGap=10, bgSep=28, gearW=kBtnH;
                int totalW = 3*btnW + 2*btnGap + bgSep + btnW + btnGap + gearW;
                int bx0   = cx - totalW/2;
                int bxLtd = bx0 + 2*(btnW+btnGap);
                int bxBg  = bxLtd + btnW + btnGap + bgSep;
                int bxGear = bxBg + btnW + btnGap;
                // A: import
                FillRect(bx0,             btnY, btnW, kBtnH, COL_PANEL);
                DrawRect(bx0,             btnY, btnW, kBtnH, COL_ACCENT);
                DrawTextC("A  " + Lang::T("ugc.import"), bx0+btnW/2, btnY+kBtnH/2, COL_ACCENT, Font::Md);
                // Y: export PNG
                FillRect(bx0+btnW+btnGap, btnY, btnW, kBtnH, COL_PANEL);
                DrawRect(bx0+btnW+btnGap, btnY, btnW, kBtnH, COL_GOLD);
                DrawTextC("Y  " + Lang::T("ugc.exportpic"), bx0+btnW+btnGap+btnW/2, btnY+kBtnH/2, COL_GOLD, Font::Md);
                // export .ltd (touch)
                FillRect(bxLtd,           btnY, btnW, kBtnH, COL_PANEL);
                DrawRect(bxLtd,           btnY, btnW, kBtnH, COL_GOLD);
                DrawTextC(Lang::T("ugc.exportltd"), bxLtd+btnW/2, btnY+kBtnH/2, COL_GOLD, Font::Md);
                HitAdd(bxLtd, btnY, btnW, kBtnH, TDoUgcExportLtd, 0);
                // X: remove BG (separated by gap)
                FillRect(bxBg,            btnY, btnW, kBtnH, COL_SEL);
                DrawRect(bxBg,            btnY, btnW, kBtnH, {90,140,200,255});
                DrawTextC("X  " + Lang::T("ugc.removebg"), bxBg+btnW/2, btnY+kBtnH/2, {90,140,200,255}, Font::Md);
                // Gear: open encoding settings tab (touch only, small square)
                FillRect(bxGear, btnY, gearW, kBtnH, COL_PANEL);
                DrawRect(bxGear, btnY, gearW, kBtnH, COL_BORDER);
                DrawGearIcon(bxGear + gearW/2, btnY + kBtnH/2, kBtnH * 30 / 100, COL_DIM);
                HitAdd(bxGear, btnY, gearW, kBtnH, TSetMode, (int)OnSwitchMode::TexSettings);
            }
        }
        DrawFooter(Lang::T("ugc.footer"));
    }

    // ── WebUI tab ─────────────────────────────────────────────────────────────
    else {
        bool wifiActive = !gIP.empty() && gIP!="?.?.?.?";
        std::string url = "http://"+gIP+":"+std::to_string(HTTP_PORT);

        // Layout: log box on left, wifi status + QR on right
        int margin=12;
        int contentY = LIST_Y;
        int contentH = SCREEN_H-contentY-32;

        // Log box — left half
        int logBoxW = SCREEN_W/2-margin-margin/2;
        int logBoxH = contentH;
        FillRect(margin,contentY,logBoxW,logBoxH,COL_PANEL);
        DrawRect(margin,contentY,logBoxW,logBoxH,COL_BORDER);
        DrawText(Lang::T("webui.log.title"), margin+8, contentY+6, COL_DIM);
        {
            int logY=contentY+26, logMaxY=contentY+logBoxH-10, lineH=20;
            int maxLines=(logMaxY-logY)/lineH;
            int startLine=(int)gLog.size()>maxLines?(int)gLog.size()-maxLines:0;
            for (int i=startLine;i<(int)gLog.size();i++) {
                if (logY+lineH>logMaxY) break;
                DrawText(gLog[i].text, margin+8, logY, gLog[i].col);
                logY+=lineH;
            }
            if (gLog.empty())
                DrawTextC(Lang::T("webui.log.empty"), margin+logBoxW/2, contentY+logBoxH/2, COL_BORDER);
        }

        // Right half — URL top, WiFi status under it, QR code at bottom
        int rightX = SCREEN_W/2+margin/2;
        int rightW = SCREEN_W-rightX-margin;

        if (wifiActive) {
            TTF_Font* fsm=GetFont(Font::Sm); int lw=0,lh=0;
            if(fsm) TTF_SizeUTF8(fsm,"WiFi : ",&lw,&lh);

            // URL in styled box with gap from tab bar
            int urlY = contentY + 16;
            int urlBoxH = 0;
            {
                TTF_Font* fmd = GetFont(Font::Md);
                int urlTW=0, urlTH=0;
                if (fmd) TTF_SizeUTF8(fmd, url.c_str(), &urlTW, &urlTH);
                int bpx=18, bpy=8;
                int urlBoxW = urlTW + bpx*2;
                urlBoxH = urlTH + bpy*2;
                int urlBoxX = rightX + (rightW - urlBoxW) / 2;
                FillRect(urlBoxX, urlY, urlBoxW, urlBoxH, {18, 18, 38, 255});
                DrawRect(urlBoxX, urlY, urlBoxW, urlBoxH, COL_GOLD);
                DrawText(url, urlBoxX + bpx, urlY + bpy, COL_GOLD, Font::Md);
            }

            // WiFi status below URL box with comfortable gap
            int statusY = urlY + urlBoxH + 16;
            int iw=0;
            std::string activeS = Lang::T("webui.active");
            std::string inactiveS = Lang::T("webui.inactive");
            if(fsm) TTF_SizeUTF8(fsm, (wifiActive?activeS:inactiveS).c_str(),&iw,&lh);
            int totalStatusW=lw+iw;
            int sxBase=rightX+(rightW-totalStatusW)/2;
            DrawText("WiFi : ", sxBase, statusY, COL_DIM);
            DrawText(activeS, sxBase+lw, statusY, COL_GREEN);

            // MTP status below WiFi status
            {
                bool mtpOn = MtpServer::IsSessionOpen();
                std::string mtpVal = mtpOn ? activeS : inactiveS;
                SDL_Color mtpCol = mtpOn ? COL_GREEN : COL_DIM;
                int lineH = (lh > 0) ? lh : 20;
                int mtpY = statusY + lineH + 8;
                int mw=0, mw2=0, mh=0;
                TTF_Font* fss = GetFont(Font::Sm);
                if(fss) TTF_SizeUTF8(fss, "MTP  : ", &mw, &mh);
                if(fss) TTF_SizeUTF8(fss, mtpVal.c_str(), &mw2, &mh);
                int mxBase = rightX + (rightW - (mw + mw2)) / 2;
                DrawText("MTP  : ", mxBase, mtpY, COL_DIM, Font::Sm);
                DrawText(mtpVal, mxBase + mw, mtpY, mtpCol, Font::Sm);
            }
            
            // QR code below MTP status
            QR::Mat mat;
            if (QR::build(url, mat)) {
                int qrTop = statusY + lh + 8 + lh + 14;
                int qrAvailW = rightW;
                int qrAvailH = contentY + contentH - qrTop;
                int ms = std::min(qrAvailW, qrAvailH) / (mat.N+4);
                if (ms<3) ms=3;
                int border=2;
                int qrSize=mat.N*ms+border*2*ms;
                int qrX=rightX+(rightW-qrSize)/2;
                FillRect(qrX,qrTop,qrSize,qrSize,{255,255,255,255});
                SDL_SetRenderDrawColor(gRen,0,0,0,255);
                for (int y=0;y<mat.N;y++) for (int x=0;x<mat.N;x++) {
                    if (mat.get(x,y)) {
                        SDL_Rect r{qrX+(x+border)*ms, qrTop+(y+border)*ms, ms, ms};
                        SDL_RenderFillRect(gRen,&r);
                    }
                }
            }
        } else {
            // No wifi — show WiFi and MTP status centered
            TTF_Font* fsm=GetFont(Font::Sm); int lw=0,lh=0;
            if(fsm) TTF_SizeUTF8(fsm,"WiFi : ",&lw,&lh);
            int cy2 = contentY+contentH/2-lh;
            std::string activeS = Lang::T("webui.active");
            std::string inactiveS = Lang::T("webui.inactive");
            DrawText("WiFi : ",  rightX+(rightW-lw)/2-20, cy2, COL_DIM);
            DrawText(inactiveS, rightX+(rightW-lw)/2-20+lw, cy2, COL_RED);

            // MTP status below
            {
                bool mtpOn = MtpServer::IsSessionOpen();
                std::string mtpVal = mtpOn ? activeS : inactiveS;
                SDL_Color mtpCol = mtpOn ? COL_GREEN : COL_DIM;
                int lineH = (lh > 0) ? lh : 20;
                int mw=0, mw2=0, mh=0;
                TTF_Font* fss = GetFont(Font::Sm);
                if(fss) TTF_SizeUTF8(fss, "MTP  : ", &mw, &mh);
                if(fss) TTF_SizeUTF8(fss, mtpVal.c_str(), &mw2, &mh);
                int mxBase = rightX + (rightW - (mw + mw2)) / 2;
                DrawText("MTP  : ", mxBase, cy2 + lineH + 8, COL_DIM, Font::Sm);
                DrawText(mtpVal, mxBase + mw, cy2 + lineH + 8, mtpCol, Font::Sm);
            }
        }

        DrawFooter(Lang::T("webui.footer"));
    }

    Present();
}

// ─── Wishes bulk operations ──────────────────────────────────────────────────
// The wish list is stored as three parallel arrays on the player save:
//   Liberation.WishInfo.WishIdValue  — UIntArray of wish hashes
//   Liberation.WishInfo.IsLiberated  — BoolArray, parallel
//   Liberation.WishInfo.IsNew        — BoolArray, parallel (optional)
// Since these arrays grow when new wishes are appended, we rewrite the whole
// entry payload instead of using SetUIntAt/SetBoolAt (which require fixed size).
static bool WishesAvailable() {
    if (!gPlayerSav.loaded) return false;
    static const uint32_t H_IDS = SaveEditor::Hash("Liberation.WishInfo.WishIdValue");
    static const uint32_t H_LIB = SaveEditor::Hash("Liberation.WishInfo.IsLiberated");
    bool a = false, b = false;
    for (const auto& e : gPlayerSav.entries) {
        if (e.hash == H_IDS && e.type == SaveEditor::DT_UIntArray) a = true;
        if (e.hash == H_LIB && e.type == SaveEditor::DT_BoolArray) b = true;
    }
    return a && b;
}
static SaveEditor::Entry* WishesFind(uint32_t h) {
    for (auto& e : gPlayerSav.entries) if (e.hash == h) return &e;
    return nullptr;
}
static std::vector<uint32_t> WishesReadUIntArr(uint32_t h) {
    std::vector<uint32_t> out;
    auto* e = WishesFind(h);
    if (!e || e->type != SaveEditor::DT_UIntArray || e->payload.size() < 4) return out;
    uint32_t n = (uint32_t)e->payload[0] | ((uint32_t)e->payload[1]<<8) | ((uint32_t)e->payload[2]<<16) | ((uint32_t)e->payload[3]<<24);
    out.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        size_t off = 4 + i*4;
        if (off + 4 > e->payload.size()) break;
        uint32_t v = (uint32_t)e->payload[off] | ((uint32_t)e->payload[off+1]<<8)
                   | ((uint32_t)e->payload[off+2]<<16) | ((uint32_t)e->payload[off+3]<<24);
        out.push_back(v);
    }
    return out;
}
static std::vector<bool> WishesReadBoolArr(uint32_t h) {
    std::vector<bool> out;
    auto* e = WishesFind(h);
    if (!e || e->type != SaveEditor::DT_BoolArray || e->payload.size() < 4) return out;
    uint32_t n = (uint32_t)e->payload[0] | ((uint32_t)e->payload[1]<<8) | ((uint32_t)e->payload[2]<<16) | ((uint32_t)e->payload[3]<<24);
    out.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        size_t off = 4 + (size_t)(i >> 3);
        if (off >= e->payload.size()) break;
        out[i] = ((e->payload[off] >> (i & 7)) & 1) != 0;
    }
    return out;
}
static bool WishesWriteUIntArr(uint32_t h, const std::vector<uint32_t>& vals) {
    auto* e = WishesFind(h);
    if (!e || e->type != SaveEditor::DT_UIntArray) return false;
    size_t n = vals.size();
    std::vector<uint8_t> buf(4 + n * 4, 0);
    buf[0] = (uint8_t)(n & 0xFF); buf[1] = (uint8_t)((n >> 8) & 0xFF);
    buf[2] = (uint8_t)((n >>16) & 0xFF); buf[3] = (uint8_t)((n >> 24) & 0xFF);
    for (size_t i = 0; i < n; i++) {
        uint32_t v = vals[i];
        size_t off = 4 + i*4;
        buf[off  ] = (uint8_t)(v & 0xFF); buf[off+1] = (uint8_t)((v >> 8) & 0xFF);
        buf[off+2] = (uint8_t)((v >> 16) & 0xFF); buf[off+3] = (uint8_t)((v >> 24) & 0xFF);
    }
    e->payload = std::move(buf);
    return true;
}
static bool WishesWriteBoolArr(uint32_t h, const std::vector<bool>& vals) {
    auto* e = WishesFind(h);
    if (!e || e->type != SaveEditor::DT_BoolArray) return false;
    size_t n = vals.size();
    // On-disk bit-packing is 4-byte aligned.
    size_t byteCount = std::max<size_t>(4, (n + 7) / 8);
    size_t padded = (byteCount + 3) & ~size_t{3};
    std::vector<uint8_t> buf(4 + padded, 0);
    buf[0] = (uint8_t)(n & 0xFF); buf[1] = (uint8_t)((n >> 8) & 0xFF);
    buf[2] = (uint8_t)((n >>16) & 0xFF); buf[3] = (uint8_t)((n >> 24) & 0xFF);
    for (size_t i = 0; i < n; i++) {
        if (!vals[i]) continue;
        buf[4 + (i >> 3)] |= (uint8_t)(1u << (i & 7));
    }
    e->payload = std::move(buf);
    return true;
}
// Returns the number of entries that actually changed.
static int WishesBulkUnlock() {
    static const uint32_t H_IDS = SaveEditor::Hash("Liberation.WishInfo.WishIdValue");
    static const uint32_t H_LIB = SaveEditor::Hash("Liberation.WishInfo.IsLiberated");
    static const uint32_t H_NEW = SaveEditor::Hash("Liberation.WishInfo.IsNew");
    auto ids = WishesReadUIntArr(H_IDS);
    auto lib = WishesReadBoolArr(H_LIB);
    bool hasNew = (WishesFind(H_NEW) != nullptr);
    std::vector<bool> isNew = hasNew ? WishesReadBoolArr(H_NEW) : std::vector<bool>{};
    while (lib.size()   < ids.size()) lib.push_back(false);
    if (hasNew) while (isNew.size() < ids.size()) isNew.push_back(false);
    // Build a hash → index map.
    std::map<uint32_t,size_t> idx;
    for (size_t i = 0; i < ids.size(); i++) idx[ids[i]] = i;
    int changed = 0;
    for (int wi = 0; wi < WishesData::WISH_COUNT; wi++) {
        uint32_t h = WishesData::WISHES[wi].hash;
        auto it = idx.find(h);
        if (it == idx.end()) {
            ids.push_back(h); lib.push_back(true);
            if (hasNew) isNew.push_back(false);
            idx[h] = ids.size() - 1;
            changed++;
        } else if (!lib[it->second]) {
            lib[it->second] = true; changed++;
        }
    }
    if (changed) {
        WishesWriteUIntArr(H_IDS, ids);
        WishesWriteBoolArr(H_LIB, lib);
        if (hasNew) WishesWriteBoolArr(H_NEW, isNew);
        gPlayerSavDirty = true;
    }
    return changed;
}
static int WishesBulkReset() {
    static const uint32_t H_LIB = SaveEditor::Hash("Liberation.WishInfo.IsLiberated");
    auto lib = WishesReadBoolArr(H_LIB);
    int changed = 0;
    for (auto v : lib) if (v) changed++;
    if (changed == 0) return 0;
    for (size_t i = 0; i < lib.size(); i++) lib[i] = false;
    WishesWriteBoolArr(H_LIB, lib);
    gPlayerSavDirty = true;
    return changed;
}
static int WishesLiberatedCount() {
    static const uint32_t H_LIB = SaveEditor::Hash("Liberation.WishInfo.IsLiberated");
    auto lib = WishesReadBoolArr(H_LIB);
    int n = 0;
    for (auto v : lib) if (v) n++;
    return n;
}

// ─── Player save editor ───────────────────────────────────────────────────────

static const int SE_LIST_X   = 12;
static const int SE_LIST_W   = 320;
static const int SE_DETAIL_X = SE_LIST_X + SE_LIST_W + 8;
static const int SE_DETAIL_W = SCREEN_W - SE_DETAIL_X - 8;
static const int SE_TOP_Y    = LIST_Y;
static const int SE_ROW_H    = 52;

// i18n key = prefix + label with spaces stripped (e.g. "Boot Count" → "player.field.BootCount").
static std::string FieldKey(const char* prefix, const char* label) {
    std::string k = prefix;
    for (const char* p = label; *p; p++) if (*p != ' ') k += *p;
    return Lang::T(k);
}
static std::string MiiLabelT   (const char* label) { return FieldKey("mii.field.",    label); }
static std::string MiiDescT    (const char* label) { return FieldKey("mii.desc.",     label); }
static std::string PlayerLabelT(const char* label) { return FieldKey("player.field.", label); }
static std::string PlayerDescT (const char* label) { return FieldKey("player.desc.",  label); }

static void DrawPlayer() {
    HitClear();
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    DrawHeader(Lang::T("tab.player"));

    // Tab bar (same as DrawOnSwitch)
    {
        int tw=130, th=22, gap=4;
        int totalW=tw*5+gap*4, tx=SCREEN_W/2-totalW/2, ty=28;
        struct { std::string label; OnSwitchMode mode; } tabs[]={
            {Lang::T("tab.webui"),OnSwitchMode::WebUI},{Lang::T("tab.textures"),OnSwitchMode::UGC},
            {Lang::T("tab.mii"),OnSwitchMode::MiiStats},{Lang::T("tab.player"),OnSwitchMode::Player},
            {Lang::T("tab.map"),OnSwitchMode::Map}};
        int ulX = tx, ulY = ty + th + 2;
        bool ulHave = false;
        for (auto& t : tabs) {
            bool sel=gOnSwitchMode==t.mode;
            HitAdd(tx,ty,tw,th,TSetMode,(int)t.mode);
            DrawTextC(t.label,tx+tw/2,ty+th/2,sel?COL_GOLD:COL_DIM);
            if (sel) { ulX = tx + (tw-72)/2; ulHave = true; }
            tx+=tw+gap;
        }
        if (ulHave) {
            RequestTabUnderline(0, 100, ulX, ulY, 72, 3, gDtSec);
            DrawTabUnderline(0);
        }
    }

    if (!gPlayerSav.loaded) {
        if (!gPlayerMsg.empty()) {
            DrawTextC(gPlayerMsg, SCREEN_W/2, SCREEN_H/2 - 14, COL_RED, Font::Md);
            if (gPlayerMsg.find("Cannot open") != std::string::npos) {
                DrawTextC(Lang::T("player.notfound.hint"),
                          SCREEN_W/2, SCREEN_H/2 + 18, COL_DIM, Font::Sm);
            } else {
                DrawTextC(Lang::T("player.parse.fail.l1"),
                          SCREEN_W/2, SCREEN_H/2 + 18, COL_DIM, Font::Sm);
                DrawTextC(Lang::T("player.parse.fail.l2"),
                          SCREEN_W/2, SCREEN_H/2 + 38, COL_DIM, Font::Sm);
            }
        } else {
            DrawTextC(Lang::T("player.loading"), SCREEN_W/2, SCREEN_H/2, COL_DIM, Font::Md);
        }
        DrawFooter(Lang::T("footer.back"));
        Present();
        return;
    }

    // Left panel: field list (compact rows)
    FillRect(SE_LIST_X-4, SE_TOP_Y, SE_LIST_W+8, SCREEN_H-SE_TOP_Y-32, COL_PANEL);
    DrawRect(SE_LIST_X-4, SE_TOP_Y, SE_LIST_W+8, SCREEN_H-SE_TOP_Y-32, COL_BORDER);

    static const int PL_ROW_H = 33;
    int plListH = SCREEN_H - SE_TOP_Y - 32;
    int plVis   = plListH / PL_ROW_H;
    if (gPlayerFieldSel < gPlayerScroll) gPlayerScroll = gPlayerFieldSel;
    if (gPlayerFieldSel >= gPlayerScroll + plVis) gPlayerScroll = gPlayerFieldSel - plVis + 1;
    if (gPlayerScroll < 0) gPlayerScroll = 0;

    TTF_Font* fsm_pl = GetFont(Font::Sm);
    int plSelRowY = SE_TOP_Y;
    bool plSawSel = false;
    for (int i = gPlayerScroll; i < PLAYER_FIELD_COUNT && i < gPlayerScroll + plVis; i++) {
        bool sel = (i == gPlayerFieldSel);
        int ry = SE_TOP_Y + (i - gPlayerScroll) * PL_ROW_H;
        if (sel) { plSelRowY = ry; plSawSel = true; }
        HitAdd(SE_LIST_X, ry, SE_LIST_W, PL_ROW_H-1, TSelPlayerField, i);
        FillRect(SE_LIST_X, ry, SE_LIST_W, PL_ROW_H-1, sel?COL_SEL:COL_BG);
        if (sel && gTheme.cornerRadius == 0)
            DrawRectSquare(SE_LIST_X, ry, SE_LIST_W, PL_ROW_H-1, COL_GOLD);

        const auto& fd = PLAYER_FIELDS[i];
        uint32_t fh = PFHash(fd);
        std::string labelTr = PlayerLabelT(fd.label);
        int lw=0,lh=0;
        if(fsm_pl) TTF_SizeUTF8(fsm_pl, labelTr.c_str(), &lw, &lh);
        DrawText(labelTr, SE_LIST_X+8, ry+(PL_ROW_H-lh)/2, sel?COL_TEXT:COL_DIM);

        std::string val;
        if (fd.isStr) {
            val = SaveEditor::GetWStr32(gPlayerSav, fh);
            if (val.size() > 16) val = val.substr(0,14) + "..";
        } else if (fd.isEnum) {
            uint32_t ev = SaveEditor::GetEnum(gPlayerSav, fh);
            val = std::string(CurrencySymbol(ev));
        } else if (fd.isSkinTone) {
            val = std::to_string(SaveEditor::GetUInt(gPlayerSav, fh)+1);
        } else if (fd.isIslandSz) {
            static const char* SL[] = {"50x36","70x50","90x64","120x80"};
            uint32_t sv = SaveEditor::GetAnyScalar(gPlayerSav, fh);
            val = (sv>=1&&sv<=4) ? SL[sv-1] : "?";
        } else if (fd.isRegion) {
            uint32_t ev = SaveEditor::GetEnum(gPlayerSav, fh);
            val = "?";
            for (int ri=0; ri<REGION_OPTION_COUNT; ri++)
                if (SaveEditor::Hash(REGION_OPTIONS[ri].name)==ev) { val=REGION_OPTIONS[ri].label; break; }
            if (val.size()>12) val=val.substr(0,10)+"..";
        } else if (fd.isInt) {
            val = std::to_string((int32_t)SaveEditor::GetAnyScalar(gPlayerSav, fh));
        } else if (fd.isUInt) {
            val = std::to_string(SaveEditor::GetUInt(gPlayerSav, fh));
        } else {
            val = std::to_string(SaveEditor::GetAnyScalar(gPlayerSav, fh));
        }
        int vw=0,vh=0;
        if(fsm_pl) TTF_SizeUTF8(fsm_pl, val.c_str(), &vw, &vh);
        DrawText(val, SE_LIST_X+SE_LIST_W-vw-8, ry+(PL_ROW_H-vh)/2, sel?COL_ACCENT:COL_DIM);
    }

    if (plSawSel) {
        RequestHighlight((int)Screen::OnSwitch, /*sub=player field list*/2,
                         SE_LIST_X, plSelRowY, SE_LIST_W, PL_ROW_H-1, gDtSec);
        DrawAnimatedHighlight();
    }

    // Right panel: edit controls for selected field
    FillRect(SE_DETAIL_X, SE_TOP_Y, SE_DETAIL_W, SCREEN_H-SE_TOP_Y-32, {8,8,8,255});
    DrawRect(SE_DETAIL_X, SE_TOP_Y, SE_DETAIL_W, SCREEN_H-SE_TOP_Y-32, COL_BORDER);

    const auto& fd = PLAYER_FIELDS[gPlayerFieldSel];
    uint32_t ph = PFHash(fd);
    int cx = SE_DETAIL_X + SE_DETAIL_W/2;
    int midY = SE_TOP_Y + (SCREEN_H-SE_TOP_Y-32)/2 - 36;

    DrawTextC(PlayerLabelT(fd.label), cx, midY-102, COL_DIM, Font::Md);
    DrawTextC(PlayerDescT(fd.label),  cx, midY-76,  COL_DIM, Font::Sm);

    if (fd.isStr) {
        std::string val = SaveEditor::GetWStr32(gPlayerSav, ph);
        DrawTextC(val.empty() ? Lang::T("empty") : val, cx, midY-40, COL_TEXT, Font::Lg);
        int bw=260, bh=40, bx=cx-bw/2, by=midY+10;
        HitAdd(bx,by,bw,bh, TPlayerKbd, 0);
        FillRect(bx,by,bw,bh,COL_SEL); DrawRect(bx,by,bw,bh,COL_ACCENT);
        DrawTextC("A  " + Lang::T("player.kbd.edit"), cx, by+bh/2, COL_ACCENT, Font::Md);
        const char* howName = (strcmp(fd.fieldName,"Player.Name")==0)       ? "Player.HowToCallName"
                            : (strcmp(fd.fieldName,"Player.IslandName")==0)  ? "Player.HowToCallIslandName"
                            : nullptr;
        if (howName) {
            std::string how = SaveEditor::GetWStr32(gPlayerSav, SaveEditor::Hash(howName));
            const std::string& dispHow = how.empty() ? val : how;
            DrawTextC(dispHow.empty()? Lang::T("empty") : dispHow, cx, by+bh+16, COL_TEXT, Font::Sm);
            int pbX=cx-bw/2, pbY=by+bh+32;
            HitAdd(pbX,pbY,bw,bh,TSimBtn,(int)HidNpadButton_X);
            FillRect(pbX,pbY,bw,bh,COL_SEL); DrawRect(pbX,pbY,bw,bh,COL_ACCENT);
            DrawTextC("X  " + Lang::T("player.edit.pron"),cx,pbY+bh/2,COL_ACCENT,Font::Md);
            // Language section
            bool isNameField = strcmp(fd.fieldName,"Player.Name")==0;
            const char* langFieldName = isNameField ? "Player.NameRegionLanguageID"
                                                    : "Player.IslandNameRegionLanguageID";
            uint32_t langEv = SaveEditor::GetEnum(gPlayerSav, SaveEditor::Hash(langFieldName));
            const char* langLabel = "Unknown";
            for (int li=0; li<LANG_OPTION_COUNT; li++)
                if (SaveEditor::Hash(LANG_OPTIONS[li].name)==langEv) { langLabel=LANG_OPTIONS[li].label; break; }
            int langY = pbY + bh + 28;
            DrawTextC(langLabel, cx, langY, COL_TEXT, Font::Md);
            const int lbW=130, lbH=36, lbGap=24;
            int lbxL=cx-lbGap/2-lbW, lbxR=cx+lbGap/2, lbY=langY+22;
            HitAdd(lbxL,lbY,lbW,lbH,TSimBtn,(int)HidNpadButton_Left);
            FillRect(lbxL,lbY,lbW,lbH,COL_PANEL); DrawRect(lbxL,lbY,lbW,lbH,COL_BORDER);
            DrawTextC("< " + Lang::T("prev"),lbxL+lbW/2,lbY+lbH/2,COL_DIM);
            HitAdd(lbxR,lbY,lbW,lbH,TSimBtn,(int)HidNpadButton_Right);
            FillRect(lbxR,lbY,lbW,lbH,COL_PANEL); DrawRect(lbxR,lbY,lbW,lbH,COL_BORDER);
            DrawTextC(Lang::T("next") + " >",lbxR+lbW/2,lbY+lbH/2,COL_DIM);
            DrawTextC(Lang::T("player.cycle.lang"), cx, lbY+lbH+14, COL_DIM, Font::Sm);
            EmitPrevNextHighlight(/*sub=player lang*/112 + (isNameField?0:1),
                                  lbxL, lbxR, lbY, lbW, lbH);
            bool canUndoLang = isNameField ? gNameLangUndoValid : gIslandLangUndoValid;
            void (*undoLangFn)(int) = isNameField ? TUndoNameLang : TUndoIslandLang;
            int ubW=100, ubH=44, ubX=cx-ubW/2, ubY=lbY+lbH+36;
            HitAdd(ubX,ubY,ubW,ubH,undoLangFn,0);
            FillRect(ubX,ubY,ubW,ubH,COL_PANEL);
            DrawRect(ubX,ubY,ubW,ubH, canUndoLang?COL_GOLD:COL_BORDER);
            DrawTextC(Lang::T("undo"),ubX+ubW/2,ubY+ubH/2, canUndoLang?COL_GOLD:COL_DIM, Font::Md);
        }
    } else if (fd.isEnum) {
        uint32_t ev = SaveEditor::GetEnum(gPlayerSav, ph);
        DrawTextC(CurrencySymbol(ev), cx, midY-36, COL_TEXT, Font::Lg);
        DrawTextC(CurrencyName(ev), cx, midY-4, COL_DIM, Font::Md);
        const int btnW=140, btnH=36, btnGap=24;
        int bxL=cx-btnGap/2-btnW, bxR=cx+btnGap/2, btnY=midY+22;
        HitAdd(bxL,btnY,btnW,btnH, TSimBtn, (int)HidNpadButton_Left);
        FillRect(bxL,btnY,btnW,btnH,COL_PANEL); DrawRect(bxL,btnY,btnW,btnH,COL_BORDER);
        DrawTextC("< " + Lang::T("prev"), bxL+btnW/2, btnY+btnH/2, COL_DIM);
        HitAdd(bxR,btnY,btnW,btnH, TSimBtn, (int)HidNpadButton_Right);
        FillRect(bxR,btnY,btnW,btnH,COL_PANEL); DrawRect(bxR,btnY,btnW,btnH,COL_BORDER);
        DrawTextC(Lang::T("next") + " >", bxR+btnW/2, btnY+btnH/2, COL_DIM);
        DrawTextC(Lang::T("player.cycle.currency"), cx, btnY+btnH+14, COL_DIM);
        EmitPrevNextHighlight(/*sub=player currency*/110, bxL, bxR, btnY, btnW, btnH);
    } else if (fd.isSkinTone) {
        static const SDL_Color SKIN_COLS[] = {
            {246,217,189,255},{236,193,157,255},{210,160,122,255},
            {176,122,84,255},{130,85,51,255},{90,57,28,255}
        };
        uint32_t cur = SaveEditor::GetUInt(gPlayerSav, ph);
        int sw=66, sh=66, sgap=12;
        int totalSW = 6*(sw+sgap)-sgap;
        int sbx = cx - totalSW/2, sby = midY-30;
        int toneSelX = sbx; bool toneSawSel = false;
        for (int si=0; si<6; si++) {
            bool ssel = ((uint32_t)si == cur);
            if (ssel) { toneSelX = sbx; toneSawSel = true; }
            HitAdd(sbx, sby, sw, sh, TSetSkinTone, si);
            FillRect(sbx, sby, sw, sh, SKIN_COLS[si]);
            DrawRect(sbx, sby, sw, sh,
                     (ssel && gTheme.cornerRadius == 0) ? SDL_Color{255,255,255,255} : SDL_Color{80,80,80,255});
            if (ssel && gTheme.cornerRadius == 0)
                DrawRectSquare(sbx+2, sby+2, sw-4, sh-4, SDL_Color{255,255,255,255});
            sbx += sw+sgap;
        }
        DrawTextC(Lang::T("player.cycle.tone"), cx, sby+sh+14, COL_DIM, Font::Sm);
        if (toneSawSel) {
            RequestHighlight((int)Screen::OnSwitch, /*sub=player skin tone*/21,
                             toneSelX, sby, sw, sh, gDtSec, /*slot=*/1);
            DrawAnimatedHighlight(/*slot=*/1);
        }
    } else if (fd.isIslandSz) {
        static const char* SZ_LABELS[] = {"50 x 36","70 x 50","90 x 64","120 x 80"};
        uint32_t cur = SaveEditor::GetAnyScalar(gPlayerSav, ph, 0);
        DrawTextC(Lang::T("player.islandsize.warn"), cx, midY-46, COL_RED, Font::Sm);
        int bw=140, bh=44, bgap=12;
        int totalBW = 4*(bw+bgap)-bgap;
        int bbx = cx - totalBW/2, bby = midY-16;
        int szSelX = bbx; bool szSawSel = false;
        for (int pi=0; pi<4; pi++) {
            bool psel = (cur == (uint32_t)(pi+1));
            if (psel) { szSelX = bbx; szSawSel = true; }
            HitAdd(bbx, bby, bw, bh, TSetIslandSize, pi+1);
            FillRect(bbx, bby, bw, bh, psel?COL_SEL:COL_PANEL);
            DrawRect(bbx, bby, bw, bh,
                     (psel && gTheme.cornerRadius == 0) ? COL_GOLD : COL_BORDER);
            DrawTextC(SZ_LABELS[pi], bbx+bw/2, bby+bh/2, psel?COL_GOLD:COL_DIM);
            bbx += bw+bgap;
        }
        DrawTextC(Lang::T("player.cycle.size"), cx, bby+bh+14, COL_DIM, Font::Sm);
        if (szSawSel) {
            RequestHighlight((int)Screen::OnSwitch, /*sub=player island size*/22,
                             szSelX, bby, bw, bh, gDtSec, /*slot=*/1);
            DrawAnimatedHighlight(/*slot=*/1);
        }
    } else if (fd.isRegion) {
        uint32_t ev = SaveEditor::GetEnum(gPlayerSav, ph);
        const char* rlabel = "Unknown";
        for (int ri=0; ri<REGION_OPTION_COUNT; ri++) {
            if (SaveEditor::Hash(REGION_OPTIONS[ri].name)==ev) { rlabel=REGION_OPTIONS[ri].label; break; }
        }
        DrawTextC(rlabel, cx, midY-36, COL_TEXT, Font::Lg);
        const int btnW=140, btnH=36, btnGap=24;
        int bxL=cx-btnGap/2-btnW, bxR=cx+btnGap/2, btnY=midY+22;
        HitAdd(bxL,btnY,btnW,btnH, TSimBtn, (int)HidNpadButton_Left);
        FillRect(bxL,btnY,btnW,btnH,COL_PANEL); DrawRect(bxL,btnY,btnW,btnH,COL_BORDER);
        DrawTextC("< " + Lang::T("prev"), bxL+btnW/2, btnY+btnH/2, COL_DIM);
        HitAdd(bxR,btnY,btnW,btnH, TSimBtn, (int)HidNpadButton_Right);
        FillRect(bxR,btnY,btnW,btnH,COL_PANEL); DrawRect(bxR,btnY,btnW,btnH,COL_BORDER);
        DrawTextC(Lang::T("next") + " >", bxR+btnW/2, btnY+btnH/2, COL_DIM);
        DrawTextC(Lang::T("player.cycle.region"), cx, btnY+btnH+14, COL_DIM);
        EmitPrevNextHighlight(/*sub=player region*/114, bxL, bxR, btnY, btnW, btnH);
    } else {
        // Generic numeric: UInt, Int, RawEnum
        uint32_t rawV = fd.isUInt ? SaveEditor::GetUInt(gPlayerSav, ph) : SaveEditor::GetAnyScalar(gPlayerSav, ph);
        std::string dispV = fd.isInt ? std::to_string((int32_t)rawV) : std::to_string(rawV);
        DrawTextC(dispV, cx, midY-40, COL_TEXT, Font::Lg);

        // Wishes field: show "currently liberated: X / Y" next to the wish-count
        // value so the user has the liberated-count context without having to
        // glance at the bulk-action panel below.
        if (fd.rawHash == 0xa32f7e47u && WishesAvailable()) {
            std::string info = Lang::T("player.wishes.liberated") + ": "
                             + std::to_string(WishesLiberatedCount()) + " / "
                             + std::to_string(WishesData::WISH_COUNT);
            DrawTextC(info, cx, midY-12, COL_DIM, Font::Sm);
        }

        static const int STEPS[]={1000,100,10,1};
        int bw=52, bh=52, gap=6;
        int totalBW = 4*(bw+gap)*2 + 20;
        int bx = cx - totalBW/2;
        int by = midY+10;
        int btnIdx = 0;
        int pStepFocX = bx, pStepFocY = by; bool pStepFoc = false;
        for (int s : STEPS) {
            bool focused = (gPlayerBtnSel == btnIdx);
            if (focused) { pStepFocX = bx; pStepFocY = by; pStepFoc = true; }
            HitAdd(bx,by,bw,bh, TAdjust, -s);
            FillRect(bx,by,bw,bh, focused?COL_SEL:COL_PANEL);
            DrawRect(bx,by,bw,bh,
                     (focused && gTheme.cornerRadius == 0) ? COL_GOLD : COL_RED);
            DrawTextC("-"+std::to_string(s), bx+bw/2, by+bh/2, focused?COL_GOLD:COL_RED, Font::Md);
            bx += bw+gap; btnIdx++;
        }
        bx += 20;
        for (int j = 3; j >= 0; j--) {
            bool focused = (gPlayerBtnSel == btnIdx);
            if (focused) { pStepFocX = bx; pStepFocY = by; pStepFoc = true; }
            int s = STEPS[j];
            HitAdd(bx,by,bw,bh, TAdjust, s);
            FillRect(bx,by,bw,bh, focused?COL_SEL:COL_PANEL);
            DrawRect(bx,by,bw,bh,
                     (focused && gTheme.cornerRadius == 0) ? COL_GOLD : COL_GREEN);
            DrawTextC("+"+std::to_string(s), bx+bw/2, by+bh/2, focused?COL_GOLD:COL_GREEN, Font::Md);
            bx += bw+gap; btnIdx++;
        }
        if (pStepFoc) {
            RequestHighlight((int)Screen::OnSwitch, /*sub=player steppers*/20,
                             pStepFocX, pStepFocY, bw, bh, gDtSec,
                             /*slot=*/1);
            DrawAnimatedHighlight(/*slot=*/1);
        }
    }
    // Undo button for all non-string fields
    if (!fd.isStr) {
        bool canUndo = gPlayerUndoValid[gPlayerFieldSel];
        int ubW=100, ubH=44, ubX=cx-ubW/2, ubY=midY+130;
        HitAdd(ubX, ubY, ubW, ubH, TUndoPlayer, 0);
        FillRect(ubX, ubY, ubW, ubH, COL_PANEL);
        DrawRect(ubX, ubY, ubW, ubH, canUndo ? COL_GOLD : COL_BORDER);
        DrawTextC(Lang::T("undo"), ubX+ubW/2, ubY+ubH/2, canUndo ? COL_GOLD : COL_DIM, Font::Md);

        // Wishes field only: two extra TOUCH-ONLY buttons under Undo for the
        // bulk Unlock / Reset operations. No controller binding so they can't
        // conflict with the existing A-button numeric edit on this field.
        if (fd.rawHash == 0xa32f7e47u) {
            bool unlockArmed = (gPlayerActionArmed == 1);
            bool resetArmed  = (gPlayerActionArmed == 2);
            int wbW = 220, wbH = 38, wbGap = 10;
            int totalW = wbW + wbGap + wbW;
            // Push the bulk section well below the Undo button so the header
            // text and Undo never feel cramped against each other.
            int wbY = ubY + ubH + 64;
            int wbX1 = cx - totalW/2;
            int wbX2 = wbX1 + wbW + wbGap;

            // Header label for the section
            DrawTextC(Lang::T("player.wishes.bulk"), cx, wbY - 16, COL_DIM, Font::Sm);

            // Unlock all
            SDL_Color uFill   = unlockArmed ? SDL_Color{90, 38, 18, 255}  : COL_PANEL;
            SDL_Color uBorder = unlockArmed ? SDL_Color{255,170, 40, 255} : COL_ACCENT;
            SDL_Color uText   = unlockArmed ? SDL_Color{255,200,100, 255} : COL_ACCENT;
            FillRect(wbX1, wbY, wbW, wbH, uFill);
            DrawRect(wbX1, wbY, wbW, wbH, uBorder);
            HitAdd(wbX1, wbY, wbW, wbH, TWishesUnlockTap, 0);
            DrawTextC(unlockArmed ? Lang::T("player.wishes.unlock.confirm")
                                  : Lang::T("player.wishes.unlock"),
                      wbX1 + wbW/2, wbY + wbH/2, uText, Font::Sm);

            // Reset liberated
            SDL_Color rFill   = resetArmed ? SDL_Color{60, 18, 18, 255}  : COL_PANEL;
            SDL_Color rBorder = resetArmed ? SDL_Color{255,170, 40, 255} : SDL_Color{200, 90, 90, 255};
            SDL_Color rText   = resetArmed ? SDL_Color{255,200,100, 255} : SDL_Color{230,130,130, 255};
            FillRect(wbX2, wbY, wbW, wbH, rFill);
            DrawRect(wbX2, wbY, wbW, wbH, rBorder);
            HitAdd(wbX2, wbY, wbW, wbH, TWishesResetTap, 0);
            DrawTextC(resetArmed ? Lang::T("player.wishes.reset.confirm")
                                 : Lang::T("player.wishes.reset"),
                      wbX2 + wbW/2, wbY + wbH/2, rText, Font::Sm);

            // Armed countdown bar + transient status message
            if (gPlayerActionArmed != 0) {
                int barW  = totalW;
                int barX  = wbX1;
                int barY  = wbY + wbH + 6;
                float pct = (float)gPlayerActionArmedFrames / 180.0f;
                if (pct < 0) pct = 0;
                if (pct > 1) pct = 1;
                FillRect(barX, barY, barW, 3, SDL_Color{40,30,16,255});
                FillRect(barX, barY, (int)(barW * pct), 3, SDL_Color{255,170,40,255});
            }
            if (gPlayerActionMsgFrames > 0) {
                DrawTextC(gPlayerActionMsg, cx, wbY + wbH + 18,
                          gPlayerActionMsgCol, Font::Sm);
            }
        }
    }

    if (!gPlayerMsg.empty())
        DrawTextC(gPlayerMsg, cx, SE_TOP_Y+16, gPlayerMsgCol);

    {
        bool hasPron = fd.isStr && (strcmp(fd.fieldName,"Player.Name")==0 || strcmp(fd.fieldName,"Player.IslandName")==0);
        DrawFooter(Lang::T(hasPron ? "player.footer.pron" : "player.footer.value"));
    }
    Present();
}

// ─── Mii stats editor ─────────────────────────────────────────────────────────
static const int MSE_LIST_W = 240; // mii selector on left

// ── Belongings constants ───────────────────────────────────────────────────────
static const uint32_t BL_H_CLOTH_OWN  = 0x84824827u;
static const uint32_t BL_H_COORD_OWN  = 0x89ce9150u;
static const uint32_t BL_H_GOODS_ID   = 0xe3d6005eu;
static const uint32_t BL_H_GOODS_TIME = 0x48c3881au;
static const uint32_t BL_H_GOODS_UGC  = 0x2115afb6u;
static const int      BL_SLOTS_PER_MII    = 1200;
static const int      BL_COORD_SLOTS_MII  = 400;
static const int      BL_GOODS_SLOTS_MII  = 12;
static const uint32_t BL_H_INVALID        = 0x7e3d1e46u;

struct BlWornSlotDef { const char* name; uint32_t kh; uint32_t ch; const char* cat; bool addSocks; };
static const BlWornSlotDef BL_WORN_DEFS[] = {
    {"All",       0xb20fecedu, 0xee37f256u, "All",       false},
    {"Topslong",  0x35256740u, 0x1124095du, "Topslong",  false},
    {"Tops",      0xe81ee9c4u, 0x77cf0ff7u, "Tops",      false},
    {"BottomsA",  0x15aea81du, 0xec5010fbu, "Bottoms",   false},
    {"BottomsB",  0x7a1809d1u, 0x536dbec2u, "Bottoms",   true},
    {"Headwear",  0x98cc9cccu, 0xc499cda5u, "Headwear",  false},
    {"Shoes",     0x0a903d03u, 0xc0529e53u, "Shoes",     false},
    {"Accessory", 0xc1094d9fu, 0x64af701au, "Accessory", false},
};
static const uint32_t BL_COORD_KH = 0xd5ee521au;
static const uint32_t BL_COORD_CH = 0x763c97ccu;

// Selectable rows in Belongings:
//   0-7  : cloth worn slots (BL_WORN_DEFS[0-7])
//   8    : coordinate worn slot
//   9-20 : goods pocket slots 0-11
//   21   : Unlock All Clothes
//   22   : Lock All Clothes
//   23   : Unlock All Coords
//   24   : Lock All Coords
static const int BL_SEL_MAX = 25;
// Visual rows (includes section headers):
//   vr 0        : "── WORN OUTFIT ──" header
//   vr 1-9      : worn slots 0-8
//   vr 10       : "── GOODS POCKET ──" header
//   vr 11-22    : goods slots 0-11
//   vr 23       : "── OWNERSHIP ──" header
//   vr 24-27    : action rows 0-3
static const int BL_VIS_ROWS  = 28;
static const int BL_ROW_H     = 32;

static int BlSelToVis(int sel) {
    if (sel < 9)  return sel + 1;
    if (sel < 21) return sel + 2;
    return sel + 3;
}

static const char* BlClothLabel(uint32_t h) {
    if (!h || h == BL_H_INVALID) return "(none)";
    for (int i = 0; i < BL_CLOTH_COUNT; i++)
        if (BL_CLOTH_ITEMS[i].nameHash == h) return BL_CLOTH_ITEMS[i].label;
    return "?";
}
static const char* BlCoordLabel(uint32_t h) {
    if (!h || h == BL_H_INVALID) return "(none)";
    for (int i = 0; i < BL_COORD_COUNT; i++)
        if (BL_COORD_ITEMS[i].keyHash == h) return BL_COORD_ITEMS[i].label;
    return "?";
}
static const char* BlGoodsLabel(uint32_t h) {
    if (!h) return "(empty)";
    for (int i = 0; i < BL_GOODS_COUNT; i++)
        if (BL_GOODS_ITEMS[i].nameHash == h) return BL_GOODS_ITEMS[i].label;
    return "?";
}

static void BlOpenPicker(int blSel, int miiIdx) {
    gBlPickerHashes.clear();
    gBlPickerLabels.clear();
    uint32_t curHash = 0;
    if (blSel < 8) {
        const auto& ws = BL_WORN_DEFS[blSel];
        curHash = SaveEditor::GetAnyEnumAt(gMiiSav, ws.kh, miiIdx, BL_H_INVALID);
        gBlPickerHashes.push_back(BL_H_INVALID); gBlPickerLabels.push_back("(none)");
        for (int k = 0; k < BL_CLOTH_COUNT; k++) {
            const char* cat = BL_CLOTH_ITEMS[k].category;
            if (strcmp(cat, ws.cat)==0 || (ws.addSocks && strcmp(cat,"Socks")==0)) {
                gBlPickerHashes.push_back(BL_CLOTH_ITEMS[k].nameHash);
                gBlPickerLabels.push_back(BL_CLOTH_ITEMS[k].label);
            }
        }
    } else if (blSel == 8) {
        curHash = SaveEditor::GetAnyEnumAt(gMiiSav, BL_COORD_KH, miiIdx, BL_H_INVALID);
        gBlPickerHashes.push_back(BL_H_INVALID); gBlPickerLabels.push_back("(none)");
        for (int k = 0; k < BL_COORD_COUNT; k++) {
            gBlPickerHashes.push_back(BL_COORD_ITEMS[k].keyHash);
            gBlPickerLabels.push_back(BL_COORD_ITEMS[k].label);
        }
    } else if (blSel >= 9 && blSel < 21) {
        int ai = miiIdx * BL_GOODS_SLOTS_MII + (blSel - 9);
        curHash = SaveEditor::GetUIntAt(gMiiSav, BL_H_GOODS_ID, ai);
        gBlPickerHashes.push_back(0u); gBlPickerLabels.push_back("(empty)");
        for (int k = 0; k < BL_GOODS_COUNT; k++) {
            gBlPickerHashes.push_back(BL_GOODS_ITEMS[k].nameHash);
            gBlPickerLabels.push_back(BL_GOODS_ITEMS[k].label);
        }
    } else {
        return;
    }
    gBlFilter.clear();
    BlRebuildFilter();
    gBlPickerSel = 0;
    for (int i = 0; i < (int)gBlFiltered.size(); i++)
        if (gBlPickerHashes[gBlFiltered[i]] == curHash) { gBlPickerSel = i; break; }
    gBlPickerScroll = std::max(0, std::min(gBlPickerSel, (int)gBlFiltered.size() - 1));
    gBlPickerOpen = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Map tab (on-Switch)
// Mirrors the WebUI Map tab: a 120×80 island grid rendered into a 720×480
// region of the screen with an inspector card on the right.
//   • D-pad / left stick move a single-tile cursor (the right stick pans the
//     cursor faster, useful since the island is 120 tiles wide).
//   • A on a tile with an object opens the inspector; A on an empty tile in
//     Place mode drops the chosen actor there.
//   • X deletes the inspected object, +/- start the actor picker for place /
//     reassign, Y commits Map.sav to disk.
//   • Touch: tap a tile to move the cursor (auto-opens inspector if there's
//     an object); tap any inspector / picker control via HitAdd zones.
// All schema reads/writes go through SaveEditor::{Get,Set}{Int,UInt,Float}At
// on gMapSav so behaviour matches the WebUI exactly.
// ─────────────────────────────────────────────────────────────────────────────

enum class MapMode : uint8_t {
    Idle    = 0,
    Inspect = 1,
    Picker  = 2,
    Place   = 3,
};

static int       gMapCursorX     = 60;
static int       gMapCursorY     = 40;
static int       gMapSelSlot     = -1;
static MapMode   gMapMode        = MapMode::Idle;
// gMapDirty is declared earlier (above the Housing helpers that set it).
static std::string gMapMsg;
static SDL_Color gMapMsgCol      = COL_TEXT;
static int       gMapMsgFrames   = 0;
static uint32_t  gMapPlaceActor  = 0;
static int       gMapPickerSel   = 0;
static int       gMapPickerScroll= 0;
static std::string gMapPickerFilter;
static int       gMapInsField    = 0;  // focused inspector field (0..N-1)
static int       gMapDelConfirm  = 0;  // 0 idle, 1 awaiting Y/A confirm
static int       gMapStickXAcc   = 0;
static int       gMapStickYAcc   = 0;

// On-screen geometry (1280×720). Canvas at 7 px/tile fills the height between
// the tab bar and footer; inspector is sized to the form's actual content.
static constexpr int MAP_CANVAS_X = 24;
static constexpr int MAP_CANVAS_Y = 84;
static constexpr int MAP_CANVAS_W = 840;  // 7 px / tile
static constexpr int MAP_CANVAS_H = 560;  // 7 px / tile
static constexpr int MAP_INS_X    = 880;
static constexpr int MAP_INS_Y    = 84;
static constexpr int MAP_INS_W    = 376;
static constexpr int MAP_INS_H    = 560;

// Compile-time list of actor categories to draw an actor in the picker. We
// reuse the generated group constants from map_data.h.
static const SDL_Color MAP_GROUP_SDL_COL[] = {
    {225, 29, 72,  255},   // house    — rose
    { 37, 99, 235, 255},   // facility — blue
    { 22,163, 74,  255},   // deco     — green
    {147, 51, 234, 255},   // room     — purple
    {202,138,  4,  255},   // step     — amber
    {167,139,250, 255},    // ugc      — violet
    {239, 68, 68,  255},   // unknown  — red
};

static SDL_Color MapHexColor(const MapData::TileInfo* t) {
    if (!t) return {0x88, 0x00, 0x88, 255};   // visible magenta for unknowns
    return SDL_Color{t->r, t->g, t->b, 255};
}

static void MapSetMsg(const std::string& s, SDL_Color col, int frames = 120) {
    gMapMsg = s; gMapMsgCol = col; gMapMsgFrames = frames;
}

// Read one object slot's actor (we use this everywhere for "is this slot
// occupied?"). Returns 0 for empty / unloaded.
static uint32_t MapActorAt(int slot) {
    if (!gMapSav.loaded || slot < 0) return 0;
    return SaveEditor::GetUIntAt(gMapSav, SaveEditor::Hash(MapKeys::ActorKey), slot, 0);
}

static int MapObjectCount() {
    if (!gMapSav.loaded) return 0;
    return SaveEditor::ArraySize(gMapSav, SaveEditor::Hash(MapKeys::ActorKey));
}

// Footprint rect after applying the actor's quarter-turn Y rotation. Mirrors
// upstream's rotateActorFootprint (ltd-save-editor src/lib/map/actors/actors.ts).
// Houses and facilities store negative x0/y0 anchors so their stored grid
// position is the *goal point*, not the top-left corner; without applying
// (x + x0, y + y0) the building renders shifted off the terrain plot.
struct MapFootprint { int x0, y0, w, h; };
static MapFootprint MapActorRect(const MapData::ActorInfo* info, float rotDeg) {
    if (!info) return { 0, 0, 1, 1 };
    int x0 = (int)info->x0, y0 = (int)info->y0;
    int w  = (int)info->w,  h  = (int)info->h;
    int t = (((int)std::lround(rotDeg / 90.0f) % 4) + 4) % 4;
    if (t == 0) return { x0, y0, w, h };
    int x1 = x0 + w - 1, y1 = y0 + h - 1;
    int corners[4][2] = {{x0,y0},{x1,y0},{x0,y1},{x1,y1}};
    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
    for (auto& c : corners) {
        int cx = c[0], cy = c[1];
        for (int i = 0; i < t; i++) { int nx = cy; int ny = -cx; cx = nx; cy = ny; }
        if (cx < minX) minX = cx;
        if (cy < minY) minY = cy;
        if (cx > maxX) maxX = cx;
        if (cy > maxY) maxY = cy;
    }
    return { minX, minY, maxX - minX + 1, maxY - minY + 1 };
}

// Find the topmost object at (tx, ty) with a 2-tile snap radius.
static int MapObjectAt(int tx, int ty, int snap = 2) {
    if (!gMapSav.loaded) return -1;
    uint32_t hAct = SaveEditor::Hash(MapKeys::ActorKey);
    uint32_t hX   = SaveEditor::Hash(MapKeys::GridPosX);
    uint32_t hY   = SaveEditor::Hash(MapKeys::GridPosY);
    uint32_t hR   = SaveEditor::Hash(MapKeys::RotY);
    int n = SaveEditor::ArraySize(gMapSav, hAct);
    int best = -1;
    int bestDist = INT_MAX;
    for (int i = 0; i < n; i++) {
        uint32_t a = SaveEditor::GetUIntAt(gMapSav, hAct, i, 0);
        if (!a) continue;
        int x = SaveEditor::GetIntAt(gMapSav, hX, i, -1);
        int y = SaveEditor::GetIntAt(gMapSav, hY, i, -1);
        const MapData::ActorInfo* info = MapData::ActorLookup(a);
        MapFootprint fp = MapActorRect(info, SaveEditor::GetFloatAt(gMapSav, hR, i, 0.0f));
        int bx = x + fp.x0, by = y + fp.y0;
        if (tx >= bx && tx < bx + fp.w && ty >= by && ty < by + fp.h) return i;
        int cx = bx + fp.w / 2;
        int cy = by + fp.h / 2;
        int d = std::max(std::abs(tx - cx), std::abs(ty - cy));
        if (d <= snap && d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

// Human-readable label for an actor row. For UGC actors we resolve the
// underlying player-saved UGC name (same source as the Textures tab) so the
// list reads as e.g. "Tomato Plant · 02" instead of "UGC item 13-02".
// Key shape from map_data.cpp: "ObjUgcXX_YY" (MapObject UGC) or
// "HouseUgcXX_YY" (Exterior UGC). XX is the 1-based UGC slot, YY the variant.
static std::string MapActorDisplayLabel(const MapData::ActorInfo* info) {
    if (!info || !info->key) return info ? std::string(info->label ? info->label : "?") : "?";
    if (info->group != MapData::ACTOR_UGC) return info->label ? info->label : "?";
    std::string key = info->key;
    const char* stemPrefix = nullptr;
    size_t numStart = 0;
    if (key.rfind("Obj", 0) == 0)        { stemPrefix = "UgcMapObject"; numStart = 6; }
    else if (key.rfind("House", 0) == 0) { stemPrefix = "UgcExterior";  numStart = 8; }
    else return info->label ? info->label : key;
    size_t end = key.find('_', numStart);
    if (end == std::string::npos || end <= numStart) return info->label ? info->label : key;
    int slot = atoi(key.substr(numStart, end - numStart).c_str());
    if (slot <= 0) return info->label ? info->label : key;
    char stem[32];
    snprintf(stem, sizeof(stem), "%s%03d", stemPrefix, slot);
    std::string name = MiiManager::GetUgcName(stem);
    if (name.empty()) name = stem;
    std::string variant = (end + 1 < key.size()) ? key.substr(end + 1) : std::string();
    return variant.empty() ? name : (name + " \xC2\xB7 " + variant); // " · "
}

// Build the list of unique actor hashes for the picker, filtered by current
// search. Sorted by group then label. Cached when filter doesn't change.
static std::vector<uint32_t> gMapPickerActors;
static std::string           gMapPickerFilterCache;
static void MapRebuildPicker() {
    if (gMapPickerFilter == gMapPickerFilterCache && !gMapPickerActors.empty()) return;
    gMapPickerActors.clear();
    std::string filter = gMapPickerFilter;
    std::transform(filter.begin(), filter.end(), filter.begin(),
                   [](char c){ return (char)tolower((unsigned char)c); });
    for (int i = 0; i < MapData::ACTOR_INFO_COUNT; i++) {
        const auto& a = MapData::ACTOR_INFO[i];
        if (!filter.empty()) {
            // Match against the resolved display label so a search for the
            // user's UGC name finds the matching actor row.
            std::string label = MapActorDisplayLabel(&a);
            std::transform(label.begin(), label.end(), label.begin(),
                           [](char c){ return (char)tolower((unsigned char)c); });
            if (label.find(filter) == std::string::npos) continue;
        }
        gMapPickerActors.push_back(a.hash);
    }
    gMapPickerFilterCache = gMapPickerFilter;
    if (gMapPickerSel >= (int)gMapPickerActors.size())
        gMapPickerSel = std::max(0, (int)gMapPickerActors.size() - 1);
    gMapPickerScroll = std::max(0, std::min(gMapPickerScroll, std::max(0, (int)gMapPickerActors.size() - 1)));
}

// Touch callbacks (registered via HitAdd). Translate the global last-touch
// coords into tile coords inside the map canvas region.
static void TMapTapCanvas(int /*unused*/) {
    int px = gLastTouchFireX - MAP_CANVAS_X;
    int py = gLastTouchFireY - MAP_CANVAS_Y;
    if (px < 0 || py < 0 || px >= MAP_CANVAS_W || py >= MAP_CANVAS_H) return;
    int tx = px * MAP_WIDTH  / MAP_CANVAS_W;
    int ty = py * MAP_HEIGHT / MAP_CANVAS_H;
    if (tx < 0) tx = 0; if (tx >= MAP_WIDTH)  tx = MAP_WIDTH  - 1;
    if (ty < 0) ty = 0; if (ty >= MAP_HEIGHT) ty = MAP_HEIGHT - 1;
    gMapCursorX = tx;
    gMapCursorY = ty;
    if (gMapMode == MapMode::Place) {
        // Simulate A so the place-confirm runs in the input loop next frame.
        gSimKDown |= HidNpadButton_A;
        return;
    }
    int slot = MapObjectAt(tx, ty);
    if (slot >= 0) {
        gMapSelSlot = slot;
        gMapMode = MapMode::Inspect;
        gMapInsField = 0;
    } else {
        gMapSelSlot = -1;
        if (gMapMode == MapMode::Inspect) gMapMode = MapMode::Idle;
    }
}

static void TMapBtn(int code) { gSimKDown |= (u64)code; }
static void TMapInsField(int idx) { gMapInsField = idx; }
static void TMapInsNudge(int param) {
    // param: high byte = field index, low byte = +1/-1 encoded as 1/255
    int field = param >> 8;
    int sign  = (param & 0xFF) == 1 ? +1 : -1;
    gMapInsField = field;
    if (gMapSelSlot < 0) return;
    uint32_t hX = SaveEditor::Hash(MapKeys::GridPosX);
    uint32_t hY = SaveEditor::Hash(MapKeys::GridPosY);
    uint32_t hR = SaveEditor::Hash(MapKeys::RotY);
    uint32_t hL = SaveEditor::Hash(MapKeys::LinkedMapId);
    if (field == 0) {  // x
        int v = SaveEditor::GetIntAt(gMapSav, hX, gMapSelSlot, 0) + sign;
        if (v < 0) v = 0; if (v >= MAP_WIDTH)  v = MAP_WIDTH  - 1;
        SaveEditor::SetIntAt(gMapSav, hX, gMapSelSlot, v);
        gMapCursorX = v; gMapDirty = true;
    } else if (field == 1) {  // y
        int v = SaveEditor::GetIntAt(gMapSav, hY, gMapSelSlot, 0) + sign;
        if (v < 0) v = 0; if (v >= MAP_HEIGHT) v = MAP_HEIGHT - 1;
        SaveEditor::SetIntAt(gMapSav, hY, gMapSelSlot, v);
        gMapCursorY = v; gMapDirty = true;
    } else if (field == 2) {  // rotation
        float r = SaveEditor::GetFloatAt(gMapSav, hR, gMapSelSlot, 0.0f);
        int cur = (int)std::round(r);
        cur = ((cur + sign * 90) % 360 + 360) % 360;
        SaveEditor::SetFloatAt(gMapSav, hR, gMapSelSlot, (float)cur);
        gMapDirty = true;
    } else if (field == 3) {  // link
        int v = SaveEditor::GetIntAt(gMapSav, hL, gMapSelSlot, -1) + sign;
        SaveEditor::SetIntAt(gMapSav, hL, gMapSelSlot, v);
        gMapDirty = true;
    }
}
static void TMapPickerSelect(int idx) { gMapPickerSel = idx; gSimKDown |= HidNpadButton_A; }

static void DrawMap() {
    HitClear();
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);

    if (gMapMsgFrames > 0 && --gMapMsgFrames == 0) gMapMsg.clear();

    DrawHeader(Lang::T("tab.map"));
    // Tab bar
    {
        int tw=130, th=22, gap=4;
        int totalW=tw*5+gap*4, tx=SCREEN_W/2-totalW/2, ty=28;
        struct { std::string label; OnSwitchMode mode; } tabs[]={
            {Lang::T("tab.webui"),OnSwitchMode::WebUI},{Lang::T("tab.textures"),OnSwitchMode::UGC},
            {Lang::T("tab.mii"),OnSwitchMode::MiiStats},{Lang::T("tab.player"),OnSwitchMode::Player},
            {Lang::T("tab.map"),OnSwitchMode::Map}};
        int ulX = tx, ulY = ty + th + 2;
        bool ulHave = false;
        for (auto& t : tabs) {
            bool sel = gOnSwitchMode==t.mode;
            HitAdd(tx,ty,tw,th,TSetMode,(int)t.mode);
            DrawTextC(t.label,tx+tw/2,ty+th/2,sel?COL_GOLD:COL_DIM);
            if (sel) { ulX = tx + (tw-72)/2; ulHave = true; }
            tx+=tw+gap;
        }
        if (ulHave) {
            RequestTabUnderline(0, 100, ulX, ulY, 72, 3, gDtSec);
            DrawTabUnderline(0);
        }
    }

    if (!gMapSav.loaded) {
        std::string err;
        SaveEditor::Load(SAVE_MAP_SAV, gMapSav, err);
        if (!gMapSav.loaded) {
            DrawTextC(Lang::T("map.loading"), SCREEN_W/2, SCREEN_H/2, COL_DIM, Font::Md);
            DrawFooter(Lang::T("footer.back"));
            Present();
            return;
        }
    }

    // Canvas background
    FillRect(MAP_CANVAS_X - 2, MAP_CANVAS_Y - 2, MAP_CANVAS_W + 4, MAP_CANVAS_H + 4, COL_BORDER);
    FillRect(MAP_CANVAS_X, MAP_CANVAS_Y, MAP_CANVAS_W, MAP_CANVAS_H, COL_PANEL);

    // Floor pass: read FloorKeyHash UIntArray
    {
        uint32_t hFloor = SaveEditor::Hash(MapKeys::FloorKeyHash);
        int n = SaveEditor::ArraySize(gMapSav, hFloor);
        if (n >= MAP_TILE_COUNT) {
            const int tw = MAP_CANVAS_W / MAP_WIDTH;   // = 6
            const int th = MAP_CANVAS_H / MAP_HEIGHT;  // = 6
            for (int x = 0; x < MAP_WIDTH; x++) {
                for (int y = 0; y < MAP_HEIGHT; y++) {
                    uint32_t k = SaveEditor::GetUIntAt(gMapSav, hFloor, x * MAP_HEIGHT + y, 0);
                    SDL_Color c = MapHexColor(MapData::TileLookup(k));
                    FillRect(MAP_CANVAS_X + x * tw, MAP_CANVAS_Y + y * th, tw, th, c);
                }
            }
        }
    }

    // Objects pass — apply the actor's footprint origin offset (x0/y0). For
    // houses and facilities x0/y0 are negative, so the saved grid position is
    // the goal point, not the top-left of the building. Without this the
    // rendered rect drifts off the actual terrain plot.
    {
        uint32_t hAct = SaveEditor::Hash(MapKeys::ActorKey);
        uint32_t hX   = SaveEditor::Hash(MapKeys::GridPosX);
        uint32_t hY   = SaveEditor::Hash(MapKeys::GridPosY);
        uint32_t hR   = SaveEditor::Hash(MapKeys::RotY);
        int n = SaveEditor::ArraySize(gMapSav, hAct);
        const int tw = MAP_CANVAS_W / MAP_WIDTH;
        const int th = MAP_CANVAS_H / MAP_HEIGHT;
        for (int i = 0; i < n; i++) {
            uint32_t a = SaveEditor::GetUIntAt(gMapSav, hAct, i, 0);
            if (!a) continue;
            int x = SaveEditor::GetIntAt(gMapSav, hX, i, -1);
            int y = SaveEditor::GetIntAt(gMapSav, hY, i, -1);
            if (x < 0 || y < 0 || x >= MAP_WIDTH || y >= MAP_HEIGHT) continue;
            const MapData::ActorInfo* info = MapData::ActorLookup(a);
            MapFootprint fp = MapActorRect(info, SaveEditor::GetFloatAt(gMapSav, hR, i, 0.0f));
            int g = info ? info->group : MapData::ACTOR_UNKNOWN;
            const SDL_Color& col = MAP_GROUP_SDL_COL[g];
            int rx = MAP_CANVAS_X + (x + fp.x0) * tw;
            int ry = MAP_CANVAS_Y + (y + fp.y0) * th;
            int rw = std::max(2, fp.w * tw);
            int rh = std::max(2, fp.h * th);
            FillRect(rx, ry, rw, rh, col);
            if (i == gMapSelSlot) {
                DrawRect(rx - 1, ry - 1, rw + 2, rh + 2, COL_GOLD);
                DrawRect(rx,     ry,     rw,     rh,     COL_GOLD);
            }
        }
    }

    // Cursor reticle
    {
        const int tw = MAP_CANVAS_W / MAP_WIDTH;
        const int th = MAP_CANVAS_H / MAP_HEIGHT;
        int cx = MAP_CANVAS_X + gMapCursorX * tw;
        int cy = MAP_CANVAS_Y + gMapCursorY * th;
        DrawRect(cx - 2, cy - 2, tw + 4, th + 4, COL_GOLD);
        DrawRect(cx - 1, cy - 1, tw + 2, th + 2, {255, 255, 255, 255});
    }

    // Canvas-wide touch region. The callback reads the raw touch coords from
    // gLastTouchFireX/Y (set by HitFire) and converts to a tile coord, which
    // keeps the work to one HitAdd entry instead of 9600.
    HitAdd(MAP_CANVAS_X, MAP_CANVAS_Y, MAP_CANVAS_W, MAP_CANVAS_H, TMapTapCanvas, 0);

    // Inspector card
    FillRect(MAP_INS_X, MAP_INS_Y, MAP_INS_W, MAP_INS_H, COL_PANEL);
    DrawRect(MAP_INS_X, MAP_INS_Y, MAP_INS_W, MAP_INS_H, COL_BORDER);

    int iy = MAP_INS_Y + 8;
    if (gMapMode == MapMode::Picker) {
        MapRebuildPicker();
        DrawText(Lang::T("map.picker.title"), MAP_INS_X + 12, iy, COL_GOLD, Font::Md); iy += 28;
        DrawText(Lang::T("map.picker.filter") + ": " + (gMapPickerFilter.empty() ? Lang::T("map.picker.any") : gMapPickerFilter),
                 MAP_INS_X + 12, iy, COL_DIM, Font::Sm); iy += 22;

        const int rowH = 22;
        int visible = (MAP_INS_Y + MAP_INS_H - 70 - iy) / rowH;
        if ((int)gMapPickerActors.size() < gMapPickerSel) gMapPickerSel = 0;
        if (gMapPickerSel < gMapPickerScroll) gMapPickerScroll = gMapPickerSel;
        if (gMapPickerSel >= gMapPickerScroll + visible)
            gMapPickerScroll = gMapPickerSel - visible + 1;
        int mpSelY = iy;
        bool mpSawSel = false;
        for (int i = 0; i < visible; i++) {
            int idx = gMapPickerScroll + i;
            if (idx >= (int)gMapPickerActors.size()) break;
            uint32_t a = gMapPickerActors[idx];
            const MapData::ActorInfo* info = MapData::ActorLookup(a);
            int ry = iy + i * rowH;
            bool sel = (idx == gMapPickerSel);
            if (sel) { mpSelY = ry; mpSawSel = true; }
            FillRect(MAP_INS_X + 6, ry, MAP_INS_W - 12, rowH - 2, sel ? COL_SEL : COL_PANEL);
            if (sel && gTheme.cornerRadius == 0)
                DrawRectSquare(MAP_INS_X + 6, ry, MAP_INS_W - 12, rowH - 2, COL_GOLD);
            FillRect(MAP_INS_X + 12, ry + 5, 10, 10, MAP_GROUP_SDL_COL[info ? info->group : 6]);
            std::string lbl = MapActorDisplayLabel(info);
            int lw=0, lh=0;
            if (TTF_Font* fsm = GetFont(Font::Sm)) TTF_SizeUTF8(fsm, lbl.c_str(), &lw, &lh);
            DrawText(lbl, MAP_INS_X + 28, ry + (rowH - 2 - lh)/2, sel ? COL_GOLD : COL_TEXT);
            HitAdd(MAP_INS_X + 6, ry, MAP_INS_W - 12, rowH - 2, TMapPickerSelect, idx);
        }
        if (mpSawSel) {
            RequestHighlight((int)Screen::OnSwitch, /*sub=map actor picker*/40,
                             MAP_INS_X + 6, mpSelY, MAP_INS_W - 12, rowH - 2, gDtSec);
            DrawAnimatedHighlight();
        }

        if ((int)gMapPickerActors.size() > visible) {
            int sbH = (MAP_INS_H - 70 - 60) * visible / std::max(1, (int)gMapPickerActors.size());
            int sbY = iy + (MAP_INS_H - 70 - 60) * gMapPickerScroll / std::max(1, (int)gMapPickerActors.size());
            FillRect(MAP_INS_X + MAP_INS_W - 6, sbY, 3, sbH, COL_BORDER);
        }
    } else if (gMapSelSlot >= 0 && MapActorAt(gMapSelSlot)) {
        uint32_t hX = SaveEditor::Hash(MapKeys::GridPosX);
        uint32_t hY = SaveEditor::Hash(MapKeys::GridPosY);
        uint32_t hR = SaveEditor::Hash(MapKeys::RotY);
        uint32_t hL = SaveEditor::Hash(MapKeys::LinkedMapId);
        uint32_t actor = MapActorAt(gMapSelSlot);
        const MapData::ActorInfo* info = MapData::ActorLookup(actor);

        // Title
        FillRect(MAP_INS_X + 12, iy + 2, 12, 12,
                 MAP_GROUP_SDL_COL[info ? info->group : 6]);
        DrawText(MapActorDisplayLabel(info), MAP_INS_X + 30, iy, COL_GOLD, Font::Md);
        iy += 24;
        DrawText(Lang::T("map.slot") + " #" + std::to_string(gMapSelSlot), MAP_INS_X + 12, iy, COL_DIM, Font::Sm);
        iy += 22;

        int xVal = SaveEditor::GetIntAt(gMapSav, hX, gMapSelSlot, 0);
        int yVal = SaveEditor::GetIntAt(gMapSav, hY, gMapSelSlot, 0);
        int rVal = (int)std::round(SaveEditor::GetFloatAt(gMapSav, hR, gMapSelSlot, 0.0f));
        int lVal = SaveEditor::GetIntAt(gMapSav, hL, gMapSelSlot, -1);

        // Field rows with -/+ nudge buttons
        struct Row { std::string label; int field; std::string value; };
        Row rows[] = {
            {Lang::T("map.row.posX"),    0, std::to_string(xVal)},
            {Lang::T("map.row.posY"),    1, std::to_string(yVal)},
            {Lang::T("map.row.rotation"),2, std::to_string(((rVal % 360) + 360) % 360) + "°"},
            {Lang::T("map.row.linked"),  3, lVal >= 0 ? std::to_string(lVal) : Lang::T("none.paren")},
        };
        int rowsCount = (info && info->group == 0 /* house */) ? 4
                       : (info && info->group == 1 /* facility */) ? 4
                       : 3;
        const int rowH = 32;
        for (int r = 0; r < rowsCount; r++) {
            int ry = iy + r * rowH;
            bool focused = (r == gMapInsField);
            if (focused) {
                FillRect(MAP_INS_X + 6, ry - 2, MAP_INS_W - 12, rowH - 4, COL_SEL);
                DrawRect(MAP_INS_X + 6, ry - 2, MAP_INS_W - 12, rowH - 4, COL_GOLD);
            }
            // Vertically center the label and value inside the row using the
            // font's actual surface height — same pattern other tabs (word
            // slots, settings rows) use to keep text level with row content.
            TTF_Font* fsm_r = GetFont(Font::Sm);
            int fh_r = 0;
            if (fsm_r) TTF_SizeUTF8(fsm_r, "Tg", nullptr, &fh_r);
            int textY = ry + (rowH - fh_r) / 2;
            DrawText(rows[r].label, MAP_INS_X + 14, textY, focused ? COL_GOLD : COL_DIM, Font::Sm);
            // Anchor the nudge controls to the right side of the inspector so
            // the layout adapts to any MAP_INS_W. Buttons are big enough for
            // a finger tap (~40×28) — touch is the primary way to edit non-XY
            // fields since the D-pad now moves the object on the grid instead.
            const int btnW = 40, btnH = 28;
            int plusX  = MAP_INS_X + MAP_INS_W - btnW - 6;
            int minusX = plusX - btnW - 8;
            int valX   = MAP_INS_X + 130;
            int btnY   = ry + (rowH - btnH) / 2 - 1;
            // Register button hit zones BEFORE the row catch-all below so
            // HitFire's first-match-wins picks the button when a tap lands
            // on it.
            HitAdd(minusX, btnY, btnW, btnH, TMapInsNudge, (r << 8) | 255);
            HitAdd(plusX,  btnY, btnW, btnH, TMapInsNudge, (r << 8) | 1);
            HitAdd(MAP_INS_X + 6, ry - 2, MAP_INS_W - 12, rowH - 4, TMapInsField, r);
            FillRect(minusX, btnY, btnW, btnH, COL_PANEL2);
            DrawRect(minusX, btnY, btnW, btnH, COL_BORDER);
            DrawTextC("-", minusX + btnW/2, btnY + btnH/2, COL_TEXT, Font::Md);
            DrawText(rows[r].value, valX, textY, COL_TEXT);
            FillRect(plusX, btnY, btnW, btnH, COL_PANEL2);
            DrawRect(plusX, btnY, btnW, btnH, COL_BORDER);
            DrawTextC("+", plusX + btnW/2, btnY + btnH/2, COL_TEXT, Font::Md);
        }
        iy += rowsCount * rowH + 8;

        // Change-actor button
        FillRect(MAP_INS_X + 12, iy, MAP_INS_W - 24, 28, COL_PANEL2);
        DrawRect(MAP_INS_X + 12, iy, MAP_INS_W - 24, 28, COL_BORDER);
        DrawTextC(Lang::T("map.add.actor"), MAP_INS_X + MAP_INS_W/2, iy + 14, COL_TEXT, Font::Sm);
        HitAdd(MAP_INS_X + 12, iy, MAP_INS_W - 24, 28, TMapBtn, (int)HidNpadButton_Y);
        iy += 36;

        // Delete (X) button — confirm-on-second-press model
        FillRect(MAP_INS_X + 12, iy, MAP_INS_W - 24, 28,
                 gMapDelConfirm ? COL_RED : COL_PANEL2);
        DrawRect(MAP_INS_X + 12, iy, MAP_INS_W - 24, 28,
                 gMapDelConfirm ? COL_RED : COL_BORDER);
        DrawTextC(Lang::T(gMapDelConfirm ? "map.delete.confirm.tap" : "map.delete.btn"),
                  MAP_INS_X + MAP_INS_W/2, iy + 14,
                  gMapDelConfirm ? COL_BG : COL_RED, Font::Sm);
        HitAdd(MAP_INS_X + 12, iy, MAP_INS_W - 24, 28, TMapBtn, (int)HidNpadButton_X);
    } else if (gMapMode == MapMode::Place) {
        DrawText(Lang::T("map.place.title"), MAP_INS_X + 12, iy, COL_GOLD, Font::Md); iy += 28;
        DrawText(Lang::T("map.place.prompt"), MAP_INS_X + 12, iy, COL_DIM, Font::Sm); iy += 22;
        const MapData::ActorInfo* info = MapData::ActorLookup(gMapPlaceActor);
        if (info) {
            DrawText(Lang::T("map.actor") + ": " + MapActorDisplayLabel(info), MAP_INS_X + 12, iy, COL_TEXT, Font::Sm);
            iy += 22;
        }
        DrawText(Lang::T("map.cursor") + ": " + std::to_string(gMapCursorX) + ", " + std::to_string(gMapCursorY),
                 MAP_INS_X + 12, iy, COL_DIM, Font::Sm);
    } else {
        DrawText(Lang::T("map.viewer.title"), MAP_INS_X + 12, iy, COL_GOLD, Font::Md); iy += 28;
        DrawText(Lang::T("map.viewer.l1"),
                 MAP_INS_X + 12, iy, COL_TEXT, Font::Sm); iy += 20;
        DrawText(Lang::T("map.viewer.l2"),
                 MAP_INS_X + 12, iy, COL_DIM, Font::Sm); iy += 20;
        DrawText(Lang::T("map.viewer.l3"),
                 MAP_INS_X + 12, iy, COL_DIM, Font::Sm); iy += 20;
        DrawText(Lang::T("map.viewer.l4"),
                 MAP_INS_X + 12, iy, COL_DIM, Font::Sm); iy += 18;
        DrawText(Lang::T("map.viewer.l5"),
                 MAP_INS_X + 12, iy, COL_DIM, Font::Sm); iy += 22;
        DrawText(Lang::T("map.objects") + ": " + std::to_string(MapObjectCount()) + " " + Lang::T("map.slots"),
                 MAP_INS_X + 12, iy, COL_DIM, Font::Sm); iy += 18;
        if (gMapDirty) {
            DrawText(Lang::T("map.dirty"), MAP_INS_X + 12, iy, COL_GOLD, Font::Sm); iy += 18;
        }
    }

    // Transient status message (drawn above the footer)
    if (!gMapMsg.empty()) {
        DrawTextC(gMapMsg, SCREEN_W/2, SCREEN_H - 56, gMapMsgCol, Font::Sm);
    }

    // Footer
    const char* footerKey =
        gMapMode == MapMode::Inspect ? "map.footer.inspect" :
        gMapMode == MapMode::Picker  ? "map.footer.picker"  :
        gMapMode == MapMode::Place   ? "map.footer.place"   :
                                       "map.footer.idle";
    DrawFooter(Lang::T(footerKey));
    Present();
}

static void DrawMiiStats() {
    HitClear();
    // Auto-fade transient messages (e.g. habit toggle confirmations)
    if (gMiiStatsMsgFrames > 0) {
        if (--gMiiStatsMsgFrames == 0) { gMiiStatsMsg.clear(); gMiiStatsMsgCol = COL_TEXT; }
    }
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b,255);
    SDL_RenderClear(gRen);
    if (gShowFileBrowser) { DrawFileBrowser(); Present(); return; }
    DrawHeader(Lang::T("mii.header"));

    // Tab bar
    {
        int tw=130, th=22, gap=4;
        int totalW=tw*5+gap*4, tx=SCREEN_W/2-totalW/2, ty=28;
        struct { std::string label; OnSwitchMode mode; } tabs[]={
            {Lang::T("tab.webui"),OnSwitchMode::WebUI},{Lang::T("tab.textures"),OnSwitchMode::UGC},
            {Lang::T("tab.mii"),OnSwitchMode::MiiStats},{Lang::T("tab.player"),OnSwitchMode::Player},
            {Lang::T("tab.map"),OnSwitchMode::Map}};
        int ulX = tx, ulY = ty + th + 2;
        bool ulHave = false;
        for (auto& t : tabs) {
            bool sel=gOnSwitchMode==t.mode;
            HitAdd(tx,ty,tw,th,TSetMode,(int)t.mode);
            DrawTextC(t.label,tx+tw/2,ty+th/2,sel?COL_GOLD:COL_DIM);
            if (sel) { ulX = tx + (tw-72)/2; ulHave = true; }
            tx+=tw+gap;
        }
        if (ulHave) {
            RequestTabUnderline(0, 100, ulX, ulY, 72, 3, gDtSec);
            DrawTabUnderline(0);
        }
    }

    if (!gMiiSav.loaded) {
        // Surface the actual load error instead of leaving "loading..." on
        // screen forever. SaveEditor::Load writes the reason into gMiiStatsMsg
        // ("Cannot open", "Bad magic", "Bad saveDataOffset", etc.).
        if (!gMiiStatsMsg.empty()) {
            DrawTextC(gMiiStatsMsg, SCREEN_W/2, SCREEN_H/2 - 14, COL_RED, Font::Md);
            // "Cannot open" almost always means the file doesn't exist —
            // either no Mii has been created yet or the wrong profile was
            // picked. Show a friendlier hint instead of the "Mii.sav
            // couldn't be parsed" wording which implies a corrupt save.
            if (gMiiStatsMsg.find("Cannot open") != std::string::npos) {
                DrawTextC(Lang::T("mii.notfound.hint"),
                          SCREEN_W/2, SCREEN_H/2 + 18, COL_DIM, Font::Sm);
            } else {
                DrawTextC(Lang::T("mii.parse.fail.l1"),
                          SCREEN_W/2, SCREEN_H/2 + 18, COL_DIM, Font::Sm);
                DrawTextC(Lang::T("mii.parse.fail.l2"),
                          SCREEN_W/2, SCREEN_H/2 + 38, COL_DIM, Font::Sm);
            }
        } else {
            DrawTextC(Lang::T("mii.loading"), SCREEN_W/2, SCREEN_H/2, COL_DIM, Font::Md);
        }
        DrawFooter(Lang::T("footer.back"));
        Present();
        return;
    }

    // Left: mii selector list (with scroll)
    int mseListH = SCREEN_H - SE_TOP_Y - 32;
    int mseVis   = mseListH / ITEM_H;
    FillRect(SE_LIST_X-4, SE_TOP_Y, MSE_LIST_W+8, mseListH, COL_PANEL);
    DrawRect(SE_LIST_X-4, SE_TOP_Y, MSE_LIST_W+8, mseListH, COL_BORDER);
    int leftSelY = SE_TOP_Y + LIST_PAD_TOP;
    bool leftSawSel = false;
    for (int i = 0; i < mseVis; i++) {
        int idx = gMiiStatsScroll + i;
        if (idx >= (int)gMiis.size()) break;
        bool sel = (idx == gMiiStatsMiiSel);
        int ry = SE_TOP_Y + LIST_PAD_TOP + i * ITEM_H;
        if (sel) { leftSelY = ry; leftSawSel = true; }
        HitAdd(SE_LIST_X, ry, MSE_LIST_W, ITEM_H-1, TSelMiiStatsMii, idx);
        FillRect(SE_LIST_X, ry, MSE_LIST_W, ITEM_H-1, sel?COL_SEL:COL_BG);
        if (sel && gTheme.cornerRadius == 0)
            DrawRectSquare(SE_LIST_X, ry, MSE_LIST_W, ITEM_H-1, COL_GOLD);
        TTF_Font* fsm=GetFont(Font::Sm); int fw=0,fh=0;
        if(fsm) TTF_SizeUTF8(fsm,gMiis[idx].name.c_str(),&fw,&fh);
        DrawText(gMiis[idx].name, SE_LIST_X+6, ry+(ITEM_H-fh)/2, sel?COL_TEXT:COL_DIM);
    }
    if (leftSawSel) {
        RequestHighlight((int)Screen::OnSwitch, /*sub=mii left list*/33,
                         SE_LIST_X, leftSelY, MSE_LIST_W, ITEM_H-1, gDtSec,
                         /*slot=*/1);
        DrawAnimatedHighlight(/*slot=*/1);
    }
    if ((int)gMiis.size() > mseVis) {
        int barH = mseListH * mseVis / (int)gMiis.size();
        int barY = SE_TOP_Y + mseListH * gMiiStatsScroll / (int)gMiis.size();
        FillRect(SE_LIST_X+MSE_LIST_W+2, barY, 4, barH, COL_BORDER);
    }
    if (gMiis.empty()) {
        DrawTextC(Lang::T("mii.no.miis"), SE_LIST_X+MSE_LIST_W/2, SE_TOP_Y+80, COL_DIM);
    }

    // Middle: field list for selected mii (hidden in Social/Belongings — panel expands)
    int midX = SE_LIST_X + MSE_LIST_W + 12;
    int midW = 340;
    bool midHidden = (gMiiStatsSubTab == MiiStatsSubTab::Social || gMiiStatsSubTab == MiiStatsSubTab::Belongings || gMiiStatsSubTab == MiiStatsSubTab::Habits || gMiiStatsSubTab == MiiStatsSubTab::Housing || gMiiStatsSubTab == MiiStatsSubTab::Browse);
    if (!midHidden) {
        FillRect(midX, SE_TOP_Y, midW, SCREEN_H-SE_TOP_Y-32, {8,8,8,255});
        DrawRect(midX, SE_TOP_Y, midW, SCREEN_H-SE_TOP_Y-32, COL_BORDER);
    }

    if (!gMiis.empty() && gMiiStatsMiiSel < (int)gMiis.size()) {
        int miiSlotIdx = gMiis[gMiiStatsMiiSel].slot - 1; // 0-based array index

        if (gMiiStatsSubTab == MiiStatsSubTab::Words) {
            // Word slots list (12 items)
            static const uint32_t H_WKIND_L = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordKind");
            static const uint32_t H_WTXT_L  = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordText");
            static const uint32_t H_WINV_L  = SaveEditor::Hash("Invalid");
            TTF_Font* fsm = GetFont(Font::Sm);
            int mwSelY = SE_TOP_Y + 2;
            bool mwSawSel = false;
            for (int i = 0; i < 12; i++) {
                bool sel = (i == gMiiWordsSlotSel);
                int ry = SE_TOP_Y + 2 + i * 34;
                if (ry + 34 > SCREEN_H - 32) break;
                if (sel) { mwSelY = ry; mwSawSel = true; }
                HitAdd(midX+1, ry, midW-2, 33, TSelMiiWordsSlot, i);
                FillRect(midX+1, ry, midW-2, 33, sel?COL_SEL:COL_BG);
                if (sel && gTheme.cornerRadius == 0)
                    DrawRectSquare(midX+1, ry, midW-2, 33, COL_GOLD);
                int wIdx = miiSlotIdx * 12 + i;
                uint32_t kh = SaveEditor::GetAnyEnumAt(gMiiSav, H_WKIND_L, wIdx, H_WINV_L);
                bool filled = (kh != H_WINV_L);
                const char* kLbl = filled ? WordKindLabel(kh) : nullptr;
                // Kind label right-aligned
                int kw=0,kh2=0;
                if (fsm && kLbl) TTF_SizeUTF8(fsm, kLbl, &kw, &kh2);
                if (kLbl) DrawText(kLbl, midX+midW-kw-8, ry+(33-kh2)/2, sel?COL_ACCENT:COL_DIM);
                // Slot number
                std::string numS = std::to_string(i+1)+".";
                int nw=0,nh=0;
                if (fsm) TTF_SizeUTF8(fsm, numS.c_str(), &nw, &nh);
                DrawText(numS, midX+8, ry+(33-nh)/2, sel?COL_TEXT:COL_DIM);
                // Word text or status
                int availW = midW - kw - (kw?8:0) - 8 - nw - 8;
                std::string wt = filled ? SaveEditor::GetWStr64At(gMiiSav, H_WTXT_L, wIdx) : "";
                std::string disp = filled ? (wt.empty()?"—":FitNodeName(wt,fsm,availW)) : "—";
                DrawText(disp, midX+8+nw+4, ry+(33-nh)/2, sel?(filled?COL_TEXT:COL_DIM):(filled?COL_DIM:COL_DIM));
            }
            if (mwSawSel) {
                RequestHighlight((int)Screen::OnSwitch, /*sub=mii words*/30,
                                 midX+1, mwSelY, midW-2, 33, gDtSec);
                DrawAnimatedHighlight();
            }
        } else if (gMiiStatsSubTab == MiiStatsSubTab::Relations) {
            // Relations list: pairs involving this Mii
            static const uint32_t H_IDA_ML   = 0xf7420afbu;
            static const uint32_t H_IDB_ML   = 0x4071f71cu;
            static const uint32_t H_BASE_ML  = 0x8b41897eu;
            static const uint32_t H_MNAME_ML = 0x2499bfdau;
            int pairCount = SaveEditor::ArraySize(gMiiSav, H_IDA_ML);
            TTF_Font* fsm2 = GetFont(Font::Sm);
            int visRow = 0, filtRow = 0;
            int mrSelY = SE_TOP_Y + 2;
            bool mrSawSel = false;
            gMiiRelPairIdx = -1;
            for (int pi = 0; pi < pairCount; pi++) {
                int a = SaveEditor::GetIntAt(gMiiSav, H_IDA_ML, pi);
                int b = SaveEditor::GetIntAt(gMiiSav, H_IDB_ML, pi);
                if (a < 0 || b < 0) continue;
                if (a != miiSlotIdx && b != miiSlotIdx) continue;
                bool selfA = (a == miiSlotIdx);
                int other  = selfA ? b : a;
                int myDir  = selfA ? pi*2 : pi*2+1;
                uint32_t myType = SaveEditor::GetEnumAt(gMiiSav, H_BASE_ML, myDir, 0x0784a8dcu);
                SDL_Color tc = RelTypeColor(myType);
                bool sel = (filtRow == gMiiRelSel);
                if (sel) { gMiiRelPairIdx = pi; gMiiRelSelfA = selfA; }
                if (filtRow >= gMiiRelScroll) {
                    int ry = SE_TOP_Y + 2 + visRow * 34;
                    if (ry + 34 <= SCREEN_H - 32) {
                        if (sel) { mrSelY = ry; mrSawSel = true; }
                        HitAdd(midX+1, ry, midW-2, 33, TSelMiiRelation, filtRow);
                        FillRect(midX+1, ry, midW-2, 33, sel?COL_SEL:COL_BG);
                        if (sel && gTheme.cornerRadius == 0)
                            DrawRectSquare(midX+1, ry, midW-2, 33, COL_GOLD);
                        // Type label right-aligned (colored)
                        const char* tName = RelTypeName(myType);
                        int tw=0,th=0;
                        if (fsm2 && tName) TTF_SizeUTF8(fsm2, tName, &tw, &th);
                        DrawText(tName?tName:"?", midX+midW-tw-8, ry+(33-th)/2, tc);
                        // Other Mii name left
                        std::string oName = SaveEditor::GetWStr32At(gMiiSav, H_MNAME_ML, other);
                        int availRL = midW - tw - (tw?8:0) - 16;
                        std::string oFit = FitNodeName(oName.empty()?"?":oName, fsm2, availRL);
                        int rnw=0,rnh=0;
                        if (fsm2) TTF_SizeUTF8(fsm2, oFit.c_str(), &rnw, &rnh);
                        DrawText(oFit, midX+8, ry+(33-rnh)/2, sel?COL_TEXT:COL_DIM);
                        visRow++;
                    }
                }
                filtRow++;
            }
            gMiiRelCount = filtRow;
            if (filtRow > 0 && gMiiRelSel >= filtRow) gMiiRelSel = filtRow-1;
            if (filtRow == 0)
                DrawTextC(Lang::T("mii.no.relations"), midX+midW/2, SE_TOP_Y+80, COL_DIM);
            if (mrSawSel) {
                RequestHighlight((int)Screen::OnSwitch, /*sub=mii relations*/31,
                                 midX+1, mrSelY, midW-2, 33, gDtSec);
                DrawAnimatedHighlight();
            }
        } else if (!midHidden) {
            // Stats fields list (only when the middle panel is visible)
            int msSelY = SE_TOP_Y + 2;
            bool msSawSel = false;
            for (int i = 0; i < MII_STATS_FIELD_COUNT; i++) {
                bool sel = (i == gMiiStatsFieldSel);
                int ry = SE_TOP_Y + 2 + i * 34;
                if (ry + 34 > SCREEN_H - 32) break;
                if (sel) { msSelY = ry; msSawSel = true; }
                HitAdd(midX+1, ry, midW-2, 33, TSelMiiStatsField, i);
                FillRect(midX+1, ry, midW-2, 33, sel?COL_SEL:COL_BG);
                const auto& fd = MII_STATS_FIELDS[i];
                SDL_Color borderCol = fd.isAction ? COL_GOLD : COL_GOLD;
                if (sel && gTheme.cornerRadius == 0)
                    DrawRectSquare(midX+1, ry, midW-2, 33, borderCol);
                TTF_Font* fsm=GetFont(Font::Sm); int lw=0,lh=0,vw=0,vh=0;
                std::string fdLabel = MiiLabelT(fd.label);
                if(fsm) TTF_SizeUTF8(fsm,fdLabel.c_str(),&lw,&lh);
                if (fd.isAction) {
                    SDL_Color lCol = sel ? COL_GOLD : SDL_Color{160,120,60,255};
                    DrawText(fdLabel, midX+8, ry+(33-lh)/2, lCol);
                    const char* arrow = "▶";
                    int aw=0,ah=0;
                    if(fsm) TTF_SizeUTF8(fsm,arrow,&aw,&ah);
                    DrawText(arrow, midX+midW-aw-8, ry+(33-ah)/2, sel?COL_GOLD:SDL_Color{100,80,40,255});
                } else {
                    std::string val;
                    if (fd.isStr) {
                        val = SaveEditor::GetWStr32At(gMiiSav, SaveEditor::Hash(fd.fieldName), miiSlotIdx);
                    } else if (fd.isUInt) {
                        val = std::to_string(SaveEditor::GetUIntAt(gMiiSav, SaveEditor::Hash(fd.fieldName), miiSlotIdx));
                    } else if (fd.isEnum) {
                        uint32_t ev = SaveEditor::GetEnumAt(gMiiSav, SaveEditor::Hash(fd.fieldName), miiSlotIdx);
                        const char* lbl = FeelingName(ev);
                        val = lbl ? lbl : ("?" + std::to_string(ev));
                    } else {
                        int32_t v = SaveEditor::GetIntAt(gMiiSav, SaveEditor::Hash(fd.fieldName), miiSlotIdx);
                        val = std::to_string(v + fd.dispOffset);
                    }
                    if(fsm) TTF_SizeUTF8(fsm,val.c_str(),&vw,&vh);
                    DrawText(fdLabel, midX+8, ry+(33-lh)/2, sel?COL_TEXT:COL_DIM);
                    DrawText(val, midX+midW-vw-8, ry+(33-vh)/2, sel?COL_ACCENT:COL_DIM);
                }
            }
            if (msSawSel) {
                RequestHighlight((int)Screen::OnSwitch, /*sub=mii stats*/32,
                                 midX+1, msSelY, midW-2, 33, gDtSec);
                DrawAnimatedHighlight();
            }
        }
    }

    // Right: edit controls / social / belongings view
    // Social and Belongings expand left to cover the hidden middle panel
    int editX = midX + midW + 8;
    int editW = SCREEN_W - editX - 8;
    int panelX = midHidden ? midX   : editX;
    int panelW = midHidden ? (SCREEN_W - midX - 8) : editW;
    FillRect(panelX, SE_TOP_Y, panelW, SCREEN_H-SE_TOP_Y-32, {6,6,6,255});
    DrawRect(panelX, SE_TOP_Y, panelW, SCREEN_H-SE_TOP_Y-32, COL_BORDER);

    // Sub-tab bar: [Stats] [Belongings] [Habits] [Words] [Relations] [Social] [Housing]
    // The Browse view is reached via the upload-icon button next to "export .ltd"
    // on the Stats sub-tab, not from the pill bar.
    {
        int stW=76, stH=22, stGap=4, stX=editX+8, stY=SE_TOP_Y+6;
        const char* stKeys[] = {"subtab.stats","subtab.items","subtab.habits","subtab.words","subtab.relations","subtab.housing","subtab.social"};
        int sulX = stX, sulY = stY + stH + 2;
        bool sulHave = false;
        for (int ti=0; ti<MII_SUBTAB_COUNT; ti++) {
            bool sSel = ((int)gMiiStatsSubTab == ti);
            int sx = stX + ti*(stW+stGap);
            // Hit area is taller than the visual so it's easier to tap on touchscreen
            HitAdd(sx, stY-4, stW, stH+8, TSetSocialView, ti);
            DrawTextC(Lang::T(stKeys[ti]), sx+stW/2, stY+stH/2, sSel?COL_GOLD:COL_DIM);
            if (sSel) { sulX = sx + (stW-44)/2; sulHave = true; }
        }
        if (sulHave) {
            RequestTabUnderline(/*slot*/1, /*owner=mii subtabs*/101,
                                sulX, sulY, 44, 3, gDtSec);
            DrawTabUnderline(/*slot*/1);
        }
    }

    if (!gMiis.empty() && gMiiStatsMiiSel < (int)gMiis.size()) {
        int miiSlotIdx = gMiis[gMiiStatsMiiSel].slot - 1;
        int panelTop = SE_TOP_Y + 32;
        int panelH   = SCREEN_H - panelTop - 32;

        if (gMiiStatsSubTab == MiiStatsSubTab::Social) {
            static const uint32_t H_ID_A  = 0xf7420afbu;
            static const uint32_t H_ID_B  = 0x4071f71cu;
            static const uint32_t H_BASE  = 0x8b41897eu;
            static const uint32_t H_METER = 0x42c2fc2fu;
            static const uint32_t H_NAME  = 0x2499bfdau;
            int pairCount = SaveEditor::ArraySize(gMiiSav, H_ID_A);

            if (gSocialExpanded) {
                // ── Global graph — multi-ring, handles up to 70 miis ─────────────
                int gcx = panelX + panelW/2;
                int gcy = panelTop + panelH/2;
                int N   = (int)gMiis.size();
                if (N == 0) {
                    DrawTextC(Lang::T("mii.no.miis"), gcx, gcy, COL_DIM, Font::Md);
                } else {
                    // True-circle nodes with comfortable minimum size. Label
                    // widths (NOT circle widths) drive ring capacity so names
                    // stay readable, and we paginate with "+N more" when the
                    // panel really can't fit everyone.
                    int nodeR = N<=8?26 : N<=16?22 : N<=28?20 : 18;
                    int lblGap = 3, lblH = 14;
                    int lblW   = 96;  // generous label width; FitNodeName clips with ellipsis
                    int slotH  = 2*nodeR + lblGap + lblH;

                    int maxR = std::min(panelW/2 - lblW/2 - 8, panelH/2 - slotH/2 - 8);
                    int Rcur = std::max(nodeR * 4 + 8, 80);

                    struct GRing { int R, first, count; };
                    std::vector<GRing> grings;
                    int placed = 0;
                    while (placed < N && Rcur <= maxR) {
                        // Spacing must clear BOTH the circle and the wider
                        // label; otherwise neighbours' names overlap.
                        int spacing = std::max(2*nodeR + 12, lblW + 8);
                        float s = (float)spacing / (2.f * (float)Rcur);
                        int cap = (s >= 1.f) ? 1 : (int)((float)M_PI / asinf(s));
                        cap = std::max(1, std::min(cap, N - placed));
                        grings.push_back({Rcur, placed, cap});
                        placed += cap;
                        Rcur += slotH + 8;
                    }
                    int hidden = N - placed;

                    std::vector<int> nx_(N, -9999), ny_(N, -9999);
                    for (auto& gr : grings) {
                        for (int i = 0; i < gr.count; i++) {
                            int j = gr.first + i;
                            float a = -(float)M_PI/2.f + (2.f*(float)M_PI*i)/gr.count;
                            nx_[j] = gcx + (int)((float)gr.R * cosf(a));
                            ny_[j] = gcy + (int)((float)gr.R * sinf(a));
                        }
                    }

                    // Edges (under nodes), semi-transparent rel-type tint.
                    SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_BLEND);
                    for (int i = 0; i < pairCount; i++) {
                        int sa = SaveEditor::GetIntAt(gMiiSav, H_ID_A, i);
                        int sb = SaveEditor::GetIntAt(gMiiSav, H_ID_B, i);
                        if (sa < 0 || sb < 0) continue;
                        int ai = -1, bi = -1;
                        for (int j = 0; j < N; j++) {
                            if (gMiis[j].slot-1 == sa) ai = j;
                            if (gMiis[j].slot-1 == sb) bi = j;
                        }
                        if (ai < 0 || bi < 0 || nx_[ai] < -9990 || nx_[bi] < -9990) continue;
                        SDL_Color lc = RelTypeColor(SaveEditor::GetEnumAt(gMiiSav, H_BASE, i*2));
                        SDL_SetRenderDrawColor(gRen, lc.r, lc.g, lc.b, 100);
                        SDL_RenderDrawLine(gRen, nx_[ai], ny_[ai], nx_[bi], ny_[bi]);
                    }
                    SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_NONE);

                    // Nodes: circle + label below. Selected mii gets a gold
                    // halo and bold gold label so it pops without the circle
                    // size changing (avoids layout shift on select).
                    TTF_Font* fsm = GetFont(Font::Sm);
                    for (int i = 0; i < N; i++) {
                        if (nx_[i] < -9990) continue;
                        bool isSel = (i == gMiiStatsMiiSel);
                        SDL_Color fill   = isSel ? COL_ACCENT : COL_PANEL2;
                        SDL_Color border = isSel ? COL_GOLD   : COL_BORDER;

                        HitAdd(nx_[i] - lblW/2, ny_[i] - nodeR - 4,
                               lblW, 2*nodeR + lblGap + lblH + 8,
                               TSelMiiStatsMii, i);

                        FillCircle(nx_[i], ny_[i], nodeR, fill);
                        DrawCircle(nx_[i], ny_[i], nodeR, border);
                        if (isSel) DrawRing(nx_[i], ny_[i], nodeR + 2, COL_GOLD);

                        std::string raw  = SaveEditor::GetWStr32At(gMiiSav, H_NAME, gMiis[i].slot-1);
                        std::string name = FitNodeName(raw, fsm, lblW - 4);
                        DrawTextC(name, nx_[i], ny_[i] + nodeR + lblGap + lblH/2,
                                  isSel ? COL_GOLD : COL_TEXT, Font::Sm);
                    }

                    // Center count chip + overflow notice.
                    DrawTextC(std::to_string(N) + " " + Lang::T("mii.miis.word"),
                              gcx, gcy, COL_DIM, Font::Sm);
                    if (hidden > 0) {
                        std::string hstr = "+" + std::to_string(hidden) + " " + Lang::T("more");
                        DrawTextC(hstr, panelX + panelW - 60, panelTop + panelH - 16,
                                  COL_DIM, Font::Sm);
                    }
                }
            } else {
                // ── Focus graph: multi-ring radial, handles up to 70 connections ─
                struct SRow { int other; uint32_t outT; int32_t outM; };
                std::vector<SRow> rows;
                for (int i = 0; i < pairCount; i++) {
                    int a = SaveEditor::GetIntAt(gMiiSav, H_ID_A, i);
                    int b = SaveEditor::GetIntAt(gMiiSav, H_ID_B, i);
                    if (a < 0 || b < 0) continue;
                    if (a != miiSlotIdx && b != miiSlotIdx) continue;
                    bool selfA = (a == miiSlotIdx);
                    SRow r;
                    r.other = selfA ? b : a;
                    r.outT  = SaveEditor::GetEnumAt(gMiiSav, H_BASE, selfA ? i*2 : i*2+1);
                    r.outM  = SaveEditor::GetIntAt(gMiiSav, H_METER, selfA ? i*2 : i*2+1);
                    rows.push_back(r);
                }
                // Group same relationship types together for visual clarity
                std::sort(rows.begin(), rows.end(), [](const SRow& a, const SRow& b){ return a.outT < b.outT; });

                int gcx = panelX + panelW/2;
                int gcy = panelTop + panelH/2;

                if (rows.empty()) {
                    DrawTextC(Lang::T("mii.no.relationships"), gcx, gcy, COL_DIM, Font::Md);
                } else {
                    int n = (int)rows.size();

                    // Center circle is big; satellites stay at a comfortable
                    // minimum so names below them are legible. Label width is
                    // budgeted independently of the circle so a short name in
                    // a tiny circle still has room to render in full.
                    int cRad  = 38;
                    int nodeR = n<=6?28 : n<=14?24 : n<=24?22 : 20;
                    int lblGap = 4, lblH = 14;
                    int lblW   = 120;        // wider so most names fit in full
                    int slotH  = 2*nodeR + lblGap + lblH;       // single name line only

                    int maxR = std::min(panelW/2 - lblW/2 - 8, panelH/2 - slotH/2 - 8);
                    int Rcur = std::max(cRad + nodeR + 40, 110);

                    struct SRing { int R, first, count; };
                    std::vector<SRing> rings;
                    int placed = 0;
                    while (placed < n && Rcur <= maxR) {
                        int spacing = std::max(2*nodeR + 12, lblW + 10);
                        float s = (float)spacing / (2.f * (float)Rcur);
                        int cap = (s >= 1.f) ? 1 : (int)((float)M_PI / asinf(s));
                        cap = std::max(1, std::min(cap, n - placed));
                        rings.push_back({Rcur, placed, cap});
                        placed += cap;
                        Rcur += slotH + 14;
                    }
                    int hidden = n - placed;

                    TTF_Font* fsm = GetFont(Font::Sm);
                    TTF_Font* fmd = GetFont(Font::Md);
                    std::string cName = SaveEditor::GetWStr32At(gMiiSav, H_NAME, miiSlotIdx);
                    std::string cFit  = FitNodeName(cName.empty()?"?":cName, fmd, 200);

                    // 1. Edges — drawn under everything else.
                    SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_BLEND);
                    for (auto& ring : rings) {
                        for (int i = 0; i < ring.count; i++) {
                            const SRow& row = rows[ring.first + i];
                            SDL_Color lc = RelTypeColor(row.outT);
                            float angle = -(float)M_PI/2.f + (2.f*(float)M_PI*i)/ring.count;
                            int nx = gcx + (int)((float)ring.R * cosf(angle));
                            int ny = gcy + (int)((float)ring.R * sinf(angle));
                            SDL_SetRenderDrawColor(gRen, lc.r, lc.g, lc.b, 150);
                            SDL_RenderDrawLine(gRen, gcx, gcy, nx, ny);
                        }
                    }
                    SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_NONE);

                    // 2. Center — big accent circle, gold halo, focused mii's
                    //    name below in Font::Md so it reads as the subject.
                    FillCircle(gcx, gcy, cRad,     COL_ACCENT);
                    DrawCircle(gcx, gcy, cRad,     COL_TEXT);
                    DrawRing  (gcx, gcy, cRad + 2, COL_GOLD);
                    DrawTextC(cFit, gcx, gcy + cRad + lblGap + 9,
                              COL_GOLD, Font::Md);

                    // 3. Satellites: filled disc with a 2-px thick ring in the
                    //    relationship-type color (the same color as the spoke
                    //    edge to the centre). The ring + edge together carry
                    //    the rel-type signal, so we don't repeat it in text —
                    //    that's what made the previous layout feel crowded.
                    for (auto& ring : rings) {
                        for (int i = 0; i < ring.count; i++) {
                            const SRow& row = rows[ring.first + i];
                            SDL_Color lc = RelTypeColor(row.outT);
                            float angle = -(float)M_PI/2.f + (2.f*(float)M_PI*i)/ring.count;
                            int nx = gcx + (int)((float)ring.R * cosf(angle));
                            int ny = gcy + (int)((float)ring.R * sinf(angle));

                            FillCircle(nx, ny, nodeR,     COL_PANEL2);
                            DrawRing  (nx, ny, nodeR,     lc);
                            DrawRing  (nx, ny, nodeR - 1, lc);   // double-stroke ⇒ thicker

                            std::string nn   = SaveEditor::GetWStr32At(gMiiSav, H_NAME, row.other);
                            std::string nFit = FitNodeName(nn.empty()?"?":nn, fsm, lblW - 4);
                            DrawTextC(nFit, nx, ny + nodeR + lblGap + lblH/2,
                                      COL_TEXT, Font::Sm);
                        }
                    }

                    if (hidden > 0) {
                        std::string hstr = "+" + std::to_string(hidden) + " " + Lang::T("more");
                        DrawTextC(hstr, panelX + panelW - 60, panelTop + panelH - 16, COL_DIM, Font::Sm);
                    }
                }
            }
        } else if (gMiiStatsSubTab == MiiStatsSubTab::Words) {
            // ── Words editor ──────────────────────────────────────────────────────
            static const uint32_t H_WKIND_R  = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordKind");
            static const uint32_t H_WTXT_R   = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordText");
            static const uint32_t H_WHOW_R   = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordHowToCall");
            static const uint32_t H_WINV_R   = SaveEditor::Hash("Invalid");
            int wIdx = miiSlotIdx * 12 + gMiiWordsSlotSel;
            uint32_t curKind  = SaveEditor::GetAnyEnumAt(gMiiSav, H_WKIND_R, wIdx, H_WINV_R);
            std::string curText = SaveEditor::GetWStr64At(gMiiSav, H_WTXT_R, wIdx);
            std::string curHow  = SaveEditor::GetWStr64At(gMiiSav, H_WHOW_R, wIdx);
            bool isFilled = (curKind != H_WINV_R);
            const char* kindLbl = WordKindLabel(curKind);
            int cx2 = editX + editW/2;
            // Slot header
            DrawTextC(Lang::T("words.slot") + " " + std::to_string(gMiiWordsSlotSel+1) + " / 12",
                      cx2, panelTop+16, COL_DIM, Font::Sm);
            // Kind section
            SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
            SDL_RenderDrawLine(gRen, editX+12, panelTop+32, editX+editW-12, panelTop+32);
            DrawTextC(Lang::T("words.kind"), cx2, panelTop+50, COL_DIM, Font::Sm);
            DrawTextC(isFilled?(kindLbl?kindLbl:"?"):"—",
                      cx2, panelTop+84, isFilled?COL_TEXT:COL_DIM, Font::Lg);
            {
                int bW=108,bH=38,bGap=14;
                int bxL=cx2-bGap/2-bW, bxR=cx2+bGap/2, bY=panelTop+122;
                HitAdd(bxL,bY,bW,bH,TSimBtn,(int)HidNpadButton_Left);
                FillRect(bxL,bY,bW,bH,COL_PANEL); DrawRect(bxL,bY,bW,bH,COL_BORDER);
                DrawTextC("<",bxL+bW/2,bY+bH/2,COL_DIM,Font::Md);
                HitAdd(bxR,bY,bW,bH,TSimBtn,(int)HidNpadButton_Right);
                FillRect(bxR,bY,bW,bH,COL_PANEL); DrawRect(bxR,bY,bW,bH,COL_BORDER);
                DrawTextC(">",bxR+bW/2,bY+bH/2,COL_DIM,Font::Md);
                EmitPrevNextHighlight(/*sub=mii words kind*/121, bxL, bxR, bY, bW, bH);
            }
            // Text section
            SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
            SDL_RenderDrawLine(gRen, editX+12, panelTop+182, editX+editW-12, panelTop+182);
            DrawTextC(Lang::T("words.text"), cx2, panelTop+200, COL_DIM, Font::Sm);
            std::string dispTxt = curText.empty() ? Lang::T("none.paren") : curText;
            DrawTextC(dispTxt, cx2, panelTop+232, (isFilled&&!curText.empty())?COL_TEXT:COL_DIM, Font::Md);
            {
                int bW=240,bH=42,bX=cx2-bW/2,bY=panelTop+262;
                HitAdd(bX,bY,bW,bH,TSimBtn,(int)HidNpadButton_A);
                FillRect(bX,bY,bW,bH,COL_SEL); DrawRect(bX,bY,bW,bH,COL_ACCENT);
                DrawTextC("A  " + Lang::T("words.edit.text"),cx2,bY+bH/2,COL_ACCENT,Font::Sm);
            }
            // Pronunciation section
            SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
            SDL_RenderDrawLine(gRen, editX+12, panelTop+334, editX+editW-12, panelTop+334);
            DrawTextC(Lang::T("words.pron"), cx2, panelTop+352, COL_DIM, Font::Sm);
            std::string dispHow = curHow.empty() ? Lang::T("words.same.as.text") : curHow;
            DrawTextC(dispHow, cx2, panelTop+384, !curHow.empty()?COL_TEXT:COL_DIM, Font::Sm);
            {
                int bW=240,bH=42,bX=cx2-bW/2,bY=panelTop+410;
                HitAdd(bX,bY,bW,bH,TSimBtn,(int)HidNpadButton_X);
                FillRect(bX,bY,bW,bH,COL_SEL); DrawRect(bX,bY,bW,bH,COL_ACCENT);
                DrawTextC("X  " + Lang::T("words.edit.pron"),cx2,bY+bH/2,COL_ACCENT,Font::Sm);
            }
            // Undo button
            {
                bool canUndo = gWordUndo.valid;
                int ubW=120,ubH=44,ubX=cx2-ubW/2,ubY=panelTop+490;
                HitAdd(ubX,ubY,ubW,ubH,TUndoWords,0);
                FillRect(ubX,ubY,ubW,ubH,COL_PANEL);
                DrawRect(ubX,ubY,ubW,ubH,canUndo?COL_GOLD:COL_BORDER);
                DrawTextC("-  " + Lang::T("undo"),cx2,ubY+ubH/2,canUndo?COL_GOLD:COL_DIM,Font::Md);
            }
            if (!gMiiStatsMsg.empty())
                DrawTextC(gMiiStatsMsg, cx2, panelTop+554, gMiiStatsMsgCol);
        } else if (gMiiStatsSubTab == MiiStatsSubTab::Relations) {
            // ── Relations editor ─────────────────────────────────────────────────
            static const uint32_t H_BASE_RP  = 0x8b41897eu;
            static const uint32_t H_METER_RP = 0x42c2fc2fu;
            static const uint32_t H_IDA_RP   = 0xf7420afbu;
            static const uint32_t H_IDB_RP   = 0x4071f71cu;
            static const uint32_t H_MNAME_RP = 0x2499bfdau;
            int cx2 = editX + editW/2;
            if (gMiiRelPairIdx < 0) {
                DrawTextC(Lang::T("mii.no.relations"), cx2, panelTop + panelH/2, COL_DIM, Font::Md);
            } else {
                int myDir  = gMiiRelSelfA ? gMiiRelPairIdx*2   : gMiiRelPairIdx*2+1;
                int otDir  = gMiiRelSelfA ? gMiiRelPairIdx*2+1 : gMiiRelPairIdx*2;
                uint32_t myType = SaveEditor::GetEnumAt(gMiiSav, H_BASE_RP, myDir, 0x0784a8dcu);
                uint32_t otType = SaveEditor::GetEnumAt(gMiiSav, H_BASE_RP, otDir, 0x0784a8dcu);
                int32_t  myMeter = SaveEditor::GetIntAt(gMiiSav, H_METER_RP, myDir);
                bool fixed = RelMeterFixed(myType);
                int other = gMiiRelSelfA ? SaveEditor::GetIntAt(gMiiSav, H_IDB_RP, gMiiRelPairIdx)
                                         : SaveEditor::GetIntAt(gMiiSav, H_IDA_RP, gMiiRelPairIdx);
                std::string oName = SaveEditor::GetWStr32At(gMiiSav, H_MNAME_RP, other < 0 ? 0 : other);
                // Header: other Mii name
                DrawTextC(oName.empty()? Lang::T("unknown.paren") :oName, cx2, panelTop+18, COL_TEXT, Font::Md);
                SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
                SDL_RenderDrawLine(gRen, editX+12, panelTop+42, editX+editW-12, panelTop+42);
                // Your relationship type
                DrawTextC(Lang::T("rel.your.type"), cx2, panelTop+62, COL_DIM, Font::Sm);
                SDL_Color myTC = RelTypeColor(myType);
                const char* myTN = RelTypeName(myType);
                DrawTextC(myTN?myTN:"?", cx2, panelTop+102, myTC, Font::Lg);
                {
                    int bW=130,bH=40,bGap=14;
                    int bxL=cx2-bGap/2-bW, bxR=cx2+bGap/2, bY=panelTop+144;
                    HitAdd(bxL,bY,bW,bH,TSimBtn,(int)HidNpadButton_Left);
                    FillRect(bxL,bY,bW,bH,COL_PANEL); DrawRect(bxL,bY,bW,bH,COL_BORDER);
                    DrawTextC("< " + Lang::T("rel.type"),bxL+bW/2,bY+bH/2,COL_DIM);
                    HitAdd(bxR,bY,bW,bH,TSimBtn,(int)HidNpadButton_Right);
                    FillRect(bxR,bY,bW,bH,COL_PANEL); DrawRect(bxR,bY,bW,bH,COL_BORDER);
                    DrawTextC(Lang::T("rel.type") + " >",bxR+bW/2,bY+bH/2,COL_DIM);
                    EmitPrevNextHighlight(/*sub=mii rel type*/122, bxL, bxR, bY, bW, bH);
                    // When the current relationship is Unknown, expose a
                    // "Batch Know" shortcut that flips every Unknown relation
                    // this Mii has into Know with natural game values.
                    if (myType == 0x0784a8dcu) {
                        int bkW = 150;
                        int bxB = bxR + bW + 10;
                        if (bxB + bkW <= editX + editW - 8) {
                            HitAdd(bxB,bY,bkW,bH,TRelBatchKnow,0);
                            FillRect(bxB,bY,bkW,bH,COL_PANEL);
                            DrawRect(bxB,bY,bkW,bH,COL_GREEN);
                            DrawTextC(Lang::T("rel.batch.know"),bxB+bkW/2,bY+bH/2,COL_GREEN);
                        }
                    }
                }
                // Their type (auto-counterpart, read-only display)
                SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
                SDL_RenderDrawLine(gRen, editX+12, panelTop+222, editX+editW-12, panelTop+222);
                DrawTextC(Lang::T("rel.their.type"), cx2, panelTop+244, COL_DIM, Font::Sm);
                SDL_Color otTC = RelTypeColor(otType);
                const char* otTN = RelTypeName(otType);
                DrawTextC(otTN?otTN:"?", cx2, panelTop+282, otTC, Font::Md);
                // Meter
                SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
                SDL_RenderDrawLine(gRen, editX+12, panelTop+336, editX+editW-12, panelTop+336);
                DrawTextC(Lang::T("rel.meter"), cx2, panelTop+358, COL_DIM, Font::Sm);
                std::string mStr = fixed ? ("100 (" + Lang::T("rel.fixed") + ")") : std::to_string(myMeter);
                DrawTextC(mStr, cx2, panelTop+398, fixed?COL_DIM:COL_TEXT, Font::Lg);
                if (!fixed) {
                    static const int MSTEPS[] = {10, 1};
                    int bW=76,bH=44,bGap=6;
                    int totalBW = (2*(bW+bGap))*2 + 20;
                    int bxBase = cx2 - totalBW/2, bY = panelTop+442;
                    int bx = bxBase;
                    for (int s : MSTEPS) {
                        HitAdd(bx,bY,bW,bH,TRelMeterAdj,-s);
                        FillRect(bx,bY,bW,bH,COL_PANEL); DrawRect(bx,bY,bW,bH,COL_RED);
                        DrawTextC("-"+std::to_string(s),bx+bW/2,bY+bH/2,COL_RED,Font::Sm);
                        bx+=bW+bGap;
                    }
                    bx+=20;
                    for (int j=1;j>=0;j--) {
                        int s=MSTEPS[j];
                        HitAdd(bx,bY,bW,bH,TRelMeterAdj,s);
                        FillRect(bx,bY,bW,bH,COL_PANEL); DrawRect(bx,bY,bW,bH,COL_GREEN);
                        DrawTextC("+"+std::to_string(s),bx+bW/2,bY+bH/2,COL_GREEN,Font::Sm);
                        bx+=bW+bGap;
                    }
                    DrawTextC(Lang::T("rel.both.directions"), cx2, panelTop+514, COL_DIM, Font::Sm);
                }
                // ── Since (TypeSetTime) ───────────────────────────────────
                {
                    static const uint32_t H_TST_RP = 0x1a892e50u;
                    int sinceY = fixed ? panelTop+428 : panelTop+530;
                    SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
                    SDL_RenderDrawLine(gRen, editX+12, sinceY, editX+editW-12, sinceY);
                    uint64_t tst = SaveEditor::GetUInt64At(gMiiSav, H_TST_RP, gMiiRelPairIdx);
                    char dateBuf[36] = "";
                    if (tst > 0) {
                        time_t t = (time_t)tst;
                        struct tm* ti = localtime(&t);
                        snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d",
                                 ti->tm_year+1900, ti->tm_mon+1, ti->tm_mday);
                    }
                    DrawTextC(Lang::T("rel.since"), cx2, sinceY+14, COL_DIM, Font::Sm);
                    int rowH = 30, rowW = 150;
                    int rowY = sinceY+28, rowX = cx2 - rowW/2;
                    HitAdd(rowX, rowY, rowW, rowH, TRelDateKbd, gMiiRelPairIdx);
                    FillRect(rowX, rowY, rowW, rowH, COL_PANEL);
                    DrawRect(rowX, rowY, rowW, rowH, COL_BORDER);
                    DrawTextC(dateBuf[0] ? std::string(dateBuf) : Lang::T("not.set"), rowX+rowW/2, rowY+rowH/2,
                              dateBuf[0] ? COL_TEXT : COL_DIM, Font::Sm);
                }
            }
        } else if (gMiiStatsSubTab == MiiStatsSubTab::Belongings) {
            // ── Belongings panel (expanded, uses panelX/panelW) ─────────────────
            TTF_Font* fsm = GetFont(Font::Sm);

            if (gBlPickerOpen) {
                // ── Item picker overlay ──────────────────────────────────────────
                const int PK_ROW_H = 30;
                const int PK_HDR_H = 32;
                const int PK_FOOT_H = 22;
                int listTop = panelTop + PK_HDR_H;
                int listH   = panelH - PK_HDR_H - PK_FOOT_H;
                int pkVis   = listH / PK_ROW_H;
                int total   = (int)gBlFiltered.size();
                int totalAll = (int)gBlPickerHashes.size();

                if (gBlPickerSel < 0) gBlPickerSel = 0;
                if (total > 0 && gBlPickerSel >= total) gBlPickerSel = total - 1;
                if (gBlPickerScroll < 0) gBlPickerScroll = 0;
                if (total > 0 && gBlPickerScroll > total - pkVis) gBlPickerScroll = std::max(0, total - pkVis);

                // Header
                const char* slotName = (gBlSel < 8) ? BL_WORN_DEFS[gBlSel].name :
                                       (gBlSel == 8) ? "Coord" : "Pocket";
                FillRect(panelX+2, panelTop, panelW-4, PK_HDR_H, {28,28,28,255});
                DrawRect(panelX+2, panelTop, panelW-4, PK_HDR_H, COL_BORDER);
                std::string hdrStr = std::string("  ") + slotName;
                if (!gBlFilter.empty()) hdrStr += "  —  " + Lang::T("bl.filter") + ": \"" + gBlFilter + "\"";
                else                    hdrStr += "  —  " + Lang::T("bl.select.item");
                int hw=0,hh=0; if(fsm) TTF_SizeUTF8(fsm, hdrStr.c_str(), &hw, &hh);
                DrawText(hdrStr, panelX+10, panelTop+(PK_HDR_H-hh)/2,
                         gBlFilter.empty() ? COL_GOLD : COL_ACCENT);
                std::string cntStr = gBlFilter.empty()
                    ? (std::to_string(total) + " " + Lang::T("items"))
                    : (std::to_string(total) + " " + Lang::T("of") + " " + std::to_string(totalAll) + " " + Lang::T("items"));
                int cw=0,cntH=0; if(fsm) TTF_SizeUTF8(fsm, cntStr.c_str(), &cw, &cntH);
                DrawText(cntStr, panelX+panelW-cw-14, panelTop+(PK_HDR_H-cntH)/2, COL_DIM);

                // Item list (uses filtered indices)
                int bpSelY = listTop;
                bool bpSawSel = false;
                for (int i = 0; i < pkVis; i++) {
                    int filtIdx = gBlPickerScroll + i;
                    if (filtIdx >= total) break;
                    int idx = gBlFiltered[filtIdx];
                    bool sel = (filtIdx == gBlPickerSel);
                    int ry = listTop + i * PK_ROW_H;
                    if (sel) { bpSelY = ry; bpSawSel = true; }
                    FillRect(panelX+2, ry, panelW-4, PK_ROW_H-1, sel?COL_SEL:COL_BG);
                    if (sel && gTheme.cornerRadius == 0)
                        DrawRectSquare(panelX+2, ry, panelW-4, PK_ROW_H-1, COL_GOLD);
                    // Big "select" button on the left — same size as in the main belongings list
                    const int SEL_W = 18;
                    int selBoxX = panelX + 8;
                    int selBoxY = ry + (PK_ROW_H - SEL_W)/2;
                    FillRect(selBoxX, selBoxY, SEL_W, SEL_W, sel?COL_GOLD:COL_PANEL);
                    DrawRect(selBoxX, selBoxY, SEL_W, SEL_W, sel?COL_GOLD:COL_BORDER);
                    if (sel) FillRect(selBoxX+5, selBoxY+5, SEL_W-10, SEL_W-10, {10,10,10,255});
                    HitAdd(panelX+2, ry, panelW-4, PK_ROW_H-1, TBLPickSel, filtIdx);
                    int lw=0,lh=0; if(fsm) TTF_SizeUTF8(fsm, gBlPickerLabels[idx], &lw, &lh);
                    DrawText(gBlPickerLabels[idx], panelX+44, ry+(PK_ROW_H-lh)/2,
                             sel ? COL_GOLD : COL_TEXT);
                }

                // No-results hint
                if (total == 0) {
                    DrawTextC(Lang::T("bl.no.matches"),
                              panelX+panelW/2, listTop+listH/2, COL_DIM, Font::Md);
                }

                // Scrollbar
                if (total > pkVis) {
                    int sbH = listH * pkVis / total;
                    int sbY = listTop + listH * gBlPickerScroll / total;
                    FillRect(panelX+panelW-5, sbY, 3, sbH, COL_BORDER);
                }

                // Footer hint
                int footY = panelTop + panelH - PK_FOOT_H;
                FillRect(panelX+2, footY, panelW-4, PK_FOOT_H, {18,18,18,255});
                const char* foot = gBlFilter.empty()
                    ? "A  confirm    -  search    B  cancel"
                    : "A  confirm    -  edit filter    X  clear filter    B  cancel";
                DrawTextC(foot, panelX+panelW/2, footY+PK_FOOT_H/2, COL_DIM, Font::Sm);

                if (bpSawSel) {
                    RequestHighlight((int)Screen::OnSwitch, /*sub=bl picker*/34,
                                     panelX+2, bpSelY, panelW-4, PK_ROW_H-1, gDtSec);
                    DrawAnimatedHighlight();
                }
            } else {
                // ── Card-grid belongings (mirrors the WebUI layout) ──────────────
                // Sections: WORN OUTFIT (4 cloth cards × 2 rows + a wide Coord
                // card), GOODS POCKET (4 × 3), OWNERSHIP (4 action buttons).
                // gBlSel (0..24) keeps the same meaning as the old list so all
                // the input handlers (A=open picker, X=clear, ZL/ZR=mii) stay
                // identical — only the visual layout changed.
                const int HEAD_H        = 22;
                const int CARD_H        = 60;
                const int CARD_GAP      = 6;
                const int CARDS_PER_ROW = 4;
                const int SEC_GAP       = 6;
                const int ACT_H         = 42;
                const int contentX      = panelX + 8;
                const int contentW      = panelW - 16;
                const int cardW         = (contentW - (CARDS_PER_ROW-1) * CARD_GAP) / CARDS_PER_ROW;
                int yc = panelTop + 2;

                auto fitText = [&](const std::string& s, int maxW) -> std::string {
                    if (!fsm || s.empty()) return s;
                    int w=0, h=0; TTF_SizeUTF8(fsm, s.c_str(), &w, &h);
                    if (w <= maxW) return s;
                    std::string t = s;
                    while (t.size() > 1) {
                        // Drop a UTF-8 code-point off the end.
                        size_t b = t.size() - 1;
                        while (b > 0 && ((unsigned char)t[b] & 0xC0) == 0x80) b--;
                        t.erase(b);
                        std::string candidate = t + "...";
                        TTF_SizeUTF8(fsm, candidate.c_str(), &w, &h);
                        if (w <= maxW) return candidate;
                    }
                    return s;
                };

                auto drawHeader = [&](const char* title) {
                    FillRect(panelX+2, yc, panelW-4, HEAD_H, {28,28,28,255});
                    DrawRect(panelX+2, yc, panelW-4, HEAD_H, COL_BORDER);
                    DrawText(title, panelX+10, yc + (HEAD_H-14)/2 - 2, COL_GOLD, Font::Sm);
                    yc += HEAD_H + 4;
                };

                // Capture the rect of the currently-selected belongings card or
                // action button so the animated outline can glide over it.
                int blSelX = 0, blSelY = 0, blSelW = 0, blSelH = 0;
                bool blSawSel = false;
                auto drawSlotCard = [&](int sel, int x, int y, int w, int h,
                                        const char* slotLbl, const char* itemLbl,
                                        const std::string& extra, bool dimItem) {
                    bool selFlag = (sel == gBlSel);
                    if (selFlag) { blSelX = x; blSelY = y; blSelW = w; blSelH = h; blSawSel = true; }
                    FillRect(x, y, w, h, selFlag ? COL_SEL : COL_PANEL);
                    DrawRect(x, y, w, h,
                             (selFlag && gTheme.cornerRadius == 0) ? COL_GOLD : COL_BORDER);
                    // Slot prefix (dim, top-left).
                    DrawText(slotLbl, x + 8, y + 4, COL_DIM, Font::Sm);
                    // Item name — fit to card width, ellipsised if needed.
                    int innerW = w - 16;
                    int extraW = 0;
                    if (!extra.empty() && fsm) {
                        int eh=0; TTF_SizeUTF8(fsm, extra.c_str(), &extraW, &eh);
                        extraW += 8;
                    }
                    std::string nm = fitText(itemLbl, innerW - extraW);
                    DrawText(nm, x + 8, y + h/2 - 4,
                             dimItem ? COL_DIM : (selFlag ? COL_GOLD : COL_TEXT), Font::Sm);
                    if (!extra.empty()) {
                        int ew=0, eh=0; if (fsm) TTF_SizeUTF8(fsm, extra.c_str(), &ew, &eh);
                        DrawText(extra, x + w - ew - 8, y + h - eh - 4, COL_ACCENT, Font::Sm);
                    }
                    HitAdd(x, y, w, h, TBLSel, sel);
                };

                // ── WORN OUTFIT ─────────────────────────────────────────────
                drawHeader("WORN OUTFIT");
                for (int sel = 0; sel < 8; sel++) {
                    int row = sel / CARDS_PER_ROW;
                    int col = sel % CARDS_PER_ROW;
                    int cx = contentX + col * (cardW + CARD_GAP);
                    int cy = yc + row * (CARD_H + CARD_GAP);
                    const auto& ws = BL_WORN_DEFS[sel];
                    uint32_t ih = SaveEditor::GetAnyEnumAt(gMiiSav, ws.kh, miiSlotIdx, BL_H_INVALID);
                    int ci = SaveEditor::GetIntAt(gMiiSav, ws.ch, miiSlotIdx);
                    int cc = 1;
                    for (int k = 0; k < BL_CLOTH_COUNT; k++)
                        if (BL_CLOTH_ITEMS[k].nameHash == ih) { cc = BL_CLOTH_ITEMS[k].colorCount; break; }
                    bool dim = (ih == BL_H_INVALID || ih == 0);
                    const char* lbl = dim ? "(none)" : BlClothLabel(ih);
                    std::string color = dim ? "" : (std::to_string(ci+1) + "/" + std::to_string(cc));
                    drawSlotCard(sel, cx, cy, cardW, CARD_H, ws.name, lbl, color, dim);
                }
                yc += 2 * (CARD_H + CARD_GAP);
                {
                    // Coord — full-width row to stress that it overrides the
                    // individual cloth slots when set.
                    int sel = 8;
                    int cx = contentX;
                    int cy = yc;
                    int coordW = contentW;
                    uint32_t ih = SaveEditor::GetAnyEnumAt(gMiiSav, BL_COORD_KH, miiSlotIdx, BL_H_INVALID);
                    int ci = SaveEditor::GetIntAt(gMiiSav, BL_COORD_CH, miiSlotIdx);
                    int cc = 1;
                    for (int k = 0; k < BL_COORD_COUNT; k++)
                        if (BL_COORD_ITEMS[k].keyHash == ih) { cc = BL_COORD_ITEMS[k].colorCount; break; }
                    bool dim = (ih == BL_H_INVALID || ih == 0);
                    const char* lbl = dim ? "(none — using individual slots)" : BlCoordLabel(ih);
                    std::string color = dim ? "" : (std::to_string(ci+1) + "/" + std::to_string(cc));
                    drawSlotCard(sel, cx, cy, coordW, CARD_H, "Coordinate outfit", lbl, color, dim);
                }
                yc += CARD_H + SEC_GAP;

                // ── GOODS POCKET ────────────────────────────────────────────
                drawHeader("GOODS POCKET");
                for (int p = 0; p < 12; p++) {
                    int sel = 9 + p;
                    int row = p / CARDS_PER_ROW;
                    int col = p % CARDS_PER_ROW;
                    int cx = contentX + col * (cardW + CARD_GAP);
                    int cy = yc + row * (CARD_H + CARD_GAP);
                    int ai = miiSlotIdx * BL_GOODS_SLOTS_MII + p;
                    uint32_t sid = SaveEditor::GetUIntAt(gMiiSav, BL_H_GOODS_ID, ai);
                    char pfx[16]; snprintf(pfx, sizeof(pfx), "Pocket %d", p+1);
                    bool dim = (sid == 0);
                    const char* lbl = dim ? "(empty)" : BlGoodsLabel(sid);
                    drawSlotCard(sel, cx, cy, cardW, CARD_H, pfx, lbl, "", dim);
                }
                yc += 3 * (CARD_H + CARD_GAP) - CARD_GAP + SEC_GAP;

                // ── OWNERSHIP (bulk actions) ────────────────────────────────
                drawHeader("OWNERSHIP");
                {
                    const char* labels[4] = {
                        "Unlock Clothes",
                        "Lock Clothes",
                        "Unlock Coords",
                        "Lock Coords",
                    };
                    for (int a = 0; a < 4; a++) {
                        int sel = 21 + a;
                        int cx = contentX + a * (cardW + CARD_GAP);
                        int cy = yc;
                        bool selFlag = (sel == gBlSel);
                        if (selFlag) { blSelX = cx; blSelY = cy; blSelW = cardW; blSelH = ACT_H; blSawSel = true; }
                        SDL_Color border = selFlag ? COL_GOLD : (a < 2 ? COL_ACCENT : SDL_Color{180,140,90,255});
                        FillRect(cx, cy, cardW, ACT_H, selFlag ? COL_SEL : COL_PANEL);
                        DrawRect(cx, cy, cardW, ACT_H,
                                 (selFlag && gTheme.cornerRadius == 0) ? COL_GOLD : border);
                        DrawTextC(labels[a], cx + cardW/2, cy + ACT_H/2 - 2,
                                  selFlag ? COL_GOLD : border, Font::Sm);
                        HitAdd(cx, cy, cardW, ACT_H, TBLSel, sel);
                    }
                }

                if (blSawSel) {
                    RequestHighlight((int)Screen::OnSwitch, /*sub=bl cards*/35,
                                     blSelX, blSelY, blSelW, blSelH, gDtSec);
                    DrawAnimatedHighlight();
                }
            }
            if (!gMiiStatsMsg.empty())
                DrawTextC(gMiiStatsMsg, panelX+panelW/2, panelTop+panelH-22, gMiiStatsMsgCol, Font::Sm);
        } else if (gMiiStatsSubTab == MiiStatsSubTab::Habits) {
            // ── Habits panel ────────────────────────────────────────────────────
            EnsureHabitHashes();
            TTF_Font* fsm = GetFont(Font::Sm);
            (void)fsm;
            TTF_Font* fmd = GetFont(Font::Md);

            // Category strip (top row)
            const int CAT_H = 34, CAT_GAP = 4;
            int catStripY = panelTop;
            int catX0 = panelX + 8;
            int catW  = (panelW - 16 - (HABIT_CAT_COUNT-1)*CAT_GAP) / HABIT_CAT_COUNT;
            int catSelX = catX0;
            bool catSawSel = false;
            for (int c = 0; c < HABIT_CAT_COUNT; c++) {
                int cx = catX0 + c*(catW+CAT_GAP);
                bool sel = (c == gHabitCatSel);
                if (sel) { catSelX = cx; catSawSel = true; }
                // Indicator: gold dot if any habit is currently active in this category
                bool catHasActive = false;
                for (int i = 0; i < HABIT_COUNT; i++) {
                    if (HABITS[i].category != c) continue;
                    if (SaveEditor::GetBoolAt(gMiiSav, gHabitHashes[i].isChecked, miiSlotIdx, false)) {
                        catHasActive = true; break;
                    }
                }
                HitAdd(cx, catStripY, catW, CAT_H, TSelHabitCat, c);
                FillRect(cx, catStripY, catW, CAT_H, sel?COL_SEL:COL_PANEL);
                DrawRect(cx, catStripY, catW, CAT_H,
                         (sel && gTheme.cornerRadius == 0) ? COL_GOLD : COL_BORDER);
                DrawTextC(HABIT_CAT_LABEL[c], cx+catW/2, catStripY+CAT_H/2,
                          sel?COL_GOLD:(catHasActive?COL_ACCENT:COL_DIM), Font::Sm);
                if (catHasActive) {
                    int dotX = cx+catW-9, dotY = catStripY+5;
                    FillRect(dotX, dotY, 4, 4, COL_GOLD);
                }
            }

            // Item list (selected category)
            std::vector<int> items = HabitsInCategory(gHabitCatSel);
            int listTop = catStripY + CAT_H + 8;
            int listH   = panelH - (CAT_H + 8) - 36;
            const int ROW_H = 34;
            int visRows = listH / ROW_H;
            if ((int)items.size() <= visRows) gHabitScroll = 0;
            else {
                if (gHabitItemSel < gHabitScroll) gHabitScroll = gHabitItemSel;
                if (gHabitItemSel >= gHabitScroll+visRows) gHabitScroll = gHabitItemSel-visRows+1;
            }
            if (gHabitItemSel >= (int)items.size()) gHabitItemSel = std::max(0,(int)items.size()-1);

            int hbSelY = listTop;
            bool hbSawSel = false;
            for (int i = 0; i < visRows && i + gHabitScroll < (int)items.size(); i++) {
                int idx = items[i + gHabitScroll];
                bool selRow = (i + gHabitScroll == gHabitItemSel);
                int ry = listTop + i*ROW_H;
                if (selRow) { hbSelY = ry; hbSawSel = true; }
                bool isOwn     = SaveEditor::GetBoolAt(gMiiSav, gHabitHashes[idx].isOwn,     miiSlotIdx, false);
                bool isChecked = SaveEditor::GetBoolAt(gMiiSav, gHabitHashes[idx].isChecked, miiSlotIdx, false);

                // Row backdrop (no hit — the two boxes on the left are the only tap targets)
                FillRect(panelX+8, ry, panelW-16, ROW_H-2, selRow?COL_SEL:COL_BG);
                if (selRow && gTheme.cornerRadius == 0)
                    DrawRectSquare(panelX+8, ry, panelW-16, ROW_H-2, COL_GOLD);

                // Big touch zones — 50px wide, full row height — with the
                // visible 22×22 box centered. Visible row height is ROW_H-2
                // so the centering math is based on that, not ROW_H itself.
                const int VIS_H   = ROW_H - 2;
                int rcy = ry + VIS_H/2;
                const int BOX_VIS = 22;          // visible box edge
                const int HIT_W   = 50;          // touch hit width per box

                // Active (gold) box
                int aZoneX = panelX + 8;
                int aBoxX  = aZoneX + (HIT_W - BOX_VIS)/2;
                HitAdd(aZoneX, ry, HIT_W, VIS_H, TSelHabitItem, i + gHabitScroll);
                FillRect(aBoxX, rcy-BOX_VIS/2, BOX_VIS, BOX_VIS, isChecked?COL_GOLD:COL_PANEL);
                DrawRect(aBoxX, rcy-BOX_VIS/2, BOX_VIS, BOX_VIS, isChecked?COL_GOLD:COL_BORDER);
                if (isChecked) FillRect(aBoxX+6, rcy-5, 10, 10, {10,10,10,255});

                // Owned (blue) box
                const SDL_Color C_OWN = {90,140,200,255};
                int oZoneX = aZoneX + HIT_W + 4;
                int oBoxX  = oZoneX + (HIT_W - BOX_VIS)/2;
                HitAdd(oZoneX, ry, HIT_W, VIS_H, TToggleHabitOwn, i + gHabitScroll);
                FillRect(oBoxX, rcy-BOX_VIS/2, BOX_VIS, BOX_VIS, isOwn?C_OWN:COL_PANEL);
                DrawRect(oBoxX, rcy-BOX_VIS/2, BOX_VIS, BOX_VIS, isOwn?C_OWN:COL_BORDER);
                if (isOwn) FillRect(oBoxX+6, rcy-5, 10, 10, {10,10,10,255});

                // Label — Font::Sm renders into a surface a few pixels taller
                // than its cap height because of ascent/descent leading; nudge
                // up so the visual baseline sits on the row's centerline.
                SDL_Color lblCol = isChecked ? COL_GOLD : (isOwn ? COL_TEXT : COL_DIM);
                int lblW = 0, lblH = 0;
                SDL_Texture* lblTex = GetTextTexture(HABITS[idx].label, Font::Sm, lblCol, lblW, lblH);
                if (lblTex) {
                    SDL_Rect dst{ oZoneX + HIT_W + 8, rcy - lblH/2, lblW, lblH };
                    SDL_RenderCopy(gRen, lblTex, nullptr, &dst);
                }
                // Label area hit — tapping the text also toggles active (same as the gold checkbox)
                int lblHitX = oZoneX + HIT_W;
                int lblHitW = (panelX + panelW - 8) - lblHitX;
                if (lblHitW > 0)
                    HitAdd(lblHitX, ry, lblHitW, ROW_H-2, TSelHabitItem, i + gHabitScroll);
            }

            if (items.empty()) {
                DrawTextC(Lang::T("habits.empty"), panelX+panelW/2, listTop+listH/2, COL_DIM, Font::Md);
            }

            if (catSawSel) {
                RequestHighlight((int)Screen::OnSwitch, /*sub=habits cats*/37,
                                 catSelX, catStripY, catW, CAT_H, gDtSec,
                                 /*slot=*/2);
                DrawAnimatedHighlight(/*slot=*/2);
            }
            if (hbSawSel) {
                RequestHighlight((int)Screen::OnSwitch, /*sub=habits items*/36,
                                 panelX+8, hbSelY, panelW-16, ROW_H-2, gDtSec);
                DrawAnimatedHighlight();
            }

            // Scrollbar
            if ((int)items.size() > visRows) {
                int sbH = listH * visRows / (int)items.size();
                int sbY = listTop + listH * gHabitScroll / (int)items.size();
                FillRect(panelX+panelW-5, sbY, 3, sbH, COL_BORDER);
            }

            // Footer hint inside panel
            std::string hint = Lang::T("habits.hint");
            DrawTextC(hint, panelX+panelW/2, panelTop+panelH-14, COL_DIM, Font::Sm);

            (void)fmd;
            if (!gMiiStatsMsg.empty())
                DrawTextC(gMiiStatsMsg, panelX+panelW/2, panelTop+panelH-32, gMiiStatsMsgCol, Font::Sm);
        } else if (gMiiStatsSubTab == MiiStatsSubTab::Housing) {
            // ── Housing panel (card grid — touch + controller) ──────────────────
            // Layout mirrors the WebUI: sticky action bar, unhoused chip row,
            // then a 3-wide grid of house cards. Each clickable region is also
            // registered as a HousingNavItem so the D-pad selection model and
            // touch share the same activation entry-point (THousingNavAct).
            TTF_Font* fsm = GetFont(Font::Sm);
            TTF_Font* fmd = GetFont(Font::Md);
            (void)fsm; (void)fmd;
            // Lazy-load Map.sav so house names appear even if Mii.sav was loaded
            // via a path that didn't pre-load it.
            if (!gMapSav.loaded) {
                std::string mapErr;
                SaveEditor::Load(SAVE_MAP_SAV, gMapSav, mapErr);
            }
            if (!HousingAvailable()) {
                DrawTextC(Lang::T("housing.unavailable"),
                          panelX+panelW/2, panelTop+panelH/2-10, COL_DIM, Font::Md);
                DrawTextC(Lang::T("housing.unavailable.detail"),
                          panelX+panelW/2, panelTop+panelH/2+14, COL_DIM, Font::Sm);
            } else {
                // Drop the pick if the previously-picked mii vanished.
                if (gHousingPicked >= 0) {
                    bool found = false;
                    for (auto& m : gMiis) if (m.slot - 1 == gHousingPicked) { found = true; break; }
                    if (!found) HousingDoCancel();
                }

                // Group residents per-house and collect unhoused.
                struct Card {
                    int houseId = -1;
                    int maxRoom = -1;
                    // residents indexed by room — multiple if conflict.
                    std::map<int, std::vector<int>> byRoom;
                    int residentCount = 0;
                };
                std::map<int, Card> byHouse;
                std::vector<int> unhoused;
                // Seed every known house from Map.sav so vacated houses
                // remain visible in the editor (cleaned up at save time).
                for (int hid : HousingAllHouseIds()) { byHouse[hid].houseId = hid; }
                for (auto& m : gMiis) {
                    int idx = m.slot - 1;
                    int h = SaveEditor::GetIntAt(gMiiSav, HASH_HOUSE_MAPID, idx);
                    int r = SaveEditor::GetIntAt(gMiiSav, HASH_ROOM_INDEX,  idx);
                    if (h < 0) { unhoused.push_back(idx); continue; }
                    Card& c = byHouse[h];
                    c.houseId = h;
                    c.byRoom[r].push_back(idx);
                    if (r > c.maxRoom) c.maxRoom = r;
                    c.residentCount++;
                }
                std::vector<Card> cards;
                cards.reserve(byHouse.size());
                for (auto& kv : byHouse) cards.push_back(std::move(kv.second));
                std::sort(cards.begin(), cards.end(),
                          [](const Card& a, const Card& b){ return a.houseId < b.houseId; });

                // Layout constants declared up-front so the addNav lambda can
                // clip hit-areas to the visible content region.
                const int abH         = 52;
                const int contentTopY = panelTop + abH + 6;
                const int contentBotY = panelTop + panelH - 26; // footer hint reserved
                const int contentH    = contentBotY - contentTopY;
                const int contentX    = panelX + 8;
                const int contentW    = panelW - 16;

                // ── Focus-follow scroll (pre-render) ────────────────────────
                // The input handler bumped gHousingNavSel based on last frame's
                // gHousingNav. If we waited until after this frame's draw to
                // adjust the scroll, the newly-focused item would render off
                // the viewport and its outline would get clipped — which is
                // what produced the "outline disappears and reappears" flicker
                // when spamming the d-pad. Adjust scroll BEFORE rebuilding
                // gHousingNav so the focused item is always inside the clip
                // region for the upcoming draw.
                if (gHousingNavSel >= 0 && gHousingNavSel < (int)gHousingNav.size()) {
                    const auto& f = gHousingNav[gHousingNavSel];
                    if (f.y < contentTopY)       gHousingScrollY -= (contentTopY - f.y) + 10;
                    if (f.y + f.h > contentBotY) gHousingScrollY += (f.y + f.h - contentBotY) + 10;
                    if (gHousingScrollY < 0)     gHousingScrollY = 0;
                }

                gHousingNav.clear();
                auto addNav = [&](int kind, int miiSlotIdx, int houseId, int roomIdx,
                                  int abAction, int rx, int ry, int rw, int rh, bool conflict){
                    HousingNavItem it{};
                    it.kind = kind; it.miiSlotIdx = miiSlotIdx;
                    it.houseId = houseId; it.roomIdx = roomIdx;
                    it.abAction = abAction;
                    it.x = rx; it.y = ry; it.w = rw; it.h = rh;
                    it.conflict = conflict;
                    gHousingNav.push_back(it);
                    // Only register a touch hit-area for items that are
                    // actually on-screen. Action-bar items (kind=5) live
                    // above the scrolling content area so they're always
                    // visible; everything else must be inside the viewport.
                    bool visible = (kind == 5) ||
                                   (ry >= contentTopY && ry + rh <= contentBotY);
                    if (visible)
                        HitAdd(rx, ry, rw, rh, THousingNavAct, (int)gHousingNav.size() - 1);
                };

                // ── Action bar (sticky at the top of the panel) ─────────────
                FillRect(panelX, panelTop, panelW, abH, {28,28,28,255});
                DrawRect(panelX, panelTop, panelW, abH, COL_BORDER);

                std::string statusLine, subLine;
                SDL_Color statusCol;
                if (gHousingPicked >= 0) {
                    std::string nm = SaveEditor::GetWStr32At(gMiiSav, HASH_MII_NAME, gHousingPicked);
                    if (nm.empty()) nm = "(" + Lang::T("housing.slot.short") + " " + std::to_string(gHousingPicked+1) + ")";
                    statusLine = Lang::T("housing.moving") + ": " + nm;
                    subLine    = Lang::T("housing.moving.hint");
                    statusCol  = COL_GOLD;
                } else {
                    int housed = (int)gMiis.size() - (int)unhoused.size();
                    std::string sbuf = std::to_string(gMiis.size()) + " " + Lang::T("mii.miis.word") + "  ·  "
                                     + std::to_string(housed) + " " + Lang::T("housing.housed") + "  ·  "
                                     + std::to_string(unhoused.size()) + " " + Lang::T("housing.unhoused.lower") + "  ·  "
                                     + std::to_string(cards.size()) + " " + Lang::T(cards.size()==1?"housing.house":"housing.houses");
                    statusLine = Lang::T("housing.idle.hint");
                    subLine    = sbuf;
                    statusCol  = COL_TEXT;
                }
                DrawText(statusLine, panelX + 16, panelTop + 4,  statusCol, Font::Md);
                DrawText(subLine,    panelX + 16, panelTop + 28, COL_DIM,   Font::Sm);

                // Action bar buttons (right-aligned).
                int btnY = panelTop + (abH - 32) / 2;
                int btnX = panelX + panelW - 16;
                auto abButton = [&](const char* lbl, int w, SDL_Color border, SDL_Color fg, int abAction){
                    btnX -= w;
                    bool focused = (gHousingNavSel == (int)gHousingNav.size());
                    FillRect(btnX, btnY, w, 32, COL_SEL);
                    DrawRect(btnX, btnY, w, 32, focused ? COL_ACCENT : border);
                    if (focused) DrawRect(btnX+1, btnY+1, w-2, 30, COL_ACCENT);
                    DrawTextC(lbl, btnX + w/2, btnY + 16, fg, Font::Sm);
                    addNav(5, -1, -1, -1, abAction, btnX, btnY, w, 32, false);
                    btnX -= 8;
                };
                if (gHousingPicked >= 0) {
                    std::string cancelL = Lang::T("housing.cancel") + " (B)";
                    std::string evictL  = Lang::T("housing.evict") + " (-)";
                    abButton(cancelL.c_str(),   120, COL_BORDER, COL_DIM, 2);
                    abButton(evictL.c_str(),    110, {200,90,90,255}, {230,130,130,255}, 1);
                }
                // No "New House" button: houses are placed in-game and listed
                // here read-only. When nothing is picked the action bar just
                // shows status + counts.

                // ── Scrolling content area ──────────────────────────────────
                SDL_Rect clip = { contentX, contentTopY, contentW, contentH };
                SDL_RenderSetClipRect(gRen, &clip);

                int yc = contentTopY - gHousingScrollY;

                auto sectionLabel = [&](const char* lbl){
                    DrawText(lbl, contentX, yc, COL_DIM, Font::Sm);
                    yc += 22;
                };
                auto fitName = [&](int miiSlotIdx) -> std::string {
                    std::string nm = SaveEditor::GetWStr32At(gMiiSav, HASH_MII_NAME, miiSlotIdx);
                    if (nm.empty()) nm = "(" + Lang::T("housing.slot.short") + " " + std::to_string(miiSlotIdx+1) + ")";
                    return nm;
                };

                // ── Section: Unhoused chips ─────────────────────────────────
                sectionLabel(Lang::T("housing.unhoused").c_str());
                if (unhoused.empty()) {
                    DrawText(Lang::T("housing.all.housed"), contentX, yc, COL_DIM, Font::Sm);
                    yc += 24;
                } else {
                    const int chipH = 34, chipGap = 8;
                    int cx = contentX;
                    for (int idx : unhoused) {
                        std::string nm = fitName(idx);
                        int nw=0, nh=0; if (fsm) TTF_SizeUTF8(fsm, nm.c_str(), &nw, &nh);
                        int cw = std::max(80, nw + 24);
                        if (cx + cw > contentX + contentW) { cx = contentX; yc += chipH + 6; }
                        bool picked = (gHousingPicked == idx);
                        bool focused = (gHousingNavSel == (int)gHousingNav.size());
                        SDL_Color cb = picked ? COL_GOLD : (focused ? COL_ACCENT : COL_BORDER);
                        SDL_Color ct = picked ? COL_GOLD : COL_TEXT;
                        FillRect(cx, yc, cw, chipH, picked ? COL_SEL : COL_PANEL);
                        DrawRect(cx, yc, cw, chipH, cb);
                        if (focused) DrawRect(cx+1, yc+1, cw-2, chipH-2, COL_ACCENT);
                        DrawTextC(nm, cx + cw/2, yc + chipH/2 - 2, ct, Font::Sm);
                        addNav(0, idx, -1, -1, -1, cx, yc, cw, chipH, false);
                        cx += cw + chipGap;
                    }
                    yc += chipH + 6;
                }
                yc += 8;

                // ── Section: Houses (card grid) ─────────────────────────────
                sectionLabel(Lang::T("housing.houses").c_str());
                if (cards.empty()) {
                    DrawText(Lang::T("housing.no.houses"),
                             contentX, yc, COL_DIM, Font::Sm);
                    yc += 24;
                } else {
                    const int CARDS_PER_ROW = 3;
                    const int cardGap = 12;
                    const int cardW = (contentW - (CARDS_PER_ROW-1) * cardGap) / CARDS_PER_ROW;
                    const int HEAD_H = 30, SLOT_H = 30, SLOT_GAP = 4, ADD_H = 28, CARD_PAD = 10;

                    int rowStartCard = 0;
                    while (rowStartCard < (int)cards.size()) {
                        int rowEnd = std::min(rowStartCard + CARDS_PER_ROW, (int)cards.size());
                        // Compute row height: each card has rooms = max(maxRoom+1, residents).
                        int rowH = 0;
                        for (int ci = rowStartCard; ci < rowEnd; ci++) {
                            int rooms = std::max(cards[ci].maxRoom + 1, 1);
                            int h = HEAD_H + rooms*(SLOT_H+SLOT_GAP) + ADD_H + CARD_PAD*2;
                            if (h > rowH) rowH = h;
                        }
                        // Draw each card in this row.
                        for (int ci = rowStartCard; ci < rowEnd; ci++) {
                            int col = ci - rowStartCard;
                            int cxL = contentX + col * (cardW + cardGap);
                            const Card& c = cards[ci];
                            // Card background
                            FillRect(cxL, yc, cardW, rowH, COL_PANEL);
                            DrawRect(cxL, yc, cardW, rowH, COL_BORDER);

                            // Header: house name (gold) + count + vacate button.
                            FillRect(cxL+1, yc+1, cardW-2, HEAD_H, {38,38,38,255});
                            std::string hn = HouseNameForId(c.houseId);
                            std::string title = hn.empty()
                                ? (Lang::T("housing.house") + " #" + std::to_string(c.houseId))
                                : (hn + "  #" + std::to_string(c.houseId));
                            DrawText(title, cxL + 10, yc + (HEAD_H-14)/2 - 2, COL_GOLD, Font::Sm);

                            // Resident count + delete + vacate buttons (right side of head).
                            // vacate = kick residents but keep the house;
                            // delete = also remove the house from Map.sav.
                            char cntBuf[20];
                            snprintf(cntBuf, sizeof(cntBuf), "%d", c.residentCount);
                            int btnH2 = 22;
                            int vacW = 60, delW = 56;
                            int vacX = cxL + cardW - vacW - 6;
                            int delX = vacX - delW - 4;
                            int btnY = yc + (HEAD_H - btnH2)/2;
                            // Delete button
                            bool delFocused = (gHousingNavSel == (int)gHousingNav.size());
                            FillRect(delX, btnY, delW, btnH2, {26,26,26,255});
                            DrawRect(delX, btnY, delW, btnH2, delFocused ? COL_ACCENT : COL_RED);
                            if (delFocused) DrawRect(delX+1, btnY+1, delW-2, btnH2-2, COL_ACCENT);
                            DrawTextC(Lang::T("housing.delete"), delX + delW/2, btnY + btnH2/2 - 2, COL_RED, Font::Sm);
                            addNav(6, -1, c.houseId, -1, -1, delX, btnY, delW, btnH2, false);
                            // Vacate button
                            bool vacFocused = (gHousingNavSel == (int)gHousingNav.size());
                            FillRect(vacX, btnY, vacW, btnH2, {40,16,16,255});
                            DrawRect(vacX, btnY, vacW, btnH2, vacFocused ? COL_ACCENT : COL_RED);
                            if (vacFocused) DrawRect(vacX+1, btnY+1, vacW-2, btnH2-2, COL_ACCENT);
                            DrawTextC(Lang::T("housing.vacate"), vacX + vacW/2, btnY + btnH2/2 - 2, COL_RED, Font::Sm);
                            addNav(4, -1, c.houseId, -1, -1, vacX, btnY, vacW, btnH2, false);
                            int cw=0, cwh=0; if (fsm) TTF_SizeUTF8(fsm, cntBuf, &cw, &cwh);
                            DrawText(cntBuf, delX - cw - 8, yc + (HEAD_H-cwh)/2, COL_DIM, Font::Sm);

                            // Body: one row per room 0..maxRoom (or one empty row).
                            int by = yc + HEAD_H + CARD_PAD;
                            int slotXL = cxL + CARD_PAD;
                            int slotW  = cardW - CARD_PAD*2;
                            int rooms  = std::max(c.maxRoom + 1, 1);
                            for (int r = 0; r < rooms; r++) {
                                auto it = c.byRoom.find(r);
                                int slotsForRoom = (it == c.byRoom.end()) ? 0 : (int)it->second.size();
                                bool conflict = slotsForRoom > 1;
                                if (slotsForRoom == 0) {
                                    bool dropTarget = (gHousingPicked >= 0);
                                    bool focused    = (gHousingNavSel == (int)gHousingNav.size());
                                    SDL_Color slotBg = dropTarget ? SDL_Color{30,28,18,255} : COL_BG;
                                    SDL_Color slotBd = dropTarget ? COL_GOLD : COL_BORDER;
                                    if (focused) slotBd = COL_ACCENT;
                                    FillRect(slotXL, by, slotW, SLOT_H, slotBg);
                                    DrawRect(slotXL, by, slotW, SLOT_H, slotBd);
                                    std::string rbuf = Lang::T("housing.room") + " " + std::to_string(r);
                                    DrawText(rbuf, slotXL + 8, by + (SLOT_H-14)/2 - 5, COL_DIM, Font::Sm);
                                    DrawText(Lang::T(dropTarget ? "housing.drop.here" : "housing.empty.paren"),
                                             slotXL + 80, by + (SLOT_H-14)/2 - 5,
                                             dropTarget ? COL_GOLD : COL_DIM, Font::Sm);
                                    addNav(2, -1, c.houseId, r, -1, slotXL, by, slotW, SLOT_H, false);
                                    by += SLOT_H + SLOT_GAP;
                                } else {
                                    for (int miiIdx : it->second) {
                                        bool picked  = (gHousingPicked == miiIdx);
                                        bool focused = (gHousingNavSel == (int)gHousingNav.size());
                                        SDL_Color slotBg = picked ? COL_SEL :
                                                           (conflict ? SDL_Color{36,24,8,255} : COL_BG);
                                        SDL_Color slotBd = picked ? COL_GOLD :
                                                           (conflict ? COL_ACCENT : COL_BORDER);
                                        if (focused && !picked) slotBd = COL_ACCENT;
                                        FillRect(slotXL, by, slotW, SLOT_H, slotBg);
                                        DrawRect(slotXL, by, slotW, SLOT_H, slotBd);
                                        std::string rbuf = Lang::T("housing.room") + " " + std::to_string(r);
                                        DrawText(rbuf, slotXL + 8, by + (SLOT_H-14)/2 - 5, COL_DIM, Font::Sm);
                                        std::string nm = fitName(miiIdx);
                                        SDL_Color nc = picked ? COL_GOLD :
                                                       (conflict ? COL_ACCENT : COL_TEXT);
                                        DrawText(nm, slotXL + 80, by + (SLOT_H-14)/2 - 5, nc, Font::Sm);
                                        addNav(1, miiIdx, c.houseId, r, -1, slotXL, by, slotW, SLOT_H, conflict);
                                        by += SLOT_H + SLOT_GAP;
                                    }
                                }
                            }

                            // Add-resident / drop-here-next-free button at the bottom.
                            int addY = yc + rowH - ADD_H - CARD_PAD;
                            bool focusedAdd = (gHousingNavSel == (int)gHousingNav.size());
                            SDL_Color addBd = focusedAdd ? COL_ACCENT : COL_BORDER;
                            FillRect(slotXL, addY, slotW, ADD_H, {26,26,26,255});
                            DrawRect(slotXL, addY, slotW, ADD_H, addBd);
                            std::string addLbl = Lang::T((gHousingPicked >= 0) ? "housing.drop.here" : "housing.add.resident");
                            SDL_Color addCol = (gHousingPicked >= 0) ? COL_GOLD : COL_DIM;
                            DrawTextC(addLbl, slotXL + slotW/2, addY + ADD_H/2, addCol, Font::Sm);
                            addNav(3, -1, c.houseId, std::max(c.maxRoom + 1, 0), -1,
                                   slotXL, addY, slotW, ADD_H, false);
                        }
                        yc += rowH + cardGap;
                        rowStartCard = rowEnd;
                    }
                }

                SDL_RenderSetClipRect(gRen, nullptr);

                // Clamp scroll bounds.
                int contentTotalH = yc + gHousingScrollY - contentTopY + 8;
                int maxScroll = std::max(0, contentTotalH - contentH);
                if (gHousingScrollY > maxScroll) gHousingScrollY = maxScroll;
                if (gHousingScrollY < 0)         gHousingScrollY = 0;

                // Scrollbar (only if content overflows).
                if (contentTotalH > contentH && maxScroll > 0) {
                    int sbH = std::max(20, contentH * contentH / contentTotalH);
                    int sbY = contentTopY + (contentH - sbH) * gHousingScrollY / maxScroll;
                    FillRect(panelX + panelW - 5, sbY, 3, sbH, COL_BORDER);
                }

                // Clamp nav selection to current item count.
                if (gHousingNavSel >= (int)gHousingNav.size()) gHousingNavSel = (int)gHousingNav.size() - 1;
                if (gHousingNavSel < 0) gHousingNavSel = 0;
                // Focus follow now happens BEFORE the layout pass (see the
                // "Focus-follow scroll (pre-render)" block above). No need to
                // re-adjust here.

                // Sliding selection outline — uses the focused HousingNavItem's
                // rect (built fresh every frame, so it tracks scroll/layout
                // changes correctly). Clipped to the visible content area so
                // it doesn't bleed onto the sticky action bar or footer.
                if (gHousingNavSel >= 0 && gHousingNavSel < (int)gHousingNav.size()) {
                    const auto& f = gHousingNav[gHousingNavSel];
                    bool visible = (f.kind == 5) ||
                                   (f.y >= contentTopY && f.y + f.h <= contentBotY);
                    if (visible) {
                        RequestHighlight((int)Screen::OnSwitch, /*sub=mii housing*/38,
                                         f.x, f.y, f.w, f.h, gDtSec);
                        DrawAnimatedHighlight();
                    }
                }

                // Footer hint
                std::string hint = Lang::T(gHousingPicked >= 0 ? "housing.footer.picked" : "housing.footer.idle");
                DrawTextC(hint, panelX+panelW/2, panelTop+panelH-14, COL_DIM, Font::Sm);
                if (gMiiStatsMsgFrames > 0 && !gMiiStatsMsg.empty()) {
                    DrawTextC(gMiiStatsMsg, panelX+panelW/2, panelTop+panelH-30, gMiiStatsMsgCol, Font::Sm);
                }
            }
        } else if (gMiiStatsSubTab == MiiStatsSubTab::Browse) {
            // ── TomodachiShare browser ──────────────────────────────────────
            SharePump();

            // Top status / search summary
            int summaryH = 28;
            FillRect(panelX+2, panelTop, panelW-4, summaryH, {26,26,26,255});
            DrawRect(panelX+2, panelTop, panelW-4, summaryH, COL_BORDER);
            std::string hdr;
            if (!gShareQuery.empty()) hdr = Lang::T("share.search") + ": \"" + gShareQuery + "\"  ·  ";
            std::string sortKey = gShareSort==1?"share.sort.likes":(gShareSort==2?"share.sort.oldest":"share.sort.newest");
            hdr += std::to_string(gShareTotal) + " " + Lang::T("mii.miis.word") + "  ·  "
                 + Lang::T("share.page") + " " + std::to_string(gSharePage) + " / " + std::to_string(gShareLastPage)
                 + "  ·  " + Lang::T("share.sort") + ": " + Lang::T(sortKey)
                 + (gShareFromSavOnly ? ("  ·  " + Lang::T("share.importable.only")) : std::string(""));
            DrawTextC(hdr, panelX+panelW/2, panelTop+summaryH/2, COL_DIM, Font::Sm);

            const int LIST_TOP = panelTop + summaryH + 4;
            const int FOOT_H   = 26;
            const int LIST_H   = panelH - summaryH - 4 - FOOT_H;
            // 3-column card grid; rows + height adapt to panelH so the grid
            // fills the panel.
            const int COLS     = 3;
            const int CARD_GAP = 8;
            const int CARD_W   = (panelW - 16 - (COLS - 1) * CARD_GAP) / COLS;
            int VIS_ROWS = 3;
            int CARD_H   = (LIST_H - VIS_ROWS * CARD_GAP - 4) / VIS_ROWS;
            if (CARD_H < 140) { VIS_ROWS = 2; CARD_H = (LIST_H - VIS_ROWS * CARD_GAP - 4) / VIS_ROWS; }
            if (CARD_H > 210) CARD_H = 210;
            if (CARD_H <  90) CARD_H =  90;  // floor so thumb math never goes negative
            const int VIS      = COLS * VIS_ROWS;
            int N = (int)gShareMiis.size();

            // Status / loading message (replaces grid when empty).
            if (N == 0) {
                if (gShareStatusFrames > 0)
                    DrawTextC(gShareStatusMsg, panelX+panelW/2, LIST_TOP + LIST_H/2,
                              gShareStatusCol, Font::Md);
                else
                    DrawTextC(Lang::T("share.fetching"),
                              panelX+panelW/2, LIST_TOP + LIST_H/2, COL_DIM, Font::Sm);
            } else {
                if (gShareSel < 0) gShareSel = 0;
                if (gShareSel >= N) gShareSel = N - 1;
                // Scroll by ROW so the focused card is always on-screen.
                int focusedRow = gShareSel / COLS;
                int firstVisRow = gShareScroll / COLS;
                if (focusedRow < firstVisRow)              firstVisRow = focusedRow;
                if (focusedRow >= firstVisRow + VIS_ROWS)  firstVisRow = focusedRow - VIS_ROWS + 1;
                if (firstVisRow < 0) firstVisRow = 0;
                gShareScroll = firstVisRow * COLS;

                int shSelX = panelX + 8, shSelY = LIST_TOP;
                bool shSawSel = false;
                auto fitForCard = [&](const std::string& s, Font f, int maxW) -> std::string {
                    TTF_Font* fnt = GetFont(f); if (!fnt || s.empty()) return s;
                    int w=0, h=0; TTF_SizeUTF8(fnt, s.c_str(), &w, &h);
                    if (w <= maxW) return s;
                    std::string t = s; t.append("…");
                    while (!t.empty()) {
                        size_t ell = t.size() - std::string("…").size();
                        size_t b = ell;
                        if (b == 0) break;
                        b--;
                        while (b > 0 && ((unsigned char)t[b] & 0xC0) == 0x80) b--;
                        t.erase(b, ell - b);
                        TTF_SizeUTF8(fnt, t.c_str(), &w, &h);
                        if (w <= maxW) return t;
                    }
                    return s;
                };
                for (int v = 0; v < VIS && v + gShareScroll < N; v++) {
                    int idx = v + gShareScroll;
                    auto& m = gShareMiis[idx];
                    int col = v % COLS;
                    int row = v / COLS;
                    int cx = panelX + 8 + col * (CARD_W + CARD_GAP);
                    int cy = LIST_TOP    + row * (CARD_H + CARD_GAP);
                    bool sel = (idx == gShareSel);
                    if (sel) { shSelX = cx; shSelY = cy; shSawSel = true; }

                    if (sel) DrawShadow(cx, cy, CARD_W, CARD_H, gTheme.cornerRadius);
                    FillRect(cx, cy, CARD_W, CARD_H, sel ? COL_SEL : COL_PANEL);
                    DrawRect(cx, cy, CARD_W, CARD_H,
                             (sel && gTheme.cornerRadius == 0) ? COL_GOLD : COL_BORDER);
                    HitAdd  (cx, cy, CARD_W, CARD_H, TSelShareMii, idx);

                    // Fetch thumb only when its card is on-screen.
                    if (!m.thumbRequested) ShareRequestThumb(idx);

                    // Square thumb, centered. TEXT_RESERVED is the vertical
                    // budget below the thumb for name + author/likes.
                    const int TEXT_RESERVED = 56;
                    int thumbPad  = 8;
                    int thumbSize = std::min(CARD_W - 2*thumbPad, CARD_H - TEXT_RESERVED);
                    int thumbX    = cx + (CARD_W - thumbSize) / 2;
                    int thumbY    = cy + thumbPad;
                    FillRect(thumbX, thumbY, thumbSize, thumbSize, COL_BG);
                    DrawRect(thumbX, thumbY, thumbSize, thumbSize, COL_BORDER);
                    if (m.thumb) {
                        SDL_Rect dst = { thumbX, thumbY, thumbSize, thumbSize };
                        SDL_RenderCopy(gRen, m.thumb, nullptr, &dst);
                    } else {
                        DrawTextC("…", thumbX + thumbSize/2, thumbY + thumbSize/2,
                                  COL_DIM, Font::Md);
                    }

                    // Two lines under the thumb: name (centered), then
                    // "by Author" (left) + "N likes" (right) on one row.
                    int textY    = thumbY + thumbSize + 6;
                    int textMaxW = CARD_W - 12;
                    std::string nm = fitForCard(m.name.empty() ? Lang::T("share.unnamed") : m.name,
                                                Font::Md, textMaxW);
                    DrawTextC(nm, cx + CARD_W/2, textY + 9,
                              sel ? COL_GOLD : COL_TEXT, Font::Md);

                    // Right-aligned likes chip — go through the text cache
                    // so we get its width for the right-edge offset.
                    std::string likesStr = std::to_string(m.likes) + " " + Lang::T("share.likes");
                    int likesW = 0, likesH = 0;
                    SDL_Texture* likesTex = GetTextTexture(likesStr, Font::Sm, COL_ACCENT, likesW, likesH);

                    // Author fills whatever's left of the row, truncated.
                    const int padX = 8;
                    int subMaxW = CARD_W - 2*padX - likesW - 6;
                    if (subMaxW < 24) subMaxW = 24;
                    std::string sub = fitForCard(Lang::T("share.by") + " " +
                                                 (m.author.empty()?"?":m.author),
                                                 Font::Sm, subMaxW);
                    int subRowY = textY + 30;
                    int subW = 0, subH = 0;
                    SDL_Texture* subTex = GetTextTexture(sub, Font::Sm, COL_DIM, subW, subH);
                    if (subTex) {
                        SDL_Rect dst{ cx + padX, subRowY - subH/2, subW, subH };
                        SDL_RenderCopy(gRen, subTex, nullptr, &dst);
                    }
                    if (likesTex) {
                        SDL_Rect dst{ cx + CARD_W - padX - likesW, subRowY - likesH/2,
                                      likesW, likesH };
                        SDL_RenderCopy(gRen, likesTex, nullptr, &dst);
                    }
                }
                if (shSawSel) {
                    RequestHighlight((int)Screen::OnSwitch, /*sub=share browser*/41,
                                     shSelX, shSelY, CARD_W, CARD_H, gDtSec);
                    DrawAnimatedHighlight();
                }
                // Scrollbar — track total page progress, not visible-only.
                int rowsTotal = (N + COLS - 1) / COLS;
                if (rowsTotal > VIS_ROWS) {
                    int sbH = LIST_H * VIS_ROWS / rowsTotal;
                    int sbY = LIST_TOP + LIST_H * firstVisRow / rowsTotal;
                    FillRect(panelX+panelW-5, sbY, 3, sbH, COL_BORDER);
                }
            }

            // Footer hint
            std::string foot = Lang::T("share.footer");
            DrawTextC(foot, panelX+panelW/2, panelTop+panelH-FOOT_H/2-2, COL_DIM, Font::Sm);

            // Status banner (transient)
            if (gShareStatusFrames > 0 && N > 0) {
                int bH = 20;
                int bY = LIST_TOP + LIST_H - bH - 2;
                FillRect(panelX+8, bY, panelW-16, bH, {18,18,18,230});
                DrawRect(panelX+8, bY, panelW-16, bH, COL_BORDER);
                DrawTextC(gShareStatusMsg, panelX+panelW/2, bY+bH/2, gShareStatusCol, Font::Sm);
                gShareStatusFrames--;
            }

            // Detail overlay
            if (gShareDetailOpen && gShareSel >= 0 && gShareSel < N) {
                SDL_SetRenderDrawBlendMode(gRen, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(gRen, 0, 0, 0, 200);
                SDL_Rect dim = {panelX, panelTop, panelW, panelH};
                SDL_RenderFillRect(gRen, &dim);

                int dW = std::min(panelW-20, 720);
                int dH = std::min(panelH-20, 460);
                int dx = panelX + (panelW - dW)/2;
                int dy = panelTop + (panelH - dH)/2;
                FillRect(dx, dy, dW, dH, {18,14,10,255});
                DrawRect(dx, dy, dW, dH, COL_GOLD);
                FillRect(dx, dy, dW, 4, COL_GOLD);

                auto& m = gShareMiis[gShareSel];
                DrawText(m.name.empty()? Lang::T("share.unnamed") :m.name, dx + 16, dy + 14, COL_GOLD, Font::Lg);
                DrawText(Lang::T("share.by") + " " + (m.author.empty()?"?":m.author), dx + 16, dy + 44, COL_DIM, Font::Sm);

                int imgSize = std::min(dW/2 - 20, dH - 80);
                int imgX = dx + 16;
                int imgY = dy + 72;
                FillRect(imgX, imgY, imgSize, imgSize, COL_PANEL);
                DrawRect(imgX, imgY, imgSize, imgSize, COL_BORDER);
                if (gShareDetailTex) {
                    SDL_Rect dst = { imgX, imgY, imgSize, imgSize };
                    SDL_RenderCopy(gRen, gShareDetailTex, nullptr, &dst);
                } else if (!gShareDetailErr.empty()) {
                    DrawTextC(Lang::T("share.image.failed"), imgX+imgSize/2, imgY+imgSize/2, COL_DIM, Font::Sm);
                } else {
                    DrawTextC(Lang::T("loading"), imgX+imgSize/2, imgY+imgSize/2, COL_DIM, Font::Sm);
                }

                // Info on the right
                int infoX = dx + imgSize + 36;
                int infoY = imgY;
                auto kv = [&](const std::string& k, const std::string& v){
                    DrawText(k, infoX, infoY, COL_DIM, Font::Sm);
                    DrawText(v.empty()?std::string("-"):v, infoX + 80, infoY, COL_TEXT, Font::Sm);
                    infoY += 22;
                };
                kv(Lang::T("share.platform"), m.platform);
                kv(Lang::T("share.gender"),   m.gender);
                kv(Lang::T("share.likes"),    std::to_string(m.likes));
                kv(Lang::T("share.mii.id"),   "#" + std::to_string(m.id));
                infoY += 12;
                DrawText(Lang::T("share.overwrite.slot"), infoX, infoY, COL_DIM, Font::Sm);
                DrawText(std::to_string(gShareImportSlot), infoX + 110, infoY, COL_GOLD, Font::Md);
                // Show the name of the Mii currently in that slot so the user
                // knows exactly who they're about to replace.
                if (gMiiSav.loaded) {
                    std::string occName = SaveEditor::GetWStr32At(gMiiSav, HASH_MII_NAME, gShareImportSlot - 1);
                    std::string sub = occName.empty()
                        ? ("(" + Lang::T("share.slot.empty") + ")")
                        : ("(" + Lang::T("share.replaces") + " \"" + occName + "\")");
                    DrawText(sub, infoX, infoY + 22, COL_DIM, Font::Sm);
                }
                // Bottom-right stack: slot stepper → Download → Back.
                // The stepper used to sit in the info column under "overwrite
                // slot N" and crowded the text above it. Moving it directly
                // above the Download button gives it breathing room.
                {
                    int btnW = 320, btnH = 44, gap = 10;
                    int stepH = 36, stepBtnW = 44;
                    int bx   = dx + dW - btnW - 14;
                    int byB  = dy + dH - btnH - 14;             // bottom: Back
                    int byA  = byB - btnH - gap;                // above:  Download
                    int byS  = byA - stepH - 10;                // above:  ZL/ZR slot stepper

                    // ── Slot stepper row ─────────────────────────────────
                    int sX = bx;
                    int nX = bx + btnW - stepBtnW;
                    int labelX = sX + stepBtnW + 4;
                    int labelW = nX - labelX - 4;

                    FillRect(sX, byS, stepBtnW, stepH, COL_PANEL);
                    DrawRect(sX, byS, stepBtnW, stepH, COL_BORDER);
                    DrawTextC("<", sX + stepBtnW/2, byS + stepH/2 - 2, COL_TEXT, Font::Md);
                    HitAdd(sX, byS, stepBtnW, stepH, TShareImportSlotPrev, 0);

                    FillRect(labelX, byS, labelW, stepH, {28,28,28,255});
                    DrawRect(labelX, byS, labelW, stepH, COL_BORDER);
                    std::string slotLabel = Lang::T("share.slot") + " " + std::to_string(gShareImportSlot);
                    if (gMiiSav.loaded) {
                        std::string occ = SaveEditor::GetWStr32At(gMiiSav, HASH_MII_NAME, gShareImportSlot - 1);
                        if (occ.empty()) slotLabel += "  ·  " + Lang::T("empty.paren");
                        else             slotLabel += "  ·  " + occ;
                    }
                    DrawTextC(slotLabel, labelX + labelW/2, byS + stepH/2 - 2, COL_GOLD, Font::Sm);

                    FillRect(nX, byS, stepBtnW, stepH, COL_PANEL);
                    DrawRect(nX, byS, stepBtnW, stepH, COL_BORDER);
                    DrawTextC(">", nX + stepBtnW/2, byS + stepH/2 - 2, COL_TEXT, Font::Md);
                    HitAdd(nX, byS, stepBtnW, stepH, TShareImportSlotNext, 0);

                    // ── Download & overwrite (A) ─────────────────────────
                    bool canImport = gMiiSav.loaded && gShareSel >= 0 && gShareSel < (int)gShareMiis.size();
                    SDL_Color dlBg = canImport ? COL_GOLD   : COL_PANEL;
                    SDL_Color dlFg = canImport ? COL_BG     : COL_DIM;
                    FillRect(bx, byA, btnW, btnH, dlBg);
                    DrawRect(bx, byA, btnW, btnH, COL_GOLD);
                    DrawTextC(Lang::T("share.download.overwrite") + " (A)", bx + btnW/2, byA + btnH/2 - 2, dlFg, Font::Md);
                    if (canImport) HitAdd(bx, byA, btnW, btnH, TShareDoImport, 0);

                    // ── Back (B) ────────────────────────────────────────
                    FillRect(bx, byB, btnW, btnH, COL_PANEL);
                    DrawRect(bx, byB, btnW, btnH, COL_BORDER);
                    DrawTextC(Lang::T("back") + " (B)", bx + btnW/2, byB + btnH/2 - 2, COL_TEXT, Font::Md);
                    HitAdd(bx, byB, btnW, btnH, TShareCloseDetail, 0);
                }

                if (gShareStatusFrames > 0) {
                    int bH = 22;
                    int bY = dy + dH - bH - 8;
                    FillRect(dx + 10, bY, dW - 20, bH, {18,18,18,255});
                    DrawRect(dx + 10, bY, dW - 20, bH, COL_BORDER);
                    DrawTextC(gShareStatusMsg, dx + dW/2, bY + bH/2, gShareStatusCol, Font::Sm);
                }
            }
        } else {
            // Stats editor
            const auto& fd = MII_STATS_FIELDS[gMiiStatsFieldSel];
            int cx2 = editX + editW/2;
            int midY2 = panelTop + panelH/2 - 20;

            DrawTextC(MiiLabelT(fd.label), cx2, midY2-102 - (fd.isStr ? 30 : 0), COL_DIM, Font::Md);
            DrawTextC(MiiDescT(fd.label),  cx2, midY2-76 - (fd.isStr ? 30 : 0), COL_DIM, Font::Sm);

            if (fd.isStr) {
                std::string val = SaveEditor::GetWStr32At(gMiiSav, SaveEditor::Hash(fd.fieldName), miiSlotIdx);
                DrawTextC(val.empty()? Lang::T("empty") :val, cx2, midY2-70, COL_TEXT, Font::Lg);
                int bw=220, bh=46, bx=cx2-bw/2, by=midY2-20;
                HitAdd(bx,by,bw,bh, TMiiStatsKbd, 0);
                FillRect(bx,by,bw,bh,COL_SEL); DrawRect(bx,by,bw,bh,COL_ACCENT);
                DrawTextC(Lang::T("keyboard"), cx2, by+bh/2, COL_ACCENT, Font::Md);
                {
                    static const uint32_t H_HOW_NAME = SaveEditor::Hash("Mii.Name.HowToCallName");
                    std::string how = SaveEditor::GetWStr64At(gMiiSav, H_HOW_NAME, miiSlotIdx);
                    DrawTextC(how.empty()? Lang::T("mii.same.as.name") :how, cx2, by+bh+16, how.empty()?COL_DIM:COL_TEXT, Font::Sm);
                    int pw=220,phh=42,px=cx2-pw/2,py=by+bh+32;
                    HitAdd(px,py,pw,phh,TSimBtn,(int)HidNpadButton_X);
                    FillRect(px,py,pw,phh,COL_SEL); DrawRect(px,py,pw,phh,COL_ACCENT);
                    DrawTextC("X  " + Lang::T("player.edit.pron"),cx2,py+phh/2,COL_ACCENT,Font::Sm);
                }
            } else if (fd.isEnum) {
                uint32_t enumVal = SaveEditor::GetEnumAt(gMiiSav, SaveEditor::Hash(fd.fieldName), miiSlotIdx);
                const char* lbl = FeelingName(enumVal);
                DrawTextC(lbl ? lbl : ("?" + std::to_string(enumVal)).c_str(), cx2, midY2-40, COL_TEXT, Font::Lg);
                const int btnW=140, btnH=40, btnGap=24;
                int bxL=cx2-btnGap/2-btnW, bxR=cx2+btnGap/2, btnY=midY2+10;
                HitAdd(bxL,btnY,btnW,btnH, TSimBtn, (int)HidNpadButton_Left);
                FillRect(bxL,btnY,btnW,btnH,COL_PANEL); DrawRect(bxL,btnY,btnW,btnH,COL_BORDER);
                DrawTextC("< " + Lang::T("prev"), bxL+btnW/2, btnY+btnH/2, COL_DIM);
                HitAdd(bxR,btnY,btnW,btnH, TSimBtn, (int)HidNpadButton_Right);
                FillRect(bxR,btnY,btnW,btnH,COL_PANEL); DrawRect(bxR,btnY,btnW,btnH,COL_BORDER);
                DrawTextC(Lang::T("next") + " >", bxR+btnW/2, btnY+btnH/2, COL_DIM);
                DrawTextC(Lang::T("mii.cycle.mood"), cx2, btnY+btnH+14, COL_DIM);
                EmitPrevNextHighlight(/*sub=mii stats mood*/120, bxL, bxR, btnY, btnW, btnH);
                EmitPrevNextHighlight(/*sub=mii mood*/120, bxL, bxR, btnY, btnW, btnH);
            } else {
                int32_t rawVal = fd.isUInt ? (int32_t)SaveEditor::GetUIntAt(gMiiSav,SaveEditor::Hash(fd.fieldName),miiSlotIdx)
                                           : SaveEditor::GetIntAt(gMiiSav,SaveEditor::Hash(fd.fieldName),miiSlotIdx);
                DrawTextC(std::to_string(rawVal + fd.dispOffset), cx2, midY2-40, COL_TEXT, Font::Lg);

                static const int STEPS2[]={100,10,1};
                int bw2=60, bh2=64, gap2=6;
                int totalBW2=(3*(bw2+gap2))*2+16;
                int bx2=cx2-totalBW2/2, by2=midY2+10;
                int btn2Idx=0;
                int focBtnX = bx2, focBtnY = by2; bool gotFocBtn = false;
                for (int s : STEPS2) {
                    bool focused=(gMiiBtnSel==btn2Idx);
                    if (focused) { focBtnX = bx2; focBtnY = by2; gotFocBtn = true; }
                    HitAdd(bx2,by2,bw2,bh2, TAdjust, -s);
                    FillRect(bx2,by2,bw2,bh2, focused?COL_SEL:COL_PANEL);
                    DrawRect(bx2,by2,bw2,bh2,
                             (focused && gTheme.cornerRadius == 0) ? COL_GOLD : COL_RED);
                    DrawTextC("-"+std::to_string(s), bx2+bw2/2, by2+bh2/2, focused?COL_GOLD:COL_RED, Font::Md);
                    bx2+=bw2+gap2; btn2Idx++;
                }
                bx2+=16;
                for (int j=2;j>=0;j--) {
                    bool focused=(gMiiBtnSel==btn2Idx);
                    if (focused) { focBtnX = bx2; focBtnY = by2; gotFocBtn = true; }
                    int s=STEPS2[j];
                    HitAdd(bx2,by2,bw2,bh2, TAdjust, s);
                    FillRect(bx2,by2,bw2,bh2, focused?COL_SEL:COL_PANEL);
                    DrawRect(bx2,by2,bw2,bh2,
                             (focused && gTheme.cornerRadius == 0) ? COL_GOLD : COL_GREEN);
                    DrawTextC("+"+std::to_string(s), bx2+bw2/2, by2+bh2/2, focused?COL_GOLD:COL_GREEN, Font::Md);
                    bx2+=bw2+gap2; btn2Idx++;
                }
                if (gotFocBtn) {
                    RequestHighlight((int)Screen::OnSwitch, /*sub=mii stat steppers*/39,
                                     focBtnX, focBtnY, bw2, bh2, gDtSec,
                                     /*slot=*/3);
                    DrawAnimatedHighlight(/*slot=*/3);
                }
                // Undo button
                {
                    bool canUndo = gMiiUndoValid[gMiiStatsFieldSel];
                    int ubW=100, ubH=44, ubX=cx2-ubW/2, ubY=by2+bh2+20;
                    HitAdd(ubX, ubY, ubW, ubH, TUndoMii, 0);
                    FillRect(ubX, ubY, ubW, ubH, COL_PANEL);
                    DrawRect(ubX, ubY, ubW, ubH, canUndo ? COL_GOLD : COL_BORDER);
                    DrawTextC(Lang::T("undo"), ubX+ubW/2, ubY+ubH/2, canUndo ? COL_GOLD : COL_DIM, Font::Md);
                }
            }

            // Import / Export .ltd section — only shown on Name field (str)
            if (fd.isStr && !gMiis.empty() && gMiiStatsMiiSel < (int)gMiis.size()) {
                // Anchor to panel bottom so it stays fixed regardless of upper block position
                int ibW=220,ibH=66,ibGap=14;
                int ibSqr=ibH; // square upload-icon button matches row height
                // Layout: [import .ltd] [export .ltd] [upload-icon (download from share)]
                int totalW = ibW + ibGap + ibW + ibGap + ibSqr;
                int ibxL=cx2 - totalW/2;
                int ibxR=ibxL + ibW + ibGap;
                int ibxS=ibxR + ibW + ibGap;
                int ibY = panelTop + panelH - ibH - 20;
                SDL_SetRenderDrawColor(gRen,COL_BORDER.r,COL_BORDER.g,COL_BORDER.b,80);
                SDL_RenderDrawLine(gRen, editX+12, ibY-44, editX+editW-12, ibY-44);
                DrawTextC(Lang::T("mii.file"), cx2, ibY-22, COL_DIM, Font::Sm);
                bool ltdImp = (gMiiLtdBtnSel == 0);
                bool ltdExp = (gMiiLtdBtnSel == 1);
                bool ltdShr = (gMiiLtdBtnSel == 2);
                int ltdSelX = ibxL, ltdSelY = ibY, ltdSelW = ibW;
                bool ltdSawSel = false;
                if (ltdImp) { ltdSelX = ibxL; ltdSelW = ibW;   ltdSawSel = true; }
                if (ltdExp) { ltdSelX = ibxR; ltdSelW = ibW;   ltdSawSel = true; }
                if (ltdShr) { ltdSelX = ibxS; ltdSelW = ibSqr; ltdSawSel = true; }
                HitAdd(ibxL,ibY,ibW,ibH,TOpenImportBrowser,0);
                FillRect(ibxL,ibY,ibW,ibH,ltdImp?COL_SEL:COL_PANEL);
                DrawRect(ibxL,ibY,ibW,ibH,COL_ACCENT);
                DrawTextC(Lang::T("mii.import.ltd"),ibxL+ibW/2,ibY+ibH/2,COL_ACCENT,Font::Lg);
                HitAdd(ibxR,ibY,ibW,ibH,TDoMiiExport,0);
                FillRect(ibxR,ibY,ibW,ibH,ltdExp?COL_SEL:COL_PANEL);
                DrawRect(ibxR,ibY,ibW,ibH,COL_GOLD);
                DrawTextC(Lang::T("mii.export.ltd"),ibxR+ibW/2,ibY+ibH/2,COL_GOLD,Font::Lg);
                // Square upload-icon button — opens the TomodachiShare browser
                // so the user can overwrite the selected slot with a downloaded Mii.
                // Icon-only: the arrow glyph is sized to fill the box; no text label.
                static const SDL_Color C_SHARE = {120,170,220,255};
                HitAdd(ibxS,ibY,ibSqr,ibH,TOpenMiiShareBrowser,0);
                FillRect(ibxS,ibY,ibSqr,ibH,ltdShr?COL_SEL:COL_PANEL);
                DrawRect(ibxS,ibY,ibSqr,ibH,C_SHARE);
                {
                    int gcx = ibxS + ibSqr/2;
                    int gcy = ibY + ibH/2;
                    int gW  = ibSqr * 7 / 12;   // ~58% of the box
                    int gH  = ibSqr * 7 / 12;
                    SDL_SetRenderDrawColor(gRen,C_SHARE.r,C_SHARE.g,C_SHARE.b,255);
                    // Tray (open box at the bottom)
                    for (int t = 0; t < 2; t++) {
                        SDL_RenderDrawLine(gRen, gcx-gW/2,   gcy+gH/2-4+t, gcx-gW/2,   gcy+gH/2+t);
                        SDL_RenderDrawLine(gRen, gcx+gW/2-1, gcy+gH/2-4+t, gcx+gW/2-1, gcy+gH/2+t);
                        SDL_RenderDrawLine(gRen, gcx-gW/2,   gcy+gH/2+t,   gcx+gW/2-1, gcy+gH/2+t);
                    }
                    // Upward arrow shaft (3px wide)
                    for (int t = -1; t <= 1; t++) {
                        SDL_RenderDrawLine(gRen, gcx+t, gcy-gH/2+2, gcx+t, gcy+gH/2-6);
                    }
                    // Arrow head (thicker than before)
                    for (int t = 0; t < 2; t++) {
                        SDL_RenderDrawLine(gRen, gcx,   gcy-gH/2+1+t, gcx-gW/3, gcy-gH/2+1+gW/3+t);
                        SDL_RenderDrawLine(gRen, gcx,   gcy-gH/2+1+t, gcx+gW/3, gcy-gH/2+1+gW/3+t);
                    }
                }
                if (ltdSawSel) {
                    RequestHighlight((int)Screen::OnSwitch, /*sub=mii ltd buttons*/41,
                                     ltdSelX, ibY, ltdSelW, ibH, gDtSec,
                                     /*slot=*/3);
                    DrawAnimatedHighlight(/*slot=*/3);
                }
            }

            if (!gMiiStatsMsg.empty())
                DrawTextC(gMiiStatsMsg, cx2, panelTop+16, gMiiStatsMsgCol);
        }
    }

    DrawFooter(
        gMiiStatsSubTab==MiiStatsSubTab::Social
            ? Lang::T(gSocialExpanded ? "mii.footer.social.global" : "mii.footer.social.focus")
        : gMiiStatsSubTab==MiiStatsSubTab::Words
            ? Lang::T("mii.footer.words")
        : gMiiStatsSubTab==MiiStatsSubTab::Relations
            ? Lang::T("mii.footer.relations")
        : gMiiStatsSubTab==MiiStatsSubTab::Belongings
            ? Lang::T(gBlPickerOpen ? "mii.footer.bl.picker" : "mii.footer.bl")
        : gMiiStatsSubTab==MiiStatsSubTab::Housing
            ? Lang::T(gHousingPicked >= 0 ? "mii.footer.housing.picked" : "mii.footer.housing.idle")
        : gMiiStatsSubTab==MiiStatsSubTab::Browse
            ? Lang::T(gShareDetailOpen ? "mii.footer.browse.detail" : "mii.footer.browse")
        : Lang::T(gMiiStatsFieldSel==0 ? "mii.footer.stats.name" : "mii.footer.stats.value"));
    Present();
}

static void DrawAppletWarning() {
    HitClear();
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);

    const int mW = 740, mH = 420;
    int mx = (SCREEN_W - mW) / 2, my = (SCREEN_H - mH) / 2;
    FillRect(mx, my, mW, mH, {18, 14, 10, 255});
    DrawRect(mx, my, mW, mH, COL_GOLD);
    FillRect(mx, my, mW, 4, COL_GOLD);

    DrawTextC(Lang::T("applet.title"), SCREEN_W/2, my + 36, COL_GOLD, Font::Lg);

    SDL_SetRenderDrawColor(gRen, COL_BORDER.r, COL_BORDER.g, COL_BORDER.b, 100);
    SDL_RenderDrawLine(gRen, mx + 24, my + 60, mx + mW - 24, my + 60);

    DrawTextC(Lang::T("applet.body.l1"),
              SCREEN_W/2, my + 84, COL_TEXT, Font::Sm);
    DrawTextC(Lang::T("applet.body.l2"),
              SCREEN_W/2, my + 106, COL_TEXT, Font::Sm);

    SDL_SetRenderDrawColor(gRen, COL_BORDER.r, COL_BORDER.g, COL_BORDER.b, 100);
    SDL_RenderDrawLine(gRen, mx + 24, my + 130, mx + mW - 24, my + 130);

    DrawTextC(Lang::T("applet.how"), SCREEN_W/2, my + 154, COL_ACCENT, Font::Md);

    // Step boxes
    const int sW = 320, sH = 72, sGap = 20;
    int sxL = SCREEN_W/2 - sGap/2 - sW;
    int sxR = SCREEN_W/2 + sGap/2;
    int sY  = my + 182;

    FillRect(sxL, sY, sW, sH, {20, 28, 20, 255});
    DrawRect(sxL, sY, sW, sH, COL_RED);
    DrawTextC(Lang::T("applet.wrong"), sxL + sW/2, sY + 18, COL_RED, Font::Sm);
    DrawTextC(Lang::T("applet.wrong.hint"), sxL + sW/2, sY + 44, COL_DIM, Font::Sm);

    FillRect(sxR, sY, sW, sH, {16, 28, 16, 255});
    DrawRect(sxR, sY, sW, sH, COL_GREEN);
    DrawTextC(Lang::T("applet.right"), sxR + sW/2, sY + 18, COL_GREEN, Font::Sm);
    DrawTextC(Lang::T("applet.right.hint"), sxR + sW/2, sY + 44, COL_TEXT, Font::Sm);

    DrawTextC(Lang::T("applet.note"),
              SCREEN_W/2, sY + sH + 26, COL_DIM, Font::Sm);

    const int btnW = 180, btnH = 44;
    int bx = SCREEN_W/2 - btnW/2, by = my + mH - 66;
    FillRect(bx, by, btnW, btnH, COL_PANEL);
    DrawRect(bx, by, btnW, btnH, COL_BORDER);
    DrawTextC(Lang::T("applet.exit"), SCREEN_W/2, by + btnH/2, COL_DIM, Font::Md);

    DrawFooter(Lang::T("applet.footer"));
    Present();
}

static void DrawSaveWarning() {
    HitClear();
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);
    const int mW = 620, mH = 230;
    int mx = (SCREEN_W - mW) / 2, my = (SCREEN_H - mH) / 2;
    FillRect(mx, my, mW, mH, {30, 18, 18, 255});
    DrawRect(mx, my, mW, mH, COL_RED);
    DrawTextC(Lang::T("warn.title"), SCREEN_W/2, my+28, COL_RED, Font::Lg);
    DrawTextC(Lang::T("warn.body.l1"), SCREEN_W/2, my+84, COL_TEXT, Font::Md);
    DrawTextC(Lang::T("warn.body.l2"), SCREEN_W/2, my+114, COL_DIM, Font::Sm);
    const int btnW = 200, btnH = 44;
    int bx = SCREEN_W/2 - btnW/2, by = my + mH - 60;
    bool wReady = (gSaveWarningCountdown <= 0);
    if (wReady) HitAdd(bx, by, btnW, btnH, TAckSaveWarning, 0);
    FillRect(bx, by, btnW, btnH, COL_SEL);
    DrawRect(bx, by, btnW, btnH, wReady ? COL_ACCENT : COL_BORDER);
    int wSecs = (gSaveWarningCountdown + 59) / 60;
    std::string wLabel = wReady ? Lang::T("warn.understand")
                                : (Lang::T("warn.understand") + "  (" + std::to_string(wSecs) + ")");
    DrawTextC(wLabel, SCREEN_W/2, by + btnH/2, wReady ? COL_ACCENT : COL_DIM, Font::Md);
    DrawFooter(wReady ? "A  " + Lang::T("warn.understand") : std::string(""));
    Present();
}

static void TAckBackYes(int) {
    gShowBackPrompt = false;
    if (gBackPromptIsMii && gMiiSavDirty) {
        std::string err = SaveEditor::Save(SAVE_MII_SAV, gMiiSav);
        if (err.empty()) { SaveMount::Commit(); gMiiSavDirty=false; }
    }
    if (!gBackPromptIsMii && gPlayerSavDirty) {
        std::string err = SaveEditor::Save(SAVE_PLAYER_SAV, gPlayerSav);
        if (err.empty()) { SaveMount::Commit(); gPlayerSavDirty=false; }
    }
    // Map.sav is independent of which tab triggered the prompt — commit it
    // whenever it's dirty. Before saving, prune any house that's now empty
    // (vacated by the user and not re-populated) so the game doesn't keep
    // residentless house actors around on the island.
    HousingCleanupEmptyHouses();
    if (gMapDirty) {
        std::string err = SaveEditor::Save(SAVE_MAP_SAV, gMapSav);
        if (err.empty()) { SaveMount::Commit(); gMapDirty=false; }
    }
    if (gUgcDirty) { SaveMount::Commit(); gUgcDirty=false; }
    if (gWebUiDirty) { SaveMount::Commit(); gWebUiDirty=false; }
    FreePreview(); HttpServer::Stop(); gLog.clear();
    gEntries.clear(); gMiis.clear();
    gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{}; gMapSav=SaveEditor::SavFile{};
    gPlayerSavDirty=false; gMiiSavDirty=false; gMapDirty=false; gUgcDirty=false; gWebUiDirty=false;
    memset(gPlayerUndoValid, 0, sizeof(gPlayerUndoValid));
    gNameLangUndoValid=false; gIslandLangUndoValid=false;
    memset(gMiiUndoValid,    0, sizeof(gMiiUndoValid));
    gWordUndo.valid = false;
    SaveMount::Unmount();
    gSaveFeedbackFrames=90; gSaveFeedbackQuit=!gBackPromptToUserPick;
    gScreen=Screen::SaveFeedback;
}
static void TAckBackNo(int) {
    gShowBackPrompt = false;
    FreePreview(); HttpServer::Stop(); gLog.clear();
    gEntries.clear(); gMiis.clear();
    gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{}; gMapSav=SaveEditor::SavFile{};
    gPlayerSavDirty=false; gMiiSavDirty=false; gMapDirty=false; gUgcDirty=false; gWebUiDirty=false;
    memset(gPlayerUndoValid, 0, sizeof(gPlayerUndoValid));
    gNameLangUndoValid=false; gIslandLangUndoValid=false;
    memset(gMiiUndoValid,    0, sizeof(gMiiUndoValid));
    gWordUndo.valid = false;
    SaveMount::Unmount();
    if (gBackPromptToUserPick) gScreen = Screen::UserPick;
    else gQuitApp = true;
}
static void TDismissThumbTip(int) {
    if (gThumbTipCountdown > 0) return;
    gShowThumbTip = false;
    gThumbTipSeen = true;
    SaveConfig();
}
static void DrawThumbTip() {
    HitClear();
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);

    const int mW = 820, mH = 310;
    int mx = (SCREEN_W - mW) / 2, my = (SCREEN_H - mH) / 2;
    FillRect(mx, my, mW, mH, {18, 22, 18, 255});
    DrawRect(mx, my, mW, mH, COL_GOLD);

    DrawTextC(Lang::T("thumbtip.title"), SCREEN_W/2, my + 32, COL_GOLD, Font::Lg);

    DrawTextC(Lang::T("thumbtip.l1"), SCREEN_W/2, my + 88,  COL_TEXT, Font::Md);
    DrawTextC(Lang::T("thumbtip.l2"), SCREEN_W/2, my + 114, COL_TEXT, Font::Md);
    DrawTextC(Lang::T("thumbtip.l3"), SCREEN_W/2, my + 152, COL_DIM,  Font::Md);
    DrawTextC(Lang::T("thumbtip.l4"), SCREEN_W/2, my + 176, COL_DIM,  Font::Md);
    DrawTextC(Lang::T("thumbtip.l5"), SCREEN_W/2, my + 200, COL_DIM,  Font::Md);

    const int btnW = 220, btnH = 44;
    int bx = SCREEN_W/2 - btnW/2, by = my + mH - 62;
    bool ready = (gThumbTipCountdown <= 0);
    SDL_Color btnBorder = ready ? COL_ACCENT : COL_BORDER;
    SDL_Color btnText   = ready ? COL_ACCENT : COL_DIM;
    if (ready) HitAdd(bx, by, btnW, btnH, TDismissThumbTip, 0);
    FillRect(bx, by, btnW, btnH, COL_PANEL);
    DrawRect(bx, by, btnW, btnH, btnBorder);
    int secs = (gThumbTipCountdown + 59) / 60;
    std::string label = ready ? Lang::T("thumbtip.gotit")
                              : (Lang::T("thumbtip.gotit") + "  (" + std::to_string(secs) + ")");
    DrawTextC(label, SCREEN_W/2, by + btnH/2, btnText, Font::Md);
    Present();
}

// ── Background-removal modal ───────────────────────────────────────────────

static void TConfirmBgRemove(int);
static void TCancelBgRemove(int) { gShowBgRemovePrompt = false; }

static void DrawBgRemovePrompt() {
    HitClear();
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);
    const int mW = 580, mH = 230;
    int mx = (SCREEN_W - mW) / 2, my = (SCREEN_H - mH) / 2;
    FillRect(mx, my, mW, mH, {18, 22, 30, 255});
    DrawRect(mx, my, mW, mH, COL_ACCENT);
    DrawTextC(Lang::T("bg.title"), SCREEN_W/2, my + 36, COL_ACCENT, Font::Lg);
    DrawTextC(Lang::T("bg.body.l1"), SCREEN_W/2, my + 88, COL_TEXT, Font::Md);
    DrawTextC(Lang::T("bg.body.l2"), SCREEN_W/2, my + 114, COL_DIM, Font::Sm);
    const int bW = 180, bH = 46, gap = 28;
    int by2 = my + mH - 62;
    HitAdd(mx + mW/2 - bW - gap/2, by2, bW, bH, TConfirmBgRemove, 0);
    FillRect(mx + mW/2 - bW - gap/2, by2, bW, bH, COL_SEL);
    DrawRect(mx + mW/2 - bW - gap/2, by2, bW, bH, COL_ACCENT);
    DrawTextC("A  " + Lang::T("confirm"), mx + mW/2 - gap/2 - bW/2, by2 + bH/2, COL_ACCENT, Font::Md);
    HitAdd(mx + mW/2 + gap/2, by2, bW, bH, TCancelBgRemove, 0);
    FillRect(mx + mW/2 + gap/2, by2, bW, bH, COL_SEL);
    DrawRect(mx + mW/2 + gap/2, by2, bW, bH, {90,140,200,255});
    DrawTextC("B  " + Lang::T("cancel"), mx + mW/2 + gap/2 + bW/2, by2 + bH/2, {90,140,200,255}, Font::Md);
    DrawFooter("A  " + Lang::T("confirm") + "    B  " + Lang::T("cancel"));
    Present();
}

static void DrawBgRemoving() {
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawTextC(Lang::T("bg.removing"), SCREEN_W/2, SCREEN_H/2 - 18, COL_ACCENT, Font::Lg);
    DrawTextC(Lang::T("bg.body.l2"), SCREEN_W/2, SCREEN_H/2 + 22, COL_DIM, Font::Md);
    Present();
}

struct BgAnimState { int frame = 0; };

static void OnBgRemoveProgress(int done, int total, void* ud) {
    static const char kSpinner[] = "|/-\\";
    auto* s = static_cast<BgAnimState*>(ud);
    s->frame++;
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawTextC(Lang::T("bg.removing"), SCREEN_W/2, SCREEN_H/2 - 60, COL_ACCENT, Font::Lg);
    // Spinner
    char spin[2] = {kSpinner[s->frame % 4], '\0'};
    DrawTextC(spin, SCREEN_W/2, SCREEN_H/2 - 10, {90,140,200,255}, Font::Lg);
    // Progress bar
    const int barW = 480, barH = 12;
    int barX = (SCREEN_W - barW) / 2, barY = SCREEN_H/2 + 20;
    float pct = (total > 0) ? (float)done / total : 0.f;
    FillRect(barX, barY, barW, barH, COL_PANEL);
    DrawRect(barX, barY, barW, barH, COL_BORDER);
    if (pct > 0.f) FillRect(barX+1, barY+1, (int)((barW-2)*pct), barH-2, {90,140,200,255});
    DrawTextC(Lang::T("bg.body.l2"), SCREEN_W/2, SCREEN_H/2 + 52, COL_DIM, Font::Md);
    Present();
}

static void TConfirmBgRemove(int) {
    gShowBgRemovePrompt = false;
    if (gEntries.empty()) return;
    const auto& e = gEntries[gEntrySel];

    // Show loading screen before blocking inference
    DrawBgRemoving();

    // Decode current texture to sRGB
    RgbaImage img;
    std::string err = TextureProcessor::DecodeFile(e.ugctexPath, img);
    if (!err.empty()) { gOnSwitchMsg = err; gOnSwitchMsgCol = COL_RED; return; }

    // Run u2netp inference with animated progress
    BgAnimState bgAnim{};
    err = U2Net::RemoveBackground(img, kBgModelPath, OnBgRemoveProgress, &bgAnim);
    if (!err.empty()) { gOnSwitchMsg = "BG remove: " + err; gOnSwitchMsgCol = COL_RED; return; }

    // Re-encode and overwrite files on disk
    TextureProcessor::ImportOptions opts;
    opts.destStem           = e.directory() + "/" + e.stem;
    opts.writeCanvas        = e.hasCanvas();
    opts.writeThumb         = e.hasThumb();
    opts.thumbPath          = e.thumbPath;
    opts.noSrgb             = gPreviewNoSrgb;
    opts.originalUgctexPath = e.ugctexPath;
    err = TextureProcessor::ImportRgbaImage(img, opts);
    if (!err.empty()) { gOnSwitchMsg = err; gOnSwitchMsgCol = COL_RED; return; }

    // Reload preview and mark dirty
    gEntries = UgcScanner::Scan(SAVE_UGC_PATH);
    MiiManager::LoadUgcNames();
    RebuildUgcFilter();
    if (gEntrySel >= (int)gEntries.size()) gEntrySel = (int)gEntries.size() - 1;
    if (!gEntries.empty()) LoadPreview(gEntries[gEntrySel]);
    gUgcDirty = true;
    gOnSwitchMsg = "Background removed — save or discard on exit";
    gOnSwitchMsgCol = COL_GREEN;
}

static void DrawBackPrompt() {
    HitClear();
    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
    SDL_RenderClear(gRen);
    const int mW=540, mH=200;
    int mx=(SCREEN_W-mW)/2, my=(SCREEN_H-mH)/2;
    FillRect(mx, my, mW, mH, {20,24,20,255});
    DrawRect(mx, my, mW, mH, COL_GOLD);
    DrawTextC(Lang::T("backprompt.title"), SCREEN_W/2, my+28, COL_GOLD, Font::Lg);
    DrawTextC(Lang::T("backprompt.body"), SCREEN_W/2, my+70, COL_DIM, Font::Sm);
    const int bW=180, bH=48, gap=30, bOff=90;
    std::string yesS = "A  " + Lang::T(gBackPromptToUserPick ? "backprompt.save.back" : "backprompt.save.exit");
    std::string noS  = "B  " + Lang::T(gBackPromptToUserPick ? "backprompt.discard.back" : "backprompt.discard.exit");
    std::string footerS = gBackPromptToUserPick
        ? Lang::T("backprompt.footer.back")
        : Lang::T("backprompt.footer.exit");
    const char* yesLbl = yesS.c_str();
    const char* noLbl  = noS.c_str();
    const char* footer = footerS.c_str();
    HitAdd(mx+mW/2-bW-gap/2, my+mH-bOff, bW, bH, TAckBackYes, 0);
    FillRect(mx+mW/2-bW-gap/2, my+mH-bOff, bW, bH, COL_SEL);
    DrawRect(mx+mW/2-bW-gap/2, my+mH-bOff, bW, bH, COL_GREEN);
    DrawTextC(yesLbl, mx+mW/2-gap/2-bW/2, my+mH-bOff+bH/2, COL_GREEN, Font::Md);
    HitAdd(mx+mW/2+gap/2, my+mH-bOff, bW, bH, TAckBackNo, 0);
    FillRect(mx+mW/2+gap/2, my+mH-bOff, bW, bH, COL_SEL);
    DrawRect(mx+mW/2+gap/2, my+mH-bOff, bW, bH, COL_RED);
    DrawTextC(noLbl, mx+mW/2+gap/2+bW/2, my+mH-bOff+bH/2, COL_RED, Font::Md);
    DrawFooter(footer);
    Present();
}
static void TUndoPlayer(int) {
    if (!gPlayerUndoValid[gPlayerFieldSel]) return;
    const auto& fd = PLAYER_FIELDS[gPlayerFieldSel];
    uint32_t h = PFHash(fd);
    uint32_t v = (uint32_t)gPlayerUndoVal[gPlayerFieldSel];
    if (fd.isEnum)          SaveEditor::SetEnum(gPlayerSav, h, v);
    else if (fd.isSkinTone) SaveEditor::SetUInt(gPlayerSav, h, v);
    else if (fd.isUInt)     SaveEditor::SetUInt(gPlayerSav, h, v);
    else if (fd.isRegion)   SaveEditor::SetEnum(gPlayerSav, h, v);
    else                    SaveEditor::SetAnyScalar(gPlayerSav, h, v);
    gPlayerUndoValid[gPlayerFieldSel] = false;
    gPlayerSavDirty = true;
}
[[maybe_unused]] static void TUndoNameLang(int) {
    if (!gNameLangUndoValid || !gPlayerSav.loaded) return;
    SaveEditor::SetEnum(gPlayerSav, SaveEditor::Hash("Player.NameRegionLanguageID"), (uint32_t)gNameLangUndo);
    gNameLangUndoValid = false;
    gPlayerSavDirty = true;
}
[[maybe_unused]] static void TUndoIslandLang(int) {
    if (!gIslandLangUndoValid || !gPlayerSav.loaded) return;
    SaveEditor::SetEnum(gPlayerSav, SaveEditor::Hash("Player.IslandNameRegionLanguageID"), (uint32_t)gIslandLangUndo);
    gIslandLangUndoValid = false;
    gPlayerSavDirty = true;
}
static void TUndoMii(int) {
    if (!gMiiUndoValid[gMiiStatsFieldSel]) return;
    if (gMiis.empty() || gMiiStatsMiiSel >= (int)gMiis.size()) return;
    const auto& fd = MII_STATS_FIELDS[gMiiStatsFieldSel];
    uint32_t h = SaveEditor::Hash(fd.fieldName);
    int miiIdx = gMiis[gMiiStatsMiiSel].slot - 1;
    if (fd.isUInt) SaveEditor::SetUIntAt(gMiiSav, h, miiIdx, (uint32_t)gMiiUndoVal[gMiiStatsFieldSel]);
    else           SaveEditor::SetIntAt(gMiiSav, h, miiIdx, gMiiUndoVal[gMiiStatsFieldSel]);
    gMiiUndoValid[gMiiStatsFieldSel] = false;
    gMiiSavDirty = true;
}
static void TUndoWords(int) {
    if (!gWordUndo.valid) return;
    if (gMiis.empty() || gMiiStatsMiiSel >= (int)gMiis.size()) return;
    static const uint32_t H_WKIND_U = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordKind");
    static const uint32_t H_WTXT_U  = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordText");
    static const uint32_t H_WHOW_U  = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordHowToCall");
    int miiIdx = gMiis[gMiiStatsMiiSel].slot - 1;
    int wIdx   = miiIdx * 12 + gMiiWordsSlotSel;
    SaveEditor::SetAnyEnumAt(gMiiSav, H_WKIND_U, wIdx, gWordUndo.kind);
    SaveEditor::SetWStr64At(gMiiSav,  H_WTXT_U,  wIdx, gWordUndo.text);
    SaveEditor::SetWStr64At(gMiiSav,  H_WHOW_U,  wIdx, gWordUndo.how);
    gWordUndo.valid = false;
    gMiiSavDirty = true;
}

// Touch-only two-tap handlers for the bulk Wishes buttons under the Wishes
// field's Undo button. First tap arms; second tap within ~3 s performs the
// action. The mechanism deliberately uses HitAdd taps (no controller binding)
// so it cannot conflict with the existing A-button numeric edit on Wishes.
static void TWishesArmOrFire(int kind) {
    if (!WishesAvailable()) {
        gPlayerActionMsg = "wish data not present in save";
        gPlayerActionMsgCol = SDL_Color{220, 80, 80, 255};
        gPlayerActionMsgFrames = 180;
        return;
    }
    if (gPlayerActionArmed != kind) {
        gPlayerActionArmed = kind;
        gPlayerActionArmedFrames = 180; // ~3s @ 60 fps
        return;
    }
    int changed = (kind == 1) ? WishesBulkUnlock() : WishesBulkReset();
    gPlayerActionArmed = 0;
    gPlayerActionArmedFrames = 0;
    char buf[64];
    snprintf(buf, sizeof(buf),
             kind == 1 ? "unlocked %d wish%s" : "reset %d wish%s",
             changed, changed == 1 ? "" : "es");
    gPlayerActionMsg = buf;
    gPlayerActionMsgCol = changed > 0
        ? SDL_Color{0, 200, 0, 255}
        : SDL_Color{180, 180, 180, 255};
    gPlayerActionMsgFrames = 240;
}
static void TWishesUnlockTap(int) { TWishesArmOrFire(1); }
static void TWishesResetTap (int) { TWishesArmOrFire(2); }

static void DrawError() {
    SDL_SetRenderDrawColor(gRen,COL_BG.r,COL_BG.g,COL_BG.b, 255);
    SDL_RenderClear(gRen);
    DrawHeader("");
    DrawTextC(Lang::T("error.title"), SCREEN_W/2, SCREEN_H/2-20, COL_RED, Font::Lg);
    DrawTextC(gError,  SCREEN_W/2, SCREEN_H/2+16, COL_DIM);
    DrawFooter(Lang::T("footer.quit"));
    Present();
}

// ─── Touch input ──────────────────────────────────────────────────────────────

static struct {
    bool         active   = false;
    bool         dragging = false;
    float        startX   = 0.f, startY = 0.f;
    float        accumX   = 0.f, accumY = 0.f;
    SDL_FingerID fid      = 0;
} s_touch;

// Mark s_touch.dragging only when a scroll variable actually changes value.
// Called unconditionally from FINGERMOTION so any drift that doesn't complete a
// scroll step is invisible to the tap-fire logic.
static void ApplyTouchScroll(float ddx, float ddy) {
    if (gScreen == Screen::UserPick) {
        int prev = gUserSel;
        s_touch.accumX += ddx;
        while (s_touch.accumX >=  100.f && gUserSel > 0)
            { s_touch.accumX -= 100.f; gUserSel--; }
        while (s_touch.accumX <= -100.f && gUserSel < (int)gUsers.size()-1)
            { s_touch.accumX += 100.f; gUserSel++; }
        if (gUserSel != prev) s_touch.dragging = true;
        return;
    }
    if (gScreen != Screen::OnSwitch) return;

    if (gShowFileBrowser) {
        int itemTop = gBrowseForExportDir ? 106 : 88;
        if (s_touch.startY < itemTop || s_touch.startY >= SCREEN_H-30) return;
        const int ph   = 26;
        const int pvis = (SCREEN_H - (gBrowseForExportDir ? 166 : 148)) / ph;
        int prev = gBrowseScroll;
        s_touch.accumY += ddy;
        while (s_touch.accumY >=  ph) { s_touch.accumY -= ph; gBrowseScroll = std::max(0, gBrowseScroll-1); }
        while (s_touch.accumY <= -ph) { s_touch.accumY += ph; gBrowseScroll = std::min(gBrowseScroll+1, std::max(0,(int)gBrowseEntries.size()-pvis)); }
        if (gBrowseScroll != prev) s_touch.dragging = true;
        return;
    }

    if (gOnSwitchMode == OnSwitchMode::UGC && !gEntries.empty()) {
        if (s_touch.startX < LIST_X || s_touch.startX >= LIST_X+LIST_W) return;
        if (s_touch.startY < LIST_Y || s_touch.startY >= LIST_Y+LIST_H) return;
        int prev = gEntryScroll;
        s_touch.accumY += ddy;
        while (s_touch.accumY >=  ITEM_H) { s_touch.accumY -= ITEM_H; gEntryScroll = std::max(0, gEntryScroll-1); }
        while (s_touch.accumY <= -ITEM_H) { s_touch.accumY += ITEM_H; gEntryScroll = std::min(gEntryScroll+1, std::max(0,(int)gFilteredEntries.size()-VISIBLE)); }
        if (gEntryScroll != prev) s_touch.dragging = true;
    }

    if (gOnSwitchMode == OnSwitchMode::MiiStats) {
        if (s_touch.startX >= SE_LIST_X && s_touch.startX < SE_LIST_X + MSE_LIST_W
            && s_touch.startY >= SE_TOP_Y && s_touch.startY < SCREEN_H - 32
            && !gMiis.empty()) {
            int mseVis = (SCREEN_H - SE_TOP_Y - 32) / ITEM_H;
            int prev = gMiiStatsScroll;
            s_touch.accumY += ddy;
            while (s_touch.accumY >=  ITEM_H) { s_touch.accumY -= ITEM_H; gMiiStatsScroll = std::max(0, gMiiStatsScroll - 1); }
            while (s_touch.accumY <= -ITEM_H) { s_touch.accumY += ITEM_H; gMiiStatsScroll = std::min(gMiiStatsScroll + 1, std::max(0, (int)gMiis.size() - mseVis)); }
            if (gMiiStatsScroll != prev) s_touch.dragging = true;
            return;
        }

        const int panelTop = SE_TOP_Y + 32;
        const int panelH   = SCREEN_H - panelTop - 32;
        const int midX2    = SE_LIST_X + MSE_LIST_W + 12;
        const int midW2    = 340;
        const int blPanX   = midX2;
        const int blPanW   = SCREEN_W - blPanX - 8;

        if (gMiiStatsSubTab == MiiStatsSubTab::Relations) {
            if (s_touch.startX < midX2 || s_touch.startX >= midX2 + midW2) return;
            if (s_touch.startY < SE_TOP_Y || s_touch.startY >= SCREEN_H - 32) return;
            const int relVis = (SCREEN_H - 32 - (SE_TOP_Y + 2)) / 34;
            int prev = gMiiRelScroll;
            s_touch.accumY += ddy;
            while (s_touch.accumY >=  ITEM_H) { s_touch.accumY -= ITEM_H; gMiiRelScroll = std::max(0, gMiiRelScroll - 1); }
            while (s_touch.accumY <= -ITEM_H) { s_touch.accumY += ITEM_H; gMiiRelScroll = std::min(gMiiRelScroll + 1, std::max(0, gMiiRelCount - relVis)); }
            if (gMiiRelScroll != prev) s_touch.dragging = true;
        } else if (gMiiStatsSubTab == MiiStatsSubTab::Belongings) {
            if (s_touch.startX < blPanX || s_touch.startX >= blPanX + blPanW) return;
            if (gBlPickerOpen) {
                const int PK_HDR_H = 32;
                const int listTop  = panelTop + PK_HDR_H;
                const int listH    = panelH - PK_HDR_H;
                if (s_touch.startY < listTop || s_touch.startY >= listTop + listH) return;
                const int total = (int)gBlFiltered.size();
                int prev = gBlPickerScroll;
                s_touch.accumY += ddy;
                {
                    const int pkVis = listH / 30;
                    while (s_touch.accumY >=  ITEM_H) { s_touch.accumY -= ITEM_H; gBlPickerScroll = std::max(0, gBlPickerScroll - 1); }
                    while (s_touch.accumY <= -ITEM_H) { s_touch.accumY += ITEM_H; gBlPickerScroll = std::min(gBlPickerScroll + 1, std::max(0, total - pkVis)); }
                }
                if (gBlPickerScroll != prev) s_touch.dragging = true;
            } else {
            }
        }
    }
}


static void ProcessTouch(int tx, int ty, u64& kDown) {
    auto hit = [&](int x, int y, int w, int h) -> bool {
        return tx>=x && tx<x+w && ty>=y && ty<y+h;
    };

    if (gScreen == Screen::UserPick) {
        int n = (int)gUsers.size();
        if (n == 0) return;
        const int GAP     = 24;
        const int MAX_VIS = std::min(n, 5);
        const int CARD_W  = std::min(220, (SCREEN_W - 120 - (MAX_VIS-1)*GAP) / MAX_VIS);
        const int CARD_H  = (CARD_W - 30) + 66;
        int scroll = 0;
        if (n > MAX_VIS) { scroll = gUserSel - MAX_VIS/2; scroll = std::max(0, std::min(scroll, n-MAX_VIS)); }
        int startX = (SCREEN_W - (MAX_VIS*CARD_W + (MAX_VIS-1)*GAP)) / 2;
        int startY = 68 + (SCREEN_H - 36 - 68 - CARD_H) / 2;
        for (int i = 0; i < MAX_VIS; i++) {
            int idx = scroll + i;
            if (idx >= n) break;
            if (hit(startX + i*(CARD_W+GAP), startY, CARD_W, CARD_H)) {
                gUserSel = idx;
                kDown |= HidNpadButton_A;
                break;
            }
        }
        return;
    }

    if (gScreen == Screen::BackupPrompt) {
        const int cw=460, ch=148, gap=20;
        const int bx = SCREEN_W/2 - (2*cw + gap)/2;
        const int by = SCREEN_H/2 - ch/2 + 10;
        if      (hit(bx,        by, cw, ch)) kDown |= HidNpadButton_A;
        else if (hit(bx+cw+gap, by, cw, ch)) kDown |= HidNpadButton_B;
        return;
    }

    if (gScreen != Screen::OnSwitch) return;

    if (gShowFileBrowser) {
        int itemTop = gBrowseForExportDir ? 106 : 88;
        // Tap header area (above items) to cancel
        if (ty >= 36 && ty < itemTop) { kDown |= HidNpadButton_X; return; }
        // Tap footer to go up one directory level
        if (ty >= SCREEN_H-30) { kDown |= HidNpadButton_B; return; }
        // Tap an item to select and activate it
        int ph = 26;
        int pvis = (SCREEN_H - (gBrowseForExportDir ? 166 : 148)) / ph;
        for (int i = 0; i < pvis; i++) {
            int idx = gBrowseScroll + i;
            if (idx >= (int)gBrowseEntries.size()) break;
            if (hit(52, itemTop + i*ph, SCREEN_W-104, ph)) {
                gBrowseSel = idx;
                kDown |= HidNpadButton_A;
                break;
            }
        }
        return;
    }

    if (gOnSwitchMode == OnSwitchMode::UGC) {
        // List items — tap to select, loads preview
        for (int i = 0; i < VISIBLE; i++) {
            int filtPos = gEntryScroll + i;
            if (filtPos >= (int)gFilteredEntries.size()) break;
            int realIdx = gFilteredEntries[filtPos];
            if (hit(LIST_X, UGC_LIST_TOP_Y+i*ITEM_H, LIST_W, ITEM_H)) {
                if (gEntrySel != realIdx) { gEntrySel = realIdx; LoadPreview(gEntries[realIdx]); }
                break;
            }
        }
        // Import / Export buttons (coordinates must match the draw code)
        if (!gEntries.empty()) {
            const int kBtnH=46, kBtnGap=8, kBtnSlot=kBtnH+kBtnGap+4;
            int imgY = PREVIEW_Y + 28;
            int imgH = PREVIEW_H - 28 - kBtnSlot;
            int cx   = PREVIEW_X + PREVIEW_W/2;
            int btnY = imgY + imgH + kBtnGap;
            const int btnW=155, btnGap=12, bgSep=36;
            int bx0  = cx - (4*btnW + 3*btnGap + bgSep)/2;
            int bxBg = bx0 + 3*(btnW+btnGap) + bgSep;
            if (hit(bx0,             btnY, btnW, kBtnH)) kDown |= HidNpadButton_A;
            if (hit(bx0+btnW+btnGap, btnY, btnW, kBtnH)) kDown |= HidNpadButton_Y;
            if (hit(bxBg,            btnY, btnW, kBtnH)) kDown |= HidNpadButton_X;
        }
    }
}


// ─── Main ─────────────────────────────────────────────────────────────────────

int main(int,char**) {
    romfsInit();
    Lang::Init();
    nifmInitialize(NifmServiceType_User);
    socketInitialize(socketGetDefaultInitConfig());
    SDL_Init(SDL_INIT_VIDEO|SDL_INIT_JOYSTICK);
    IMG_Init(IMG_INIT_PNG|IMG_INIT_JPG);
    TTF_Init();

    SDL_Window* win=SDL_CreateWindow("TomoToolNX",
        SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,SCREEN_W,SCREEN_H,0);
    gRen=SDL_CreateRenderer(win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(gRen,SDL_BLENDMODE_BLEND);
    FontInit();
    Audio::Init();

    padConfigureInput(1,HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&gPad);

    if (appletGetAppletType() != AppletType_Application) {
        gScreen = Screen::AppletWarning;
    } else {

    mkdir("/switch/TomoToolNX", 0777);
    // Two-step rename so exFAT (case-insensitive) picks up the capital E
    rename("/switch/TomoToolNX/exports", "/switch/TomoToolNX/__exports_tmp__");
    rename("/switch/TomoToolNX/__exports_tmp__", "/switch/TomoToolNX/Exports");
    mkdir("/switch/TomoToolNX/Exports", 0777);
    LoadConfig();
    // LoadConfig may have picked a non-Latin language (saved config or system
    // language detection on first launch). Reload glyphs so ru/zh actually
    // render — FontInit ran before the language was known.
    FontReload(Lang::Current());
    mkdir(gExportPath.c_str(), 0777);
    MtpServer::SetLogCallback([](const std::string& msg, bool ok){
        if (ok) LogOK(msg); else LogERR(msg);
    });
    // MTP USB file-sharing disabled at boot: haze's Initialize spins up a
    // worker thread that registers USB endpoint descriptors via a flurry of
    // usbDs* IPC calls. Ryujinx tolerates usbDsInitialize but exhausts its
    // IPC domain pool partway through the descriptor setup and aborts the
    // process (OutOfDomains → libnx fatal). On real hardware this means the
    // "plug Switch into PC for file transfer" feature is off until wired to
    // a Settings toggle; FTP/WebUI transfer paths are unaffected.
    // MtpServer::Init();

    // Kick off the GitHub release check in the background. The handler at
    // case Screen::UpdateCheck polls Updater::GetState() and either shows
    // the "update available" screen or falls through to UserPick.
    // Note: on Ryujinx this can crash mid-handshake (curl/SSL + nifm IPC
    // domain exhaustion). Hardware is fine.
    Updater::StartCheck();
    gScreen = Screen::UpdateCheck;

    gUsers=SaveMount::GetUsers();
    if (gUsers.empty()){
        gError="No user accounts found.";
        gScreen=Screen::Error;
    }

    } // end else (not applet mode)

    // Load avatar textures from JPEG data
    for (auto& u : gUsers) {
        SDL_Texture* tex = nullptr;
        if (!u.avatarJpeg.empty()) {
            SDL_RWops* rw = SDL_RWFromConstMem(u.avatarJpeg.data(), (int)u.avatarJpeg.size());
            if (rw) {
                SDL_Surface* surf = IMG_Load_RW(rw, 1);
                if (surf) { tex = SDL_CreateTextureFromSurface(gRen, surf); SDL_FreeSurface(surf); }
            }
        }
        gAvatarTextures.push_back(tex);
    }

    bool running = true; (void)running;

    gLastTickMs = SDL_GetTicks();
    Screen      gPrevScreen = gScreen;
    // Overlay open-state snapshots so the fade fires on false→true.
    bool prevShowBackPrompt   = gShowBackPrompt;
    bool prevShowFileBrowser  = gShowFileBrowser;
    bool prevShowRestorePicker= gShowRestorePicker;

    while (appletMainLoop()) {
        // Per-frame timing for animations.
        Uint32 nowMs = SDL_GetTicks();
        gDtSec = (nowMs - gLastTickMs) / 1000.f;
        if (gDtSec > 0.1f) gDtSec = 0.1f;          // clamp huge stalls (mount, etc.)
        gLastTickMs = nowMs;
        gElapsedSec += gDtSec;
        gTextCacheFrame++;  // advances the LRU clock for the text-texture cache
        gFade.Step(gDtSec);
        // Fade on screen change or any modal overlay opening.
        if (gScreen != gPrevScreen) { gFade.Begin(); gPrevScreen = gScreen; }
        if (gShowBackPrompt    && !prevShowBackPrompt)    gFade.Begin();
        if (gShowFileBrowser   && !prevShowFileBrowser)   gFade.Begin();
        if (gShowRestorePicker && !prevShowRestorePicker) gFade.Begin();
        prevShowBackPrompt    = gShowBackPrompt;
        prevShowFileBrowser   = gShowFileBrowser;
        prevShowRestorePicker = gShowRestorePicker;

        padUpdate(&gPad);
        u64 kDown=padGetButtonsDown(&gPad);
        u64 kHeld=padGetButtons(&gPad);

        {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_FINGERDOWN) {
                    // Always reset on any new finger — prevents stuck-active state when
                    // a previous FINGERUP was silently dropped by the platform.
                    s_touch = { true, false,
                                ev.tfinger.x*SCREEN_W, ev.tfinger.y*SCREEN_H,
                                0.f, 0.f, ev.tfinger.fingerId };
                }
                else if (ev.type == SDL_FINGERMOTION && s_touch.active
                         && ev.tfinger.fingerId == s_touch.fid) {
                    ApplyTouchScroll(ev.tfinger.dx * SCREEN_W,
                                     ev.tfinger.dy * SCREEN_H);
                }
                else if (ev.type == SDL_FINGERUP && s_touch.active
                         && ev.tfinger.fingerId == s_touch.fid) {
                    if (!s_touch.dragging) {
                        HitFire((int)s_touch.startX, (int)s_touch.startY);
                        ProcessTouch((int)s_touch.startX, (int)s_touch.startY, kDown);
                    }
                    s_touch.active = false;
                }
            }
        }
        if (gQuitApp) break;

        // + returns to user select globally; OnSwitch tabs handle it themselves (auto-save)
        bool plusOwned = gScreen == Screen::OnSwitch;
        if ((kDown&HidNpadButton_Plus) && !plusOwned) break;

        kDown |= gSimKDown;
        gSimKDown = 0;
        gFrameKDown = kDown;   // expose to render code (prev/next click feedback)
        u64 kNav = NavRepeat(kDown, kHeld);

        // UI sound dispatch. Nav for movement / tab cycling, click for actions.
        // Y is a sub-tab cycle on the Mii editor, so override it to nav there.
        const bool yIsNav = (gScreen == Screen::OnSwitch &&
                             gOnSwitchMode == OnSwitchMode::MiiStats);

        constexpr u64 NAV_REPEAT_MASK =
            HidNpadButton_Up    | HidNpadButton_Down  | HidNpadButton_Left  | HidNpadButton_Right
            | HidNpadButton_StickLUp   | HidNpadButton_StickLDown
            | HidNpadButton_StickLLeft | HidNpadButton_StickLRight
            | HidNpadButton_StickRUp   | HidNpadButton_StickRDown
            | HidNpadButton_StickRLeft | HidNpadButton_StickRRight
            | HidNpadButton_ZL | HidNpadButton_ZR;
        u64 navEdgeMask = HidNpadButton_L | HidNpadButton_R;
        if (yIsNav) navEdgeMask |= HidNpadButton_Y;

        constexpr u64 CLICK_BASE_MASK =
            HidNpadButton_A | HidNpadButton_B | HidNpadButton_X
            | HidNpadButton_Minus | HidNpadButton_Plus;
        u64 clickMask = CLICK_BASE_MASK | (yIsNav ? 0 : HidNpadButton_Y);

        if ((kNav & NAV_REPEAT_MASK) || (kDown & navEdgeMask)) Audio::Play(Audio::SfxNav);
        if (kDown & clickMask)                                  Audio::Play(Audio::SfxClick);

        switch (gScreen) {

        case Screen::AppletWarning:
            if (kDown&(HidNpadButton_B|HidNpadButton_Plus)) gQuitApp=true;
            DrawAppletWarning();
            break;

        case Screen::UpdateCheck:
            // Auto-check: started at launch only if WiFi is active
            {
                auto state = Updater::GetState();
                if (state==Updater::State::UpdateAvailable) gScreen=Screen::UpdateAvailable;
                else if (state==Updater::State::NoUpdate||state==Updater::State::Error||state==Updater::State::Idle)
                    gScreen=Screen::UserPick;
                if (kDown&HidNpadButton_B) gScreen=Screen::UserPick;
            }
            DrawUpdateCheck();
            break;

        case Screen::UpdateAvailable:
            if (kDown&(HidNpadButton_A|HidNpadButton_B)) gScreen=Screen::UserPick;
            DrawUpdateAvailable();
            break;

        case Screen::UserPick:
            if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))   { gUserSel = (gUserSel>0) ? gUserSel-1 : (int)gUsers.size()-1; }
            if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) { gUserSel = (gUserSel+1<(int)gUsers.size()) ? gUserSel+1 : 0; }
            if (kDown&HidNpadButton_X) { TOpenSettings(0); break; }
            if (kDown&HidNpadButton_A) {
                // Mount save first so we can check backup state
                gScreen=Screen::Mounting;
                DrawMounting();
                {
                    std::string err=SaveMount::Mount(gUsers[gUserSel].uid);
                    if (!err.empty()){gError=err;gScreen=Screen::Error;break;}
                }
                gBackupFull = (BackupService::CountBackups() >= gMaxBackups);
                gScreen=Screen::BackupPrompt;
            }
            DrawUserPick();
            break;

        case Screen::Settings:
            if (gShowRestorePicker) {
                const int RVIS = 7;
                // ── Confirmation modal handling (A = confirm, B = cancel) ───
                if (gRestoreConfirmKind != 0) {
                    if (kDown & HidNpadButton_B) {
                        gRestoreConfirmKind = 0;
                    } else if (kDown & HidNpadButton_A) {
                        int kind = gRestoreConfirmKind;
                        gRestoreConfirmKind = 0;
                        if (gRestoreSel < 0 || gRestoreSel >= (int)gRestoreList.size()) {
                            // nothing to do
                        } else if (kind == 2) {
                            // Delete the selected backup
                            std::string target = gRestoreList[gRestoreSel];
                            bool ok = BackupService::DeleteBackupAt(target);
                            if (ok) {
                                gRestoreList = BackupService::ListBackups();
                                if (gRestoreList.empty()) {
                                    gShowRestorePicker = false;
                                    gSettingsMsg = "Backup deleted (no backups left).";
                                    gSettingsMsgCol = COL_GREEN;
                                } else {
                                    if (gRestoreSel >= (int)gRestoreList.size())
                                        gRestoreSel = (int)gRestoreList.size() - 1;
                                    if (gRestoreSel < gRestoreScroll) gRestoreScroll = gRestoreSel;
                                    int maxScroll = std::max(0, (int)gRestoreList.size() - RVIS);
                                    if (gRestoreScroll > maxScroll) gRestoreScroll = maxScroll;
                                    gSettingsMsg = "Backup deleted.";
                                    gSettingsMsgCol = COL_GREEN;
                                }
                            } else {
                                gSettingsMsg = "Delete failed."; gSettingsMsgCol = COL_RED;
                            }
                        } else if (kind == 1) {
                            // Restore the selected backup
                            if (gUsers.empty()) {
                                gSettingsMsg = "No user selected."; gSettingsMsgCol = COL_RED;
                                gShowRestorePicker = false;
                            } else {
                                std::string merr = SaveMount::Mount(gUsers[gUserSel].uid);
                                if (!merr.empty()) {
                                    gSettingsMsg = "Mount failed: " + merr; gSettingsMsgCol = COL_RED;
                                    gShowRestorePicker = false;
                                } else {
                                    SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
                                    SDL_RenderClear(gRen);
                                    DrawTextC(Lang::T("restore.progress.title"), SCREEN_W/2, SCREEN_H/2 - 18, COL_ACCENT, Font::Lg);
                                    DrawTextC(Lang::T("restore.progress.wait"), SCREEN_W/2, SCREEN_H/2 + 22, COL_DIM, Font::Md);
                                    Present();
                                    std::string rerr = BackupService::RestoreBackup(gRestoreList[gRestoreSel], "tomodata:/");
                                    SaveMount::Commit();
                                    SaveMount::Unmount();
                                    gShowRestorePicker = false;
                                    if (rerr.empty()) { gSettingsMsg = "Backup restored successfully."; gSettingsMsgCol = COL_GREEN; }
                                    else              { gSettingsMsg = "Restore failed: " + rerr;       gSettingsMsgCol = COL_RED; }
                                }
                            }
                        }
                    }
                    break;
                }
                // ── Normal picker navigation (no modal) ─────────────────────
                if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp)) {
                    if (gRestoreSel > 0) { gRestoreSel--; if (gRestoreSel < gRestoreScroll) gRestoreScroll = gRestoreSel; }
                }
                if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown)) {
                    if (gRestoreSel+1 < (int)gRestoreList.size()) {
                        gRestoreSel++;
                        if (gRestoreSel >= gRestoreScroll+RVIS) gRestoreScroll = gRestoreSel-RVIS+1;
                    }
                }
                if (kDown&HidNpadButton_B) { gShowRestorePicker = false; }
                if (kDown&HidNpadButton_A && !gRestoreList.empty()) {
                    gRestoreConfirmKind = 1; // ask before restoring
                }
                if (kDown&HidNpadButton_X && !gRestoreList.empty()) {
                    gRestoreConfirmKind = 2; // ask before deleting
                }
            } else if (gShowFileBrowser) {
                int pvis = (SCREEN_H - 148) / 26;
                if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp)) {
                    if (gBrowseSel > 0) { gBrowseSel--; if (gBrowseSel < gBrowseScroll) gBrowseScroll = gBrowseSel; }
                }
                if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown)) {
                    if (gBrowseSel+1 < (int)gBrowseEntries.size()) {
                        gBrowseSel++;
                        if (gBrowseSel >= gBrowseScroll+pvis) gBrowseScroll = gBrowseSel-pvis+1;
                    }
                }
                if (kDown&HidNpadButton_A && !gBrowseEntries.empty()) {
                    auto& entry = gBrowseEntries[gBrowseSel];
                    if (entry.isDir) {
                        std::string newPath;
                        if (entry.name == "[..]") {
                            size_t pos = gBrowseCurPath.rfind('/');
                            newPath = (pos == 0) ? "/" : gBrowseCurPath.substr(0, pos);
                        } else {
                            newPath = gBrowseCurPath + "/" + entry.name;
                        }
                        BrowseRefresh(newPath);
                    }
                }
                if (kDown&HidNpadButton_B && gBrowseCurPath != "/") {
                    size_t pos = gBrowseCurPath.rfind('/');
                    BrowseRefresh((pos == 0) ? "/" : gBrowseCurPath.substr(0, pos));
                }
                if (kDown&HidNpadButton_Y) {
                    gExportPath = gBrowseCurPath;
                    mkdir(gExportPath.c_str(), 0777);
                    SaveConfig();
                    gShowFileBrowser = false;
                    gSettingsMsg = "Export path set to: " + gExportPath;
                    gSettingsMsgCol = COL_GREEN;
                }
                if (kDown&HidNpadButton_X) { gShowFileBrowser = false; }
            } else {
                if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp)) {
                    gSettingsSel = std::max(0, gSettingsSel - 1);
                }
                if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown)) {
                    gSettingsSel = std::min(4, gSettingsSel + 1);
                }
                // Row mapping: 0 = Language, 1 = Theme, 2 = Export Path,
                //              3 = Max Backups, 4 = Restore Backup.
                if (gSettingsSel == 4 && kDown&HidNpadButton_A) {
                    gRestoreList        = BackupService::ListBackups();
                    gRestoreSel         = 0;
                    gRestoreScroll      = 0;
                    gRestoreConfirmKind = 0;
                    gShowRestorePicker  = true;
                }
                if (gSettingsSel == 2 && kDown&HidNpadButton_A) {
                    gBrowseForExportDir = true; gBrowseForMii = false;
                    std::string sp = gExportPath;
                    struct stat _st;
                    if (stat(sp.c_str(),&_st)!=0||!S_ISDIR(_st.st_mode)) {
                        size_t p=sp.rfind('/'); sp=(p&&p!=std::string::npos)?sp.substr(0,p):"/";
                    }
                    BrowseRefresh(sp); gShowFileBrowser = true;
                }
                if (gSettingsSel == 3) {
                    if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft)) {
                        gMaxBackups = std::max(1, gMaxBackups - 1); SaveConfig();
                    }
                    if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) {
                        gMaxBackups = std::min(99, gMaxBackups + 1); SaveConfig();
                    }
                }
                if (gSettingsSel == 0) {
                    bool left  = kNav&(HidNpadButton_Left |HidNpadButton_StickLLeft |HidNpadButton_StickRLeft);
                    bool right = kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight);
                    bool a     = kDown&HidNpadButton_A;
                    if (left || right || a) {
                        const auto& avail = Lang::Available();
                        if (!avail.empty()) {
                            int curIdx = 0;
                            for (int li = 0; li < (int)avail.size(); li++)
                                if (avail[li].code == Lang::Current()) { curIdx = li; break; }
                            int next = curIdx;
                            if (left)            next = (curIdx > 0) ? curIdx - 1 : (int)avail.size() - 1;
                            else if (right || a) next = (curIdx + 1 < (int)avail.size()) ? curIdx + 1 : 0;
                            Lang::SetCurrent(avail[next].code);
                            FontReload(Lang::Current());
                            SaveConfig();
                        }
                    }
                }
                if (gSettingsSel == 1) {
                    bool left  = kNav&(HidNpadButton_Left |HidNpadButton_StickLLeft |HidNpadButton_StickRLeft);
                    bool right = kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight);
                    bool a     = kDown&HidNpadButton_A;
                    if (left || right || a) {
                        const auto& avail = ThemeNS::Available();
                        if (!avail.empty()) {
                            int curIdx = 0;
                            for (int ti = 0; ti < (int)avail.size(); ti++)
                                if (avail[ti].code == ThemeNS::Current()) { curIdx = ti; break; }
                            int next = curIdx;
                            if (left)            next = (curIdx > 0) ? curIdx - 1 : (int)avail.size() - 1;
                            else if (right || a) next = (curIdx + 1 < (int)avail.size()) ? curIdx + 1 : 0;
                            ThemeNS::SetCurrent(avail[next].code);
                            SaveConfig();
                        }
                    }
                }
                if (kDown&HidNpadButton_B) { gScreen = Screen::UserPick; }
            }
            DrawSettings();
            break;

        case Screen::BackupPrompt:
            if (kDown&HidNpadButton_A) {
                if (gBackupFull) BackupService::DeleteOldestBackup();
                BackupService::StartFullBackup("tomodata:/");
                gScreen=Screen::BackingUp;
            }
            if (kDown&HidNpadButton_B) {
                gScreen=Screen::OnSwitch;
            }
            DrawBackupPrompt();
            break;

        case Screen::BackingUp:
            if (BackupService::BackupDone()) {
                gScreen=Screen::OnSwitch;
            }
            DrawBackingUp();
            break;

        case Screen::Mounting:
            DrawMounting();
            // Init happens in mounting thread — once save is mounted we go to OnSwitch
            // and start both HTTP server and scan entries
            break;

        case Screen::OnSwitch:
            // On first entry: start HTTP server, scan UGC and Miis
            if (gEntries.empty() && gMiis.empty()) {
                gIP=SaveMount::GetLocalIP();
                if (gIP.empty()) gIP="?.?.?.?";
                HttpServer::Start(HTTP_PORT, SAVE_UGC_PATH);
                HttpServer::SetSaveWarnAcked(gSaveWarningAcked);
                gEntries=UgcScanner::Scan(SAVE_UGC_PATH);
                MiiManager::LoadUgcNames();
                gUgcFilter.clear();
                RebuildUgcFilter();
                gMiis=MiiManager::ListMiis();
                gEntrySel=0; gEntryScroll=0;
                gOnSwitchMsg="";
                if (!gEntries.empty()) LoadPreview(gEntries[0]);
            }
            // HTTP server events — process regardless of active tab
            {
                std::vector<HttpServer::LogEntry> httpLogs;
                HttpServer::DrainLog(httpLogs);
                for (auto& hl:httpLogs) Log(hl.text, hl.isError?COL_RED:COL_GOLD);
            }
            if (HttpServer::HasPendingImport()) {
                auto job=HttpServer::TakePendingImport();
                std::string importErr=TextureProcessor::ImportPng(job.opts);
                remove(job.tmpPath.c_str());
                HttpServer::FinishImport(importErr);
                if (!importErr.empty()) LogERR("Import: "+importErr);
                else { LogOK("Import OK"); gEntries=UgcScanner::Scan(SAVE_UGC_PATH);
                       MiiManager::LoadUgcNames();
                       RebuildUgcFilter();
                       if (!gEntries.empty()) LoadPreview(gEntries[gEntrySel < (int)gEntries.size() ? gEntrySel : 0]); }
            }
            if (HttpServer::HasPendingBgRemove()) {
                auto job = HttpServer::TakePendingBgRemove();
                DrawBgRemoving();
                RgbaImage img;
                std::string bgErr = TextureProcessor::DecodeFile(job.ugctexPath, img, false);
                BgAnimState bgAnim{};
                if (bgErr.empty()) bgErr = U2Net::RemoveBackground(img, kBgModelPath, OnBgRemoveProgress, &bgAnim);
                if (bgErr.empty()) bgErr = TextureProcessor::ImportRgbaImage(img, job.opts);
                HttpServer::FinishBgRemove(bgErr);
                if (!bgErr.empty()) LogERR("BgRemove: "+bgErr);
                else { LogOK("BgRemove OK"); gEntries=UgcScanner::Scan(SAVE_UGC_PATH);
                       MiiManager::LoadUgcNames();
                       RebuildUgcFilter();
                       if (!gEntries.empty()) LoadPreview(gEntries[gEntrySel < (int)gEntries.size() ? gEntrySel : 0]);
                       gUgcDirty=true; }
            }
            if (HttpServer::HasPendingCommit()) {
                HttpServer::ClearPendingCommit();
                gWebUiDirty = true;
                LogINF("WebUI change pending — will save on exit");
            }
            if (HttpServer::HasPendingMiiRefresh()) {
                HttpServer::ClearPendingMiiRefresh();
                gMiis = MiiManager::ListMiis();
                if (gMiiStatsMiiSel >= (int)gMiis.size()) gMiiStatsMiiSel = (int)gMiis.size()-1;
                if (!gMiiSavDirty) {
                    gMiiSav = SaveEditor::SavFile{};
                    std::string lerr;
                    if (SaveEditor::Load(SAVE_MII_SAV, gMiiSav, lerr))
                        gMiiSavDirty = true;
                }
                LogOK("Mii list updated");
            }
            if (HttpServer::HasPendingPlayerSavReload()) {
                HttpServer::ClearPendingPlayerSavReload();
                if (gPlayerSav.loaded) {
                    std::string lerr;
                    SaveEditor::Load(SAVE_PLAYER_SAV, gPlayerSav, lerr);
                    gPlayerSavDirty = false;
                    memset(gPlayerUndoValid, 0, sizeof(gPlayerUndoValid));
                    gNameLangUndoValid = false; gIslandLangUndoValid = false;
                    LogOK("Player save synced from WebUI");
                }
            }
            if (HttpServer::HasPendingMiiSavReload()) {
                HttpServer::ClearPendingMiiSavReload();
                if (gMiiSav.loaded) {
                    std::string lerr;
                    SaveEditor::Load(SAVE_MII_SAV, gMiiSav, lerr);
                    gMiiSavDirty = false;
                    memset(gMiiUndoValid, 0, sizeof(gMiiUndoValid));
                    gWordUndo.valid = false;
                    LogOK("Mii save synced from WebUI");
                }
            }
            if (HttpServer::HasPendingMapSavReload()) {
                HttpServer::ClearPendingMapSavReload();
                if (gMapSav.loaded) {
                    std::string lerr;
                    SaveEditor::Load(SAVE_MAP_SAV, gMapSav, lerr);
                    LogOK("Map save synced from WebUI");
                }
            }
            // Thumb tip modal
            if (gShowThumbTip) {
                if (gThumbTipCountdown > 0) gThumbTipCountdown--;
                if (gThumbTipCountdown <= 0 && (kDown & HidNpadButton_A)) TDismissThumbTip(0);
                DrawThumbTip();
                break;
            }
            // Warning modal takes priority over all other input
            if (gShowSaveWarning) {
                if (gSaveWarningCountdown > 0) gSaveWarningCountdown--;
                if (gSaveWarningCountdown <= 0 && (kDown & HidNpadButton_A)) TAckSaveWarning(0);
                if (gShowSaveWarning) DrawSaveWarning();
                break;
            }
            // Background-removal confirm modal
            if (gShowBgRemovePrompt) {
                if (kDown & HidNpadButton_A) TConfirmBgRemove(0);
                if (kDown & HidNpadButton_B) TCancelBgRemove(0);
                if (gShowBgRemovePrompt) DrawBgRemovePrompt();
                break;
            }
            // Back-prompt modal
            if (gShowBackPrompt) {
                if (kDown & HidNpadButton_A) TAckBackYes(0);
                if (kDown & HidNpadButton_B) TAckBackNo(0);
                if (kDown & HidNpadButton_X) gShowBackPrompt = false;
                if (gShowBackPrompt) DrawBackPrompt();
                break;
            }
            // File browser takes priority
            if (gShowFileBrowser) {
                int pvis=(SCREEN_H-148)/26;
                if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp)){
                    if(gBrowseSel>0){gBrowseSel--;if(gBrowseSel<gBrowseScroll)gBrowseScroll=gBrowseSel;}
                }
                if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown)){
                    if(gBrowseSel+1<(int)gBrowseEntries.size()){
                        gBrowseSel++;
                        if(gBrowseSel>=gBrowseScroll+pvis)gBrowseScroll=gBrowseSel-pvis+1;
                    }
                }
                if (kDown&HidNpadButton_A && !gBrowseEntries.empty()){
                    auto& entry=gBrowseEntries[gBrowseSel];
                    if (entry.isDir){
                        std::string newPath;
                        if (entry.name=="[..]"){
                            size_t pos=gBrowseCurPath.rfind('/');
                            newPath=(pos==0)?"/":gBrowseCurPath.substr(0,pos);
                        } else {
                            newPath=gBrowseCurPath+"/"+entry.name;
                        }
                        BrowseRefresh(newPath);
                    } else if (!gBrowseForExportDir) {
                        std::string fullPath=gBrowseCurPath+"/"+entry.name;
                        if (gBrowseForMii) gBrowseLtdPath=gBrowseCurPath;
                        else gBrowsePngPath=gBrowseCurPath;
                        gShowFileBrowser=false;
                        if (gBrowseForMii) {
                            DoMiiImport(fullPath);
                        } else {
                            std::string lower=entry.name;
                            for(auto& c:lower)c=(char)tolower((unsigned char)c);
                            // UGC item files are 5-char extensions: .ltdf .ltdc
                            // .ltdg .ltdi .ltde .ltdo .ltdl. Check the 4 chars
                            // before the type letter — checking just ".ltd"
                            // at size-4 misses every variant since their last
                            // 4 chars are "ltd<x>", not ".ltd".
                            bool isLtdx = lower.size() >= 5
                                       && lower.compare(lower.size()-5, 4, ".ltd") == 0;
                            if (isLtdx) DoUgcImportLtd(fullPath);
                            else        DoOnSwitchImport(fullPath);
                        }
                    }
                }
                if (kDown&HidNpadButton_B && gBrowseCurPath!="/"){
                    size_t pos=gBrowseCurPath.rfind('/');
                    BrowseRefresh((pos==0)?"/":gBrowseCurPath.substr(0,pos));
                }
                if (kDown&HidNpadButton_Y && gBrowseForExportDir) {
                    gExportPath = gBrowseCurPath;
                    mkdir(gExportPath.c_str(), 0777);
                    SaveConfig();
                    gShowFileBrowser = false;
                    gOnSwitchMsg = "Export folder set to: "+gExportPath;
                    gOnSwitchMsgCol = COL_GREEN;
                }
                if (kDown&HidNpadButton_X) {
                    if (gBrowseForExportDir) {
                        gShowFileBrowser = false;
                    } else if (gBrowseForMii) {
                        gBrowseLtdPath=gBrowseCurPath;
                        gShowFileBrowser=false;
                    } else {
                        gBrowsePngPath=gBrowseCurPath;
                        gShowFileBrowser=false;
                    }
                }
            } else if (gOnSwitchMode == OnSwitchMode::UGC) {
                // Tab switch (cycle: UGC → MiiStats → Player → WebUI → UGC)
                if (kDown&HidNpadButton_R){
                    if (!gSaveWarningAcked) {
                        gSaveWarningTarget=OnSwitchMode::MiiStats; gShowSaveWarning=true; gSaveWarningCountdown=120; break;
                    }
                    gOnSwitchMode=OnSwitchMode::MiiStats;
                    if (!gMiiSav.loaded) {
                        std::string err;
                        if (!SaveEditor::Load(SAVE_MII_SAV, gMiiSav, err))
                            gMiiStatsMsg="Load error: "+err;
                    }
                    gOnSwitchMsg=""; break;
                }
                if (kDown&HidNpadButton_L){
                    gOnSwitchMode=OnSwitchMode::WebUI; gOnSwitchMsg=""; break;
                }
                // Filtered-list navigation helpers (gEntrySel is real index, gEntryScroll is filtered position)
                auto navPrev = [&](){
                    if (gFilteredEntries.empty()) return;
                    int p = UgcFilterPos(gEntrySel);
                    if (p < 0) p = 0;
                    p = (p > 0) ? p - 1 : (int)gFilteredEntries.size() - 1;
                    gEntrySel = gFilteredEntries[p];
                    if (p < gEntryScroll) gEntryScroll = p;
                    if (p >= gEntryScroll + VISIBLE) gEntryScroll = p - VISIBLE + 1;
                    LoadPreview(gEntries[gEntrySel]);
                };
                auto navNext = [&](){
                    if (gFilteredEntries.empty()) return;
                    int p = UgcFilterPos(gEntrySel);
                    if (p < 0) p = 0;
                    p = (p + 1 < (int)gFilteredEntries.size()) ? p + 1 : 0;
                    gEntrySel = gFilteredEntries[p];
                    if (p >= gEntryScroll + VISIBLE) gEntryScroll = p - VISIBLE + 1;
                    if (p < gEntryScroll) gEntryScroll = p;
                    LoadPreview(gEntries[gEntrySel]);
                };
                if ((kNav&HidNpadButton_ZL) || (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp))) navPrev();
                if ((kNav&HidNpadButton_ZR) || (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown))) navNext();
                if (kDown&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft) && !gFilteredEntries.empty()){
                    gEntrySel = gFilteredEntries.front(); gEntryScroll = 0; LoadPreview(gEntries[gEntrySel]);
                }
                if (kDown&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight) && !gFilteredEntries.empty()){
                    gEntrySel = gFilteredEntries.back();
                    gEntryScroll = std::max(0, (int)gFilteredEntries.size() - VISIBLE);
                    LoadPreview(gEntries[gEntrySel]);
                }
                if (kDown&HidNpadButton_Minus){
                    TUgcOpenSearch(0);
                }
                if (kDown&HidNpadButton_A){
                    gBrowseForMii=false; gBrowseForExportDir=false;
                    BrowseRefresh(gBrowsePngPath);
                    gShowFileBrowser=true;
                }
                if (kDown&HidNpadButton_Y && !gEntries.empty()){
                    const auto& e=gEntries[gEntrySel];
                    LogINF("Exporting "+e.stem+"...");
                    RgbaImage img;
                    std::string err=TextureProcessor::DecodeFile(e.ugctexPath,img,true);
                    if (!err.empty()){gOnSwitchMsg=err;gOnSwitchMsgCol=COL_RED;LogERR("Export failed: "+err);}
                    else {
                        TextureProcessor::ConvertLinearToSrgb(img.pixels);
                        std::string outPath=gExportPath+"/"+e.stem+".png";
                        SDL_Surface* surf=SDL_CreateRGBSurfaceWithFormatFrom(
                            img.pixels.data(),img.width,img.height,32,img.width*4,
                            SDL_PIXELFORMAT_RGBA32);
                        if(surf){IMG_SavePNG(surf,outPath.c_str());SDL_FreeSurface(surf);
                            gOnSwitchMsg="Exported to: "+outPath;gOnSwitchMsgCol=COL_GREEN;
                            LogOK("Exported to: "+outPath);}
                    }
                }
                if (kDown&HidNpadButton_X && !gEntries.empty()) {
                    FILE* mf = fopen(kBgModelPath, "rb");
                    if (!mf) {
                        gOnSwitchMsg = "u2netp.bin missing from romfs (corrupt install?)";
                        gOnSwitchMsgCol = COL_RED;
                    } else {
                        fclose(mf);
                        gShowBgRemovePrompt = true;
                    }
                }
                if (kDown&(HidNpadButton_B|HidNpadButton_Plus)){
                    bool hasDirty = gPlayerSavDirty || gMiiSavDirty || gUgcDirty || gWebUiDirty;
                    bool toUserPick = (kDown&HidNpadButton_Plus) == 0;
                    if (hasDirty) {
                        gShowBackPrompt=true;
                        gBackPromptIsMii=gMiiSavDirty; gBackPromptToUserPick=toUserPick;
                    } else if (toUserPick) {
                        FreePreview(); HttpServer::Stop(); gLog.clear();
                        gEntries.clear(); gMiis.clear();
                        gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{};
                        gPlayerSavDirty=false; gMiiSavDirty=false; gUgcDirty=false;
                        SaveMount::Unmount(); gScreen=Screen::UserPick;
                    } else { gQuitApp=true; }
                }
            } else if (gOnSwitchMode == OnSwitchMode::Player) {
                // Lazy-load
                if (!gPlayerSav.loaded) {
                    std::string err;
                    if (!SaveEditor::Load(SAVE_PLAYER_SAV, gPlayerSav, err))
                        gPlayerMsg = "Load error: " + err;
                }
                // Tab switch (changes stay in memory; prompt only on back/quit).
                // Visual order: WebUI · Textures · Mii · Player · Map.
                if (kDown&HidNpadButton_L){
                    gOnSwitchMode=OnSwitchMode::MiiStats;
                    if (!gMiiSav.loaded) {
                        std::string err;
                        if (!SaveEditor::Load(SAVE_MII_SAV, gMiiSav, err))
                            gMiiStatsMsg="Load error: "+err;
                    }
                    gOnSwitchMsg=""; break;
                }
                if (kDown&HidNpadButton_R){
                    gOnSwitchMode=OnSwitchMode::Map; gOnSwitchMsg=""; break;
                }
                // Field navigation
                if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp|HidNpadButton_ZL))
                    gPlayerFieldSel = (gPlayerFieldSel>0) ? gPlayerFieldSel-1 : PLAYER_FIELD_COUNT-1;
                if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown|HidNpadButton_ZR))
                    gPlayerFieldSel = (gPlayerFieldSel+1<PLAYER_FIELD_COUNT) ? gPlayerFieldSel+1 : 0;
                // Tick down the wish-button arm timeout (used by the touch-only
                // bulk Unlock/Reset buttons under the Wishes field's Undo button).
                // It is purely visual — driven by HitAdd taps, never by A presses.
                if (gPlayerActionArmed != 0) {
                    if (--gPlayerActionArmedFrames <= 0) gPlayerActionArmed = 0;
                }
                if (gPlayerActionMsgFrames > 0) gPlayerActionMsgFrames--;
                // Edit via callbacks
                if (gPlayerSav.loaded) {
                    const auto& fd = PLAYER_FIELDS[gPlayerFieldSel];
                    uint32_t h = PFHash(fd);

                    // Numeric nudge: UInt, Int, RawEnum
                    bool isNumeric = fd.isUInt || fd.isInt || fd.isRawEnum;
                    if (isNumeric) {
                        int scale = gTouchScale; gTouchScale = 1;
                        auto getV = [&]() -> uint32_t {
                            return fd.isUInt ? SaveEditor::GetUInt(gPlayerSav, h)
                                             : SaveEditor::GetAnyScalar(gPlayerSav, h);
                        };
                        auto setV = [&](uint32_t v) {
                            if (fd.isUInt) SaveEditor::SetUInt(gPlayerSav, h, v);
                            else           SaveEditor::SetAnyScalar(gPlayerSav, h, v);
                            gPlayerSavDirty = true;
                        };
                        auto adjustV = [&](int delta) {
                            uint32_t v = getV();
                            if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)v; gPlayerUndoValid[gPlayerFieldSel]=true; }
                            if (fd.isInt) {
                                int32_t sv = (int32_t)v + delta;
                                if (fd.minVal != -1 && sv < fd.minVal) sv = fd.minVal;
                                if (fd.maxVal != -1 && sv > fd.maxVal) sv = fd.maxVal;
                                setV((uint32_t)sv);
                            } else {
                                if (delta < 0) {
                                    uint32_t ad=(uint32_t)(-delta);
                                    uint32_t nv=(v>=ad)?v-ad:0;
                                    if(fd.minVal>=0&&(int32_t)nv<fd.minVal)nv=(uint32_t)fd.minVal;
                                    setV(nv);
                                } else {
                                    uint32_t nv=v+(uint32_t)delta;
                                    if(fd.maxVal>=0&&(int32_t)nv>fd.maxVal)nv=(uint32_t)fd.maxVal;
                                    setV(nv);
                                }
                            }
                        };
                        if (gBtnTouchActive) {
                            gBtnTouchActive = false;
                            if (kDown&HidNpadButton_Left)  adjustV(-scale);
                            if (kDown&HidNpadButton_Right) adjustV( scale);
                        } else {
                            if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))
                                gPlayerBtnSel = (gPlayerBtnSel-1+8)%8;
                            if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight))
                                gPlayerBtnSel = (gPlayerBtnSel+1)%8;
                            if (kDown&HidNpadButton_A) {
                                static const int PSTEPS[]={1000,100,10,1};
                                int pdelta = (gPlayerBtnSel<4) ? -(int)PSTEPS[gPlayerBtnSel] : (int)PSTEPS[7-gPlayerBtnSel];
                                adjustV(pdelta);
                            }
                        }
                    }
                    // Currency enum cycle
                    if (fd.isEnum) {
                        uint32_t cur = SaveEditor::GetEnum(gPlayerSav, h);
                        int idx = CurrencyIndex(cur);
                        if (idx < 0) idx = 0;
                        if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft)) {
                            if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
                            int nidx = (idx > 0) ? idx-1 : CURRENCY_LABEL_COUNT-1;
                            SaveEditor::SetEnum(gPlayerSav, h, CURRENCY_LABELS[nidx].hash);
                            gPlayerSavDirty=true;
                        }
                        if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) {
                            if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
                            int nidx = (idx+1 < CURRENCY_LABEL_COUNT) ? idx+1 : 0;
                            SaveEditor::SetEnum(gPlayerSav, h, CURRENCY_LABELS[nidx].hash);
                            gPlayerSavDirty=true;
                        }
                    }
                    // Region enum cycle
                    if (fd.isRegion) {
                        uint32_t cur = SaveEditor::GetEnum(gPlayerSav, h);
                        int ridx = 0;
                        for (int ri=0; ri<REGION_OPTION_COUNT; ri++)
                            if (SaveEditor::Hash(REGION_OPTIONS[ri].name)==cur) { ridx=ri; break; }
                        if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft)) {
                            if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
                            int nidx = (ridx-1+REGION_OPTION_COUNT) % REGION_OPTION_COUNT;
                            SaveEditor::SetEnum(gPlayerSav, h, SaveEditor::Hash(REGION_OPTIONS[nidx].name));
                            gPlayerSavDirty=true;
                        }
                        if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) {
                            if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
                            int nidx = (ridx+1) % REGION_OPTION_COUNT;
                            SaveEditor::SetEnum(gPlayerSav, h, SaveEditor::Hash(REGION_OPTIONS[nidx].name));
                            gPlayerSavDirty=true;
                        }
                    }
                    // Language cycle embedded in Name/Island str fields
                    if (fd.isStr) {
                        const char* lf = (strcmp(fd.fieldName,"Player.Name")==0)      ? "Player.NameRegionLanguageID"
                                       : (strcmp(fd.fieldName,"Player.IslandName")==0) ? "Player.IslandNameRegionLanguageID"
                                       : nullptr;
                        if (lf) {
                            bool isNameF = strcmp(fd.fieldName,"Player.Name")==0;
                            uint32_t lh = SaveEditor::Hash(lf);
                            uint32_t cur = SaveEditor::GetEnum(gPlayerSav, lh);
                            int lidx = 0;
                            for (int li=0; li<LANG_OPTION_COUNT; li++)
                                if (SaveEditor::Hash(LANG_OPTIONS[li].name)==cur) { lidx=li; break; }
                            auto cycleLang = [&](int dir) {
                                if (isNameF) { if (!gNameLangUndoValid)   { gNameLangUndo  =(int32_t)cur; gNameLangUndoValid  =true; } }
                                else         { if (!gIslandLangUndoValid) { gIslandLangUndo=(int32_t)cur; gIslandLangUndoValid=true; } }
                                int nidx = (lidx+dir+LANG_OPTION_COUNT)%LANG_OPTION_COUNT;
                                SaveEditor::SetEnum(gPlayerSav, lh, SaveEditor::Hash(LANG_OPTIONS[nidx].name));
                                gPlayerSavDirty=true;
                            };
                            if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))   cycleLang(-1);
                            if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) cycleLang(1);
                        }
                    }
                    // Skin tone cycle
                    if (fd.isSkinTone) {
                        uint32_t cur = SaveEditor::GetUInt(gPlayerSav, h);
                        if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft)) {
                            if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
                            SaveEditor::SetUInt(gPlayerSav, h, (cur>0)?cur-1:5); gPlayerSavDirty=true;
                        }
                        if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) {
                            if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
                            SaveEditor::SetUInt(gPlayerSav, h, (cur<5)?cur+1:0); gPlayerSavDirty=true;
                        }
                    }
                    // Island size cycle (safe presets 1-4 only)
                    if (fd.isIslandSz) {
                        uint32_t cur = SaveEditor::GetAnyScalar(gPlayerSav, h);
                        if (cur<1||cur>4) cur=1;
                        if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft)) {
                            if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
                            SaveEditor::SetAnyScalar(gPlayerSav, h, (cur>1)?cur-1:4); gPlayerSavDirty=true;
                        }
                        if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) {
                            if (!gPlayerUndoValid[gPlayerFieldSel]) { gPlayerUndoVal[gPlayerFieldSel]=(int32_t)cur; gPlayerUndoValid[gPlayerFieldSel]=true; }
                            SaveEditor::SetAnyScalar(gPlayerSav, h, (cur<4)?cur+1:1); gPlayerSavDirty=true;
                        }
                    }
                    // A = keyboard for string fields
                    if (fd.isStr && (kDown&HidNpadButton_A)) {
                        std::string cur = SaveEditor::GetWStr32(gPlayerSav, h);
                        std::string nv = ShowKeyboard(fd.label, cur, 30);
                        if (nv != cur) { SaveEditor::SetWStr32(gPlayerSav, h, nv); gPlayerSavDirty=true; }
                        gPlayerMsg = ""; gPlayerMsgCol = COL_TEXT;
                    }
                    // X = pronunciation for Name/Island; X = undo for non-string fields
                    if (fd.isStr && (kDown&HidNpadButton_X)) {
                        const char* howName = (strcmp(fd.fieldName,"Player.Name")==0)      ? "Player.HowToCallName"
                                            : (strcmp(fd.fieldName,"Player.IslandName")==0) ? "Player.HowToCallIslandName"
                                            : nullptr;
                        if (howName) {
                            uint32_t hh = SaveEditor::Hash(howName);
                            std::string cur = SaveEditor::GetWStr32(gPlayerSav, hh);
                            std::string nv = ShowKeyboard("Pronunciation (optional, empty = same as name)", cur, 30);
                            if (nv != cur) { SaveEditor::SetWStr32(gPlayerSav, hh, nv); gPlayerSavDirty=true; }
                            gPlayerMsg = ""; gPlayerMsgCol = COL_TEXT;
                        }
                    }
                    if (!fd.isStr && (kDown&HidNpadButton_X)) TUndoPlayer(0);
                }
                // B = back to user pick, Plus = quit app (both prompt when dirty)
                if (kDown&(HidNpadButton_B|HidNpadButton_Plus)) {
                    bool toUserPick = (kDown&HidNpadButton_B) != 0;
                    if (gPlayerSavDirty || gUgcDirty || gWebUiDirty) {
                        gShowBackPrompt=true; gBackPromptIsMii=false; gBackPromptToUserPick=toUserPick;
                    } else if (toUserPick) {
                        FreePreview(); HttpServer::Stop(); gLog.clear();
                        gEntries.clear(); gMiis.clear();
                        gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{};
                        gPlayerSavDirty=false; gMiiSavDirty=false;
                        SaveMount::Unmount();
                        gScreen=Screen::UserPick;
                    } else {
                        gQuitApp=true;
                    }
                }

            } else if (gOnSwitchMode == OnSwitchMode::MiiStats) {
                // Lazy-load
                if (!gMiiSav.loaded) {
                    std::string err;
                    if (!SaveEditor::Load(SAVE_MII_SAV, gMiiSav, err))
                        gMiiStatsMsg = "Load error: " + err;
                }
                // Tab switch (L/R for main tabs — changes stay in memory; prompt only on back/quit)
                if (kDown&HidNpadButton_L){
                    gOnSwitchMode=OnSwitchMode::UGC; gOnSwitchMsg=""; break;
                }
                if (kDown&HidNpadButton_R){
                    gOnSwitchMode=OnSwitchMode::Player;
                    if (!gPlayerSav.loaded) {
                        std::string err;
                        if (!SaveEditor::Load(SAVE_PLAYER_SAV, gPlayerSav, err))
                            gPlayerMsg="Load error: "+err;
                    }
                    gOnSwitchMsg=""; break;
                }
                // Y cycles Stats → Belongings → Habits → Words → Relations → Social → Housing → Stats
                // (suppressed while the Housing conflict modal is open so the modal isn't orphaned).
                // Browse lives outside the cycle — pressing Y from Browse returns to Stats.
                if ((kDown&HidNpadButton_Y) && !gHousingConfirmOpen) {
                    if (gMiiStatsSubTab == MiiStatsSubTab::Browse) {
                        gMiiStatsSubTab = MiiStatsSubTab::Stats;
                    } else {
                        gMiiStatsSubTab=(MiiStatsSubTab)(((int)gMiiStatsSubTab+1)%MII_SUBTAB_COUNT);
                    }
                    gSocialScroll=0; gBlPickerOpen=false;
                    if(gMiiStatsSubTab!=MiiStatsSubTab::Social) gSocialExpanded=false;
                    if(gMiiStatsSubTab==MiiStatsSubTab::Housing) {
                        HousingRebuild();
                        gHousingNavSel = 0;   // reset card-grid nav on tab enter
                        gHousingScrollY = 0;
                    }
                }
                // X toggles global graph in Social sub-tab only
                if (kDown&HidNpadButton_X && gMiiStatsSubTab==MiiStatsSubTab::Social) gSocialExpanded=!gSocialExpanded;
                // ZL/ZR switch selected mii
                if (!gMiis.empty()) {
                    int mseVis2=(SCREEN_H-SE_TOP_Y-32)/ITEM_H;
                    auto prevMii = [&](){
                        gMiiStatsMiiSel = (gMiiStatsMiiSel>0) ? gMiiStatsMiiSel-1 : (int)gMiis.size()-1;
                        if (gMiiStatsMiiSel < gMiiStatsScroll) gMiiStatsScroll=gMiiStatsMiiSel;
                        gWordUndo.valid = false; gMiiRelSel=0; gMiiRelScroll=0; gBlPickerOpen=false;
                    };
                    auto nextMii = [&](){
                        gMiiStatsMiiSel = (gMiiStatsMiiSel+1<(int)gMiis.size()) ? gMiiStatsMiiSel+1 : 0;
                        if (gMiiStatsMiiSel >= gMiiStatsScroll+mseVis2) gMiiStatsScroll=gMiiStatsMiiSel-mseVis2+1;
                        gWordUndo.valid = false; gMiiRelSel=0; gMiiRelScroll=0; gBlPickerOpen=false;
                    };
                    // Browse uses ZL/ZR for pagination, so don't also step the slot there.
                    if (!gBlPickerOpen && gMiiStatsSubTab != MiiStatsSubTab::Browse) {
                        if (kNav&HidNpadButton_ZL) prevMii();
                        if (kNav&HidNpadButton_ZR) nextMii();
                    }
                }
                // D-pad Up/Down = field/slot nav depending on sub-tab
                if (gMiiStatsSubTab==MiiStatsSubTab::Stats) {
                    if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp))
                        gMiiStatsFieldSel = (gMiiStatsFieldSel>0) ? gMiiStatsFieldSel-1 : MII_STATS_FIELD_COUNT-1;
                    if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown))
                        gMiiStatsFieldSel = (gMiiStatsFieldSel+1<MII_STATS_FIELD_COUNT) ? gMiiStatsFieldSel+1 : 0;
                } else if (gMiiStatsSubTab==MiiStatsSubTab::Words) {
                    if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp))
                        { gMiiWordsSlotSel = (gMiiWordsSlotSel>0) ? gMiiWordsSlotSel-1 : 11; gWordUndo.valid=false; }
                    if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown))
                        { gMiiWordsSlotSel = (gMiiWordsSlotSel<11) ? gMiiWordsSlotSel+1 : 0; gWordUndo.valid=false; }
                } else if (gMiiStatsSubTab==MiiStatsSubTab::Relations) {
                    const int REL_VIS = (SCREEN_H - 32 - (SE_TOP_Y + 2)) / 34;
                    if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp)) {
                        if (gMiiRelSel > 0) {
                            gMiiRelSel--;
                            if (gMiiRelSel < gMiiRelScroll) gMiiRelScroll = gMiiRelSel;
                        }
                    }
                    if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown)) {
                        if (gMiiRelSel+1 < gMiiRelCount) {
                            gMiiRelSel++;
                            if (gMiiRelSel >= gMiiRelScroll + REL_VIS) gMiiRelScroll = gMiiRelSel - REL_VIS + 1;
                        }
                    }
                    // gMiiRelSel additionally clamped to valid range in draw phase
                } else if (gMiiStatsSubTab==MiiStatsSubTab::Belongings) {
                    if (gBlPickerOpen) {
                        int total = (int)gBlFiltered.size();
                        const int pk_vis_nav = (SCREEN_H - (SE_TOP_Y + 32) - 32 - 32) / 30;
                        auto pkScrollTo = [&]() {
                            if (gBlPickerSel < gBlPickerScroll) gBlPickerScroll = gBlPickerSel;
                            else if (gBlPickerSel >= gBlPickerScroll + pk_vis_nav) gBlPickerScroll = gBlPickerSel - pk_vis_nav + 1;
                        };
                        if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp))
                            { gBlPickerSel = (gBlPickerSel > 0) ? gBlPickerSel - 1 : total - 1; pkScrollTo(); }
                        if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown))
                            { gBlPickerSel = (gBlPickerSel + 1 < total) ? gBlPickerSel + 1 : 0; pkScrollTo(); }
                        if (total > 0) {
                            if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))
                                { gBlPickerSel = std::max(0, gBlPickerSel - 12); pkScrollTo(); }
                            if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight))
                                { gBlPickerSel = std::min(total - 1, gBlPickerSel + 12); pkScrollTo(); }
                        }
                    } else {
                        // 2D grid nav for the card layout:
                        //   sel 0..7  — Worn cloth grid (4 cols × 2 rows)
                        //   sel 8     — Coord card (full-width, own row)
                        //   sel 9..20 — Pocket grid (4 cols × 3 rows)
                        //   sel 21..24— Ownership actions (4 buttons in 1 row)
                        auto moveBlSel = [&](int dir) -> int {
                            // dir: 0=up 1=down 2=left 3=right
                            int s = gBlSel;
                            if (s < 0 || s >= BL_SEL_MAX) return 0;
                            if (s < 8) {
                                int col = s % 4, row = s / 4;
                                if (dir == 0) return row > 0 ? s - 4 : 21 + col;
                                if (dir == 1) return row < 1 ? s + 4 : 8;
                                if (dir == 2) return col > 0 ? s - 1 : s;
                                if (dir == 3) return col < 3 ? s + 1 : s;
                            } else if (s == 8) {
                                if (dir == 0) return 4;
                                if (dir == 1) return 9;
                                return s;
                            } else if (s <= 20) {
                                int p = s - 9, col = p % 4, row = p / 4;
                                if (dir == 0) return row > 0 ? s - 4 : 8;
                                if (dir == 1) return row < 2 ? s + 4 : 21 + col;
                                if (dir == 2) return col > 0 ? s - 1 : s;
                                if (dir == 3) return col < 3 ? s + 1 : s;
                            } else {
                                int col = s - 21;
                                if (dir == 0) return 17 + col;     // ↑ to pocket bottom row
                                if (dir == 1) return col;          // ↓ wraps to worn top row
                                if (dir == 2) return col > 0 ? s - 1 : s;
                                if (dir == 3) return col < 3 ? s + 1 : s;
                            }
                            return s;
                        };
                        if (kNav&(HidNpadButton_Up   |HidNpadButton_StickLUp   |HidNpadButton_StickRUp))    gBlSel = moveBlSel(0);
                        if (kNav&(HidNpadButton_Down |HidNpadButton_StickLDown |HidNpadButton_StickRDown))  gBlSel = moveBlSel(1);
                        if (kNav&(HidNpadButton_Left |HidNpadButton_StickLLeft |HidNpadButton_StickRLeft))  gBlSel = moveBlSel(2);
                        if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) gBlSel = moveBlSel(3);
                    }
                } else if (gMiiStatsSubTab==MiiStatsSubTab::Habits) {
                    int catSize = 0;
                    for (int i = 0; i < HABIT_COUNT; i++) if (HABITS[i].category == gHabitCatSel) catSize++;
                    if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp))
                        gHabitItemSel = (gHabitItemSel > 0) ? gHabitItemSel-1 : std::max(0, catSize-1);
                    if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown))
                        gHabitItemSel = (gHabitItemSel+1 < catSize) ? gHabitItemSel+1 : 0;
                    if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))
                        { gHabitCatSel = (gHabitCatSel>0) ? gHabitCatSel-1 : HABIT_CAT_COUNT-1; gHabitItemSel=0; gHabitScroll=0; }
                    if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight))
                        { gHabitCatSel = (gHabitCatSel+1<HABIT_CAT_COUNT) ? gHabitCatSel+1 : 0; gHabitItemSel=0; gHabitScroll=0; }
                } else if (gMiiStatsSubTab==MiiStatsSubTab::Housing) {
                    // Housing card-grid nav: 2D selection over gHousingNav items.
                    // For each direction, find the candidate item whose centre is
                    // furthest in that direction with the smallest sideways drift.
                    if (!gHousingNav.empty()) {
                        bool up    = kNav&(HidNpadButton_Up   |HidNpadButton_StickLUp   |HidNpadButton_StickRUp);
                        bool down  = kNav&(HidNpadButton_Down |HidNpadButton_StickLDown |HidNpadButton_StickRDown);
                        bool left  = kNav&(HidNpadButton_Left |HidNpadButton_StickLLeft |HidNpadButton_StickRLeft);
                        bool right = kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight);
                        if (up || down || left || right) {
                            if (gHousingNavSel < 0 || gHousingNavSel >= (int)gHousingNav.size())
                                gHousingNavSel = 0;
                            const auto& cur = gHousingNav[gHousingNavSel];
                            int curCx = cur.x + cur.w/2, curCy = cur.y + cur.h/2;
                            // Horizontal nav: gate by Y proximity so a small
                            // "vacate" button above a slot row in the same card
                            // can't get picked when the user is moving across
                            // cards — that was the "highlight → nothing →
                            // highlight" stutter. Same for vertical nav.
                            int yGate = std::max(cur.h, 24);
                            int xGate = std::max(cur.w / 2, 24);
                            int best = -1; long bestScore = 0;
                            for (int i = 0; i < (int)gHousingNav.size(); i++) {
                                if (i == gHousingNavSel) continue;
                                const auto& it = gHousingNav[i];
                                int cx2 = it.x + it.w/2, cy2 = it.y + it.h/2;
                                int dx = cx2 - curCx, dy = cy2 - curCy;
                                bool dir = false;
                                long score = 0;
                                if (up    && dy < -2) {
                                    if (std::abs(dx) > xGate) continue;
                                    dir = true; score = (long)(-dy) + (long)std::abs(dx)*8;
                                }
                                if (down  && dy >  2) {
                                    if (std::abs(dx) > xGate) continue;
                                    dir = true; score = (long)( dy) + (long)std::abs(dx)*8;
                                }
                                if (left  && dx < -2) {
                                    if (std::abs(dy) > yGate) continue;
                                    dir = true; score = (long)(-dx) + (long)std::abs(dy)*8;
                                }
                                if (right && dx >  2) {
                                    if (std::abs(dy) > yGate) continue;
                                    dir = true; score = (long)( dx) + (long)std::abs(dy)*8;
                                }
                                if (!dir) continue;
                                if (best < 0 || score < bestScore) { best = i; bestScore = score; }
                            }
                            // If the gated search returned nothing (e.g. very
                            // tall cards mean no neighbour shares a y-band),
                            // retry without the gate so we don't dead-end.
                            if (best < 0) {
                                for (int i = 0; i < (int)gHousingNav.size(); i++) {
                                    if (i == gHousingNavSel) continue;
                                    const auto& it = gHousingNav[i];
                                    int cx2 = it.x + it.w/2, cy2 = it.y + it.h/2;
                                    int dx = cx2 - curCx, dy = cy2 - curCy;
                                    bool dir = false;
                                    long score = 0;
                                    if (up    && dy < -2) { dir = true; score = (long)(-dy) + (long)std::abs(dx)*4; }
                                    if (down  && dy >  2) { dir = true; score = (long)( dy) + (long)std::abs(dx)*4; }
                                    if (left  && dx < -2) { dir = true; score = (long)(-dx) + (long)std::abs(dy)*4; }
                                    if (right && dx >  2) { dir = true; score = (long)( dx) + (long)std::abs(dy)*4; }
                                    if (!dir) continue;
                                    if (best < 0 || score < bestScore) { best = i; bestScore = score; }
                                }
                            }
                            if (best >= 0) gHousingNavSel = best;
                        }
                    }
                } else if (gMiiStatsSubTab==MiiStatsSubTab::Browse) {
                    int N = (int)gShareMiis.size();
                    if (gShareDetailOpen) {
                        // In detail overlay: ZL/ZR cycles which of YOUR miis on the
                        // left is selected — that slot is what gets overwritten on
                        // import. A confirms, B closes.
                        int M = (int)gMiis.size();
                        if (M > 0) {
                            if (kNav & HidNpadButton_ZL)
                                gMiiStatsMiiSel = (gMiiStatsMiiSel > 0) ? gMiiStatsMiiSel - 1 : M - 1;
                            if (kNav & HidNpadButton_ZR)
                                gMiiStatsMiiSel = (gMiiStatsMiiSel + 1 < M) ? gMiiStatsMiiSel + 1 : 0;
                            if (gMiiStatsMiiSel >= 0 && gMiiStatsMiiSel < M)
                                gShareImportSlot = gMiis[gMiiStatsMiiSel].slot;
                        }
                    } else {
                        if (N > 0) {
                            // Up/Down jumps a row, Left/Right walks one card.
                            // Clamps at edges so the page-nav handler below
                            // can pick up the same press to flip pages.
                            constexpr int SHARE_COLS = 3;
                            if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp))
                                gShareSel = std::max(0, gShareSel - SHARE_COLS);
                            if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown))
                                gShareSel = std::min(N - 1, gShareSel + SHARE_COLS);
                            if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))
                                if (gShareSel > 0)     gShareSel--;
                            if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight))
                                if (gShareSel < N - 1) gShareSel++;
                        }
                        // ZL/ZR moves selection in YOUR mii list on the left.
                        // The overwrite target follows that selection — so picking
                        // a different mii on the left changes which slot will be
                        // replaced when you import. Left/Right is page nav (below).
                        int M = (int)gMiis.size();
                        if (M > 0) {
                            if (kNav & HidNpadButton_ZL)
                                gMiiStatsMiiSel = (gMiiStatsMiiSel > 0) ? gMiiStatsMiiSel - 1 : M - 1;
                            if (kNav & HidNpadButton_ZR)
                                gMiiStatsMiiSel = (gMiiStatsMiiSel + 1 < M) ? gMiiStatsMiiSel + 1 : 0;
                            if (gMiiStatsMiiSel >= 0 && gMiiStatsMiiSel < M)
                                gShareImportSlot = gMiis[gMiiStatsMiiSel].slot;
                        }
                    }
                }
                // Edit (Words sub-tab)
                if (gMiiStatsSubTab==MiiStatsSubTab::Words && gMiiSav.loaded && !gMiis.empty() && gMiiStatsMiiSel<(int)gMiis.size()) {
                    int miiIdx = gMiis[gMiiStatsMiiSel].slot - 1;
                    static const uint32_t H_WKIND_I  = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordKind");
                    static const uint32_t H_WTXT_I   = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordText");
                    static const uint32_t H_WHOW_I   = SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordHowToCall");
                    static const uint32_t H_WREGION_I= SaveEditor::Hash("Mii.MiiMisc.WordInfo.WordArray.WordRegionLanguageID");
                    static const uint32_t H_WINV_I   = SaveEditor::Hash("Invalid");
                    static const uint32_t H_WUSEN_I  = SaveEditor::Hash("USen");
                    int wIdx = miiIdx * 12 + gMiiWordsSlotSel;
                    uint32_t curKind = SaveEditor::GetAnyEnumAt(gMiiSav, H_WKIND_I, wIdx, H_WINV_I);
                    int kIdx = WordKindIndex(curKind);
                    if (kIdx < 0) kIdx = 0;
                    auto captureWordUndo = [&]() {
                        if (!gWordUndo.valid) {
                            gWordUndo.kind  = curKind;
                            gWordUndo.text  = SaveEditor::GetWStr64At(gMiiSav, H_WTXT_I, wIdx);
                            gWordUndo.how   = SaveEditor::GetWStr64At(gMiiSav, H_WHOW_I, wIdx);
                            gWordUndo.valid = true;
                        }
                    };
                    // Left/Right: cycle kind
                    auto cycleKind = [&](int dir) {
                        captureWordUndo();
                        int nk = (kIdx + dir + WORD_KIND_COUNT) % WORD_KIND_COUNT;
                        uint32_t nHash = SaveEditor::Hash(WORD_KIND_LIST[nk].name);
                        SaveEditor::SetAnyEnumAt(gMiiSav, H_WKIND_I, wIdx, nHash);
                        if (curKind == H_WINV_I && nk != 0) {
                            // activating from empty: auto-set region to USen
                            SaveEditor::SetAnyEnumAt(gMiiSav, H_WREGION_I, wIdx, H_WUSEN_I);
                        }
                        gMiiSavDirty = true;
                    };
                    if (kDown&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))   cycleKind(-1);
                    if (kDown&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) cycleKind(+1);
                    // A: edit text
                    if (kDown&HidNpadButton_A) {
                        std::string cur = SaveEditor::GetWStr64At(gMiiSav, H_WTXT_I, wIdx);
                        std::string nv = ShowKeyboard("Word Text (max 63 chars)", cur, 63);
                        if (nv != cur) {
                            captureWordUndo();
                            SaveEditor::SetWStr64At(gMiiSav, H_WTXT_I, wIdx, nv);
                            if (!nv.empty() && curKind == H_WINV_I) {
                                SaveEditor::SetAnyEnumAt(gMiiSav, H_WKIND_I, wIdx, SaveEditor::Hash("Phrase"));
                                SaveEditor::SetAnyEnumAt(gMiiSav, H_WREGION_I, wIdx, H_WUSEN_I);
                            }
                            gMiiSavDirty = true;
                        }
                        gMiiStatsMsg=""; gMiiStatsMsgCol=COL_TEXT;
                    }
                    // X: edit pronunciation
                    if (kDown&HidNpadButton_X) {
                        std::string cur = SaveEditor::GetWStr64At(gMiiSav, H_WHOW_I, wIdx);
                        std::string nv = ShowKeyboard("Pronunciation (empty = same as text)", cur, 63);
                        if (nv != cur) {
                            captureWordUndo();
                            SaveEditor::SetWStr64At(gMiiSav, H_WHOW_I, wIdx, nv);
                            gMiiSavDirty = true;
                        }
                        gMiiStatsMsg=""; gMiiStatsMsgCol=COL_TEXT;
                    }
                    // Minus: undo
                    if (kDown&HidNpadButton_Minus) TUndoWords(0);
                }
                // Edit (Relations sub-tab)
                if (gMiiStatsSubTab==MiiStatsSubTab::Relations && gMiiSav.loaded && !gMiis.empty() && gMiiStatsMiiSel<(int)gMiis.size()) {
                    if (gMiiRelPairIdx >= 0) {
                        static const uint32_t H_BASE_RI  = 0x8b41897eu;
                        static const uint32_t H_METER_RI = 0x42c2fc2fu;
                        int myDir = gMiiRelSelfA ? gMiiRelPairIdx*2   : gMiiRelPairIdx*2+1;
                        int otDir = gMiiRelSelfA ? gMiiRelPairIdx*2+1 : gMiiRelPairIdx*2;
                        uint32_t curType = SaveEditor::GetEnumAt(gMiiSav, H_BASE_RI, myDir, 0x0784a8dcu);
                        int tIdx = 0;
                        for (int ti=0; ti<RELATION_TYPE_COUNT; ti++)
                            if (RELATION_TYPE_NAMES[ti].hash == curType) { tIdx=ti; break; }
                        auto cycleType = [&](int dir) {
                            int nk = (tIdx + dir + RELATION_TYPE_COUNT) % RELATION_TYPE_COUNT;
                            uint32_t nHash = RELATION_TYPE_NAMES[nk].hash;
                            SaveEditor::SetEnumAt(gMiiSav, H_BASE_RI, myDir, nHash);
                            SaveEditor::SetEnumAt(gMiiSav, H_BASE_RI, otDir, RelTypeCounterpart(nHash));
                            if (RelMeterFixed(nHash)) {
                                SaveEditor::SetIntAt(gMiiSav, H_METER_RI, myDir, 100);
                                SaveEditor::SetIntAt(gMiiSav, H_METER_RI, otDir, 100);
                            }
                            gMiiSavDirty = true;
                        };
                        if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))   cycleType(-1);
                        if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) cycleType(+1);
                    }
                    if (gRelDateKbdReq) {
                        gRelDateKbdReq = false;
                        static const uint32_t H_TST_KBD = 0x1a892e50u;
                        uint64_t cur = SaveEditor::GetUInt64At(gMiiSav, H_TST_KBD, gRelDatePairIdx);
                        char initBuf[36] = "";
                        if (cur > 0) {
                            time_t t = (time_t)cur;
                            struct tm* ti = localtime(&t);
                            snprintf(initBuf, sizeof(initBuf), "%04d-%02d-%02d",
                                     ti->tm_year+1900, ti->tm_mon+1, ti->tm_mday);
                        }
                        std::string raw = ShowKeyboard("Since (YYYY-MM-DD)", initBuf, 10);
                        int y=0, mo=0, d=0;
                        if (sscanf(raw.c_str(), "%d-%d-%d", &y, &mo, &d)==3
                            && y>1900 && mo>=1 && mo<=12 && d>=1 && d<=31) {
                            struct tm t = {};
                            t.tm_year = y-1900; t.tm_mon = mo-1; t.tm_mday = d; t.tm_hour = 12;
                            time_t secs = mktime(&t);
                            if (secs >= 0) {
                                SaveEditor::SetUInt64At(gMiiSav, H_TST_KBD, gRelDatePairIdx, (uint64_t)secs);
                                gMiiSavDirty = true;
                            }
                        }
                    }
                }
                // Edit (Belongings sub-tab)
                if (gMiiStatsSubTab==MiiStatsSubTab::Belongings && gMiiSav.loaded && !gMiis.empty() && gMiiStatsMiiSel<(int)gMiis.size()) {
                    int miiIdx = gMiis[gMiiStatsMiiSel].slot - 1;
                    int blSel  = gBlSel;

                    // Helper: cycle color for worn slot (Left/Right)
                    auto cycleColor = [&](int dir) {
                        if (blSel < 8) {
                            const auto& ws = BL_WORN_DEFS[blSel];
                            uint32_t ih = SaveEditor::GetAnyEnumAt(gMiiSav, ws.kh, miiIdx, BL_H_INVALID);
                            if (!ih || ih == BL_H_INVALID) return;
                            int cc = 1;
                            for (int k=0;k<BL_CLOTH_COUNT;k++) if(BL_CLOTH_ITEMS[k].nameHash==ih){cc=BL_CLOTH_ITEMS[k].colorCount;break;}
                            int cur = SaveEditor::GetIntAt(gMiiSav, ws.ch, miiIdx);
                            SaveEditor::SetIntAt(gMiiSav, ws.ch, miiIdx, ((cur+dir)%cc+cc)%cc);
                            gMiiSavDirty = true;
                        } else if (blSel == 8) {
                            uint32_t ih = SaveEditor::GetAnyEnumAt(gMiiSav, BL_COORD_KH, miiIdx, BL_H_INVALID);
                            if (!ih || ih == BL_H_INVALID) return;
                            int cc = 1;
                            for (int k=0;k<BL_COORD_COUNT;k++) if(BL_COORD_ITEMS[k].keyHash==ih){cc=BL_COORD_ITEMS[k].colorCount;break;}
                            int cur = SaveEditor::GetIntAt(gMiiSav, BL_COORD_CH, miiIdx);
                            SaveEditor::SetIntAt(gMiiSav, BL_COORD_CH, miiIdx, ((cur+dir)%cc+cc)%cc);
                            gMiiSavDirty = true;
                        }
                    };

                    // Helper: apply selected item from picker
                    auto applyPickerItem = [&](uint32_t newHash) {
                        if (blSel < 8) {
                            const auto& ws = BL_WORN_DEFS[blSel];
                            uint32_t old = SaveEditor::GetAnyEnumAt(gMiiSav, ws.kh, miiIdx, BL_H_INVALID);
                            SaveEditor::SetAnyEnumAt(gMiiSav, ws.kh, miiIdx, newHash);
                            if (newHash != old) SaveEditor::SetIntAt(gMiiSav, ws.ch, miiIdx, 0);
                            gMiiSavDirty = true;
                        } else if (blSel == 8) {
                            uint32_t old = SaveEditor::GetAnyEnumAt(gMiiSav, BL_COORD_KH, miiIdx, BL_H_INVALID);
                            SaveEditor::SetAnyEnumAt(gMiiSav, BL_COORD_KH, miiIdx, newHash);
                            if (newHash != old) SaveEditor::SetIntAt(gMiiSav, BL_COORD_CH, miiIdx, 0);
                            gMiiSavDirty = true;
                        } else if (blSel >= 9 && blSel < 21) {
                            int ai = miiIdx * BL_GOODS_SLOTS_MII + (blSel - 9);
                            if (newHash == 0u) {
                                SaveEditor::SetUIntAt(gMiiSav, BL_H_GOODS_ID, ai, 0u);
                                SaveEditor::SetUInt64At(gMiiSav, BL_H_GOODS_TIME, ai, 0ull);
                                SaveEditor::SetIntAt(gMiiSav, BL_H_GOODS_UGC, ai, -1);
                            } else {
                                SaveEditor::SetUIntAt(gMiiSav, BL_H_GOODS_ID, ai, newHash);
                                uint64_t t = SaveEditor::GetUInt64At(gMiiSav, BL_H_GOODS_TIME, ai);
                                if (!t) SaveEditor::SetUInt64At(gMiiSav, BL_H_GOODS_TIME, ai, 1735689600ull);
                                SaveEditor::SetIntAt(gMiiSav, BL_H_GOODS_UGC, ai, -1);
                            }
                            gMiiSavDirty = true;
                        }
                    };

                    if (gBlPickerOpen) {
                        // A = confirm, B = cancel, Y = search, X = clear filter
                        if (kDown&HidNpadButton_A && !gBlFiltered.empty()) {
                            int realIdx = gBlFiltered[std::max(0, std::min(gBlPickerSel, (int)gBlFiltered.size()-1))];
                            applyPickerItem(gBlPickerHashes[realIdx]);
                            gBlPickerOpen = false;
                        }
                        if (kDown&HidNpadButton_Minus) {
                            std::string q = ShowKeyboard("Filter items (substring, case-insensitive)", gBlFilter, 30);
                            // lowercase
                            for (auto& c : q) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                            gBlFilter = q;
                            BlRebuildFilter();
                            gBlPickerSel = 0;
                            gBlPickerScroll = 0;
                        }
                        if (kDown&HidNpadButton_X && !gBlFilter.empty()) {
                            gBlFilter.clear();
                            BlRebuildFilter();
                            gBlPickerSel = 0;
                            gBlPickerScroll = 0;
                        }
                    } else {
                        // A = open item picker (Left/Right are reserved for
                        // grid navigation in the card layout).
                        if (kDown&HidNpadButton_A && blSel < 21)
                            BlOpenPicker(blSel, miiIdx);
                        // Minus cycles color forward on the focused slot
                        // (touch users get a dedicated chip on each card).
                        if (kDown & HidNpadButton_Minus) cycleColor(+1);
                    }

                    // X = clear selected slot (only when picker is closed)
                    if (!gBlPickerOpen && kDown&HidNpadButton_X) {
                        if (blSel < 8) {
                            const auto& ws = BL_WORN_DEFS[blSel];
                            SaveEditor::SetAnyEnumAt(gMiiSav, ws.kh, miiIdx, BL_H_INVALID);
                            SaveEditor::SetIntAt(gMiiSav, ws.ch, miiIdx, 0);
                            gMiiSavDirty = true;
                        } else if (blSel == 8) {
                            SaveEditor::SetAnyEnumAt(gMiiSav, BL_COORD_KH, miiIdx, BL_H_INVALID);
                            SaveEditor::SetIntAt(gMiiSav, BL_COORD_CH, miiIdx, 0);
                            gMiiSavDirty = true;
                        } else if (blSel >= 9 && blSel < 21) {
                            int ai = miiIdx * BL_GOODS_SLOTS_MII + (blSel - 9);
                            SaveEditor::SetUIntAt(gMiiSav, BL_H_GOODS_ID, ai, 0u);
                            SaveEditor::SetUInt64At(gMiiSav, BL_H_GOODS_TIME, ai, 0ull);
                            SaveEditor::SetIntAt(gMiiSav, BL_H_GOODS_UGC, ai, -1);
                            gMiiSavDirty = true;
                        }
                    }

                    // Action rows (A to execute, only when picker is closed)
                    if (!gBlPickerOpen && kDown&HidNpadButton_A && blSel >= 21) {
                        int act = blSel - 21;
                        int total;
                        if (act == 0) { // Unlock All Clothes
                            total = SaveEditor::ArraySize(gMiiSav, BL_H_CLOTH_OWN);
                            for (int i=0;i<BL_CLOTH_COUNT;i++) {
                                int idx = miiIdx * BL_SLOTS_PER_MII + BL_CLOTH_ITEMS[i].index;
                                if (idx < total) {
                                    int cc = BL_CLOTH_ITEMS[i].colorCount;
                                    uint32_t mask = (cc>=32) ? 0xFFFFFFFFu : ((1u<<cc)-1u);
                                    SaveEditor::SetUIntAt(gMiiSav, BL_H_CLOTH_OWN, idx, mask);
                                }
                            }
                            gMiiSavDirty = true;
                            gMiiStatsMsg = "All clothes unlocked"; gMiiStatsMsgCol = COL_GREEN;
                        } else if (act == 1) { // Lock All Clothes
                            total = SaveEditor::ArraySize(gMiiSav, BL_H_CLOTH_OWN);
                            for (int i=0;i<BL_SLOTS_PER_MII;i++) {
                                int idx = miiIdx * BL_SLOTS_PER_MII + i;
                                if (idx < total) SaveEditor::SetUIntAt(gMiiSav, BL_H_CLOTH_OWN, idx, 0u);
                            }
                            gMiiSavDirty = true;
                            gMiiStatsMsg = "All clothes locked"; gMiiStatsMsgCol = COL_GREEN;
                        } else if (act == 2) { // Unlock All Coords
                            total = SaveEditor::ArraySize(gMiiSav, BL_H_COORD_OWN);
                            for (int i=0;i<BL_COORD_COUNT;i++) {
                                int idx = miiIdx * BL_COORD_SLOTS_MII + BL_COORD_ITEMS[i].saveIndex;
                                if (idx < total) {
                                    int cc = BL_COORD_ITEMS[i].colorCount;
                                    uint32_t mask = (cc>=32) ? 0xFFFFFFFFu : ((1u<<cc)-1u);
                                    SaveEditor::SetUIntAt(gMiiSav, BL_H_COORD_OWN, idx, mask);
                                }
                            }
                            gMiiSavDirty = true;
                            gMiiStatsMsg = "All coordinates unlocked"; gMiiStatsMsgCol = COL_GREEN;
                        } else { // Lock All Coords
                            total = SaveEditor::ArraySize(gMiiSav, BL_H_COORD_OWN);
                            for (int i=0;i<BL_COORD_SLOTS_MII;i++) {
                                int idx = miiIdx * BL_COORD_SLOTS_MII + i;
                                if (idx < total) SaveEditor::SetUIntAt(gMiiSav, BL_H_COORD_OWN, idx, 0u);
                            }
                            gMiiSavDirty = true;
                            gMiiStatsMsg = "All coordinates locked"; gMiiStatsMsgCol = COL_GREEN;
                        }
                    }
                }
                // Edit (Habits sub-tab)
                if (gMiiStatsSubTab==MiiStatsSubTab::Habits && gMiiSav.loaded && !gMiis.empty() && gMiiStatsMiiSel<(int)gMiis.size()) {
                    EnsureHabitHashes();
                    int miiIdx = gMiis[gMiiStatsMiiSel].slot - 1;
                    std::vector<int> items = HabitsInCategory(gHabitCatSel);
                    if (gHabitItemSel >= (int)items.size()) gHabitItemSel = std::max(0,(int)items.size()-1);

                    // A: toggle active habit (radio: ensure exclusive within category)
                    if (kDown&HidNpadButton_A && !items.empty()) {
                        int chosen = items[gHabitItemSel];
                        for (int idx : items) {
                            bool active = (idx == chosen);
                            SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[idx].isChecked, miiIdx, active);
                            if (active) {
                                SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[idx].isOwn, miiIdx, true);
                                SaveEditor::SetEnumAt(gMiiSav, gHabitHashes[idx].state, miiIdx, gHabitStateOwnH);
                            }
                        }
                        gMiiSavDirty = true;
                        gMiiStatsMsg = std::string("Active: ") + HABITS[chosen].label;
                        gMiiStatsMsgCol = COL_GREEN;
                        gMiiStatsMsgFrames = 120; // ~2s at 60fps
                    }

                    // X: toggle owned (without affecting active selection unless un-owning the active one)
                    if (kDown&HidNpadButton_X && !items.empty()) {
                        int chosen = items[gHabitItemSel];
                        bool isOwn = SaveEditor::GetBoolAt(gMiiSav, gHabitHashes[chosen].isOwn, miiIdx, false);
                        if (isOwn) {
                            uint32_t st = SaveEditor::GetEnumAt(gMiiSav, gHabitHashes[chosen].state, miiIdx, gHabitStateNeverOwnedH);
                            SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[chosen].isOwn, miiIdx, false);
                            SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[chosen].isChecked, miiIdx, false);
                            if (st == gHabitStateOwnH)
                                SaveEditor::SetEnumAt(gMiiSav, gHabitHashes[chosen].state, miiIdx, gHabitStateUnownH);
                        } else {
                            SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[chosen].isOwn, miiIdx, true);
                            SaveEditor::SetEnumAt(gMiiSav, gHabitHashes[chosen].state, miiIdx, gHabitStateOwnH);
                        }
                        gMiiSavDirty = true;
                        gMiiStatsMsg = std::string(isOwn?"Removed: ":"Added: ") + HABITS[chosen].label;
                        gMiiStatsMsgCol = COL_GREEN;
                        gMiiStatsMsgFrames = 120;
                    }

                    // Minus: clear category (all habits in this category → NeverOwned)
                    if (kDown&HidNpadButton_Minus && !items.empty()) {
                        for (int idx : items) {
                            SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[idx].isChecked, miiIdx, false);
                            SaveEditor::SetBoolAt(gMiiSav, gHabitHashes[idx].isOwn, miiIdx, false);
                            SaveEditor::SetEnumAt(gMiiSav, gHabitHashes[idx].state, miiIdx, gHabitStateNeverOwnedH);
                        }
                        gMiiSavDirty = true;
                        gMiiStatsMsg = std::string("Cleared: ") + HABIT_CAT_LABEL[gHabitCatSel];
                        gMiiStatsMsgCol = COL_GREEN;
                        gMiiStatsMsgFrames = 120;
                    }
                }
                // Edit (Browse sub-tab) — handled before global B/A bindings.
                if (gMiiStatsSubTab == MiiStatsSubTab::Browse) {
                    if (gShareDetailOpen) {
                        if (kDown & HidNpadButton_A) ShareDoImport();
                        if (kDown & HidNpadButton_B) { ShareCloseDetail(); kDown &= ~HidNpadButton_B; }
                    } else {
                        if (kDown & HidNpadButton_A) ShareOpenDetail();
                        if (kDown & HidNpadButton_X) {
                            std::string nq = ShowKeyboard("Search miis (2+ chars; empty = all)", gShareQuery, 60);
                            if (nq != gShareQuery) {
                                gShareQuery = nq;
                                gSharePage = 1;
                                if (nq.size() == 1) {
                                    ShareSetStatus("Search needs at least 2 characters — showing all miis.",
                                                   {220,180,80,255}, 240);
                                }
                                ShareLoadPage();
                            }
                        }
                        if (kDown & HidNpadButton_Minus) {
                            // Cycle sort + toggle "importable only" via combined cycle on Minus
                            gShareSort = (gShareSort + 1) % 3;
                            gSharePage = 1; ShareLoadPage();
                        }
                        // Left/Right at the EDGE of the current grid flips
                        // pages. Inside the grid the same input walks card by
                        // card (handled above). ZL/ZR stays reserved for the
                        // overwrite-slot cycle on the left list.
                        if (!gShareListLoading) {
                            int N2 = (int)gShareMiis.size();
                            bool left  = kNav & (HidNpadButton_Left  | HidNpadButton_StickLLeft  | HidNpadButton_StickRLeft);
                            bool right = kNav & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_StickRRight);
                            bool atFirst = (N2 == 0 || gShareSel == 0);
                            bool atLast  = (N2 == 0 || gShareSel == N2 - 1);
                            if (left  && atFirst && gSharePage > 1)              { gSharePage--; ShareLoadPage(); }
                            if (right && atLast  && gSharePage < gShareLastPage) { gSharePage++; ShareLoadPage(); }
                        }
                        // B from the Browse list goes back to Stats (Browse isn't in the
                        // pill bar, so this gives users a one-press exit). Swallow B so
                        // the global handler below doesn't also navigate to UserPick.
                        if (kDown & HidNpadButton_B) {
                            gMiiStatsSubTab = MiiStatsSubTab::Stats;
                            kDown &= ~HidNpadButton_B;
                        }
                    }
                }
                // Edit (Housing sub-tab) — card-grid action handlers.
                if (gMiiStatsSubTab==MiiStatsSubTab::Housing && gMiiSav.loaded && !gMiis.empty()) {
                    // A activates whatever the navigation focus is on.
                    if ((kDown&HidNpadButton_A) && gHousingNavSel >= 0 &&
                        gHousingNavSel < (int)gHousingNav.size()) {
                        THousingNavAct(gHousingNavSel);
                    }
                    // Minus evicts the currently-picked mii (matches the action-bar Evict).
                    if ((kDown&HidNpadButton_Minus) && gHousingPicked >= 0) {
                        HousingDoEvictPicked();
                    }
                    // B cancels a pick first; falls through to back-out if no pick.
                    if ((kDown&HidNpadButton_B) && gHousingPicked >= 0) {
                        HousingDoCancel();
                        kDown &= ~HidNpadButton_B;
                    }
                }
                // Edit field (Stats sub-tab only)
                if (gMiiStatsSubTab==MiiStatsSubTab::Stats && gMiiSav.loaded && !gMiis.empty() && gMiiStatsMiiSel<(int)gMiis.size()) {
                    int miiIdx = gMiis[gMiiStatsMiiSel].slot - 1;
                    const auto& fd = MII_STATS_FIELDS[gMiiStatsFieldSel];
                    int scale = gTouchScale;
                    gTouchScale=1;
                    auto adjustInt = [&](int delta){
                        uint32_t h = SaveEditor::Hash(fd.fieldName);
                        int32_t v = SaveEditor::GetIntAt(gMiiSav, h, miiIdx);
                        if (!gMiiUndoValid[gMiiStatsFieldSel]) { gMiiUndoVal[gMiiStatsFieldSel]=v; gMiiUndoValid[gMiiStatsFieldSel]=true; }
                        v += delta;
                        if (fd.minVal!=-1 && v < fd.minVal) v=fd.minVal;
                        if (fd.maxVal!=-1 && v > fd.maxVal) v=fd.maxVal;
                        SaveEditor::SetIntAt(gMiiSav, h, miiIdx, v);
                        gMiiSavDirty=true;
                    };
                    auto adjustUInt = [&](int delta){
                        uint32_t h = SaveEditor::Hash(fd.fieldName);
                        uint32_t v = SaveEditor::GetUIntAt(gMiiSav, h, miiIdx);
                        if (!gMiiUndoValid[gMiiStatsFieldSel]) { gMiiUndoVal[gMiiStatsFieldSel]=(int32_t)v; gMiiUndoValid[gMiiStatsFieldSel]=true; }
                        int64_t nv = (int64_t)v + delta;
                        if (nv < 0) nv=0;
                        if (fd.minVal>=0 && nv < (int64_t)fd.minVal) nv=fd.minVal;
                        if (fd.maxVal!=-1 && nv > (int64_t)fd.maxVal) nv=fd.maxVal;
                        SaveEditor::SetUIntAt(gMiiSav, h, miiIdx, (uint32_t)nv);
                        gMiiSavDirty=true;
                    };
                    if (!fd.isStr) {
                        if (fd.isEnum) {
                            // cycle through FEELING_LABELS on Left/Right
                            uint32_t fh = SaveEditor::Hash(fd.fieldName);
                            uint32_t cur = SaveEditor::GetEnumAt(gMiiSav, fh, miiIdx);
                            int curIdx = 0;
                            for (int fi=0; fi<FEELING_LABEL_COUNT; fi++)
                                if (FEELING_LABELS[fi].hash==cur) { curIdx=fi; break; }
                            if (kDown&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft)) {
                                if (!gMiiUndoValid[gMiiStatsFieldSel]) { gMiiUndoVal[gMiiStatsFieldSel]=(int32_t)cur; gMiiUndoValid[gMiiStatsFieldSel]=true; }
                                curIdx = (curIdx>0) ? curIdx-1 : FEELING_LABEL_COUNT-1;
                                SaveEditor::SetEnumAt(gMiiSav, fh, miiIdx, FEELING_LABELS[curIdx].hash);
                                gMiiSavDirty=true;
                            }
                            if (kDown&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) {
                                if (!gMiiUndoValid[gMiiStatsFieldSel]) { gMiiUndoVal[gMiiStatsFieldSel]=(int32_t)cur; gMiiUndoValid[gMiiStatsFieldSel]=true; }
                                curIdx = (curIdx+1<FEELING_LABEL_COUNT) ? curIdx+1 : 0;
                                SaveEditor::SetEnumAt(gMiiSav, fh, miiIdx, FEELING_LABELS[curIdx].hash);
                                gMiiSavDirty=true;
                            }
                        } else {
                            if (gBtnTouchActive) {
                                // Touch button tapped — apply value change
                                gBtnTouchActive = false;
                                if (kDown&HidNpadButton_Left)  { if (fd.isUInt) adjustUInt(-scale); else adjustInt(-scale); }
                                if (kDown&HidNpadButton_Right) { if (fd.isUInt) adjustUInt(scale);  else adjustInt(scale);  }
                            } else {
                                // Left/Right (D-pad or stick): navigate button focus
                                if (kNav&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))   gMiiBtnSel = (gMiiBtnSel-1+6)%6;
                                if (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) gMiiBtnSel = (gMiiBtnSel+1)%6;
                                // A fires focused button
                                if (kDown&HidNpadButton_A && !fd.isStr) {
                                    static const int MSTEPS[]={100,10,1};
                                    int mdelta = (gMiiBtnSel<3) ? -MSTEPS[gMiiBtnSel] : MSTEPS[5-gMiiBtnSel];
                                    if (fd.isUInt) adjustUInt(mdelta); else adjustInt(mdelta);
                                }
                            }
                        }
                    }
                    if (fd.isStr && gMiiNameKbdReq) {
                        gMiiNameKbdReq = false;
                        uint32_t h = SaveEditor::Hash(fd.fieldName);
                        std::string cur = SaveEditor::GetWStr32At(gMiiSav, h, miiIdx);
                        std::string nv = ShowKeyboard(fd.label, cur, 30);
                        if (nv != cur) { SaveEditor::SetWStr32At(gMiiSav, h, miiIdx, nv); gMiiSavDirty=true; }
                        gMiiStatsMsg=""; gMiiStatsMsgCol=COL_TEXT;
                    }
                    if (fd.isStr && (kDown&HidNpadButton_X)) {
                        static const uint32_t H_HOW_N = SaveEditor::Hash("Mii.Name.HowToCallName");
                        std::string cur = SaveEditor::GetWStr64At(gMiiSav, H_HOW_N, miiIdx);
                        std::string nv = ShowKeyboard("Pronunciation (optional, empty = same as name)", cur, 63);
                        if (nv != cur) { SaveEditor::SetWStr64At(gMiiSav, H_HOW_N, miiIdx, nv); gMiiSavDirty=true; }
                        gMiiStatsMsg=""; gMiiStatsMsgCol=COL_TEXT;
                    }
                    if (fd.isStr && !gMiis.empty() && gMiiStatsMiiSel<(int)gMiis.size()) {
                        // Left/Right cycles between [import .ltd] [export .ltd] [share]
                        if (kDown&(HidNpadButton_Left|HidNpadButton_StickLLeft|HidNpadButton_StickRLeft))
                            gMiiLtdBtnSel = (gMiiLtdBtnSel > 0) ? gMiiLtdBtnSel - 1 : 2;
                        if (kDown&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight))
                            gMiiLtdBtnSel = (gMiiLtdBtnSel < 2) ? gMiiLtdBtnSel + 1 : 0;
                        if (kDown&HidNpadButton_A) {
                            if      (gMiiLtdBtnSel == 0) TOpenImportBrowser(0);
                            else if (gMiiLtdBtnSel == 1) TDoMiiExport(0);
                            else                          TOpenMiiShareBrowser(0);
                        }
                    }
                }
                // B = back to user pick, Plus = quit app (both prompt when dirty)
                if (kDown&(HidNpadButton_B|HidNpadButton_Plus)) {
                    if (gBlPickerOpen && (kDown&HidNpadButton_B)) {
                        gBlPickerOpen = false; // B closes picker, does not navigate back
                    } else if (gShareDetailOpen && (kDown&HidNpadButton_B)) {
                        // Already handled in the Browse edit block above, but be defensive.
                        ShareCloseDetail();
                    } else {
                        bool toUserPick = (kDown&HidNpadButton_B) != 0;
                        if (gMiiSavDirty || gUgcDirty || gWebUiDirty) {
                            gShowBackPrompt=true; gBackPromptIsMii=true; gBackPromptToUserPick=toUserPick;
                        } else if (toUserPick) {
                            FreePreview(); HttpServer::Stop(); gLog.clear();
                            gEntries.clear(); gMiis.clear();
                            gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{};
                            gPlayerSavDirty=false; gMiiSavDirty=false;
                            SaveMount::Unmount();
                            gScreen=Screen::UserPick;
                        } else {
                            gQuitApp=true;
                        }
                    }
                }

            } else if (gOnSwitchMode == OnSwitchMode::TexSettings) {
                // TexSettings: tab switching plus joystick-driven adjustment of the
                // two encoding settings (Encoder + BC1 Mode).
                if (kDown&HidNpadButton_L) {
                    gOnSwitchMode=OnSwitchMode::WebUI; gOnSwitchMsg=""; break;
                }
                if (kDown&HidNpadButton_R) {
                    gOnSwitchMode=OnSwitchMode::UGC; gOnSwitchMsg=""; break;
                }
                // Row navigation: 0 = Encoder, 1 = BC1 Mode, 2 = Fit, 3 = Background.
                if (kNav&(HidNpadButton_Up|HidNpadButton_StickLUp|HidNpadButton_StickRUp))
                    gTexSettingsSel = std::max(0, gTexSettingsSel - 1);
                if (kNav&(HidNpadButton_Down|HidNpadButton_StickLDown|HidNpadButton_StickRDown))
                    gTexSettingsSel = std::min(3, gTexSettingsSel + 1);
                bool left  = (kNav&(HidNpadButton_Left |HidNpadButton_StickLLeft |HidNpadButton_StickRLeft))  != 0;
                bool right = (kNav&(HidNpadButton_Right|HidNpadButton_StickLRight|HidNpadButton_StickRRight)) != 0;
                bool a     = (kDown&HidNpadButton_A) != 0;
                // Row 0: Encoder — Left/Right (and A) toggle Custom <-> PCA.
                if (gTexSettingsSel == 0 && (left || right || a)) {
                    gEncMode = (gEncMode == TextureProcessor::Bc1Encoder::Custom)
                             ? TextureProcessor::Bc1Encoder::PCA
                             : TextureProcessor::Bc1Encoder::Custom;
                    SaveConfig();
                }
                // Row 1: BC1 Mode — active only with Custom encoder.
                if (gTexSettingsSel == 1 && gEncMode == TextureProcessor::Bc1Encoder::Custom) {
                    int cur = (int)gBc1Mode;
                    if (right) cur = (cur + 1) % 3;
                    if (left)  cur = (cur + 2) % 3;
                    if (right || left) { gBc1Mode = (TextureProcessor::Bc1Mode)cur; SaveConfig(); }
                }
                // Row 2: Fit mode — cycle Cover -> Contain -> Fill.
                if (gTexSettingsSel == 2) {
                    int cur = (int)gFitMode;
                    if (right) cur = (cur + 1) % 3;
                    if (left)  cur = (cur + 2) % 3;
                    if (right || left) { gFitMode = (TextureProcessor::FitMode)cur; SaveConfig(); }
                }
                // Row 3: Background — Transparent / White / Black, only with Contain.
                if (gTexSettingsSel == 3 && gFitMode == TextureProcessor::FitMode::Contain) {
                    int cur = (gMatte.a == 0) ? 0
                            : (gMatte.r == 255 && gMatte.g == 255 && gMatte.b == 255) ? 1
                            : (gMatte.r == 0   && gMatte.g == 0   && gMatte.b == 0)   ? 2 : 0;
                    if (right) cur = (cur + 1) % 3;
                    if (left)  cur = (cur + 2) % 3;
                    if (right || left) TSetMatte(cur);
                }
                if (kDown&(HidNpadButton_B|HidNpadButton_Plus)) {
                    bool hasDirty = gPlayerSavDirty || gMiiSavDirty || gUgcDirty || gWebUiDirty;
                    bool toUserPick = (kDown&HidNpadButton_B) != 0;
                    if (hasDirty) {
                        gShowBackPrompt=true;
                        gBackPromptIsMii=gMiiSavDirty; gBackPromptToUserPick=toUserPick;
                    } else if (toUserPick) {
                        FreePreview(); HttpServer::Stop(); gLog.clear();
                        gEntries.clear(); gMiis.clear();
                        gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{};
                        gPlayerSavDirty=false; gMiiSavDirty=false; gUgcDirty=false;
                        SaveMount::Unmount(); gScreen=Screen::UserPick;
                    } else { gQuitApp=true; }
                }

            } else if (gOnSwitchMode == OnSwitchMode::Map) {
                // ── Map tab input ──────────────────────────────────────────
                // Tab L/R cycling (consistent with the other tabs).
                if (kDown&HidNpadButton_L) { gOnSwitchMode = OnSwitchMode::Player; gOnSwitchMsg=""; break; }
                if (kDown&HidNpadButton_R) { gOnSwitchMode = OnSwitchMode::WebUI;  gOnSwitchMsg=""; break; }

                // Cursor pan: right stick scales faster (120 tiles is wide),
                // left stick + D-pad does one tile per repeat. Cursor pan is
                // only active when we're not focused on the inspector or
                // picker; otherwise the D-pad navigates fields / rows.
                HidAnalogStickState rs = padGetStickPos(&gPad, 1);
                if (gMapMode == MapMode::Idle || gMapMode == MapMode::Place) {
                    gMapStickXAcc += rs.x / 4096;
                    gMapStickYAcc -= rs.y / 4096;
                    int stepX = gMapStickXAcc / 256; gMapStickXAcc -= stepX * 256;
                    int stepY = gMapStickYAcc / 256; gMapStickYAcc -= stepY * 256;
                    int nx = gMapCursorX + stepX;
                    int ny = gMapCursorY + stepY;
                    if (kNav&(HidNpadButton_Left  | HidNpadButton_StickLLeft))  nx--;
                    if (kNav&(HidNpadButton_Right | HidNpadButton_StickLRight)) nx++;
                    if (kNav&(HidNpadButton_Up    | HidNpadButton_StickLUp))    ny--;
                    if (kNav&(HidNpadButton_Down  | HidNpadButton_StickLDown))  ny++;
                    if (nx < 0) nx = 0; if (nx >= MAP_WIDTH)  nx = MAP_WIDTH  - 1;
                    if (ny < 0) ny = 0; if (ny >= MAP_HEIGHT) ny = MAP_HEIGHT - 1;
                    if (nx != gMapCursorX || ny != gMapCursorY) {
                        gMapCursorX = nx; gMapCursorY = ny;
                        gMapDelConfirm = 0;  // any movement cancels the delete-confirm
                    }
                }

                if (gMapMode == MapMode::Picker) {
                    MapRebuildPicker();
                    int total = (int)gMapPickerActors.size();
                    if (kNav&(HidNpadButton_Up   |HidNpadButton_StickLUp  |HidNpadButton_StickRUp))   { if (total>0) gMapPickerSel = (gMapPickerSel - 1 + total) % total; }
                    if (kNav&(HidNpadButton_Down |HidNpadButton_StickLDown|HidNpadButton_StickRDown)) { if (total>0) gMapPickerSel = (gMapPickerSel + 1) % total; }
                    if (kDown&HidNpadButton_A && total > 0) {
                        // Always go into Place mode — picker only ever adds a
                        // new actor; the previously selected slot (if any) is
                        // left untouched. Once dropped, Place mode auto-
                        // selects the new slot.
                        gMapPlaceActor = gMapPickerActors[gMapPickerSel];
                        gMapMode = MapMode::Place;
                    }
                    if (kDown&HidNpadButton_B) {
                        gMapMode = (gMapSelSlot >= 0 && MapActorAt(gMapSelSlot)) ? MapMode::Inspect : MapMode::Idle;
                    }
                } else if (gMapMode == MapMode::Place) {
                    if (kDown&HidNpadButton_A) {
                        // Find an empty slot and place at cursor.
                        uint32_t hAct = SaveEditor::Hash(MapKeys::ActorKey);
                        int n = SaveEditor::ArraySize(gMapSav, hAct);
                        int slot = -1;
                        for (int i = 0; i < n; i++) {
                            if (SaveEditor::GetUIntAt(gMapSav, hAct, i, 0) == 0) { slot = i; break; }
                        }
                        if (slot < 0) {
                            MapSetMsg(Lang::T("map.no.empty.slot"), COL_RED, 180);
                        } else {
                            SaveEditor::SetUIntAt(gMapSav, hAct, slot, gMapPlaceActor);
                            SaveEditor::SetIntAt (gMapSav, SaveEditor::Hash(MapKeys::GridPosX), slot, gMapCursorX);
                            SaveEditor::SetIntAt (gMapSav, SaveEditor::Hash(MapKeys::GridPosY), slot, gMapCursorY);
                            SaveEditor::SetFloatAt(gMapSav, SaveEditor::Hash(MapKeys::RotY),   slot, 0.0f);
                            SaveEditor::SetIntAt (gMapSav, SaveEditor::Hash(MapKeys::LinkedMapId), slot, -1);
                            gMapSelSlot = slot;
                            gMapDirty   = true;
                            gMapMode    = MapMode::Inspect;
                            MapSetMsg("placed in slot #" + std::to_string(slot), COL_GREEN, 120);
                        }
                    }
                    if (kDown&HidNpadButton_B) gMapMode = MapMode::Idle;
                } else if (gMapMode == MapMode::Inspect) {
                    // Inspector-card navigation:
                    //   Left/Right → nudge object X on the grid (field 0)
                    //   Up/Down    → nudge object Y on the grid (field 1)
                    //   A          → commit edits, back to Idle (changes are
                    //                live-applied by the nudge handlers, A is
                    //                just a "done" exit).
                    //   Y          → open actor picker for the slot
                    //   X          → delete (two-press confirm)
                    //   B          → close inspector, back to Idle
                    // Rotation / linked-map are touch-only (tap the +/- buttons).
                    int rowsCount = 3;
                    if (gMapSelSlot >= 0) {
                        const MapData::ActorInfo* info = MapData::ActorLookup(MapActorAt(gMapSelSlot));
                        if (info && (info->group == 0 || info->group == 1)) rowsCount = 4;
                    }
                    if (kNav&(HidNpadButton_Left | HidNpadButton_StickLLeft))
                        TMapInsNudge((0 << 8) | 255);
                    if (kNav&(HidNpadButton_Right| HidNpadButton_StickLRight))
                        TMapInsNudge((0 << 8) | 1);
                    if (kNav&(HidNpadButton_Up   | HidNpadButton_StickLUp))
                        TMapInsNudge((1 << 8) | 255);
                    if (kNav&(HidNpadButton_Down | HidNpadButton_StickLDown))
                        TMapInsNudge((1 << 8) | 1);
                    if (kDown&HidNpadButton_A) {
                        gMapMode = MapMode::Idle;
                        gMapDelConfirm = 0;
                    }
                    if (kDown&HidNpadButton_Y) {
                        gMapMode = MapMode::Picker;
                        gMapPickerFilter.clear();
                        gMapPickerSel = 0;
                        gMapPickerScroll = 0;
                    }
                    if (kDown&HidNpadButton_X) {
                        // Two-press confirm: first X arms, second X commits.
                        if (gMapDelConfirm == 0) {
                            gMapDelConfirm = 1;
                            MapSetMsg("press X again to confirm delete", COL_GOLD, 180);
                        } else {
                            SaveEditor::SetUIntAt(gMapSav, SaveEditor::Hash(MapKeys::ActorKey),
                                                   gMapSelSlot, 0);
                            gMapDirty = true;
                            gMapDelConfirm = 0;
                            gMapSelSlot = -1;
                            gMapMode = MapMode::Idle;
                            MapSetMsg("deleted", COL_GREEN, 90);
                        }
                    }
                    if (kDown&HidNpadButton_B) {
                        gMapMode = MapMode::Idle;
                        gMapDelConfirm = 0;
                    }
                    (void)rowsCount;
                } else { // Idle
                    if (kDown&HidNpadButton_A) {
                        int slot = MapObjectAt(gMapCursorX, gMapCursorY);
                        if (slot >= 0) {
                            gMapSelSlot = slot;
                            gMapMode    = MapMode::Inspect;
                            gMapInsField= 0;
                        } else {
                            MapSetMsg("no object here", COL_DIM, 60);
                        }
                    }
                    // Y = enter actor picker so the user can drop a new
                    // object. (+ stays reserved for the global "close app"
                    // shortcut, same as every other tab.)
                    if (kDown&HidNpadButton_Y) {
                        gMapMode = MapMode::Picker;
                        gMapPickerFilter.clear();
                        gMapPickerSel = 0;
                        gMapPickerScroll = 0;
                    }
                    // B exits to user-pick. If any save (Map, Mii, Player,
                    // textures, etc.) is dirty, defer to the existing
                    // back-prompt modal so the user can pick save/discard
                    // with on-screen buttons — same UX as the Mii / Player
                    // tabs. + lets the global handler take over for quit.
                    if (kDown&(HidNpadButton_B|HidNpadButton_Plus)) {
                        bool hasDirty = gMapDirty || gPlayerSavDirty || gMiiSavDirty
                                      || gUgcDirty || gWebUiDirty;
                        bool toUserPick = (kDown&HidNpadButton_Plus) == 0;
                        if (hasDirty) {
                            gShowBackPrompt       = true;
                            gBackPromptIsMii      = gMiiSavDirty;
                            gBackPromptToUserPick = toUserPick;
                        } else if (toUserPick) {
                            FreePreview(); HttpServer::Stop(); gLog.clear();
                            gEntries.clear(); gMiis.clear();
                            gPlayerSav = SaveEditor::SavFile{};
                            gMiiSav    = SaveEditor::SavFile{};
                            gMapSav    = SaveEditor::SavFile{};
                            gMapDirty  = false;
                            gPlayerSavDirty = false; gMiiSavDirty = false; gUgcDirty = false;
                            SaveMount::Unmount();
                            gScreen = Screen::UserPick;
                        } else { gQuitApp = true; }
                    }
                }

            } else { // WebUI tab
                // X = restart HTTP server
                if (kDown&HidNpadButton_X) {
                    HttpServer::Stop();
                    HttpServer::Start(HTTP_PORT, SAVE_UGC_PATH);
                    LogOK("WebUI restarted");
                }
                // Tab switch. The on-Switch bar wraps WebUI ↔ Map on the
                // outside; from WebUI, L goes to Map (the last tab) and R
                // moves into the textures (UGC) tab.
                if (kDown&HidNpadButton_L){
                    gOnSwitchMode=OnSwitchMode::Map; gOnSwitchMsg=""; break;
                }
                if (kDown&HidNpadButton_R){
                    gOnSwitchMode=OnSwitchMode::UGC; gOnSwitchMsg=""; break;
                }
                if (kDown&(HidNpadButton_B|HidNpadButton_Plus)){
                    bool hasDirty = gPlayerSavDirty || gMiiSavDirty || gUgcDirty || gWebUiDirty;
                    bool toUserPick = (kDown&HidNpadButton_Plus) == 0;
                    if (hasDirty) {
                        gShowBackPrompt=true;
                        gBackPromptIsMii=gMiiSavDirty; gBackPromptToUserPick=toUserPick;
                    } else if (toUserPick) {
                        FreePreview(); HttpServer::Stop(); gLog.clear();
                        gEntries.clear(); gMiis.clear();
                        gPlayerSav=SaveEditor::SavFile{}; gMiiSav=SaveEditor::SavFile{};
                        gPlayerSavDirty=false; gMiiSavDirty=false;
                        gScreen=Screen::UserPick; SaveMount::Unmount();
                    } else { gQuitApp=true; }
                }
            }
            // WebUI standby prevention: keep screen on while a client connected recently
            {
                bool onWebUI = (gOnSwitchMode == OnSwitchMode::WebUI);
                if (!onWebUI && gStandbyOff) {
                    appletSetMediaPlaybackState(false); gStandbyOff = false;
                } else if (onWebUI) {
                    u64 lastTick = HttpServer::LastConnectTick();
                    // 60-second window: armGetSystemTickFreq() = 19200000
                    bool recent = lastTick > 0 &&
                        (armGetSystemTick() - lastTick) < (u64)60 * armGetSystemTickFreq();
                    if (recent && !gStandbyOff) {
                        appletSetMediaPlaybackState(true); gStandbyOff = true;
                    } else if (!recent && gStandbyOff) {
                        appletSetMediaPlaybackState(false); gStandbyOff = false;
                    }
                }
                gPrevOnSwitchMode = gOnSwitchMode;
            }
            // Poll USB state once per second to detect unclean Windows disconnects
            {
                static u64 sMtpPollTick = 0;
                u64 now = armGetSystemTick();
                if (now - sMtpPollTick >= armGetSystemTickFreq()) {
                    sMtpPollTick = now;
                    MtpServer::PollUsbState();
                }
            }
            // Dispatch draw to correct function for current tab
            if (gShowSaveWarning) {
                DrawSaveWarning();
            } else if (gShowBackPrompt) {
                DrawBackPrompt();
            } else {
                switch (gOnSwitchMode) {
                    case OnSwitchMode::Player:      DrawPlayer();      break;
                    case OnSwitchMode::MiiStats:    DrawMiiStats();    break;
                    case OnSwitchMode::TexSettings: DrawTexSettings(); break;
                    case OnSwitchMode::Map:         DrawMap();         break;
                    default:                        DrawOnSwitch();    break;
                }
            }
            break;


        case Screen::SaveFeedback: {
            SDL_SetRenderDrawColor(gRen, COL_BG.r, COL_BG.g, COL_BG.b, 255);
            SDL_RenderClear(gRen);
            DrawTextC(Lang::T("save.feedback.title"), SCREEN_W/2, SCREEN_H/2 - 20, COL_GREEN, Font::Lg);
            DrawTextC(Lang::T("save.feedback.body"), SCREEN_W/2, SCREEN_H/2 + 28, COL_DIM, Font::Md);
            Present();
            if (--gSaveFeedbackFrames <= 0) {
                if (gSaveFeedbackQuit) gQuitApp = true;
                else gScreen = Screen::UserPick;
            }
            continue;
        }

        case Screen::Error:
            DrawError();
            break;
        }
    }

    FreePreview();
    ShareFreeThumbs();
    ShareFreeDetail();
    // Tear down the long-lived caches. The thumb cache owns its textures;
    // the list cache only holds parsed metadata so its destructor is enough.
    for (auto& kv : gShareThumbCache) if (kv.second) SDL_DestroyTexture(kv.second);
    gShareThumbCache.clear();
    gShareThumbCacheLRU.clear();
    gShareListCache.clear();
    gShareListCacheLRU.clear();
    ShareWorkerStopFn();
    HttpServer::Stop();
    MtpServer::Exit();
    BackupService::Cleanup();
    SaveMount::Unmount();
    Updater::Cleanup();
    for (auto tex : gAvatarTextures) if (tex) SDL_DestroyTexture(tex);
    gAvatarTextures.clear();
    Audio::Quit();
    FontQuit();
    nifmExit();
    socketExit();
    SDL_DestroyRenderer(gRen);
    SDL_DestroyWindow(win);
    IMG_Quit();
    SDL_Quit();
    romfsExit();
    return 0;
}
