// ─── UiIcons implementation ───────────────────────────────────────────────────
// See ui_icons.h for the overview. Geometry lives on a 24×24 design grid,
// y-down (screen convention). Arc angles use the same y-down atan2 frame:
// 0° = +x (right), 90° = +y (down), 270° = up; sweeps run a0 → a1 in the
// increasing-angle (visually clockwise) direction.

#include "ui_icons.h"
#include "ui_art.h"
#include "ui_draw.h"
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
    // An arc as short segments (angles in degrees: 0 = right, 90 = down).
    auto poly = [&](float cx, float cy, float r, float a0, float a1, int n) {
        for (int i = 0; i < n; i++) {
            const float t0 = (a0 + (a1 - a0) * i / n) * 0.0174533f, t1 = (a0 + (a1 - a0) * (i + 1) / n) * 0.0174533f;
            out.push_back(S(cx + r * cosf(t0), cy + r * sinf(t0), cx + r * cosf(t1), cy + r * sinf(t1)));
        }
    };
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
    // ── 2.0 ──────────────────────────────────────────────────────────────────
    case Id::Home: {
        out.push_back(S( 3.8f, 11.0f, 12.0f,  4.0f));    // roof
        out.push_back(S(12.0f,  4.0f, 20.2f, 11.0f));
        out.push_back(S( 6.0f,  9.6f,  6.0f, 18.0f));    // walls
        out.push_back(S(18.0f,  9.6f, 18.0f, 18.0f));
        out.push_back(A( 8.0f, 18.0f, 2.f,  90.f, 180.f));
        out.push_back(A(16.0f, 18.0f, 2.f,   0.f,  90.f));
        out.push_back(S( 8.0f, 20.0f, 16.0f, 20.0f));    // floor
        out.push_back(S(10.0f, 20.0f, 10.0f, 14.5f));    // door
        out.push_back(S(10.0f, 14.5f, 14.0f, 14.5f));
        out.push_back(S(14.0f, 14.5f, 14.0f, 20.0f));
        break;
    }
    case Id::Image: {
        out.push_back(S( 6.0f,  4.0f, 18.0f,  4.0f));    // frame
        out.push_back(S( 6.0f, 20.0f, 18.0f, 20.0f));
        out.push_back(S( 4.0f,  6.0f,  4.0f, 18.0f));
        out.push_back(S(20.0f,  6.0f, 20.0f, 18.0f));
        out.push_back(A( 6.0f,  6.0f, 2.f, 180.f, 270.f));
        out.push_back(A(18.0f,  6.0f, 2.f, 270.f, 360.f));
        out.push_back(A(18.0f, 18.0f, 2.f,   0.f,  90.f));
        out.push_back(A( 6.0f, 18.0f, 2.f,  90.f, 180.f));
        out.push_back(C( 9.0f,  9.2f, 1.5f, 1.9f));      // sun
        out.push_back(S( 4.6f, 17.2f,  9.6f, 12.6f));    // hills
        out.push_back(S( 9.6f, 12.6f, 12.6f, 15.6f));
        out.push_back(S(12.6f, 15.6f, 15.4f, 12.2f));
        out.push_back(S(15.4f, 12.2f, 19.8f, 16.6f));
        break;
    }
    case Id::Users: {
        out.push_back(C( 9.0f,  8.4f, 3.2f));                     // front head
        out.push_back(A( 9.0f, 19.6f, 6.0f, 180.f, 360.f));       // front shoulders
        out.push_back(A(16.2f,  8.8f, 2.8f, 250.f, 110.f));       // back head, right side
        out.push_back(A(15.0f, 19.6f, 6.0f, 300.f, 360.f));       // back shoulder
        break;
    }
    case Id::User: {
        out.push_back(C(12.0f,  8.2f, 3.6f));
        out.push_back(A(12.0f, 20.0f, 7.0f, 180.f, 360.f));
        break;
    }
    case Id::Map: {
        out.push_back(S( 3.5f,  6.6f,  9.0f,  4.6f));    // top edge
        out.push_back(S( 9.0f,  4.6f, 15.0f,  6.6f));
        out.push_back(S(15.0f,  6.6f, 20.5f,  4.6f));
        out.push_back(S(20.5f,  4.6f, 20.5f, 17.4f));    // right
        out.push_back(S(20.5f, 17.4f, 15.0f, 19.4f));    // bottom edge
        out.push_back(S(15.0f, 19.4f,  9.0f, 17.4f));
        out.push_back(S( 9.0f, 17.4f,  3.5f, 19.4f));
        out.push_back(S( 3.5f, 19.4f,  3.5f,  6.6f));    // left
        out.push_back(S( 9.0f,  4.6f,  9.0f, 17.4f));    // folds
        out.push_back(S(15.0f,  6.6f, 15.0f, 19.4f));
        break;
    }
    case Id::Globe: {
        out.push_back(C(12.0f, 12.0f, 8.5f));
        out.push_back(S( 3.5f, 12.0f, 20.5f, 12.0f));
        out.push_back(A(19.03f, 12.0f, 11.03f, 129.6f, 230.4f));  // meridian, bulging left
        out.push_back(A( 4.97f, 12.0f, 11.03f, 309.6f,  50.4f));  // meridian, bulging right
        break;
    }
    case Id::Wifi: {
        // Three bars and the dot, like the console's own signal icon.
        out.push_back(A(12.0f, 19.0f, 13.2f, 230.f, 310.f));
        out.push_back(A(12.0f, 19.0f,  9.0f, 230.f, 310.f));
        out.push_back(A(12.0f, 19.0f,  4.8f, 230.f, 310.f));
        out.push_back(S(12.0f, 18.4f, 12.0f, 18.4f, 2.8f));       // dot
        break;
    }
    case Id::Usb: {
        out.push_back(S(12.0f,  5.0f, 12.0f, 17.6f));    // stem
        out.push_back(S( 9.8f,  7.2f, 12.0f,  4.6f));    // arrow head
        out.push_back(S(14.2f,  7.2f, 12.0f,  4.6f));
        out.push_back(S(12.0f, 13.6f,  7.6f, 11.4f));    // left branch
        out.push_back(S( 7.6f, 11.4f,  7.6f,  9.6f));
        out.push_back(C( 7.6f,  8.2f, 1.2f, 1.9f));
        out.push_back(S(12.0f, 15.8f, 16.4f, 13.6f));    // right branch
        out.push_back(S(16.4f, 13.6f, 16.4f, 11.6f));
        out.push_back(S(15.3f, 10.2f, 17.5f, 10.2f, 2.4f));
        out.push_back(C(12.0f, 19.4f, 1.6f, 1.9f));      // plug
        break;
    }
    case Id::Phone: {
        out.push_back(S( 9.0f,  3.0f, 15.0f,  3.0f));
        out.push_back(S( 9.0f, 21.0f, 15.0f, 21.0f));
        out.push_back(S( 7.0f,  5.0f,  7.0f, 19.0f));
        out.push_back(S(17.0f,  5.0f, 17.0f, 19.0f));
        out.push_back(A( 9.0f,  5.0f, 2.f, 180.f, 270.f));
        out.push_back(A(15.0f,  5.0f, 2.f, 270.f, 360.f));
        out.push_back(A(15.0f, 19.0f, 2.f,   0.f,  90.f));
        out.push_back(A( 9.0f, 19.0f, 2.f,  90.f, 180.f));
        out.push_back(S(11.0f, 17.8f, 13.0f, 17.8f, 1.8f));
        break;
    }
    case Id::Check: {
        out.push_back(S( 5.0f, 12.6f, 10.0f, 17.6f, 2.3f));
        out.push_back(S(10.0f, 17.6f, 19.4f,  7.0f, 2.3f));
        break;
    }
    case Id::Close: {
        out.push_back(S( 6.4f,  6.4f, 17.6f, 17.6f, 2.2f));
        out.push_back(S(17.6f,  6.4f,  6.4f, 17.6f, 2.2f));
        break;
    }
    case Id::Plus: {
        out.push_back(S(12.0f,  5.2f, 12.0f, 18.8f, 2.2f));
        out.push_back(S( 5.2f, 12.0f, 18.8f, 12.0f, 2.2f));
        break;
    }
    case Id::Alert: {
        out.push_back(S(12.0f,  4.2f, 20.8f, 19.4f));
        out.push_back(S(20.8f, 19.4f,  3.2f, 19.4f));
        out.push_back(S( 3.2f, 19.4f, 12.0f,  4.2f));
        out.push_back(S(12.0f, 10.2f, 12.0f, 14.2f, 2.1f));
        out.push_back(S(12.0f, 16.9f, 12.0f, 16.9f, 2.5f));
        break;
    }
    case Id::Info: {
        out.push_back(C(12.0f, 12.0f, 8.5f));
        out.push_back(S(12.0f, 11.2f, 12.0f, 16.4f, 2.1f));
        out.push_back(S(12.0f,  7.8f, 12.0f,  7.8f, 2.5f));
        break;
    }
    case Id::Trash: {
        out.push_back(S( 4.5f,  7.0f, 19.5f,  7.0f));    // lid
        out.push_back(S( 9.4f,  7.0f,  9.4f,  4.4f));    // handle
        out.push_back(S( 9.4f,  4.4f, 14.6f,  4.4f));
        out.push_back(S(14.6f,  4.4f, 14.6f,  7.0f));
        out.push_back(S( 6.4f,  7.0f,  7.4f, 19.6f));    // bin
        out.push_back(S(17.6f,  7.0f, 16.6f, 19.6f));
        out.push_back(S( 7.4f, 19.6f, 16.6f, 19.6f));
        out.push_back(S(10.3f, 10.8f, 10.3f, 16.0f, 1.8f));
        out.push_back(S(13.7f, 10.8f, 13.7f, 16.0f, 1.8f));
        break;
    }
    case Id::Upload: {
        out.push_back(S(12.0f, 15.0f, 12.0f,  4.3f, 2.1f));       // stem
        out.push_back(S( 7.3f,  9.0f, 12.0f,  4.3f, 2.1f));       // chevron L
        out.push_back(S(16.7f,  9.0f, 12.0f,  4.3f, 2.1f));       // chevron R
        out.push_back(S( 3.5f, 15.5f,  3.5f, 18.5f));             // tray
        out.push_back(S(20.5f, 15.5f, 20.5f, 18.5f));
        out.push_back(S( 5.5f, 20.5f, 18.5f, 20.5f));
        out.push_back(A( 5.5f, 18.5f, 2.f,  90.f, 180.f));
        out.push_back(A(18.5f, 18.5f, 2.f,   0.f,  90.f));
        break;
    }
    case Id::ChevronLeft: {
        out.push_back(S(14.6f,  6.0f,  8.6f, 12.0f, 2.3f));
        out.push_back(S( 8.6f, 12.0f, 14.6f, 18.0f, 2.3f));
        break;
    }
    case Id::ChevronRight: {
        out.push_back(S( 9.4f,  6.0f, 15.4f, 12.0f, 2.3f));
        out.push_back(S(15.4f, 12.0f,  9.4f, 18.0f, 2.3f));
        break;
    }
    case Id::ChevronDown: {
        out.push_back(S( 6.0f,  9.4f, 12.0f, 15.4f, 2.3f));
        out.push_back(S(12.0f, 15.4f, 18.0f,  9.4f, 2.3f));
        break;
    }
    case Id::Palette: {
        out.push_back(A(12.0f, 12.0f, 8.5f,  60.f, 360.f));       // open at the lower right
        out.push_back(A(15.6f, 16.6f, 2.6f, 200.f,  60.f));       // thumb hole
        out.push_back(S( 7.8f, 11.4f,  7.8f, 11.4f, 2.5f));       // paints
        out.push_back(S(10.6f,  7.6f, 10.6f,  7.6f, 2.5f));
        out.push_back(S(15.0f,  7.9f, 15.0f,  7.9f, 2.5f));
        break;
    }
    case Id::Clock: {
        out.push_back(C(12.0f, 12.0f, 8.5f));
        out.push_back(S(12.0f,  7.4f, 12.0f, 12.0f, 2.1f));
        out.push_back(S(12.0f, 12.0f, 15.2f, 14.0f, 2.1f));
        break;
    }
    case Id::Folder: {
        out.push_back(S( 3.5f,  6.5f,  9.0f,  6.5f));
        out.push_back(S( 9.0f,  6.5f, 11.0f,  8.7f));
        out.push_back(S(11.0f,  8.7f, 20.5f,  8.7f));
        out.push_back(S(20.5f,  8.7f, 20.5f, 18.5f));
        out.push_back(S(20.5f, 18.5f,  3.5f, 18.5f));
        out.push_back(S( 3.5f, 18.5f,  3.5f,  6.5f));
        break;
    }
    case Id::Speaker: {
        out.push_back(S( 4.0f,  9.5f,  4.0f, 14.5f));
        out.push_back(S( 4.0f,  9.5f,  7.5f,  9.5f));
        out.push_back(S( 4.0f, 14.5f,  7.5f, 14.5f));
        out.push_back(S( 7.5f,  9.5f, 12.0f,  5.5f));
        out.push_back(S( 7.5f, 14.5f, 12.0f, 18.5f));
        out.push_back(S(12.0f,  5.5f, 12.0f, 18.5f));
        poly(12.0f, 12.0f, 4.0f, -45.f, 45.f, 6);
        poly(12.0f, 12.0f, 7.6f, -50.f, 50.f, 8);
        break;
    }
    case Id::Exit: {
        out.push_back(S(10.5f,  4.5f,  5.0f,  4.5f));
        out.push_back(S( 5.0f,  4.5f,  5.0f, 19.5f));
        out.push_back(S( 5.0f, 19.5f, 10.5f, 19.5f));
        out.push_back(S(10.5f, 12.0f, 20.0f, 12.0f, 2.1f));
        out.push_back(S(16.2f,  8.2f, 20.0f, 12.0f, 2.1f));
        out.push_back(S(16.2f, 15.8f, 20.0f, 12.0f, 2.1f));
        break;
    }
    case Id::Sliders: {
        out.push_back(S( 4.0f,  7.0f,  6.8f,  7.0f)); out.push_back(S(11.2f,  7.0f, 20.0f,  7.0f));
        out.push_back(C( 9.0f,  7.0f, 2.2f));
        out.push_back(S( 4.0f, 12.0f, 12.8f, 12.0f)); out.push_back(S(17.2f, 12.0f, 20.0f, 12.0f));
        out.push_back(C(15.0f, 12.0f, 2.2f));
        out.push_back(S( 4.0f, 17.0f,  5.8f, 17.0f)); out.push_back(S(10.2f, 17.0f, 20.0f, 17.0f));
        out.push_back(C( 8.0f, 17.0f, 2.2f));
        break;
    }
    case Id::Grid: {
        const float q[4][2] = { {4.f, 4.f}, {13.f, 4.f}, {4.f, 13.f}, {13.f, 13.f} };
        for (const auto& o : q) {
            out.push_back(S(o[0], o[1], o[0] + 7.f, o[1]));
            out.push_back(S(o[0] + 7.f, o[1], o[0] + 7.f, o[1] + 7.f));
            out.push_back(S(o[0] + 7.f, o[1] + 7.f, o[0], o[1] + 7.f));
            out.push_back(S(o[0], o[1] + 7.f, o[0], o[1]));
        }
        break;
    }
    case Id::List: {
        for (int k = 0; k < 3; k++) {
            const float yy = 6.5f + k * 5.5f;
            out.push_back(C(5.2f, yy, 0.8f, 2.0f));
            out.push_back(S(9.5f, yy, 20.0f, yy));
        }
        break;
    }
    case Id::PanelClose:
    case Id::PanelOpen: {
        // A rounded window, its side panel's edge, and a chevron that folds
        // the panel (‹) or opens it (›).
        const float x0 = 3.5f, y0 = 4.5f, x1 = 20.5f, y1 = 19.5f, r = 2.6f;
        out.push_back(S(x0 + r, y0, x1 - r, y0));
        out.push_back(S(x1, y0 + r, x1, y1 - r));
        out.push_back(S(x1 - r, y1, x0 + r, y1));
        out.push_back(S(x0, y1 - r, x0, y0 + r));
        poly(x1 - r, y0 + r, r, 270.f, 360.f, 3);
        poly(x1 - r, y1 - r, r, 0.f, 90.f, 3);
        poly(x0 + r, y1 - r, r, 90.f, 180.f, 3);
        poly(x0 + r, y0 + r, r, 180.f, 270.f, 3);
        out.push_back(S(9.0f, y0, 9.0f, y1));
        if (id == Id::PanelClose) { out.push_back(S(16.2f, 9.0f, 13.2f, 12.0f)); out.push_back(S(13.2f, 12.0f, 16.2f, 15.0f)); }
        else                      { out.push_back(S(13.2f, 9.0f, 16.2f, 12.0f)); out.push_back(S(16.2f, 12.0f, 13.2f, 15.0f)); }
        break;
    }
    case Id::Heart: {
        poly( 8.4f, 9.4f, 3.9f, 150.f, 360.f, 10);
        poly(15.6f, 9.4f, 3.9f, 180.f, 390.f, 10);
        out.push_back(S( 5.0f, 11.4f, 12.0f, 19.2f));
        out.push_back(S(19.0f, 11.4f, 12.0f, 19.2f));
        break;
    }
    case Id::Chat: {
        out.push_back(S( 6.0f,  5.0f, 18.0f,  5.0f));
        out.push_back(S(20.0f,  7.0f, 20.0f, 14.0f));
        out.push_back(S(18.0f, 16.0f, 11.0f, 16.0f));
        out.push_back(S(11.0f, 16.0f,  7.0f, 19.5f));
        out.push_back(S( 7.0f, 19.5f,  7.0f, 16.0f));
        out.push_back(S( 6.0f, 16.0f,  4.0f, 14.0f));
        out.push_back(S( 4.0f, 14.0f,  4.0f,  7.0f));
        poly( 6.0f,  7.0f, 2.0f, 180.f, 270.f, 3);
        poly(18.0f,  7.0f, 2.0f, 270.f, 360.f, 3);
        poly(18.0f, 14.0f, 2.0f,   0.f,  90.f, 3);
        break;
    }
    case Id::Smile: {
        out.push_back(C(12.0f, 12.0f, 8.5f));
        poly(12.0f, 12.0f, 4.6f, 30.f, 150.f, 8);
        out.push_back(S( 9.2f,  9.4f,  9.2f, 10.2f, 2.4f));
        out.push_back(S(14.8f,  9.4f, 14.8f, 10.2f, 2.4f));
        break;
    }
    case Id::RotateLeft:
    case Id::RotateRight: {
        // A circle three quarters round with an arrowhead at its end
        // (lucide rotate-cw; mirrored for the left turn).
        const bool left = id == Id::RotateLeft;
        auto MX = [&](float x) { return left ? 24.f - x : x; };
        if (left) out.push_back(A(12.f, 12.f, 8.5f, 228.f, 180.f));
        else      out.push_back(A(12.f, 12.f, 8.5f, 0.f, 312.f));
        // On past the end of the sweep (top right), into a corner arrowhead.
        out.push_back(S(MX(17.7f), 5.7f, MX(20.2f), 8.2f));
        out.push_back(S(MX(20.2f), 8.2f, MX(20.2f), 3.4f));
        out.push_back(S(MX(20.2f), 8.2f, MX(15.4f), 8.2f));
        break;
    }
    case Id::FlipH:
    case Id::FlipV: {
        // Two arrowheads facing a dashed mirror line, one solid.
        const bool v = id == Id::FlipV;
        auto P = [&](float x, float y, float x2, float y2, float wv = 2.0f) {
            return v ? S(y, x, y2, x2, wv) : S(x, y, x2, y2, wv);
        };
        for (int k = 0; k < 4; k++) out.push_back(P(12.f, 3.5f + k * 4.8f, 12.f, 5.3f + k * 4.8f, 1.8f));
        out.push_back(P(3.5f, 7.f, 8.5f, 12.f)); out.push_back(P(8.5f, 12.f, 3.5f, 17.f)); out.push_back(P(3.5f, 17.f, 3.5f, 7.f));
        out.push_back(P(4.9f, 9.6f, 4.9f, 14.4f, 2.4f)); out.push_back(P(6.4f, 11.1f, 6.4f, 12.9f, 2.4f));
        out.push_back(P(20.5f, 7.f, 15.5f, 12.f)); out.push_back(P(15.5f, 12.f, 20.5f, 17.f)); out.push_back(P(20.5f, 17.f, 20.5f, 7.f));
        break;
    }
    case Id::Count: break;
    case Id::Sparkle: {
        out.push_back(S(12.0f,  3.6f, 13.9f, 10.1f, 1.9f));
        out.push_back(S(13.9f, 10.1f, 20.4f, 12.0f, 1.9f));
        out.push_back(S(20.4f, 12.0f, 13.9f, 13.9f, 1.9f));
        out.push_back(S(13.9f, 13.9f, 12.0f, 20.4f, 1.9f));
        out.push_back(S(12.0f, 20.4f, 10.1f, 13.9f, 1.9f));
        out.push_back(S(10.1f, 13.9f,  3.6f, 12.0f, 1.9f));
        out.push_back(S( 3.6f, 12.0f, 10.1f, 10.1f, 1.9f));
        out.push_back(S(10.1f, 10.1f, 12.0f,  3.6f, 1.9f));
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

// The game's own line icon for this one, where there is one (Art::Id::Count:
// keep the drawn icon). Arrows, ticks and other small marks stay drawn.
static Art::Id GameIcon(Id id) {
    switch (id) {
        case Id::Home:      return Art::Id::IcoHome;
        case Id::Users:     return Art::Id::IcoMiis;
        case Id::Image:     return Art::Id::IcoTextures;
        case Id::User:      return Art::Id::IcoPlayer;
        case Id::Map:       return Art::Id::IcoMap;
        case Id::Clock:     return Art::Id::IcoBackups;
        case Id::Phone:     return Art::Id::IcoPhone;
        case Id::Gear:      return Art::Id::IcoSettings;
        case Id::Palette:   return Art::Id::IcoLook;
        case Id::Trash:     return Art::Id::IcoTrash;
        case Id::Magnifier: return Art::Id::IcoSearch;
        case Id::Undo:      return Art::Id::IcoUndo;
        case Id::Sparkle:   return Art::Id::IcoSparkle;
        case Id::Pencil:    return Art::Id::IcoPen;
        case Id::Grid:      return Art::Id::IcoGrid;
        case Id::Globe:     return Art::Id::IcoLanguage;
        case Id::Speaker:   return Art::Id::IcoVoice;
        case Id::Heart:     return Art::Id::IcoSocial;
        case Id::Chat:      return Art::Id::IcoWords;
        case Id::Smile:     return Art::Id::IcoPersonality;
        default:            return Art::Id::Count;
    }
}

void Draw(Id id, int cx, int cy, int sizePx, SDL_Color col) {
    if (sizePx >= 12) {
        const Art::Id g = GameIcon(id);
        if (g != Art::Id::Count && Art::Icon(g, cx, cy, sizePx, col)) return;
    }
    if (!gIconRen || sizePx < 6) return;
    if (sizePx > 128) sizePx = 128;                 // cache sanity bound
    SDL_Texture* t = GetIconTexture(id, sizePx);
    if (!t) return;
    SDL_SetTextureColorMod(t, col.r, col.g, col.b);
    SDL_SetTextureAlphaMod(t, XformAlpha(col.a));
    SDL_Rect dst = XformRect(cx - sizePx / 2, cy - sizePx / 2, sizePx, sizePx);
    SDL_RenderCopy(gIconRen, t, nullptr, &dst);
}

} // namespace UiIcons
