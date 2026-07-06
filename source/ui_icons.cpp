// ─── UiIcons implementation ───────────────────────────────────────────────────
// See ui_icons.h for the overview. Geometry lives on a 24×24 design grid,
// y-down (screen convention). Arc angles use the same y-down atan2 frame:
// 0° = +x (right), 90° = +y (down), 270° = up; sweeps run a0 → a1 in the
// increasing-angle (visually clockwise) direction.

#include "ui_icons.h"
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>
#include <algorithm>

namespace UiIcons {

static SDL_Renderer* gIconRen = nullptr;

// ─── Stroke primitives ────────────────────────────────────────────────────────
struct Prim {
    enum Kind { Seg, Arc } kind;
    // Seg: (x1,y1)-(x2,y2).  Arc: centre (x1,y1), radius x2, angles y2..a1 (deg).
    float x1, y1, x2, y2, a1;
    float w;          // stroke width in design units (grid = 24)
};
static Prim S(float x1,float y1,float x2,float y2,float w=2.0f){
    return { Prim::Seg, x1,y1,x2,y2, 0.f, w };
}
static Prim A(float cx,float cy,float r,float a0,float a1,float w=2.0f){
    return { Prim::Arc, cx,cy,r,a0, a1, w };
}
static Prim C(float cx,float cy,float r,float w=2.0f){   // full circle
    return { Prim::Arc, cx,cy,r,0.f, 360.f, w };
}

// Distance from point to primitive centreline, in design units.
static float PrimDist(const Prim& p, float px, float py) {
    if (p.kind == Prim::Seg) {
        float dx = p.x2 - p.x1, dy = p.y2 - p.y1;
        float len2 = dx*dx + dy*dy;
        float t = (len2 > 1e-6f)
                ? ((px - p.x1)*dx + (py - p.y1)*dy) / len2 : 0.f;
        t = std::max(0.f, std::min(1.f, t));
        float qx = p.x1 + dx*t - px, qy = p.y1 + dy*t - py;
        return sqrtf(qx*qx + qy*qy);
    }
    // Arc
    float vx = px - p.x1, vy = py - p.y1;
    float len = sqrtf(vx*vx + vy*vy);
    float span = fmodf(p.a1 - p.y2 + 720.f, 360.f);
    if (span < 1e-3f) span = 360.f;                    // full circle
    float ang = atan2f(vy, vx) * 57.29578f;            // y-down degrees
    float rel = fmodf(ang - p.y2 + 720.f, 360.f);
    if (rel <= span) return fabsf(len - p.x2);
    // Outside the sweep — distance to nearest endpoint (round cap).
    float a0r = p.y2 * 0.0174533f, a1r = p.a1 * 0.0174533f;
    float e0x = p.x1 + p.x2*cosf(a0r) - px, e0y = p.y1 + p.x2*sinf(a0r) - py;
    float e1x = p.x1 + p.x2*cosf(a1r) - px, e1y = p.y1 + p.x2*sinf(a1r) - py;
    return sqrtf(std::min(e0x*e0x + e0y*e0y, e1x*e1x + e1y*e1y));
}

// ─── Icon geometry (24×24 grid) ───────────────────────────────────────────────
static void BuildGeometry(Id id, std::vector<Prim>& out) {
    switch (id) {
    case Id::Gear: {
        // Cog: stroke body ring + hub + 8 fat round-capped tooth stubs.
        out.push_back(C(12.f, 12.f, 6.6f, 2.1f));
        out.push_back(C(12.f, 12.f, 2.5f, 2.0f));
        for (int k = 0; k < 8; k++) {
            float a  = (float)k * 45.f * 0.0174533f;
            float ca = cosf(a), sa = sinf(a);
            out.push_back(S(12.f + ca*7.2f, 12.f + sa*7.2f,
                            12.f + ca*9.9f, 12.f + sa*9.9f, 3.0f));
        }
        break;
    }
    case Id::Floppy: {
        // Disk shell — rounded square with the classic slanted top-right corner.
        out.push_back(S( 7.0f, 4.0f, 15.5f,  4.0f));   // top edge
        out.push_back(S(15.5f, 4.0f, 20.0f,  8.5f));   // corner slant
        out.push_back(S(20.0f, 8.5f, 20.0f, 17.0f));   // right edge
        out.push_back(S( 4.0f, 7.0f,  4.0f, 17.0f));   // left edge
        out.push_back(S( 7.0f,20.0f, 17.0f, 20.0f));   // bottom edge
        out.push_back(A( 7.0f, 7.0f, 3.f, 180.f, 270.f)); // TL corner
        out.push_back(A( 7.0f,17.0f, 3.f,  90.f, 180.f)); // BL corner
        out.push_back(A(17.0f,17.0f, 3.f,   0.f,  90.f)); // BR corner
        // Shutter (top) — open toward the slant, like the lucide "save" glyph.
        out.push_back(S( 8.5f, 4.0f,  8.5f,  8.0f));
        out.push_back(S( 8.5f, 8.0f, 15.5f,  8.0f));
        // Label (bottom) — open at the shell's bottom edge.
        out.push_back(S( 8.0f,20.0f,  8.0f, 14.0f));
        out.push_back(S( 8.0f,14.0f, 16.0f, 14.0f));
        out.push_back(S(16.0f,14.0f, 16.0f, 20.0f));
        break;
    }
    case Id::Pencil: {
        // Writing pencil at 45°, tip bottom-left: back cap, two body edges,
        // sharpened tip, and a ferrule band separating wood from lead.
        const float bx = 19.5f, by = 4.5f;             // back-cap centre
        const float tx =  3.2f, ty = 20.8f;            // tip point
        const float hpx = 1.63f, hpy = 1.63f;          // half-width vector ⊥ axis
        const float sx = 6.03f, sy = 17.97f;           // tip shoulder centre
        out.push_back(S(bx+hpx, by+hpy, bx-hpx, by-hpy));       // back cap
        out.push_back(S(bx+hpx, by+hpy, sx+hpx, sy+hpy));       // lower edge
        out.push_back(S(bx-hpx, by-hpy, sx-hpx, sy-hpy));       // upper edge
        out.push_back(S(sx+hpx, sy+hpy, tx,     ty));           // tip lower
        out.push_back(S(sx-hpx, sy-hpy, tx,     ty));           // tip upper
        const float cx2 = 16.32f, cy2 = 7.68f;                  // band centre
        out.push_back(S(cx2+hpx, cy2+hpy, cx2-hpx, cy2-hpy));   // ferrule band
        break;
    }
    case Id::Magnifier: {
        out.push_back(C(10.5f, 10.5f, 6.8f, 2.2f));
        out.push_back(S(15.6f, 15.6f, 21.0f, 21.0f, 2.5f));
        break;
    }
    case Id::Undo:
    case Id::Redo: {
        // lucide undo-2: chevron + straight run + half-loop swinging under.
        //   M4 9 h10.5 a5.5 5.5 0 0 1 0 11 H11   (+ arrowhead at 4,9)
        bool redo = (id == Id::Redo);
        auto MX = [&](float x){ return redo ? 24.f - x : x; };
        float d = redo ? -1.f : 1.f;
        out.push_back(S(MX(4.2f), 9.0f, MX(9.2f), 4.0f, 2.1f));   // arrow up
        out.push_back(S(MX(4.2f), 9.0f, MX(9.2f), 14.0f, 2.1f));  // arrow down
        out.push_back(S(MX(4.9f), 9.0f, MX(13.5f), 9.0f, 2.1f));  // straight run
        // Half circle from top (270°) through the outer side to bottom (90°).
        if (!redo) out.push_back(A(13.5f, 14.5f, 5.5f, 270.f,  90.f, 2.1f));
        else       out.push_back(A(10.5f, 14.5f, 5.5f,  90.f, 270.f, 2.1f));
        out.push_back(S(MX(13.5f), 20.0f, MX(13.5f) - d*2.8f, 20.0f, 2.1f));
        break;
    }
    case Id::Download: {
        out.push_back(S(12.0f,  3.5f, 12.0f, 14.2f, 2.1f));      // stem
        out.push_back(S( 7.3f, 10.3f, 12.0f, 15.0f, 2.1f));      // chevron L
        out.push_back(S(16.7f, 10.3f, 12.0f, 15.0f, 2.1f));      // chevron R
        out.push_back(S( 3.5f, 15.5f,  3.5f, 18.5f));            // tray left
        out.push_back(S(20.5f, 15.5f, 20.5f, 18.5f));            // tray right
        out.push_back(S( 5.5f, 20.5f, 18.5f, 20.5f));            // tray bottom
        out.push_back(A( 5.5f, 18.5f, 2.f,  90.f, 180.f));       // tray BL
        out.push_back(A(18.5f, 18.5f, 2.f,   0.f,  90.f));       // tray BR
        break;
    }
    case Id::Funnel: {
        out.push_back(S( 3.5f,  4.5f, 20.5f,  4.5f));
        out.push_back(S(20.5f,  4.5f, 14.0f, 12.0f));
        out.push_back(S(14.0f, 12.0f, 14.0f, 17.5f));
        out.push_back(S(14.0f, 17.5f, 10.0f, 20.0f));
        out.push_back(S(10.0f, 20.0f, 10.0f, 12.0f));
        out.push_back(S(10.0f, 12.0f,  3.5f,  4.5f));
        break;
    }
    }
}

// ─── Texture cache ────────────────────────────────────────────────────────────
// Key = (icon id << 16) | sizePx. Textures are white with an SDF-derived alpha
// channel; colour is applied per-blit with SDL colour/alpha modulation, so one
// texture covers every theme colour and selection state.
static std::map<uint32_t, SDL_Texture*> gIconCache;

static SDL_Texture* GetIconTexture(Id id, int size) {
    uint32_t key = ((uint32_t)id << 16) | (uint32_t)size;
    auto it = gIconCache.find(key);
    if (it != gIconCache.end()) return it->second;

    std::vector<Prim> prims;
    BuildGeometry(id, prims);

    const float scale = (float)size / 24.f;
    std::vector<uint32_t> px((size_t)size * size, 0);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            // Pixel centre in design units.
            float dx = ((float)x + 0.5f) / scale;
            float dy = ((float)y + 0.5f) / scale;
            float cov = 0.f;
            for (const Prim& p : prims) {
                // Distances scale to pixels; the stroke never renders thinner
                // than ~1.7px so glyphs stay readable at toolbar sizes.
                float distPx  = PrimDist(p, dx, dy) * scale;
                float halfWpx = std::max(p.w * 0.5f * scale, 0.85f);
                float c = halfWpx + 0.5f - distPx;      // 1px AA ramp
                cov = std::max(cov, std::min(1.f, c));
            }
            if (cov <= 0.f) continue;
            uint8_t a = (uint8_t)(cov * 255.f + 0.5f);
            // ABGR8888: 0xAABBGGRR — white RGB, coverage in alpha.
            px[(size_t)y * size + x] = ((uint32_t)a << 24) | 0x00FFFFFFu;
        }
    }

    SDL_Texture* t = SDL_CreateTexture(gIconRen, SDL_PIXELFORMAT_ABGR8888,
                                       SDL_TEXTUREACCESS_STATIC, size, size);
    if (t) {
        SDL_UpdateTexture(t, nullptr, px.data(), size * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }
    gIconCache.emplace(key, t);
    return t;
}

// ─── Public API ───────────────────────────────────────────────────────────────
void Init(SDL_Renderer* ren) { gIconRen = ren; }

void Quit() {
    for (auto& kv : gIconCache)
        if (kv.second) SDL_DestroyTexture(kv.second);
    gIconCache.clear();
    gIconRen = nullptr;
}

void Draw(Id id, int cx, int cy, int sizePx, SDL_Color col) {
    if (!gIconRen || sizePx < 6) return;
    if (sizePx > 128) sizePx = 128;                 // cache sanity bound
    SDL_Texture* t = GetIconTexture(id, sizePx);
    if (!t) return;
    SDL_SetTextureColorMod(t, col.r, col.g, col.b);
    SDL_SetTextureAlphaMod(t, col.a);
    SDL_Rect dst{ cx - sizePx / 2, cy - sizePx / 2, sizePx, sizePx };
    SDL_RenderCopy(gIconRen, t, nullptr, &dst);
}

} // namespace UiIcons
