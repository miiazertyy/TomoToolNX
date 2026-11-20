#pragma once
// ui_draw.h — TomoToolNX
// The drawing layer of the on-console UI: theme, fonts, text, shapes and motion.
//
// Everything here draws straight to the SDL renderer in 1280x720 screen
// coordinates. It knows nothing about the app (screens, saves, input); the
// widgets built on top of it live in ui_kit.h, and the screens in main.cpp.

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <vector>

constexpr int SCREEN_W = 1280;
constexpr int SCREEN_H = 720;
// Chrome shared by every screen: the top bar and the row of button hints.
constexpr int UI_TOPBAR_H = 56;
constexpr int UI_FOOTER_H = 30;

extern SDL_Renderer* gRen;

// ─── Theme ────────────────────────────────────────────────────────────────────
// The first block is the set of colours the screens have always asked for by
// name (COL_PANEL, COL_GOLD...). 2.0 keeps the names and changes what they mean:
//   bg      the page                    panel   a card sitting on the page
//   panel2  a tile inside a card        sel     the tinted fill of what has focus
//   border  hairlines                   text    ink          dim   muted ink
//   gold    the ACCENT (name is historical: it was gold in 1.x)
//   accent  the second accent           green / red   good / bad
struct Theme {
    SDL_Color bg, panel, panel2, sel, border, text, dim, gold, red, accent, green, glow;
    int  cornerRadius;      // radius of a control; cards and small parts derive from it
    bool shadow;
    // 2.0
    SDL_Color faint;        // the quietest ink: placeholders, captions, units
    SDL_Color warn;         // amber: "unsaved", "waiting"
    SDL_Color well;         // sunken field (search box, value box)
};
extern Theme gTheme;

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
#define COL_FAINT  (gTheme.faint)
#define COL_WARN   (gTheme.warn)
#define COL_WELL   (gTheme.well)
// A list row that does not have focus: nothing is drawn, the card shows through.
#define COL_ROW    (SDL_Color{0, 0, 0, 0})

namespace ThemeNS {
    struct Entry { std::string code; std::string labelKey; };
    const std::vector<Entry>& Available();
    const std::string&        Current();
    // Unknown codes, and the two 1.x codes ("switch", "original"), fall back to
    // a theme that exists.
    void SetCurrent(const std::string& code);
    void Apply(const std::string& code);
    // The theme's three signature colours (accent, second accent, card), for
    // the dots next to its name.
    std::vector<SDL_Color> Swatches(const std::string& code);
}

// Custom accent / background layered over the chosen theme (config.ini:
// accent_color / bg_color). ApplyCustomColors() rebuilds gTheme from the base
// theme plus these, so it is safe to call on every change.
extern bool      gAccentCustom;
extern SDL_Color gAccentColor;
extern bool      gBgCustom;
extern SDL_Color gBgColor;
void ApplyCustomColors();

// ─── Colour helpers ───────────────────────────────────────────────────────────
inline uint8_t   ClampU8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }
inline int       ColorLuma(SDL_Color c) { return (c.r * 299 + c.g * 587 + c.b * 114) / 1000; }
// How far a colour is from grey (0 = grey).
inline int       ColorChroma(SDL_Color c) {
    const int hi = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    const int lo = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
    return hi - lo;
}
inline SDL_Color ShadeBy(SDL_Color c, int amt) {
    return { ClampU8(c.r + amt), ClampU8(c.g + amt), ClampU8(c.b + amt), 255 };
}
// a + (b - a) * t, alpha 255.
SDL_Color Mix(SDL_Color a, SDL_Color b, float t);
// Same colour, new alpha.
inline SDL_Color Alpha(SDL_Color c, int a) { c.a = ClampU8(a); return c; }
bool      HexToColor(const std::string& v, SDL_Color& out);
SDL_Color HSVtoRGB(float h, float s, float v);
void      RGBtoHSV(SDL_Color col, float& h, float& s, float& v);

