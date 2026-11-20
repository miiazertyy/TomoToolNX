// ui_focus.cpp — TomoToolNX. See ui_focus.h.

#include "ui_focus.h"
#include "ui_draw.h"
#include "ui_motion.h"
#include "ui_sound.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace Focus {
namespace {

struct NodeRec { Node n; int layer; int scroll; };
struct ScrollRec { Ui::Id id; SDL_Rect view; Scroll* st; int layer; };
struct LayerRec { Layer kind; std::function<void()> onBack; std::string backHint; SDL_Rect panel; };
struct CmdRec { uint32_t btn; std::string hint; std::function<void()> fn; int layer; };

struct FrameReg {
    std::vector<NodeRec> nodes;
    std::vector<ScrollRec> scrolls;
    std::vector<LayerRec> layers;
    std::vector<CmdRec> cmds;
    std::unordered_map<Ui::Id, int> index;
    std::function<void()> baseBack;
    std::string baseBackHint;
    SDL_Rect prefer{};
    void Clear() { nodes.clear(); scrolls.clear(); layers.clear(); cmds.clear(); index.clear(); baseBack = nullptr; baseBackHint.clear(); prefer = SDL_Rect{}; }
};
FrameReg sCur, sPrev;
std::vector<int> sLayerStack, sScrollStack;
int sPrevTop = 0;

Ui::Id sFocus = 0;
bool   sFollow = true;              // keep the highlight in view (moved by pad, not by finger)
uint32_t sPage = 0;
std::unordered_map<uint32_t, Ui::Id> sMemory;
void (*sLog)(const char*) = nullptr;

// Plate
struct PlateState {
    SDL_FRect shown{}; float shownR = 12;
    SDL_FRect from{};  float fromR = 12;
    SDL_FRect target{};                    // where it was heading last frame
    float ring = 0, fromRing = 0;          // 0 = tinted plate, 1 = outline
    Ui::Id forId = 0; double start = -1, appear = -1;
    bool visibleLast = false, drawnNow = false;
    bool swift = false;                    // this move scrolls a list: no overshoot
};
// A list started gliding to keep the highlight in view this frame.
bool sScrollGlide = false;
constexpr float kGlide = 0.42f;            // the highlight's move, and a list's with it
PlateState P;
// The highlight's edges, drawn last (DrawRings): on screen, as they were.
struct RingState {
    bool pending = false;
    SDL_FRect s{}; float r = 0, k = 1, ring = 0, alpha = 1;
    bool clipped = false; SDL_Rect clip{};
} sRing;
double sBumpAt = -10; int sBumpDx = 0, sBumpDy = 0;

// Finger dragging a scroll area
Scroll* sDrag = nullptr;
// The clip each open scroll found, to put back when it ends. A stack: a menu
// with its own list, opened inside a scrolling page, nests one in the other
// (a single saved clip was overwritten by the inner one, and the outer one
// then left the page clipped to the menu, hiding the sidebar and the hints).
struct SavedClip { bool on; SDL_Rect r; };
std::vector<SavedClip> sClipStack;

int TopIndex(const FrameReg& f) { return f.layers.empty() ? 0 : (int)f.layers.size() - 1; }

const NodeRec* Find(const FrameReg& f, Ui::Id id) {
    auto it = f.index.find(id);
    return it == f.index.end() ? nullptr : &f.nodes[it->second];
}

void Log(const char* what, const NodeRec* n) {
    if (!sLog) return;
    char line[96];
    if (n) snprintf(line, sizeof line, "focus %s %08x at %d,%d %dx%d", what, (unsigned)n->n.id, n->n.r.x, n->n.r.y, n->n.r.w, n->n.r.h);
    else   snprintf(line, sizeof line, "focus %s none", what);
    sLog(line);
}

// LLM-Selfbot's nextFocus (webui/src/lib/bigpicture/spatial.ts).
// The next thing in a direction, the way Android's FocusFinder (and so TVs
// and consoles) chooses: only what really lies that way; something in line
// with the highlight (its "beam") beats something off to the side that is not
// wholly that way (Up from a button never lands on a neighbour barely higher
// beside it); otherwise the nearest, the way counting 13 times more than the
// offset across.
namespace nav {
struct R { long l, t, r, b; long cx() const { return (l + r) / 2; } long cy() const { return (t + b) / 2; } };
enum Dir { Left, Right, Up, Down };
bool IsCandidate(Dir d, const R& s, const R& x) {
    switch (d) {
        case Left:  return (s.r > x.r || s.l >= x.r) && s.l > x.l;
        case Right: return (s.l < x.l || s.r <= x.l) && s.r < x.r;
        case Up:    return (s.b > x.b || s.t >= x.b) && s.t > x.t;
        default:    return (s.t < x.t || s.b <= x.t) && s.b < x.b;
    }
}
bool BeamsOverlap(Dir d, const R& s, const R& x) {
    return (d == Left || d == Right) ? (x.b > s.t && x.t < s.b) : (x.r > s.l && x.l < s.r);
}
bool IsToDirectionOf(Dir d, const R& s, const R& x) {
    switch (d) {
        case Left:  return s.l >= x.r;
        case Right: return s.r <= x.l;
        case Up:    return s.t >= x.b;
        default:    return s.b <= x.t;
    }
}
long Major(Dir d, const R& s, const R& x) {
    const long v = d == Left ? s.l - x.r : d == Right ? x.l - s.r : d == Up ? s.t - x.b : x.t - s.b;
    return std::max(0L, v);
}
long MajorFar(Dir d, const R& s, const R& x) {
    const long v = d == Left ? s.l - x.l : d == Right ? x.r - s.r : d == Up ? s.t - x.t : x.b - s.b;
    return std::max(1L, v);
}
long Minor(Dir d, const R& s, const R& x) {
    return (d == Left || d == Right) ? labs(s.cy() - x.cy()) : labs(s.cx() - x.cx());
}
// Is x1 in line where x2 is not, and so the better one?
bool BeamBeats(Dir d, const R& s, const R& x1, const R& x2) {
    const bool in1 = BeamsOverlap(d, s, x1), in2 = BeamsOverlap(d, s, x2);
    if (in2 || !in1) return false;
    if (!IsToDirectionOf(d, s, x2)) return true;
    if (d == Left || d == Right) return true;
    // Going up or down, in line wins unless the other is wholly closer.
    return Major(d, s, x1) < MajorFar(d, s, x2);
}
long Weighted(Dir d, const R& s, const R& x) {
    const long a = Major(d, s, x), b = Minor(d, s, x);
    return 13 * a * a + b * b;
}
bool Better(Dir d, const R& s, const R& x1, const R& x2) {
    if (!IsCandidate(d, s, x1)) return false;
    if (!IsCandidate(d, s, x2)) return true;
    if (BeamBeats(d, s, x1, x2)) return true;
    if (BeamBeats(d, s, x2, x1)) return false;
    return Weighted(d, s, x1) < Weighted(d, s, x2);
}
R Of(const SDL_Rect& q) { return R{ q.x, q.y, (long)q.x + q.w, (long)q.y + q.h }; }
}  // namespace nav

const NodeRec* NextFocus(const FrameReg& f, int layer, const NodeRec* a, int dx, int dy) {
    const nav::Dir d = dx < 0 ? nav::Left : dx > 0 ? nav::Right : dy < 0 ? nav::Up : nav::Down;
    const nav::R s = nav::Of(a->n.r);
    // Two rounds, so being in line is weighed once against the best of the
    // rest (one by one it was not: a far button straight above lost to rows
    // off to the side, and those to a row merely beside the highlight).
    const NodeRec* bestIn = nullptr;     // in line, and wholly that way
    const NodeRec* bestOut = nullptr;    // everything else that lies that way
    nav::R inR{}, outR{};
    for (const NodeRec& b : f.nodes) {
        if (b.layer != layer || (b.n.flags & Disabled) || b.n.id == a->n.id) continue;
        const nav::R x = nav::Of(b.n.r);
        if (!nav::IsCandidate(d, s, x)) continue;
        if (nav::BeamsOverlap(d, s, x) && nav::IsToDirectionOf(d, s, x)) {
            if (!bestIn || nav::Weighted(d, s, x) < nav::Weighted(d, s, inR)) { bestIn = &b; inR = x; }
        } else if (!bestOut || nav::Better(d, s, x, outR)) {
            bestOut = &b; outR = x;
        }
    }
    if (!bestIn) return bestOut;
    if (!bestOut) return bestIn;
    return nav::Better(d, s, outR, inR) ? bestOut : bestIn;
}

const NodeRec* FirstOf(const FrameReg& f, int layer) {
    const NodeRec* best = nullptr;
    const NodeRec* bestIn = nullptr;     // the same, inside the page's preferred area
    const SDL_Rect& pa = f.prefer;
    for (const NodeRec& b : f.nodes) {
        if (b.layer != layer || (b.n.flags & Disabled)) continue;
        if (b.n.flags & Default) return &b;
        auto better = [&](const NodeRec* cur) {
            return !cur || b.n.r.y < cur->n.r.y || (b.n.r.y == cur->n.r.y && b.n.r.x < cur->n.r.x);
        };
        if (better(best)) best = &b;
        if (layer == 0 && pa.w > 0) {
            const int cx = b.n.r.x + b.n.r.w / 2, cy = b.n.r.y + b.n.r.h / 2;
            if (cx >= pa.x && cy >= pa.y && cx < pa.x + pa.w && cy < pa.y + pa.h && better(bestIn)) bestIn = &b;
        }
    }
    return bestIn ? bestIn : best;
}

bool Inside(const SDL_Rect& r, float x, float y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

Scroll* ScrollOfFocus(const FrameReg& f, int layer, SDL_Rect* view) {
    const NodeRec* n = Find(f, sFocus);
    if (n && n->scroll >= 0) { if (view) *view = f.scrolls[n->scroll].view; return f.scrolls[n->scroll].st; }
    for (const ScrollRec& s : f.scrolls) if (s.layer == layer) { if (view) *view = s.view; return s.st; }
    return nullptr;
}

void Press(const NodeRec* n) {
    if (!n->n.press) return;          // a press that does nothing makes no sound
    Motion::Press(n->n.id);
    if (!(n->n.flags & OwnSound)) {
        Sound::Play(Sound::Voice::Tap);
        Sound::Play(Sound::Voice::Up, { 0.5f, false, 1.f, 70.f });
    }
    if (n->n.press) n->n.press();
}

}  // namespace

void Add(Node n) {
    NodeRec rec{ std::move(n), sLayerStack.empty() ? 0 : sLayerStack.back(), sScrollStack.empty() ? -1 : sScrollStack.back() };
    sCur.index[rec.n.id] = (int)sCur.nodes.size();
    sCur.nodes.push_back(std::move(rec));
}
bool   Is(Ui::Id id) { return id != 0 && id == sFocus; }
Ui::Id Current() { return sFocus; }
void   Set(Ui::Id id) { if (id != sFocus) { sFocus = id; sFollow = true; } }
bool   Has(Ui::Id id) { return sCur.index.count(id) != 0; }
SDL_Rect RectOf(Ui::Id id) { const NodeRec* n = Find(sCur, id); return n ? n->n.r : SDL_Rect{}; }
bool CurrentIn(SDL_Rect r) {
    const NodeRec* n = Find(sPrev, sFocus);
    if (!n || n->layer != TopIndex(sPrev)) return false;
    const int cx = n->n.r.x + n->n.r.w / 2, cy = n->n.r.y + n->n.r.h / 2;
    return cx >= r.x && cx < r.x + r.w && cy >= r.y && cy < r.y + r.h;
}

void PushLayer(Layer kind, std::function<void()> onBack, const std::string& backHint, SDL_Rect panel) {
    sCur.layers.push_back({ kind, std::move(onBack), backHint, panel });
    sLayerStack.push_back((int)sCur.layers.size() - 1);
}
void PopLayer() { if (sLayerStack.size() > 1) sLayerStack.pop_back(); }
bool LayerIsTop() { return !sLayerStack.empty() && sLayerStack.back() >= sPrevTop; }

void BeginScroll(Ui::Id id, SDL_Rect view, Scroll& s) {
    sCur.scrolls.push_back({ id, view, &s, sLayerStack.empty() ? 0 : sLayerStack.back() });
    sScrollStack.push_back((int)sCur.scrolls.size() - 1);
    SavedClip saved{ SDL_RenderIsClipEnabled(gRen) == SDL_TRUE, SDL_Rect{} };
    if (saved.on) SDL_RenderGetClipRect(gRen, &saved.r);
    sClipStack.push_back(saved);
    SDL_Rect c = XformRect(view.x, view.y, view.w, view.h);
    // Inside another clip, only where both allow.
    if (saved.on && !SDL_IntersectRect(&c, &saved.r, &c)) c = SDL_Rect{ c.x, c.y, 0, 0 };
    SDL_RenderSetClipRect(gRen, &c);
}
void EndScroll(float contentHeight) {
    if (sScrollStack.empty()) return;
    sCur.scrolls[sScrollStack.back()].st->content = contentHeight;
    sScrollStack.pop_back();
    if (sClipStack.empty()) { SDL_RenderSetClipRect(gRen, nullptr); return; }
    const SavedClip saved = sClipStack.back();
    sClipStack.pop_back();
    if (saved.on) SDL_RenderSetClipRect(gRen, &saved.r); else SDL_RenderSetClipRect(gRen, nullptr);
}

void Command(uint32_t btn, const std::string& hint, std::function<void()> fn) {
    sCur.cmds.push_back({ btn, hint, std::move(fn), sLayerStack.empty() ? 0 : sLayerStack.back() });
}
void SetBaseBack(std::function<void()> fn, const std::string& hint) { sCur.baseBack = std::move(fn); sCur.baseBackHint = hint; }
void PreferArea(SDL_Rect r) { sCur.prefer = r; }
bool HasCommand(uint32_t btn) {
    const int top = sLayerStack.empty() ? 0 : sLayerStack.back();
    for (const CmdRec& c : sCur.cmds) if (c.btn == btn && c.layer == top) return true;
    return false;
}
bool LayerOpen() { return sCur.layers.size() > 1; }

void Resolve(const Input::Frame& in) {
    const FrameReg& f = sPrev;
    const int top = TopIndex(f);
    const NodeRec* cur = Find(f, sFocus);
    if (cur && cur->layer != top) cur = nullptr;

    // ── Finger ──
    if (in.pressed) {
        sDrag = nullptr;
        for (const ScrollRec& s : f.scrolls)
            if (s.layer == top && Inside(s.view, in.x, in.y)) { sDrag = s.st; break; }
    }
    if (sDrag && in.touching && in.dragging) {
        sDrag->target -= in.dy; sDrag->offset = sDrag->target; sDrag->vel = 0; sFollow = false;
    }
    if (in.released) {
        if (sDrag && in.dragging) sDrag->vel = -in.vy;
        sDrag = nullptr;
    }
    if (in.tap) {
        const NodeRec* hit = nullptr;
        for (int i = (int)f.nodes.size() - 1; i >= 0; i--) {
            const NodeRec& n = f.nodes[i];
            if (n.layer != top || (n.n.flags & Disabled) || !Inside(n.n.r, in.x, in.y)) continue;
            if (n.scroll >= 0 && !Inside(f.scrolls[n.scroll].view, in.x, in.y)) continue;
            hit = &n; break;
        }
        if (hit) {
            if (sFocus != hit->n.id) { sFocus = hit->n.id; Log("tap", hit); }
            sFollow = false;
            const SDL_Rect& c = hit->n.ctl;
            if ((hit->n.flags & AdjustX) && hit->n.adjust && c.w > 0 && Inside(c, in.x, in.y)) {
                const int pv = hit->n.pivot >= 0 ? hit->n.pivot : c.x + c.w / 2;
                if (!(hit->n.press && fabsf(in.x - pv) <= hit->n.dead / 2.f)) {
                    Motion::Press(hit->n.id);
                    hit->n.adjust(in.x < pv ? -1 : 1);
                    return;
                }
            }
            Press(hit);
            return;
        }
        if (top > 0 && f.layers[top].kind == Layer::Sheet && !Inside(f.layers[top].panel, in.x, in.y) && f.layers[top].onBack) {
            Sound::Play(Sound::Voice::Release);
            f.layers[top].onBack();
            return;
        }
    }

    // ── Directions ──
    if (in.stepX || in.stepY) {
        if (!cur) {
            if (const NodeRec* d = FirstOf(f, top)) { sFocus = d->n.id; sFollow = true; Log("first", d); }
        } else if (cur->n.flags & Capture) {
            if (cur->n.move) cur->n.move(in.stepX, in.stepY);
        } else if (in.stepX && (cur->n.flags & AdjustX) && cur->n.adjust) {
            cur->n.adjust(in.stepX);
        } else {
            const NodeRec* to = NextFocus(f, top, cur, in.stepX, in.stepY);
            if (to) {
                sFocus = to->n.id; sFollow = true;
                Sound::Play(Sound::Voice::Tick, { Sound::PitchAt(to->n.r.x + to->n.r.w / 2, to->n.r.y + to->n.r.h / 2) });
                Log("move", to);
            } else {
                sBumpAt = Motion::Now(); sBumpDx = in.stepX; sBumpDy = in.stepY;
                Sound::Play(Sound::Voice::Seat, { 0.5f, true, 0.6f });
            }
        }
    }
    // Held stick on a capture node (the map): it wants continuous moves too.
    // (The step repeat above already delivers them at the repeat rate.)

    // ── A ──
    if (in.down & Input::A) {
        if (cur) Press(cur);
        else if (const NodeRec* d = FirstOf(f, top)) { sFocus = d->n.id; sFollow = true; }
    }
    // ── B ──
    if (in.down & Input::B) {
        if (top > 0) {
            if (f.layers[top].onBack) { Sound::Play(Sound::Voice::Release); f.layers[top].onBack(); }
        } else if (f.baseBack) {
            f.baseBack();
        }
    }
    // ── Other buttons ──
    const uint32_t others[] = { Input::X, Input::Y, Input::L, Input::R, Input::ZL, Input::ZR, Input::Minus, Input::Plus };
    for (uint32_t b : others) {
        if (!(in.down & b)) continue;
        bool done = false;
        for (const CmdRec& c : f.cmds)
            if (c.btn == b && c.layer == top && c.fn) { c.fn(); done = true; break; }
        if (done || !cur) continue;
        if (b == Input::ZL || b == Input::ZR) {
            // Page up / down: the node nearest one screen of the list away.
            SDL_Rect view{};
            if (!ScrollOfFocus(f, top, &view) || cur->scroll < 0) continue;
            const int dir = b == Input::ZR ? 1 : -1;
            const float ty = cur->n.r.y + cur->n.r.h / 2.f + dir * (view.h - 40);
            const float tx = cur->n.r.x + cur->n.r.w / 2.f;
            const NodeRec* best = nullptr; float bd = 1e30f;
            for (const NodeRec& n : f.nodes) {
                if (n.layer != top || n.scroll != cur->scroll || (n.n.flags & Disabled)) continue;
                const float d = fabsf(n.n.r.y + n.n.r.h / 2.f - ty) * 1.f + fabsf(n.n.r.x + n.n.r.w / 2.f - tx) * 0.5f;
                if (d < bd) { bd = d; best = &n; }
            }
            if (best && best != cur) {
                sFocus = best->n.id; sFollow = true;
                Sound::Play(Sound::Voice::Tick, { Sound::PitchAt(best->n.r.x, best->n.r.y), true });
            }
        }
    }
    // ── Right stick: scroll ──
    if (in.ry != 0.f) {
        if (Scroll* s = ScrollOfFocus(f, top, nullptr)) { s->target += in.ry * 1100.f * Motion::Dt(); s->vel = 0; sFollow = false; }
    }
}

void BeginBuild() {
    sPrevTop = TopIndex(sPrev);
    sRing.pending = false;
    sCur.Clear();
    sLayerStack.clear(); sScrollStack.clear(); sClipStack.clear();
    sCur.layers.push_back({ Layer::Base, nullptr, std::string(), SDL_Rect{} });
    sLayerStack.push_back(0);
    P.drawnNow = false;
}

void EndBuild() {
    const int top = TopIndex(sCur);
    const NodeRec* cur = Find(sCur, sFocus);
    if (!cur || cur->layer != top || (cur->n.flags & Disabled)) {
        const NodeRec* pick = nullptr;
        if (top == 0) {
            auto m = sMemory.find(sPage);
            if (m != sMemory.end()) { const NodeRec* r = Find(sCur, m->second); if (r && r->layer == 0) pick = r; }
        }
        if (!pick) pick = FirstOf(sCur, top);
        sFocus = pick ? pick->n.id : 0;
        sFollow = true;
        cur = pick;
        if (pick) Log("land", pick);
    }
    // A page remembers where its highlight was inside it: not on what stays
    // around every page (the editor's sidebar, outside the preferred area), or
    // coming back by tapping the sidebar would land on another item.
    if (cur && top == 0) {
        const SDL_Rect& pa = sCur.prefer;
        if (pa.w <= 0 || Inside(pa, cur->n.r.x + cur->n.r.w / 2.f, cur->n.r.y + cur->n.r.h / 2.f)) sMemory[sPage] = sFocus;
    }

    // Keep the highlight in view, and move every scroll area on.
    const float dt = Motion::Dt();
    const double nowS = Motion::Now();
    sScrollGlide = false;
    for (ScrollRec& s : sCur.scrolls) {
        Scroll& st = *s.st;
        st.prev = st.offset;
        const float maxOff = std::max(0.f, st.content - (float)s.view.h);
        if (sFollow && cur && cur->scroll >= 0 && sCur.scrolls[cur->scroll].st == s.st) {
            const int margin = std::min(28, s.view.h / 6);
            const int top_ = s.view.y + margin, bot = s.view.y + s.view.h - margin;
            // Where the highlight will be once the list is where it is going
            // (its rectangle is where the list is now, maybe mid-glide).
            const float ny = (float)cur->n.r.y - (st.target - st.offset);
            float t = st.target;
            if (ny < top_) t -= (float)top_ - ny;
            else if (ny + cur->n.r.h > bot) t += ny + cur->n.r.h - (float)bot;
            // As far as the list goes: the last row near the bottom edge
            // cannot ask for more, or the glide would start over every frame.
            t = std::max(0.f, std::min(maxOff, t));
            // The highlight moved somewhere out of view: the list glides there
            // with it (same curve, same time), so on screen the highlight
            // goes straight to its place while the list slides under it.
            if (t != st.target) {
                st.target = t;
                if (st.vel == 0.f && sDrag != s.st) { st.from = st.offset; st.t0 = nowS; sScrollGlide = true; }
            }
        }
        if (st.vel != 0.f) {
            st.t0 = -1;
            st.target += st.vel * dt;
            st.vel *= expf(-dt * 4.5f);
            if (fabsf(st.vel) < 20.f) st.vel = 0;
        }
        if (st.target < 0) { st.target = 0; st.vel = 0; }
        if (st.target > maxOff) { st.target = maxOff; st.vel = 0; }
        if (sDrag == s.st || Motion::Reduced()) { st.offset = st.target; st.t0 = -1; }
        else if (st.t0 >= 0) {
            const float k = Motion::Ease(Motion::Curve::Swift, (float)((nowS - st.t0) / kGlide));
            st.offset = st.from + (st.target - st.from) * k;
            if (nowS - st.t0 >= kGlide) { st.offset = st.target; st.t0 = -1; }
        } else {
            st.offset += (st.target - st.offset) * (1.f - expf(-dt * 14.f));
            if (fabsf(st.target - st.offset) < 0.5f) st.offset = st.target;
        }
    }
    P.visibleLast = P.drawnNow;
    if (!P.drawnNow) P.forId = 0;
    std::swap(sPrev, sCur);
}

SDL_Color PlateColor() {
    // Big Picture's --bp-fill-hi: accent 20% over white 5%.
    const SDL_Color a = gTheme.gold;
    return SDL_Color{ (uint8_t)(a.r * 0.833f + 255 * 0.167f), (uint8_t)(a.g * 0.833f + 255 * 0.167f),
                      (uint8_t)(a.b * 0.833f + 255 * 0.167f), 61 };
}

void DrawPlate(Ui::Id id, const SDL_Rect* at) {
    const NodeRec* n = Find(sCur, id);
    if (!n || n->n.id != sFocus || (n->n.flags & NoPlate)) return;
    const double now = Motion::Now();
    const SDL_Rect& tr = at ? *at : n->n.r;
    const SDL_FRect target{ (float)tr.x, (float)tr.y, (float)tr.w, (float)tr.h };
    const float targetR = (float)n->n.radius;
    const float wantRing = (n->n.flags & Ring) ? 1.f : 0.f;
    if (P.forId != id) {
        if (P.visibleLast) { P.from = P.shown; P.fromR = P.shownR; P.fromRing = P.ring; P.appear = -1; }
        else { P.from = target; P.fromR = targetR; P.fromRing = wantRing; P.appear = now; }
        P.start = now; P.forId = id;
        P.swift = sScrollGlide;
    } else {
        // The same thing moved (a list scrolled, a row of tabs slid, the page
        // was laid out again): small steps carry the plate along, a jump flies
        // it over instead of snapping. Moved by its list scrolling (however
        // far in one frame), it is carried along too.
        const float dx = target.x - P.target.x, dy = target.y - P.target.y;
        const float dw = target.w - P.target.w, dh = target.h - P.target.h;
        float scrolled = 0.f;
        if (n->scroll >= 0 && n->scroll < (int)sCur.scrolls.size()) {
            const Scroll* st = sCur.scrolls[n->scroll].st;
            scrolled = -(st->offset - st->prev);
        }
        const bool byScroll = dx == 0 && dw == 0 && dh == 0 && fabsf(dy - scrolled) < 1.f;
        if (!byScroll && (fabsf(dx) > 40.f || fabsf(dy) > 40.f || fabsf(dw) > 40.f || fabsf(dh) > 40.f)) {
            P.from = P.shown; P.fromR = P.shownR; P.fromRing = P.ring; P.start = now;
        } else if (dx != 0 || dy != 0 || dw != 0 || dh != 0) {
            P.from.x += dx; P.from.y += dy; P.from.w += dw; P.from.h += dh;
        }
    }
    P.target = target;
    float e = 1.f;
    if (!Motion::Reduced()) e = Motion::Ease(P.swift ? Motion::Curve::Swift : Motion::Curve::Spring, (float)((now - P.start) / kGlide));
    SDL_FRect s{ P.from.x + (target.x - P.from.x) * e, P.from.y + (target.y - P.from.y) * e,
                 P.from.w + (target.w - P.from.w) * e, P.from.h + (target.h - P.from.h) * e };
    const float ek = std::max(0.f, std::min(1.f, e));
    const float r = P.fromR + (targetR - P.fromR) * ek;
    const float ring = P.fromRing + (wantRing - P.fromRing) * ek;
    const float bt = (float)((now - sBumpAt) / 0.26);
    if (bt >= 0 && bt < 1 && !Motion::Reduced()) {
        const float k = bt < 0.35f ? Motion::Ease(Motion::Curve::Knob, bt / 0.35f)
                                   : 1.f - Motion::Ease(Motion::Curve::EaseOut, (bt - 0.35f) / 0.65f);
        s.x += sBumpDx * 9.f * k; s.y += sBumpDy * 9.f * k;
    }
    float appear = 1.f;
    if (P.appear >= 0 && !Motion::Reduced()) appear = std::max(0.f, std::min(1.f, (float)((now - P.appear) / 0.22)));
    // A tinted plate (rows, list items): the thing lights up. The tint goes
    // under it; its edge and the outline wait for the end of the frame.
    if (ring < 0.999f) {
        SDL_Color c = PlateColor();
        c.a = (uint8_t)(c.a * (1.f - ring) * appear);
        FillRectRounded((int)lroundf(s.x), (int)lroundf(s.y), (int)lroundf(s.w), (int)lroundf(s.h), (int)lroundf(r), c);
    }
    // The edges are drawn last (Focus::DrawRings), over whatever comes after
    // this thing on the page (the next tile, a card's lifted neighbour), as
    // they are now: where this transform and clip put them.
    {
        float x0 = s.x, y0 = s.y;
        XformPoint(x0, y0);
        const float k = XformScale();
        sRing.s = SDL_FRect{ x0, y0, s.w * k, s.h * k };
        sRing.r = r * k;
        sRing.k = k;
        sRing.ring = ring;
        sRing.alpha = appear * XformAlpha(255) / 255.f;
        sRing.clipped = SDL_RenderIsClipEnabled(gRen) == SDL_TRUE;
        if (sRing.clipped) SDL_RenderGetClipRect(gRen, &sRing.clip);
        sRing.pending = true;
    }
    P.shown = s; P.shownR = r; P.ring = ring; P.drawnNow = true;
}

void DrawRings() {
    if (!sRing.pending) return;
    sRing.pending = false;
    const SDL_FRect& s = sRing.s;
    const float r = sRing.r, k = sRing.k, ring = sRing.ring, a = sRing.alpha;
    SDL_Rect oldClip{};
    const bool oldClipped = SDL_RenderIsClipEnabled(gRen) == SDL_TRUE;
    if (oldClipped) SDL_RenderGetClipRect(gRen, &oldClip);
    if (sRing.clipped) SDL_RenderSetClipRect(gRen, &sRing.clip); else SDL_RenderSetClipRect(gRen, nullptr);
    PushXformIdentity();
    // A plate's crisp edge.
    if (ring < 0.999f)
        StrokeRectRounded(s.x + 0.6f * k, s.y + 0.6f * k, s.w - 1.2f * k, s.h - 1.2f * k, std::max(0.f, r - 0.6f * k), 1.2f * k,
                          Alpha(gTheme.gold, (int)(78 * (1.f - ring) * a)));
    // An outline (buttons, chips, cards): nothing darkens what is inside it.
    if (ring > 0.001f) {
        const SDL_Color core = Mix(gTheme.gold, SDL_Color{ 255, 255, 255, 255 }, 0.3f);
        StrokeRectRounded(s.x - 1.f * k, s.y - 1.f * k, s.w + 2.f * k, s.h + 2.f * k, r + 1.f * k, 6.f * k,
                          Alpha(gTheme.gold, (int)(46 * ring * a)));
        StrokeRectRounded(s.x + 1.25f * k, s.y + 1.25f * k, s.w - 2.5f * k, s.h - 2.5f * k, std::max(0.f, r - 1.25f * k), 2.5f * k,
                          Alpha(core, (int)(235 * ring * a)));
    }
    PopXform();
    if (oldClipped) SDL_RenderSetClipRect(gRen, &oldClip); else SDL_RenderSetClipRect(gRen, nullptr);
}

void Hints(std::vector<Hint>& out) {
    out.clear();
    const int top = TopIndex(sCur);
    const NodeRec* n = Find(sCur, sFocus);
    if (n && n->layer == top) {
        if (n->n.flags & AdjustX) out.push_back({ "\xE2\x97\x80\xE2\x96\xB6", "" });   // label set by caller
        if (n->n.press && !n->n.hint.empty()) out.push_back({ "A", n->n.hint });
    }
    for (const CmdRec& c : sCur.cmds) if (c.layer == top && !c.hint.empty()) {
        const char* k = c.btn == Input::X ? "X" : c.btn == Input::Y ? "Y" : c.btn == Input::L ? "L" : c.btn == Input::R ? "R"
                      : c.btn == Input::ZL ? "ZL" : c.btn == Input::ZR ? "ZR" : c.btn == Input::Minus ? "-" : "+";
        // A pair with the same hint (L and R, ZL and ZR) shows as one: "L R  Section".
        if (!out.empty() && out.back().label == c.hint &&
            ((out.back().key == "L" && c.btn == Input::R) || (out.back().key == "ZL" && c.btn == Input::ZR))) {
            out.back().key += " ";
            out.back().key += k;
            continue;
        }
        out.push_back({ k, c.hint });
    }
    if (top > 0) { if (!sCur.layers[top].backHint.empty()) out.push_back({ "B", sCur.layers[top].backHint }); }
    else if (!sCur.baseBackHint.empty()) out.push_back({ "B", sCur.baseBackHint });
}

void EnterPage(uint32_t page) {
    if (page == sPage) return;
    sPage = page;
    auto m = sMemory.find(page);
    sFocus = m != sMemory.end() ? m->second : 0;
    sFollow = true;
    P.forId = 0; P.visibleLast = false;
}
void SetLogger(void (*fn)(const char*)) { sLog = fn; }
bool FocusCentre(float& x01, float& y01) {
    if (!P.visibleLast) return false;
    x01 = (P.shown.x + P.shown.w / 2) / SCREEN_W;
    y01 = (P.shown.y + P.shown.h / 2) / SCREEN_H;
    return true;
}

}  // namespace Focus