// ─── Fonts and text ───────────────────────────────────────────────────────────
// Sm / Md / Lg are the three sizes 1.x had (16 / 22 / 32 px); they now also
// step up in weight (regular / medium / semibold). Xs and Xl are new.
// 2.0 adds the sizes of the new screens (LLM-Selfbot's scale, about ×1.4 for
// a 720p screen read from a sofa): Body 17 (descriptions), Row 19 medium (row
// titles, sidebar items), Title 26 semibold (section titles), Hero 60.
enum class Font { Sm, Md, Lg, Xs, Xl, Body, Row, Title, Hero };
constexpr int kFontCount = 9;

void      FontInit();
void      FontQuit();
// Reopen every face for a UI language. Russian, Chinese and Japanese use the
// console's own shared font, which has the glyphs Mikhak lacks.
void      FontReload(const std::string& langCode);
TTF_Font* GetFont(Font f);

extern Uint32 gTextCacheFrame;      // bump once per frame; drives the cache's LRU
void         TextCacheFlush();
SDL_Texture* GetTextTexture(const std::string& text, Font f, SDL_Color col, int& outW, int& outH);

int  TextW(const std::string& text, Font f = Font::Sm);   // cached
int  TextH(Font f = Font::Sm);
// Shorten to fit maxW pixels, ending in an ellipsis. Cuts on UTF-8 boundaries.
std::string FitText(const std::string& text, Font f, int maxW);
// ASCII, Latin-1 and Cyrillic lower case to upper case; other scripts untouched.
std::string UpperUtf8(const std::string& s);
// The same for the first letter only ("back" -> "Back").
std::string CapFirstUtf8(const std::string& s);

void DrawText  (const std::string& text, int x,  int y,  SDL_Color col, Font f = Font::Sm);  // top-left
void DrawTextVC(const std::string& text, int x,  int cy, SDL_Color col, Font f = Font::Sm);  // left, centred on cy
void DrawTextC (const std::string& text, int cx, int cy, SDL_Color col, Font f = Font::Sm);  // centred
// Centred on what is drawn (the glyphs' own ink), not on the line's box: a
// letter on its own (an initial in a disc) sits in the middle.
void DrawTextInkC(const std::string& text, int cx, int cy, SDL_Color col, Font f = Font::Sm);
void DrawTextR (const std::string& text, int rx, int cy, SDL_Color col, Font f = Font::Sm);  // right edge at rx, centred on cy

// ─── Transform ────────────────────────────────────────────────────────────────
// A block of drawing can be moved, scaled about a point and faded as one: page
// arrivals, dialog pops, the press squash. Every primitive in this file honours
// it, and so do icons (ui_icons.cpp) and DrawTexture.
void     PushXform(float dx, float dy, float scale, float cx, float cy, float alpha);
void     PopXform();
// Drawing into a picture of its own (a render target): no page movement on
// it until the PopXform that goes with this.
void     PushXformIdentity();
bool     XformIdentity();
float    XformScale();
SDL_Rect XformRect(int x, int y, int w, int h);
void     XformPoint(float& x, float& y);
uint8_t  XformAlpha(int a);
void     DrawTexture(SDL_Texture* t, const SDL_Rect* src, SDL_Rect dst, uint8_t alpha = 255);
// The same, mirrored (a hand that points the other way).
void     DrawTextureFlipped(SDL_Texture* t, const SDL_Rect* src, SDL_Rect dst, uint8_t alpha, bool flipX, bool flipY = false);

// ─── Shapes ───────────────────────────────────────────────────────────────────
// All of these honour the colour's alpha.
void FillRectSquare (int x, int y, int w, int h, SDL_Color c);
void DrawRectSquare (int x, int y, int w, int h, SDL_Color c);
void FillRectRounded(int x, int y, int w, int h, int r, SDL_Color c);
void DrawRectRounded(int x, int y, int w, int h, int r, SDL_Color c);
// The radius FillRect / DrawRect pick for a box of this size: none for
// hairlines, a pill for thin bars, the control radius for buttons and rows, a
// larger one for cards.
int  UiRadiusFor(int w, int h);
void FillRect(int x, int y, int w, int h, SDL_Color c);
void DrawRect(int x, int y, int w, int h, SDL_Color c);
// Round the corners of something square already drawn (an avatar, a preview)
// by painting the four corner areas outside the arc in `bg`, the colour that
// surrounds it.
void MaskCorners(int x, int y, int w, int h, int r, SDL_Color bg);

void DrawShadow(int x, int y, int w, int h, int r);
void DrawGlowRect(int x, int y, int w, int h, int r, SDL_Color core, SDL_Color glow, float pulseT = 0.f);
// The ring that marks what has focus: two accent pixels and a soft halo.
void DrawFocusRing(int x, int y, int w, int h, int r = -1);

void FillCircle(int cx, int cy, int r, SDL_Color c);
// Any size, anti-aliased by a feathered edge (geometry, no texture).
void FillDisc(float cx, float cy, float r, SDL_Color c);
// A stroked arc; angles in degrees clockwise from twelve o'clock. c1 ≠ c0
// blends along the arc.
void StrokeArc(float cx, float cy, float r, float width, float a0, float a1, SDL_Color c0, SDL_Color c1);
// A rounded bar with a vertical (top → bottom) colour blend.
void FillGradientV(int x, int y, int w, int h, SDL_Color top, SDL_Color bottom);
// A rounded outline `width` px thick, anti-aliased, centred on the edge of the
// box (focus rings, card borders).
void StrokeRectRounded(float x, float y, float w, float h, float r, float width, SDL_Color c);
// The same outline in dashes that go all the way round, corners included. The
// gap is stretched a little so the dashes meet evenly where they start.
void DashRectRounded(float x, float y, float w, float h, float r, float width, float dash, float gap, SDL_Color c);
void DrawCircle(int cx, int cy, int r, SDL_Color c);
void DrawRing  (int cx, int cy, int r, SDL_Color c);

// Clear the frame: the page colour with LLM-Selfbot's aurora, four soft blobs
// of the theme's colours drifting slowly, leaning toward `SetBackdropFocus`.
void ClearScreen();
void SetBackdropFocus(float x01, float y01);
// Darken everything drawn so far (behind a dialog).
void DimScreen(int alpha = 150);

// ─── Motion ───────────────────────────────────────────────────────────────────
extern float  gDtSec;        // seconds since the previous frame (clamped)
extern Uint32 gLastTickMs;
extern float  gElapsedSec;
extern bool   gReduceMotion; // settings: things jump instead of gliding

// A rectangle that eases toward a target; settles in about six frames.
struct AnimRect {
    float x = 0, y = 0, w = 0, h = 0;
    bool  ready = false;
    void Step(int tx, int ty, int tw, int th, float dt);        // snaps on a far jump
    void StepSmooth(int tx, int ty, int tw, int th, float dt);  // always glides
    void Snap(int tx, int ty, int tw, int th);
    SDL_Rect ToRect() const;
};

// The focus ring glides between the things it marks. A screen asks for it at a
// rectangle every frame; `slot` lets a screen show more than one at a time, and
// a change of owner (screen / sub-area) makes it jump instead of sliding across.
constexpr int kHighlightSlots = 4;
void RequestHighlight(int ownerScreen, int ownerSub, int tx, int ty, int tw, int th, float dt, int slot = 0);
void DrawAnimatedHighlight(int slot = 0);
// Put the ring straight onto a rectangle, no glide (after a touch tap).
void SnapHighlight(int ownerScreen, int ownerSub, int x, int y, int w, int h, int slot = 0);

// The same idea for the marker under a row of tabs or chips.
constexpr int kTabUnderlineSlots = 4;
void RequestTabUnderline(int slot, int ownerTag, int tx, int ty, int tw, int th, float dt);
void DrawTabUnderline(int slot = 0);
// As a pill behind the active item instead of a bar under it.
void DrawTabPill(int slot = 0);

// A quick fade from the page colour when the screen changes.
struct ScreenFade {
    float alpha = 0.f;
    void Begin();
    void Step(float dt);
    void Draw();
};
extern ScreenFade gFade;
